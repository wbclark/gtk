/* gskgldriver.c
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
#include <gsk/gskrendererprivate.h>

#include "gskglcommandqueueprivate.h"
#include "gskglcompilerprivate.h"
#include "gskgldriverprivate.h"
#include "gskglglyphlibraryprivate.h"
#include "gskgliconlibraryprivate.h"
#include "gskglprogramprivate.h"
#include "gskglshadowlibraryprivate.h"

G_DEFINE_TYPE (GskNextDriver, gsk_next_driver, G_TYPE_OBJECT)

static void
gsk_next_driver_dispose (GObject *object)
{
  GskNextDriver *self = (GskNextDriver *)object;

#define GSK_GL_NO_UNIFORMS
#define GSK_GL_ADD_UNIFORM(pos, KEY, name)
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) \
  G_STMT_START {                                        \
    if (self->name)                                     \
      gsk_gl_program_delete (self->name);               \
    g_clear_object (&self->name);                       \
  } G_STMT_END;
# include "gskglprograms.defs"
#undef GSK_GL_NO_UNIFORMS
#undef GSK_GL_ADD_UNIFORM
#undef GSK_GL_DEFINE_PROGRAM

  g_clear_object (&self->command_queue);

  G_OBJECT_CLASS (gsk_next_driver_parent_class)->dispose (object);
}

static void
gsk_next_driver_class_init (GskNextDriverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsk_next_driver_dispose;
}

static void
gsk_next_driver_init (GskNextDriver *self)
{
}

static gboolean
gsk_next_driver_load_programs (GskNextDriver  *self,
                               GError        **error)
{
  GskGLCompiler *compiler;
  gboolean ret = FALSE;

  g_assert (GSK_IS_NEXT_DRIVER (self));
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self->command_queue));

  compiler = gsk_gl_compiler_new (self->command_queue, self->debug);

  /* Setup preambles that are shared by all shaders */
  gsk_gl_compiler_set_preamble_from_resource (compiler,
                                              GSK_GL_COMPILER_ALL,
                                              "/org/gtk/libgsk/glsl/preamble.glsl");
  gsk_gl_compiler_set_preamble_from_resource (compiler,
                                              GSK_GL_COMPILER_VERTEX,
                                              "/org/gtk/libgsk/glsl/preamble.vs.glsl");
  gsk_gl_compiler_set_preamble_from_resource (compiler,
                                              GSK_GL_COMPILER_FRAGMENT,
                                              "/org/gtk/libgsk/glsl/preamble.fs.glsl");

  /* Setup attributes that are provided via VBO */
  gsk_gl_compiler_bind_attribute (compiler, "aPosition", 0);
  gsk_gl_compiler_bind_attribute (compiler, "vUv", 1);

  /* Use XMacros to register all of our programs and their uniforms */
#define GSK_GL_NO_UNIFORMS
#define GSK_GL_ADD_UNIFORM(pos, KEY, name)                                                      \
  gsk_gl_program_add_uniform (program, #name, UNIFORM_##KEY);
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms)                                         \
  G_STMT_START {                                                                                \
    GskGLProgram *program;                                                                      \
    gboolean have_alpha;                                                                        \
                                                                                                \
    gsk_gl_compiler_set_source_from_resource (compiler, GSK_GL_COMPILER_ALL, resource);         \
                                                                                                \
    if (!(program = gsk_gl_compiler_compile (compiler, #name, error)))                          \
      goto failure;                                                                             \
                                                                                                \
    have_alpha = gsk_gl_program_add_uniform (program, "u_alpha", UNIFORM_SHARED_ALPHA);         \
    gsk_gl_program_add_uniform (program, "u_source", UNIFORM_SHARED_SOURCE);                    \
    gsk_gl_program_add_uniform (program, "u_clip_rect", UNIFORM_SHARED_CLIP_RECT);              \
    gsk_gl_program_add_uniform (program, "u_viewport", UNIFORM_SHARED_VIEWPORT);                \
    gsk_gl_program_add_uniform (program, "u_projection", UNIFORM_SHARED_PROJECTION);            \
    gsk_gl_program_add_uniform (program, "u_modelview", UNIFORM_SHARED_MODELVIEW);              \
                                                                                                \
    uniforms                                                                                    \
                                                                                                \
    if (have_alpha)                                                                             \
      gsk_gl_program_set_uniform1f (program, UNIFORM_SHARED_ALPHA, 1.0f);                       \
                                                                                                \
    *(GskGLProgram **)(((guint8 *)self) + G_STRUCT_OFFSET (GskNextDriver, name)) =              \
        g_steal_pointer (&program);                                                             \
  } G_STMT_END;
# include "gskglprograms.defs"
#undef GSK_GL_DEFINE_PROGRAM
#undef GSK_GL_ADD_UNIFORM

  ret = TRUE;

failure:
  g_clear_object (&compiler);

  return ret;
}

GskNextDriver *
gsk_next_driver_new (GskGLCommandQueue  *command_queue,
                     gboolean            debug,
                     GError            **error)
{
  GskNextDriver *self;
  GdkGLContext *context;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (command_queue), NULL);

  context = gsk_gl_command_queue_get_context (command_queue);

  gdk_gl_context_make_current (context);

  self = g_object_new (GSK_TYPE_NEXT_DRIVER, NULL);
  self->command_queue = g_object_ref (command_queue);
  self->debug = !!debug;

  if (!gsk_next_driver_load_programs (self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  self->glyphs = gsk_gl_glyph_library_new (context);
  self->icons = gsk_gl_icon_library_new (context);
  self->shadows = gsk_gl_shadow_library_new (context);

  return g_steal_pointer (&self);
}

void
gsk_next_driver_begin_frame (GskNextDriver *self)
{
  g_return_if_fail (GSK_IS_NEXT_DRIVER (self));
  g_return_if_fail (self->in_frame == FALSE);

  self->in_frame = TRUE;

  gsk_gl_command_queue_begin_frame (self->command_queue);
}

void
gsk_next_driver_end_frame (GskNextDriver *self)
{
  g_return_if_fail (GSK_IS_NEXT_DRIVER (self));
  g_return_if_fail (self->in_frame == TRUE);

  gsk_gl_command_queue_end_frame (self->command_queue);

  self->in_frame = FALSE;
}

GdkGLContext *
gsk_next_driver_get_context (GskNextDriver *self)
{
  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), NULL);
  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self->command_queue), NULL);

  return gsk_gl_command_queue_get_context (self->command_queue);
}

gboolean
gsk_next_driver_create_render_target (GskNextDriver *self,
                                      int            width,
                                      int            height,
                                      guint         *out_fbo_id,
                                      guint         *out_texture_id)
{
  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), FALSE);

  if (self->command_queue == NULL)
    return FALSE;

  return gsk_gl_command_queue_create_render_target (self->command_queue,
                                                    width,
                                                    height,
                                                    out_fbo_id,
                                                    out_texture_id);
}
