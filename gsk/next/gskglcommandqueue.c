/* gskglcommandqueue.c
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <gdk/gdkglcontextprivate.h>
#include <gsk/gskdebugprivate.h>
#include <epoxy/gl.h>
#include <string.h>

#include "gskglattachmentstateprivate.h"
#include "gskglbufferprivate.h"
#include "gskglcommandqueueprivate.h"
#include "gskgluniformstateprivate.h"

struct _GskGLCommandQueue
{
  GObject parent_instance;

  /* The GdkGLContext we make current before executing GL commands. */
  GdkGLContext *context;

  /* Array of GskGLCommandBatch which is a fixed size structure that will
   * point into offsets of other arrays so that all similar data is stored
   * together. The idea here is that we reduce the need for pointers so that
   * using g_realloc()'d arrays is fine.
   */
  GArray *batches;

  /* Contains array of vertices and some wrapper code to help upload them
   * to the GL driver. We can also tweak this to use double buffered arrays
   * if we find that to be faster on some hardware and/or drivers.
   */
  GskGLBuffer *vertices;

  /* The GskGLAttachmentState contains information about our FBO and texture
   * attachments as we process incoming operations. We snapshot them into
   * various batches so that we can compare differences between merge
   * candidates.
   */
  GskGLAttachmentState *attachments;

  /* The uniform state across all programs. We snapshot this into batches so
   * that we can compare uniform state between batches to give us more
   * chances at merging draw commands.
   */
  GskGLUniformState *uniforms;

  /* Array of GskGLCommandDraw which allows us to have a static size field
   * in GskGLCommandBatch to coalesce draws. Multiple GskGLCommandDraw may
   * be processed together (and out-of-order) to reduce the number of state
   * changes when submitting commands.
   */
  GArray *batch_draws;

  /* Array of GskGLCommandBind which denote what textures need to be attached
   * to which slot. GskGLCommandDraw.bind_offset and bind_count reference this
   * array to determine what to attach.
   */
  GArray *batch_binds;

  /* Array of GskGLCommandUniform denoting which uniforms must be updated
   * before the glDrawArrays() may be called. These are referenced from the
   * GskGLCommandDraw.uniform_offset and uniform_count fields.
   */
  GArray *batch_uniforms;

  /* Sometimes we want to save attachment state so that operations we do
   * cannot affect anything that is known to the command queue. We call
   * gsk_gl_command_queue_save()/restore() which stashes attachment state
   * into this pointer array.
   */
  GPtrArray *saved_state;

  /* Sometimes it is handy to keep a number of textures or framebuffers
   * around until the frame has finished drawing. That way some objects can
   * be used immediately even though they won't have any rendering until the
   * frame has finished.
   *
   * When end_frame is called, we remove these resources.
   */
  GArray *autorelease_framebuffers;
  GArray *autorelease_textures;

  /* String storage for debug groups */
  GStringChunk *debug_groups;

  /* Discovered max texture size when loading the command queue so that we
   * can either scale down or slice textures to fit within this size. Assumed
   * to be both height and width.
   */
  int max_texture_size;

  /* The index of the last batch in @batches, which may not be the element
   * at the end of the array, as batches can be reordered. This is used to
   * update the "next" index when adding a new batch.
   */
  int tail_batch_index;

  /* If we're inside of a begin_draw()/end_draw() pair. */
  guint in_draw : 1;
};

typedef enum _GskGLCommandKind
{
  /* The batch will perform a glClear() */
  GSK_GL_COMMAND_KIND_CLEAR,

  /* THe batch represents a new debug group */
  GSK_GL_COMMAND_KIND_PUSH_DEBUG_GROUP,

  /* The batch represents the end of a debug group */
  GSK_GL_COMMAND_KIND_POP_DEBUG_GROUP,

  /* The batch will perform a glDrawArrays() */
  GSK_GL_COMMAND_KIND_DRAW,
} GskGLCommandKind;

typedef struct _GskGLCommandBind
{
  /* @texture is the value passed to glActiveTexture(), the "slot" the
   * texture will be placed into. We always use GL_TEXTURE_2D so we don't
   * waste any bits here to indicate that.
   */
  guint texture : 5;

  /* The identifier for the texture created with glGenTextures(). */
  guint id : 27;
} GskGLCommandBind;

G_STATIC_ASSERT (sizeof (GskGLCommandBind) == 4);

typedef struct _GskGLCommandBatchAny
{
  /* A GskGLCommandKind indicating what the batch will do */
  guint kind : 8;

  /* The program's identifier to use for determining if we can merge two
   * batches together into a single set of draw operations. We put this
   * here instead of the GskGLCommandDraw so that we can use the extra
   * bits here without making the structure larger.
   */
  guint program : 24;

  /* The index of the next batch following this one. This is used
   * as a sort of integer-based linked list to simplify out-of-order
   * batching without moving memory around. -1 indicates last batch.
   */
  int next_batch_index;

  /* The viewport size of the batch. We check this as we process
   * batches to determine if we need to resize the viewport.
   */
  struct {
    guint16 width;
    guint16 height;
  } viewport;
} GskGLCommandBatchAny;

G_STATIC_ASSERT (sizeof (GskGLCommandBatchAny) == 12);

typedef struct _GskGLCommandDraw
{
  GskGLCommandBatchAny head;

  /* There doesn't seem to be a limit on the framebuffer identifier that
   * can be returned, so we have to use a whole unsigned for the framebuffer
   * we are drawing to. When processing batches, we check to see if this
   * changes and adjust the render target accordingly. Some sorting is
   * performed to reduce the amount we change framebuffers.
   */
  guint framebuffer;

  /* The number of uniforms to change. This must be less than or equal to
   * GL_MAX_UNIFORM_LOCATIONS but only guaranteed up to 1024 by any OpenGL
   * implementation to be conformant.
   */
  guint uniform_count : 11;

  /* The number of textures to bind, which is only guaranteed up to 16
   * by the OpenGL specification to be conformant.
   */
  guint bind_count : 5;

  /* GL_MAX_ELEMENTS_VERTICES specifies 33000 for this which requires 16-bit
   * to address all possible counts <= GL_MAX_ELEMENTS_VERTICES.
   */
  guint vbo_count : 16;

  /* The offset within the VBO containing @vbo_count vertices to send with
   * glDrawArrays().
   */
  guint vbo_offset;

  /* The offset within the array of uniform changes to be made containing
   * @uniform_count #GskGLCommandUniform elements to apply.
   */
  guint uniform_offset;

  /* The offset within the array of bind changes to be made containing
   * @bind_count #GskGLCommandBind elements to apply.
   */
  guint bind_offset;
} GskGLCommandDraw;

G_STATIC_ASSERT (sizeof (GskGLCommandDraw) == 32);

typedef struct _GskGLCommandUniform
{
  GskGLUniformInfo info;
  guint            location;
} GskGLCommandUniform;

G_STATIC_ASSERT (sizeof (GskGLCommandUniform) == 8);

typedef union _GskGLCommandBatch
{
  GskGLCommandBatchAny    any;
  GskGLCommandDraw        draw;
  struct {
    GskGLCommandBatchAny  any;
    const char           *debug_group;
  } debug_group;
  struct {
    GskGLCommandBatchAny  any;
    guint bits;
    guint framebuffer;
  } clear;
} GskGLCommandBatch;

G_STATIC_ASSERT (sizeof (GskGLCommandBatch) == 32);

G_DEFINE_TYPE (GskGLCommandQueue, gsk_gl_command_queue, G_TYPE_OBJECT)

static void
gsk_gl_command_queue_save (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  g_ptr_array_add (self->saved_state,
                   gsk_gl_attachment_state_save (self->attachments));
}

static void
gsk_gl_command_queue_restore (GskGLCommandQueue *self)
{
  GskGLAttachmentState *saved;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (self->saved_state->len > 0);

  saved = g_ptr_array_steal_index (self->saved_state,
                                   self->saved_state->len - 1);

  gsk_gl_attachment_state_restore (saved);
}

static void
gsk_gl_command_queue_dispose (GObject *object)
{
  GskGLCommandQueue *self = (GskGLCommandQueue *)object;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  g_clear_object (&self->context);
  g_clear_pointer (&self->batches, g_array_unref);
  g_clear_pointer (&self->attachments, gsk_gl_attachment_state_free);
  g_clear_pointer (&self->uniforms, gsk_gl_uniform_state_free);
  g_clear_pointer (&self->vertices, gsk_gl_buffer_free);
  g_clear_pointer (&self->batch_draws, g_array_unref);
  g_clear_pointer (&self->batch_binds, g_array_unref);
  g_clear_pointer (&self->batch_uniforms, g_array_unref);
  g_clear_pointer (&self->saved_state, g_ptr_array_unref);
  g_clear_pointer (&self->autorelease_framebuffers, g_array_unref);
  g_clear_pointer (&self->autorelease_textures, g_array_unref);

  G_OBJECT_CLASS (gsk_gl_command_queue_parent_class)->dispose (object);
}

static void
gsk_gl_command_queue_class_init (GskGLCommandQueueClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsk_gl_command_queue_dispose;
}

static void
gsk_gl_command_queue_init (GskGLCommandQueue *self)
{
  self->max_texture_size = -1;

  self->batches = g_array_new (FALSE, TRUE, sizeof (GskGLCommandBatch));
  self->batch_draws = g_array_new (FALSE, FALSE, sizeof (GskGLCommandDraw));
  self->batch_binds = g_array_new (FALSE, FALSE, sizeof (GskGLCommandBind));
  self->batch_uniforms = g_array_new (FALSE, FALSE, sizeof (GskGLCommandUniform));
  self->attachments = gsk_gl_attachment_state_new ();
  self->vertices = gsk_gl_buffer_new (GL_ARRAY_BUFFER, sizeof (GskGLDrawVertex));
  self->uniforms = gsk_gl_uniform_state_new ();
  self->saved_state = g_ptr_array_new_with_free_func ((GDestroyNotify)gsk_gl_attachment_state_free);
  self->autorelease_textures = g_array_new (FALSE, FALSE, sizeof (GLuint));
  self->autorelease_framebuffers = g_array_new (FALSE, FALSE, sizeof (GLuint));
  self->debug_groups = g_string_chunk_new (4096);
}

GskGLCommandQueue *
gsk_gl_command_queue_new (GdkGLContext *context)
{
  GskGLCommandQueue *self;

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  self = g_object_new (GSK_TYPE_GL_COMMAND_QUEUE, NULL);
  self->context = g_object_ref (context);
  
  return g_steal_pointer (&self);
}

static GskGLCommandBatch *
begin_next_batch (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;
  guint index;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  index = self->batches->len;
  g_array_set_size (self->batches, index + 1);

  if (self->tail_batch_index != -1)
    {
      batch = &g_array_index (self->batches, GskGLCommandBatch, self->tail_batch_index);
      batch->any.next_batch_index = index;
    }

  batch = &g_array_index (self->batches, GskGLCommandBatch, index);
  batch->any.next_batch_index = -1;

  self->tail_batch_index = index;

  return batch;
}

void
gsk_gl_command_queue_begin_draw (GskGLCommandQueue     *self,
                                 guint                  program,
                                 const graphene_rect_t *viewport)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (viewport != NULL);

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_DRAW;
  batch->any.program = program;
  batch->any.next_batch_index = -1;
  batch->any.viewport.width = viewport->size.width;
  batch->any.viewport.height = viewport->size.height;
  batch->draw.framebuffer = 0;
  batch->draw.uniform_count = 0;
  batch->draw.uniform_offset = self->batch_uniforms->len;
  batch->draw.bind_count = 0;
  batch->draw.bind_offset = self->batch_binds->len;
  batch->draw.vbo_count = 0;
  batch->draw.vbo_offset = gsk_gl_buffer_get_offset (self->vertices);

  self->in_draw = TRUE;
}

static void
gsk_gl_command_queue_uniform_snapshot_cb (const GskGLUniformInfo *info,
                                          guint                   location,
                                          gpointer                user_data)
{
  GskGLCommandQueue *self = user_data;
  GskGLCommandUniform uniform;

  g_assert (info != NULL);
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  uniform.location = location;
  uniform.info = *info;

  g_array_append_val (self->batch_uniforms, uniform);
}

void
gsk_gl_command_queue_end_draw (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->batches->len > 0);
  g_return_if_fail (self->in_draw == TRUE);

  batch = &g_array_index (self->batches, GskGLCommandBatch, self->batches->len - 1);
  
  g_assert (batch->any.kind == GSK_GL_COMMAND_KIND_DRAW);

  /* Track the destination framebuffer in case it changed */
  batch->draw.framebuffer = self->attachments->fbo.id;
  self->attachments->fbo.changed = FALSE;

  /* Track the list of uniforms that changed */
  batch->draw.uniform_offset = self->batch_uniforms->len;
  gsk_gl_uniform_state_snapshot (self->uniforms,
                                 batch->any.program,
                                 gsk_gl_command_queue_uniform_snapshot_cb,
                                 self);
  batch->draw.uniform_count = self->batch_uniforms->len - batch->draw.uniform_offset;

  /* Track the bind attachments that changed */
  batch->draw.bind_offset = self->batch_binds->len;
  batch->draw.bind_count = 0;
  for (guint i = 0; i < G_N_ELEMENTS (self->attachments->textures); i++)
    {
      GskGLBindTexture *texture = &self->attachments->textures[i];

      if (texture->changed)
        {
          GskGLCommandBind bind;

          texture->changed = FALSE;

          bind.texture = texture->texture;
          bind.id = texture->id;

          g_array_append_val (self->batch_binds, bind);

          batch->draw.bind_count++;
        }
    }

  self->in_draw = FALSE;
}

GskGLDrawVertex *
gsk_gl_command_queue_add_vertices (GskGLCommandQueue     *self,
                                   const GskGLDrawVertex  vertices[GSK_GL_N_VERTICES])
{
  GskGLCommandBatch *batch;
  GskGLDrawVertex *dest;
  guint offset;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), NULL);
  g_return_val_if_fail (self->in_draw == TRUE, NULL);

  batch = &g_array_index (self->batches, GskGLCommandBatch, self->batches->len - 1);
  batch->draw.vbo_count += GSK_GL_N_VERTICES;

  dest = gsk_gl_buffer_advance (self->vertices, GSK_GL_N_VERTICES, &offset);

  if (vertices != NULL)
    {
      memcpy (dest, vertices, sizeof (GskGLDrawVertex) * GSK_GL_N_VERTICES);
      return NULL;
    }

  return dest;
}

void
gsk_gl_command_queue_clear (GskGLCommandQueue     *self,
                            guint                  clear_bits,
                            const graphene_rect_t *viewport)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (self->batches->len < G_MAXINT);

  if (clear_bits == 0)
    clear_bits = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_CLEAR;
  batch->any.viewport.width = viewport->size.width;
  batch->any.viewport.height = viewport->size.height;
  batch->clear.bits = clear_bits;
  batch->clear.framebuffer = self->attachments->fbo.id;
  batch->any.next_batch_index = -1;
  batch->any.program = 0;

  self->attachments->fbo.changed = FALSE;
}

void
gsk_gl_command_queue_push_debug_group (GskGLCommandQueue *self,
                                       const char        *debug_group)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (self->batches->len < G_MAXINT);

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_PUSH_DEBUG_GROUP;
  batch->debug_group.debug_group = g_string_chunk_insert (self->debug_groups, debug_group);
  batch->any.next_batch_index = -1;
  batch->any.program = 0;
}

void
gsk_gl_command_queue_pop_debug_group (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);
  g_return_if_fail (self->batches->len < G_MAXINT);

  batch = begin_next_batch (self);
  batch->any.kind = GSK_GL_COMMAND_KIND_POP_DEBUG_GROUP;
  batch->debug_group.debug_group = NULL;
  batch->any.next_batch_index = -1;
  batch->any.program = 0;
}

GdkGLContext *
gsk_gl_command_queue_get_context (GskGLCommandQueue *self)
{
  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), NULL);

  return self->context;
}

void
gsk_gl_command_queue_make_current (GskGLCommandQueue *self)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (GDK_IS_GL_CONTEXT (self->context));

  gdk_gl_context_make_current (self->context);
}

void
gsk_gl_command_queue_delete_program (GskGLCommandQueue *self,
                                     guint              program)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  glDeleteProgram (program);
  gsk_gl_uniform_state_clear_program (self->uniforms, program);
}

void
gsk_gl_command_queue_set_uniform1i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set1i (self->uniforms, program, location, value0);
}

void
gsk_gl_command_queue_set_uniform2i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0,
                                    int                value1)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set2i (self->uniforms, program, location, value0, value1);
}

void
gsk_gl_command_queue_set_uniform3i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0,
                                    int                value1,
                                    int                value2)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set3i (self->uniforms, program, location, value0, value1, value2);
}

void
gsk_gl_command_queue_set_uniform4i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0,
                                    int                value1,
                                    int                value2,
                                    int                value3)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set4i (self->uniforms, program, location, value0, value1, value2, value3);
}

void
gsk_gl_command_queue_set_uniform1f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set1f (self->uniforms, program, location, value0);
}

void
gsk_gl_command_queue_set_uniform2f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0,
                                    float              value1)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set2f (self->uniforms, program, location, value0, value1);
}

void
gsk_gl_command_queue_set_uniform3f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0,
                                    float              value1,
                                    float              value2)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set3f (self->uniforms, program, location, value0, value1, value2);
}

void
gsk_gl_command_queue_set_uniform4f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0,
                                    float              value1,
                                    float              value2,
                                    float              value3)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set4f (self->uniforms, program, location, value0, value1, value2, value3);
}

void
gsk_gl_command_queue_set_uniform2fv (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     gsize              count,
                                     const float       *value)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set2fv (self->uniforms, program, location, count, value);
}

void
gsk_gl_command_queue_set_uniform1fv (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     gsize              count,
                                     const float       *value)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set1fv (self->uniforms, program, location, count, value);
}

void
gsk_gl_command_queue_set_uniform4fv (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     gsize              count,
                                     const float       *value)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set4fv (self->uniforms, program, location, count, value);
}

void
gsk_gl_command_queue_set_uniform_matrix (GskGLCommandQueue       *self,
                                         guint                    program,
                                         guint                    location,
                                         const graphene_matrix_t *matrix)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (program > 0);
  g_return_if_fail (matrix != NULL);

  gsk_gl_uniform_state_set_matrix (self->uniforms, program, location, matrix);
}

void
gsk_gl_command_queue_set_uniform_color (GskGLCommandQueue *self,
                                        guint              program,
                                        guint              location,
                                        const GdkRGBA     *color)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_uniform_state_set_color (self->uniforms, program, location, color);
}

/**
 * gsk_gl_command_queue_set_uniform_texture:
 * @self: A #GskGLCommandQueue
 * @program: the program id
 * @location: the location of the uniform
 * @texture_target: a texture target such as %GL_TEXTURE_2D
 * @texture_slot: the texture slot such as %GL_TEXTURE0 or %GL_TEXTURE1
 * @texture_id: the id of the texture from glGenTextures()
 *
 * This sets the value of a uniform to map to @texture_slot (after subtracting
 * GL_TEXTURE0 from the value) and ensures that @texture_id is available in the
 * same texturing slot, ensuring @texture_target.
 */
void
gsk_gl_command_queue_set_uniform_texture (GskGLCommandQueue *self,
                                          guint              program,
                                          guint              location,
                                          GLenum             texture_target,
                                          GLenum             texture_slot,
                                          guint              texture_id)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (program > 0);
  g_assert (texture_target == GL_TEXTURE_1D ||
            texture_target == GL_TEXTURE_2D ||
            texture_target == GL_TEXTURE_3D);
  g_assert (texture_slot >= GL_TEXTURE0);
  g_assert (texture_slot < GL_TEXTURE16);

  gsk_gl_attachment_state_bind_texture (self->attachments,
                                        texture_target,
                                        texture_slot,
                                        texture_id);

  gsk_gl_uniform_state_set_texture (self->uniforms,
                                    program,
                                    location,
                                    texture_slot);
}

/**
 * gsk_gl_command_queue_set_uniform_rounded_rect:
 * @self: a #GskGLCommandQueue
 * @program: the program to execute
 * @location: the location of the uniform
 * @rounded_rect: the rounded rect to apply
 *
 * Sets a uniform that is expecting a rounded rect. This is stored as a
 * 4fv using glUniform4fv() when uniforms are applied to the progrma.
 */
void
gsk_gl_command_queue_set_uniform_rounded_rect (GskGLCommandQueue    *self,
                                               guint                 program,
                                               guint                 location,
                                               const GskRoundedRect *rounded_rect)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (program > 0);
  g_return_if_fail (rounded_rect != NULL);

  gsk_gl_uniform_state_set_rounded_rect (self->uniforms,
                                         program,
                                         location,
                                         rounded_rect);
}

static void
apply_uniform (GskGLUniformState      *state,
               const GskGLUniformInfo *info,
               guint                   location)
{
  const union {
    graphene_matrix_t matrix[0];
    GskRoundedRect rounded_rect[0];
    float fval[0];
    int ival[0];
  } *data;

  data = gsk_gl_uniform_state_get_uniform_data (state, info->offset);

  switch (info->format)
    {
    case GSK_GL_UNIFORM_FORMAT_1F:
      glUniform1f (location, data->fval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_2F:
      glUniform2f (location, data->fval[0], data->fval[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3F:
      glUniform3f (location, data->fval[0], data->fval[1], data->fval[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4F:
      glUniform4f (location, data->fval[0], data->fval[1], data->fval[2], data->fval[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_1FV:
      glUniform1fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_2FV:
      glUniform2fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_3FV:
      glUniform3fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_4FV:
      glUniform4fv (location, info->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_1I:
    case GSK_GL_UNIFORM_FORMAT_TEXTURE:
      glUniform1i (location, data->ival[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_2I:
      glUniform2i (location, data->ival[0], data->ival[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3I:
      glUniform3i (location, data->ival[0], data->ival[1], data->ival[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4I:
      glUniform4i (location, data->ival[0], data->ival[1], data->ival[2], data->ival[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_MATRIX: {
      float mat[16];
      graphene_matrix_to_float (&data->matrix[0], mat);
      glUniformMatrix4fv (location, 1, GL_FALSE, mat);
      break;
    }

    case GSK_GL_UNIFORM_FORMAT_COLOR:
      glUniform4fv (location, 1, &data->fval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT:
      if (info->flags & GSK_GL_UNIFORM_FLAGS_SEND_CORNERS)
        glUniform4fv (location, 3, (const float *)&data->rounded_rect[0]);
      else
        glUniform4fv (location, 1, (const float *)&data->rounded_rect[0]);
      break;

    default:
      g_assert_not_reached ();
    }
}

static inline void
apply_viewport (guint16 *current_width,
                guint16 *current_height,
                guint16 width,
                guint16 height)
{
  if (*current_width != width || *current_height != height)
    {
      *current_width = width;
      *current_height = height;
      glViewport (0, 0, width, height);
    }
}

/**
 * gsk_gl_command_queue_execute:
 * @self: a #GskGLCommandQueue
 *
 * Executes all of the batches in the command queue.
 */
void
gsk_gl_command_queue_execute (GskGLCommandQueue *self)
{
  GLuint framebuffer = 0;
  GLuint vao_id;
  int next_batch_index;
  guint16 width = 0;
  guint16 height = 0;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->in_draw == FALSE);

  if (self->batches->len == 0)
    return;

  gsk_gl_command_queue_make_current (self);

  glEnable (GL_DEPTH_TEST);
  glDepthFunc (GL_LEQUAL);

  /* Pre-multiplied alpha */
  glEnable (GL_BLEND);
  glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation (GL_FUNC_ADD);

  glGenVertexArrays (1, &vao_id);
  glBindVertexArray (vao_id);

  gsk_gl_buffer_submit (self->vertices);

  /* 0 = position location */
  glEnableVertexAttribArray (0);
  glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE,
                         sizeof (GskGLDrawVertex),
                         (void *) G_STRUCT_OFFSET (GskGLDrawVertex, position));

  /* 1 = texture coord location */
  glEnableVertexAttribArray (1);
  glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
                         sizeof (GskGLDrawVertex),
                         (void *) G_STRUCT_OFFSET (GskGLDrawVertex, uv));

  /* Start with default framebuffer */
  glBindFramebuffer (GL_FRAMEBUFFER, 0);

  next_batch_index = 0;

  while (next_batch_index >= 0)
    {
      const GskGLCommandBatch *batch = &g_array_index (self->batches, GskGLCommandBatch, next_batch_index);

      g_assert (batch->any.next_batch_index != next_batch_index);

      switch (batch->any.kind)
        {
        case GSK_GL_COMMAND_KIND_CLEAR:
          if (framebuffer != batch->clear.framebuffer)
            {
              framebuffer = batch->clear.framebuffer;
              glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
            }

          apply_viewport (&width, &height, batch->any.viewport.width, batch->any.viewport.width);

          glClear (batch->clear.bits);
        break;

        case GSK_GL_COMMAND_KIND_PUSH_DEBUG_GROUP:
          gdk_gl_context_push_debug_group (self->context, batch->debug_group.debug_group);
        break;

        case GSK_GL_COMMAND_KIND_POP_DEBUG_GROUP:
          gdk_gl_context_pop_debug_group (self->context);
        break;

        case GSK_GL_COMMAND_KIND_DRAW:
          if (batch->draw.framebuffer != framebuffer)
            {
              framebuffer = batch->draw.framebuffer;
              glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
            }

          apply_viewport (&width, &height, batch->any.viewport.width, batch->any.viewport.width);

          if (batch->draw.bind_count > 0)
            {
              for (guint i = 0; i < batch->draw.bind_count; i++)
                {
                  guint index = batch->draw.bind_offset + i;
                  GskGLCommandBind *bind = &g_array_index (self->batch_binds, GskGLCommandBind, index);

                  glActiveTexture (GL_TEXTURE0 + bind->texture);
                  glBindTexture (GL_TEXTURE_2D, bind->id);
                }
            }

          if (batch->draw.uniform_count > 0)
            {
              for (guint i = 0; i < batch->draw.uniform_count; i++)
                {
                  guint index = batch->draw.uniform_offset + i;
                  GskGLCommandUniform *u = &g_array_index (self->batch_uniforms, GskGLCommandUniform, index);

                  apply_uniform (self->uniforms, &u->info, u->location);
                }
            }

          glDrawArrays (GL_TRIANGLES, batch->draw.vbo_offset, batch->draw.vbo_count);

        break;

        default:
          g_assert_not_reached ();
        }

      next_batch_index = batch->any.next_batch_index;
    }

  glDeleteVertexArrays (1, &vao_id);
}

void
gsk_gl_command_queue_begin_frame (GskGLCommandQueue *self)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->batches->len == 0);

  self->tail_batch_index = -1;

  if (self->max_texture_size == -1)
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &self->max_texture_size);

  glBindFramebuffer (GL_FRAMEBUFFER, 0);

  for (guint i = 0; i < 8; i++)
    {
      glActiveTexture (GL_TEXTURE0 + i);
      glBindTexture (GL_TEXTURE_2D, 0);
    }

  glBindVertexArray (0);
  glUseProgram (0);
}

/**
 * gsk_gl_command_queue_end_frame:
 * @self: a #GskGLCommandQueue
 *
 * This function performs cleanup steps that need to be done after
 * a frame has finished. This is not performed as part of the command
 * queue execution to allow for the frame to be submitted as soon
 * as possible.
 *
 * However, it should be executed after the draw contexts end_frame
 * has been called to swap the OpenGL framebuffers.
 */
void
gsk_gl_command_queue_end_frame (GskGLCommandQueue *self)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (self->saved_state->len == 0);

  gsk_gl_uniform_state_end_frame (self->uniforms);

  /* Release autoreleased framebuffers */
  if (self->autorelease_framebuffers->len > 0)
    glDeleteFramebuffers (self->autorelease_framebuffers->len,
                          (GLuint *)(gpointer)self->autorelease_framebuffers->data);

  /* Release autoreleased textures */
  if (self->autorelease_textures->len > 0)
    glDeleteTextures (self->autorelease_textures->len,
                      (GLuint *)(gpointer)self->autorelease_textures->data);

  g_string_chunk_clear (self->debug_groups);

  self->batches->len = 0;
  self->batch_draws->len = 0;
  self->batch_uniforms->len = 0;
  self->batch_binds->len = 0;
  self->autorelease_framebuffers->len = 0;
  self->autorelease_textures->len = 0;
  self->tail_batch_index = -1;
}

gboolean
gsk_gl_command_queue_create_render_target (GskGLCommandQueue *self,
                                           int                width,
                                           int                height,
                                           guint             *out_fbo_id,
                                           guint             *out_texture_id)
{
  GLuint fbo_id = 0;
  GLint texture_id;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), FALSE);
  g_return_val_if_fail (width > 0, FALSE);
  g_return_val_if_fail (height > 0, FALSE);
  g_return_val_if_fail (out_fbo_id != NULL, FALSE);
  g_return_val_if_fail (out_texture_id != NULL, FALSE);

  gsk_gl_command_queue_save (self);

  texture_id = gsk_gl_command_queue_create_texture (self, width, height, GL_NEAREST, GL_NEAREST);

  if (texture_id == -1)
    {
      *out_fbo_id = 0;
      *out_texture_id = 0;
      gsk_gl_command_queue_restore (self);
      return FALSE;
    }

  fbo_id = gsk_gl_command_queue_create_framebuffer (self);

  glBindFramebuffer (GL_FRAMEBUFFER, fbo_id);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);
  g_assert_cmphex (glCheckFramebufferStatus (GL_FRAMEBUFFER), ==, GL_FRAMEBUFFER_COMPLETE);

  gsk_gl_command_queue_restore (self);

  *out_fbo_id = fbo_id;
  *out_texture_id = texture_id;

  return TRUE;
}

void
gsk_gl_command_queue_autorelease_framebuffer (GskGLCommandQueue *self,
                                              guint              framebuffer_id)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (framebuffer_id > 0);

  g_array_append_val (self->autorelease_framebuffers, framebuffer_id);
}

void
gsk_gl_command_queue_autorelease_texture (GskGLCommandQueue *self,
                                          guint              texture_id)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));
  g_return_if_fail (texture_id > 0);

  g_array_append_val (self->autorelease_textures, texture_id);
}

int
gsk_gl_command_queue_create_texture (GskGLCommandQueue *self,
                                     int                width,
                                     int                height,
                                     int                min_filter,
                                     int                mag_filter)
{
  GLuint texture_id;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), -1);

  if (width > self->max_texture_size || height > self->max_texture_size)
    return -1;

  gsk_gl_command_queue_save (self);
  gsk_gl_command_queue_make_current (self);

  glGenTextures (1, &texture_id);

  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, texture_id);

  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (gdk_gl_context_get_use_es (self->context))
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  else
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

  gsk_gl_command_queue_restore (self);

  return (int)texture_id;
}

guint
gsk_gl_command_queue_create_framebuffer (GskGLCommandQueue *self)
{
  GLuint fbo_id;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), -1);

  gsk_gl_command_queue_make_current (self);

  glGenFramebuffers (1, &fbo_id);

  return fbo_id;
}

void
gsk_gl_command_queue_bind_framebuffer (GskGLCommandQueue *self,
                                       guint              framebuffer)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_attachment_state_bind_framebuffer (self->attachments, framebuffer);
}
