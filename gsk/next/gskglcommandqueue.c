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

#include <gsk/gskdebugprivate.h>
#include <epoxy/gl.h>

#include "gskglattachmentstateprivate.h"
#include "gskglbufferprivate.h"
#include "gskglcommandqueueprivate.h"
#include "gskgluniformstateprivate.h"

/* The MAX_MERGE_DISTANCE is used to reduce how far back we'll look for
 * programs to merge a batch with. This number is specific to batches using
 * the same program as a secondary index (See program_link field) is used
 * for tracking those.
 */
#define MAX_MERGE_DISTANCE 5

struct _GskGLCommandQueue
{
  GObject parent_instance;

  GdkGLContext *context;

  /* Queue containing a linked list of all the GskGLCommandBatch that have
   * been allocated so that we can reuse them on the next frame without
   * allocating additional memory. Using stable pointers instead of offsets
   * into an array makes this a bit easier to manage from a life-cycle
   * standpoint as well as reordering using the links instead of memmove()s
   * in an array.
   */
  GQueue unused_batches;

  /* As we build the real command queue, we place the batches into this
   * queue by pushing onto the tail. Executing commands will result in
   * walking this queue from head to tail.
   */
  GQueue all_batches;

  /* When merging batches we want to skip all items between the merge
   * candidate and the previous within it's program. To do this we keep an
   * index of commands by program to avoid iteration overhead. This array
   * contains a GQueue for each program which will point into the statically
   * allocated @program_link in GskGLCommandBatch.
   *
   * After we find a merge candidate, we check for clipping and other
   * changes which might make them unacceptable to merge.
   */
  GArray *program_batches;

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

  /* Our VBO containing all the vertices to upload to the GPU before calling
   * glDrawArrays() to draw. Each drawing operation contains 6 vec4 with the
   * positions necessary to draw with glDrawArrays().
   */
  GskGLBuffer *vertices;

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

  int max_texture_size;
};

typedef struct _GskGLCommandDraw
{
  guint vao_offset;
  guint vao_count;
} GskGLCommandDraw;

G_STATIC_ASSERT (sizeof (GskGLCommandDraw) == 8);

typedef struct _GskGLCommandUniform
{
  guint offset;
  guint array_count : 16;
  guint location : 7;
  guint format : 5;
  guint flags : 4;
} GskGLCommandUniform;

G_STATIC_ASSERT (sizeof (GskGLCommandUniform) == 8);

typedef struct _GskGLCommandBatch
{
  /* An index into GskGLCommandQueue.all_batches */
  GList all_link;

  /* An index into GskGLCommandBatch.program_batches */
  GList program_link;

  union {
    GskGLBindTexture  bind;
    GskGLBindTexture *binds;
  };

  union {
    GskGLCommandDraw  draw;
    GskGLCommandDraw *draws;
  };

  union {
    GskGLCommandUniform  uniform;
    GskGLCommandUniform *uniforms;
  };

  guint is_clear : 1;
  guint program_changed : 1;
  guint program : 14;
  guint n_draws : 16;
  guint n_binds : 16;
  guint n_uniforms : 16;

  /* The framebuffer to use and if it has changed */
  GskGLBindFramebuffer framebuffer;
} GskGLCommandBatch;

G_DEFINE_TYPE (GskGLCommandQueue, gsk_gl_command_queue, G_TYPE_OBJECT)

static inline gboolean
ispow2 (guint n)
{
  return !(n & (n-1));
}

static GskGLCommandBatch *
gsk_gl_command_queue_alloc_batch (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  if G_LIKELY (self->unused_batches.length > 0)
    return g_queue_pop_head_link (&self->unused_batches)->data;

  batch = g_slice_new0 (GskGLCommandBatch);
  batch->all_link.data = batch;
  batch->program_link.data = batch;

  return batch;
}

static void
gsk_gl_command_queue_release_batch (GskGLCommandQueue *self,
                                    GskGLCommandBatch *batch)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (batch != NULL);

  g_queue_unlink (&self->all_batches, &batch->all_link);

  g_assert (batch->program ||
            (batch->program_link.prev == NULL &&
             batch->program_link.next == NULL));

  if (batch->program_link.prev || batch->program_link.next)
    {
      GQueue *queue = &g_array_index (self->program_batches, GQueue, batch->program);
      g_queue_unlink (queue, &batch->program_link);
    }

  if (batch->n_draws > 1)
    g_free (batch->draws);

  if (batch->n_binds > 1)
    g_free (batch->binds);

  batch->n_binds = 0;
  batch->n_draws = 0;
  batch->binds = NULL;
  batch->draws = NULL;
  batch->framebuffer.id = 0;
  batch->framebuffer.changed = FALSE;
  batch->program = 0;
  batch->program_changed = FALSE;

  g_assert (batch->program_link.prev == NULL);
  g_assert (batch->program_link.next == NULL);
  g_assert (batch->program_link.data == batch);
  g_assert (batch->all_link.prev == NULL);
  g_assert (batch->all_link.next == NULL);
  g_assert (batch->all_link.data == batch);

  g_queue_push_head_link (&self->unused_batches, &batch->all_link);
}

static void
gsk_gl_command_batch_apply_uniform (GskGLCommandBatch         *batch,
                                    GskGLUniformState         *state,
                                    const GskGLCommandUniform *uniform)
{
  const union {
    graphene_matrix_t matrix[0];
    GskRoundedRect rounded_rect[0];
    float fval[0];
    int ival[0];
  } *data;

  g_assert (batch != NULL);
  g_assert (uniform != NULL);

  data = gsk_gl_uniform_state_get_uniform_data (state, uniform->offset);

  switch (uniform->format)
    {
    case GSK_GL_UNIFORM_FORMAT_1F:
      glUniform1f (uniform->location, data->fval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_2F:
      glUniform2f (uniform->location, data->fval[0], data->fval[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3F:
      glUniform3f (uniform->location, data->fval[0], data->fval[1], data->fval[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4F:
      glUniform4f (uniform->location, data->fval[0], data->fval[1], data->fval[2], data->fval[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_1FV:
      glUniform1fv (uniform->location, uniform->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_2FV:
      glUniform2fv (uniform->location, uniform->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_3FV:
      glUniform3fv (uniform->location, uniform->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_4FV:
      glUniform4fv (uniform->location, uniform->array_count, data->fval);
      break;

    case GSK_GL_UNIFORM_FORMAT_1I:
    case GSK_GL_UNIFORM_FORMAT_TEXTURE:
      glUniform1i (uniform->location, data->ival[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_2I:
      glUniform2i (uniform->location, data->ival[0], data->ival[1]);
      break;

    case GSK_GL_UNIFORM_FORMAT_3I:
      glUniform3i (uniform->location, data->ival[0], data->ival[1], data->ival[2]);
      break;

    case GSK_GL_UNIFORM_FORMAT_4I:
      glUniform4i (uniform->location, data->ival[0], data->ival[1], data->ival[2], data->ival[3]);
      break;

    case GSK_GL_UNIFORM_FORMAT_MATRIX: {
      float mat[16];
      graphene_matrix_to_float (&data->matrix[0], mat);
      glUniformMatrix4fv (uniform->location, 1, GL_FALSE, mat);
      break;
    }

    case GSK_GL_UNIFORM_FORMAT_COLOR:
      glUniform4fv (uniform->location, 1, &data->fval[0]);
      break;

    case GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT:
      if (uniform->flags & GSK_GL_UNIFORM_FLAGS_SEND_CORNERS)
        glUniform4fv (uniform->location, 3, (const float *)&data->rounded_rect[0]);
      else
        glUniform4fv (uniform->location, 1, (const float *)&data->rounded_rect[0]);
      break;

    default:
      break;
    }
}

static void
gsk_gl_command_batch_draw (GskGLCommandBatch *batch,
                           guint              vao_offset,
                           guint              vao_count)
{
  GskGLCommandDraw *last;

  g_assert (batch != NULL);

  if (batch->n_draws == 0)
    {
      batch->draw.vao_offset = vao_offset;
      batch->draw.vao_count = vao_count;
      batch->n_draws = 1;
      return;
    }

  last = batch->n_draws == 1 ? &batch->draw : &batch->draws[batch->n_draws-1];

  if (last->vao_offset + last->vao_count == vao_offset)
    {
      batch->draw.vao_count += vao_count;
    }
  else if (batch->n_draws == 1)
    {
      GskGLCommandDraw *draws = g_new (GskGLCommandDraw, 16);

      draws[0].vao_offset = batch->draw.vao_offset;
      draws[0].vao_count = batch->draw.vao_count;
      draws[1].vao_offset = vao_offset;
      draws[1].vao_count = vao_count;

      batch->draws = draws;
      batch->n_draws = 2;
    }
  else
    {
      if G_UNLIKELY (batch->n_draws >= 16 && ispow2 (batch->n_draws))
        batch->draws = g_realloc (batch->draws, sizeof (GskGLCommandDraw) * batch->n_draws * 2);

      batch->draws[batch->n_draws].vao_count = vao_count;
      batch->draws[batch->n_draws].vao_offset = vao_offset;
      batch->n_draws++;
    }
}

static void
gsk_gl_command_batch_execute (GskGLCommandBatch *batch,
                              GskGLUniformState *uniforms)
{
  g_assert (batch != NULL);
  g_assert (batch->n_draws > 0);

  if (batch->framebuffer.changed)
    glBindFramebuffer (GL_FRAMEBUFFER, batch->framebuffer.id);

  if (batch->program_changed)
    glUseProgram (batch->program);

  if (batch->n_binds == 1)
    {
      g_assert (batch->bind.changed);

      glActiveTexture (batch->bind.texture);
      glBindTexture (batch->bind.target, batch->bind.id);
    }
  else if (batch->n_binds > 1)
    {
      for (guint i = 0; i < batch->n_binds; i++)
        {
          const GskGLBindTexture *bind = &batch->binds[i];

          g_assert (bind->changed);

          glActiveTexture (bind->texture);
          glBindTexture (bind->target, bind->id);
        }
    }

  if (batch->n_uniforms == 1)
    {
      gsk_gl_command_batch_apply_uniform (batch, uniforms, &batch->uniform);
    }
  else if (batch->n_uniforms > 0)
    {
      for (guint i = 0; i < batch->n_uniforms; i++)
        {
          const GskGLCommandUniform *uniform = &batch->uniforms[i];
          gsk_gl_command_batch_apply_uniform (batch, uniforms, uniform);
        }
    }

  if (batch->is_clear)
    {
      glClearColor (0, 0, 0, 0);
      glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
  else if (batch->n_draws == 1)
    {
      glDrawArrays (GL_TRIANGLES, batch->draw.vao_offset, batch->draw.vao_count);
    }
  else if (batch->n_draws > 1)
    {
      for (guint i = 0; i < batch->n_draws; i++)
        {
          const GskGLCommandDraw *draw = &batch->draws[i];

          g_assert (draw->vao_count > 0);

          glDrawArrays (GL_TRIANGLES, draw->vao_offset, draw->vao_count);
        }
    }
}

static void
gsk_gl_command_batch_uniform_cb (const GskGLUniformInfo *info,
                                 guint                   location,
                                 gpointer                user_data)
{
  GskGLCommandBatch *batch = user_data;
  GskGLCommandUniform *u;

  g_assert (batch != NULL);
  g_assert (info != NULL);

  if (batch->n_uniforms == 0)
    {
      u = &batch->uniform;
      batch->n_uniforms = 1;
    }
  else if (batch->n_uniforms == 1)
    {
      u = g_new (GskGLCommandUniform, 2);
      u[0] = batch->uniform;
      batch->uniforms = u;
      batch->n_uniforms = 2;
      u = &u[1];
    }
  else
    {
      u = g_realloc_n (batch->uniforms, batch->n_uniforms+1, sizeof (GskGLCommandUniform));
      batch->uniforms = u;
      u = &u[batch->n_uniforms];
      batch->n_uniforms++;
    }

  u->format = info->format;
  u->flags = info->flags;
  u->array_count = info->array_count;
  u->location = location;
  u->offset = info->offset;
}

static gboolean
gsk_gl_command_batch_mergeable (GskGLCommandBatch *batch,
                                GskGLCommandBatch *other)
{
  g_assert (batch != NULL);
  g_assert (other != NULL);
  g_assert (batch != other);

  if (batch->program != other->program)
    return FALSE;

  return FALSE;
}

static void
gsk_gl_command_queue_try_merge (GskGLCommandQueue *self,
                                GskGLCommandBatch *batch)
{
  guint count = 0;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));
  g_assert (batch != NULL);
  g_assert (batch->program != 0);

  /* We probably only want to look at the past couple by program to
   * avoid pathological situations. In most cases, they will naturally
   * come within the last few submissions.
   */

  for (const GList *iter = batch->program_link.prev;
       iter != NULL && count < MAX_MERGE_DISTANCE;
       iter = iter->prev, count++)
    {
      GskGLCommandBatch *predecessor = iter->data;

      if (gsk_gl_command_batch_mergeable (predecessor, batch))
        {
          /* We need to check all the intermediates for overdrawing. */
        }
    }
}

static inline GskGLCommandBatch *
gsk_gl_command_queue_get_batch (GskGLCommandQueue *self)
{
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  return self->all_batches.tail->data;
}

static GskGLCommandBatch *
gsk_gl_command_queue_advance (GskGLCommandQueue *self,
                              guint              new_program)
{
  GskGLCommandBatch *last_batch = NULL;
  GskGLCommandBatch *batch;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  if (self->all_batches.length > 0)
    {
      last_batch = self->all_batches.tail->data;

      gsk_gl_uniform_state_snapshot (self->uniforms,
                                     last_batch->program,
                                     gsk_gl_command_batch_uniform_cb,
                                     last_batch);
    }

  batch = gsk_gl_command_queue_alloc_batch (self);

  if G_LIKELY (last_batch != NULL)
    {
      batch->program = new_program ? new_program : last_batch->program;
      batch->program_changed = batch->program != last_batch->program;
      batch->framebuffer = last_batch->framebuffer;
      batch->framebuffer.changed = FALSE;
    }
  else
    {
      batch->program = new_program;
      batch->program_changed = TRUE;
      batch->framebuffer.id = 0;
      batch->framebuffer.changed = FALSE;
    }

  g_queue_push_tail_link (&self->all_batches, &batch->all_link);

  if (batch->program)
    {
      GQueue *q;

      if (self->program_batches->len <= batch->program)
        g_array_set_size (self->program_batches, batch->program + 1);

      q = &g_array_index (self->program_batches, GQueue, batch->program);
      g_queue_push_tail_link (q, &batch->program_link);
    }

  if (last_batch != NULL)
    gsk_gl_command_queue_try_merge (self, last_batch);

  return g_steal_pointer (&batch);
}

static inline gboolean
gsk_gl_command_queue_batch_is_complete (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch = gsk_gl_command_queue_get_batch (self);

  return batch->is_clear || (batch->program && batch->n_draws > 0);
}

static void
gsk_gl_command_queue_dispose (GObject *object)
{
  GskGLCommandQueue *self = (GskGLCommandQueue *)object;

  g_assert (GSK_IS_GL_COMMAND_QUEUE (self));

  while (self->all_batches.length > 0)
    {
      GskGLCommandBatch *batch = self->all_batches.head->data;

      gsk_gl_command_queue_release_batch (self, batch);
    }

  while (self->unused_batches.length > 0)
    {
      GskGLCommandBatch *batch = self->unused_batches.head->data;

      g_queue_unlink (&self->unused_batches, self->unused_batches.head->data);
      g_slice_free (GskGLCommandBatch, batch);
    }

#ifndef G_DISABLE_DEBUG
  g_assert (self->unused_batches.length == 0);
  g_assert (self->all_batches.length == 0);

  for (guint i = 0; i < self->program_batches->len; i++)
    {
      GQueue *q = &g_array_index (self->program_batches, GQueue, i);
      g_assert (q->length == 0);
    }
#endif

  g_clear_pointer (&self->saved_state, g_ptr_array_unref);
  g_clear_pointer (&self->attachments, gsk_gl_attachment_state_free);
  g_clear_object (&self->context);
  g_clear_pointer (&self->uniforms, gsk_gl_uniform_state_free);
  g_clear_pointer (&self->vertices, gsk_gl_buffer_free);
  g_clear_pointer (&self->program_batches, g_array_unref);
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

  self->attachments = gsk_gl_attachment_state_new ();
  self->vertices = gsk_gl_buffer_new (GL_ARRAY_BUFFER, sizeof (GskGLDrawVertex));
  self->uniforms = gsk_gl_uniform_state_new ();
  self->program_batches = g_array_new (FALSE, TRUE, sizeof (GQueue));
  self->saved_state = g_ptr_array_new_with_free_func ((GDestroyNotify)gsk_gl_attachment_state_free);
  self->autorelease_textures = g_array_new (FALSE, FALSE, sizeof (GLuint));
  self->autorelease_framebuffers = g_array_new (FALSE, FALSE, sizeof (GLuint));

  gsk_gl_command_queue_advance (self, 0);
}

GskGLCommandQueue *
gsk_gl_command_queue_new (GdkGLContext *context)
{
  GskGLCommandQueue *self;

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  self = g_object_new (GSK_TYPE_GL_COMMAND_QUEUE, NULL);
  self->context = g_object_ref (context);

  if (self->max_texture_size < 0)
    {
      gdk_gl_context_make_current (context);
      glGetIntegerv (GL_MAX_TEXTURE_SIZE, (GLint *)&self->max_texture_size);
      GSK_NOTE (OPENGL, g_message ("GL max texture size: %d", self->max_texture_size));
    }

  return g_steal_pointer (&self);
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
gsk_gl_command_queue_use_program (GskGLCommandQueue *self,
                                  guint              program)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  batch = gsk_gl_command_queue_get_batch (self);

  if (batch->program == program || program == 0)
    return;

  if (batch->n_draws == 0)
    {
      batch->program = program;
      return;
    }

  batch = gsk_gl_command_queue_advance (self, program);
  batch->program = program;
  batch->program_changed = TRUE;
}

void
gsk_gl_command_queue_delete_program (GskGLCommandQueue *self,
                                     guint              program)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  glDeleteProgram (program);
  gsk_gl_uniform_state_clear_program (self->uniforms, program);
}

GskGLDrawVertex *
gsk_gl_command_queue_draw (GskGLCommandQueue    *self,
                           const GskGLDrawVertex  vertices[6])
{
  GskGLCommandBatch *batch;
  GskGLDrawVertex *dest;
  guint offset;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self), NULL);

  batch = gsk_gl_command_queue_get_batch (self);
  dest = gsk_gl_buffer_advance (self->vertices, 6, &offset);

  gsk_gl_command_batch_draw (batch, offset, 6);

  if (vertices != NULL)
    {
      memcpy (dest, vertices, sizeof (GskGLDrawVertex) * 6);
      return NULL;
    }

  return dest;
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

/**
 * gsk_gl_command_queue_execute:
 * @self: a #GskGLCommandQueue
 *
 * Executes all of the batches in the command queue.
 */
void
gsk_gl_command_queue_execute (GskGLCommandQueue *self)
{
  GskGLCommandBatch *last_batch;
  GLuint vao_id;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  if (self->all_batches.length == 0)
    return;

  /* First advance the queue to ensure that we have stashed all the
   * state we need and possibly merged the final batch.
   */
  last_batch = gsk_gl_command_queue_get_batch (self);
  if (last_batch->program != 0 && last_batch->n_draws > 0)
    gsk_gl_command_queue_advance (self, 0);

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

  for (const GList *iter = self->all_batches.head; iter != NULL; iter = iter->next)
    {
      GskGLCommandBatch *batch = self->all_batches.head->data;

      if (batch->n_draws > 0)
        gsk_gl_command_batch_execute (batch, self->uniforms);
    }

  glDeleteVertexArrays (1, &vao_id);
}

void
gsk_gl_command_queue_begin_frame (GskGLCommandQueue *self)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

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

  while (self->all_batches.length > 0)
    {
      GskGLCommandBatch *batch = self->all_batches.head->data;
      gsk_gl_command_queue_release_batch (self, batch);
    }

  gsk_gl_uniform_state_end_frame (self->uniforms);

  /* Release autoreleased framebuffers */
  if (self->autorelease_framebuffers->len > 0)
    glDeleteFramebuffers (self->autorelease_framebuffers->len,
                          (GLuint *)(gpointer)self->autorelease_framebuffers->data);

  /* Release autoreleased textures */
  if (self->autorelease_textures->len > 0)
    glDeleteTextures (self->autorelease_textures->len,
                      (GLuint *)(gpointer)self->autorelease_textures->data);
}

void
gsk_gl_command_queue_bind_framebuffer (GskGLCommandQueue *self,
                                       guint              framebuffer)
{
  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  gsk_gl_attachment_state_bind_framebuffer (self->attachments, framebuffer);
}

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

void
gsk_gl_command_queue_clear (GskGLCommandQueue *self)
{
  GskGLCommandBatch *batch;

  g_return_if_fail (GSK_IS_GL_COMMAND_QUEUE (self));

  batch = gsk_gl_command_queue_get_batch (self);

  if (batch->is_clear)
    return;

  if (gsk_gl_command_queue_batch_is_complete (self))
    batch = gsk_gl_command_queue_advance (self, 0);

  batch->is_clear = TRUE;
}
