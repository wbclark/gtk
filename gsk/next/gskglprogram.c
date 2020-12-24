/* gskglprogram.c
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

#include "gskglcommandqueueprivate.h"
#include "gskglprogramprivate.h"
#include "gskgluniformstateprivate.h"

struct _GskGLProgram
{
  GObject               parent_instance;
  int                   id;
  char                 *name;
  GArray               *uniform_locations;
  GskGLCommandQueue   *command_queue;
};

G_DEFINE_TYPE (GskGLProgram, gsk_gl_program, G_TYPE_OBJECT)

GskGLProgram *
gsk_gl_program_new (GskGLCommandQueue *command_queue,
                    const char        *name,
                    int                program_id)
{
  GskGLProgram *self;

  g_return_val_if_fail (program_id >= 0, NULL);

  self = g_object_new (GSK_TYPE_GL_PROGRAM, NULL);
  self->id = program_id;
  self->name = g_strdup (name);
  self->command_queue = g_object_ref (command_queue);

  return self;
}

static void
gsk_gl_program_finalize (GObject *object)
{
  GskGLProgram *self = (GskGLProgram *)object;

  if (self->id >= 0)
    g_warning ("Leaking GLSL program %d (%s)",
               self->id,
               self->name ? self->name : "");

  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->uniform_locations, g_array_unref);
  g_clear_object (&self->command_queue);

  G_OBJECT_CLASS (gsk_gl_program_parent_class)->finalize (object);
}

static void
gsk_gl_program_class_init (GskGLProgramClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gsk_gl_program_finalize;
}

static void
gsk_gl_program_init (GskGLProgram *self)
{
  self->id = -1;
  self->uniform_locations = g_array_new (FALSE, TRUE, sizeof (GLint));
}

/**
 * gsk_gl_program_add_uniform:
 * @self: a #GskGLProgram
 * @name: the name of the uniform such as "u_source"
 * @key: the identifier to use for the uniform
 *
 * This method will create a mapping between @key and the location
 * of the uniform on the GPU. This simplifies calling code to not
 * need to know where the uniform location is and only register it
 * when creating the program.
 *
 * You might use this with an enum of all your uniforms for the
 * program and then register each of them like:
 *
 * ```
 * gsk_gl_program_add_uniform (program, "u_source", UNIFORM_SOURCE);
 * ```
 *
 * That allows you to set values for the program with something
 * like the following:
 *
 * ```
 * gsk_gl_program_set_uniform1i (program, UNIFORM_SOURCE, 1);
 * ```
 *
 * Returns: %TRUE if the uniform was found; otherwise %FALSE
 */
gboolean
gsk_gl_program_add_uniform (GskGLProgram *self,
                            const char   *name,
                            guint         key)
{
  const GLint invalid = -1;
  GLint location;

  g_return_val_if_fail (GSK_IS_GL_PROGRAM (self), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (key < 1024, FALSE);

  if (-1 == (location = glGetUniformLocation (self->id, name)))
    return FALSE;

  while (key >= self->uniform_locations->len)
    g_array_append_val (self->uniform_locations, invalid);
  g_array_index (self->uniform_locations, GLint, key) = location;

  return TRUE;
}

static inline GLint
get_uniform_location (GskGLProgram *self,
                      guint         key)
{
  if G_LIKELY (key < self->uniform_locations->len)
    return g_array_index (self->uniform_locations, GLint, key);
  else
    return -1;
}

/**
 * gsk_gl_program_delete:
 * @self: a #GskGLProgram
 *
 * Deletes the GLSL program.
 *
 * You must call gsk_gl_program_use() before and
 * gsk_gl_program_unuse() after this function.
 */
void
gsk_gl_program_delete (GskGLProgram *self)
{
  g_return_if_fail (GSK_IS_GL_PROGRAM (self));
  g_return_if_fail (self->command_queue != NULL);

  gsk_gl_command_queue_delete_program (self->command_queue, self->id);
  self->id = -1;
}

void
gsk_gl_program_set_uniform1i (GskGLProgram *self,
                              guint         key,
                              int           value0)
{
  gsk_gl_command_queue_set_uniform1i (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0);
}

void
gsk_gl_program_set_uniform2i (GskGLProgram *self,
                              guint         key,
                              int           value0,
                              int           value1)
{
  gsk_gl_command_queue_set_uniform2i (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0,
                                      value1);
}

void
gsk_gl_program_set_uniform3i (GskGLProgram *self,
                              guint         key,
                              int           value0,
                              int           value1,
                              int           value2)
{
  gsk_gl_command_queue_set_uniform3i (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2);
}

void
gsk_gl_program_set_uniform4i (GskGLProgram *self,
                              guint         key,
                              int           value0,
                              int           value1,
                              int           value2,
                              int           value3)
{
  gsk_gl_command_queue_set_uniform4i (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2,
                                      value3);
}

void
gsk_gl_program_set_uniform1f (GskGLProgram *self,
                              guint         key,
                              float         value0)
{
  gsk_gl_command_queue_set_uniform1f (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0);
}

void
gsk_gl_program_set_uniform2f (GskGLProgram *self,
                              guint         key,
                              float         value0,
                              float         value1)
{
  gsk_gl_command_queue_set_uniform2f (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0,
                                      value1);
}

void
gsk_gl_program_set_uniform3f (GskGLProgram *self,
                              guint         key,
                              float         value0,
                              float         value1,
                              float         value2)
{
  gsk_gl_command_queue_set_uniform3f (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2);
}

void
gsk_gl_program_set_uniform4f (GskGLProgram *self,
                              guint         key,
                              float         value0,
                              float         value1,
                              float         value2,
                              float         value3)
{
  gsk_gl_command_queue_set_uniform4f (self->command_queue,
                                      self->id,
                                      get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2,
                                      value3);
}

void
gsk_gl_program_set_uniform_color (GskGLProgram  *self,
                                  guint          key,
                                  const GdkRGBA *color)
{
  g_assert (GSK_IS_GL_PROGRAM (self));

  gsk_gl_command_queue_set_uniform_color (self->command_queue,
                                          self->id,
                                          get_uniform_location (self, key),
                                          color);
}

void
gsk_gl_program_set_uniform_texture (GskGLProgram *self,
                                    guint         key,
                                    GLenum        texture_target,
                                    GLenum        texture_slot,
                                    guint         texture_id)
{
  g_assert (GSK_IS_GL_PROGRAM (self));
  g_assert (texture_target == GL_TEXTURE_1D ||
            texture_target == GL_TEXTURE_2D ||
            texture_target == GL_TEXTURE_3D);
  g_assert (texture_slot >= GL_TEXTURE0);
  g_assert (texture_slot <= GL_TEXTURE16);

  gsk_gl_command_queue_set_uniform_texture (self->command_queue,
                                            self->id,
                                            get_uniform_location (self, key),
                                            texture_target,
                                            texture_slot,
                                            texture_id);
}

void
gsk_gl_program_set_uniform_rounded_rect (GskGLProgram         *self,
                                         guint                 key,
                                         const GskRoundedRect *rounded_rect)
{
  g_assert (GSK_IS_GL_PROGRAM (self));

  gsk_gl_command_queue_set_uniform_rounded_rect (self->command_queue,
                                                 self->id,
                                                 get_uniform_location (self, key),
                                                 rounded_rect);
}

void
gsk_gl_program_begin_draw (GskGLProgram *self)
{
  g_assert (GSK_IS_GL_PROGRAM (self));

  return gsk_gl_command_queue_begin_draw (self->command_queue, self->id);
}

void
gsk_gl_program_end_draw (GskGLProgram *self)
{
  g_assert (GSK_IS_GL_PROGRAM (self));

  return gsk_gl_command_queue_end_draw (self->command_queue);
}
