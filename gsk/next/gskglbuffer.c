/* gskglbufferprivate.h
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <epoxy/gl.h>

#include "gskglbufferprivate.h"

#define N_BUFFERS 2
#define RESERVED_SIZE 1024

typedef struct
{
  GLuint   id;
  guint    size_on_gpu;
} GskGLBufferShadow;

struct _GskGLBuffer
{
  GArray            *buffer;
  GskGLBufferShadow  shadows[N_BUFFERS];
  GLenum             target;
  guint              current;
};

static void
gsk_gl_buffer_shadow_init (GskGLBufferShadow *shadow,
                           GLenum             target,
                           guint              element_size,
                           guint              reserved_size)
{
  GLuint id;

  glGenBuffers (1, &id);
  glBindBuffer (target, id);
  glBufferData (target, element_size * reserved_size, NULL, GL_STATIC_DRAW);
  glBindBuffer (target, 0);

  shadow->id = id;
  shadow->size_on_gpu = element_size * reserved_size;
}

static void
gsk_gl_buffer_shadow_destroy (GskGLBufferShadow *shadow)
{
  shadow->size_on_gpu = 0;

  if (shadow->id > 0)
    {
      glDeleteBuffers (1, &shadow->id);
      shadow->id = 0;
    }
}

static void
gsk_gl_buffer_shadow_submit (GskGLBufferShadow *shadow,
                             GLenum             target,
                             GArray            *buffer)
{
  guint to_upload = buffer->len * g_array_get_element_size (buffer);

  /* If what we generated is larger than our size on the GPU, then we need
   * to release our previous buffer and create a new one of the appropriate
   * size. We add some padding to make it more likely the next frame with this
   * buffer does not need to do the same thing again. We also try to keep
   * things aligned to the size of a whole (4096 byte) page.
   */
  if G_UNLIKELY (to_upload > shadow->size_on_gpu)
    {
      guint size_on_gpu = (to_upload & ~0xFFF) + (4 * 4096L);

      glBindBuffer (target, 0);
      glDeleteBuffers (1, &shadow->id);
      glGenBuffers (1, &shadow->id);
      glBindBuffer (target, shadow->id);
      glBufferData (target, size_on_gpu, NULL, GL_STATIC_DRAW);
      glBufferSubData (target, 0, to_upload, buffer->data);
      shadow->size_on_gpu = size_on_gpu;
    }
  else
    {
      glBindBuffer (target, shadow->id);
      glBufferSubData (target, 0, to_upload, buffer->data);
    }
}

/**
 * gsk_gl_buffer_new:
 * @target: the target buffer such as %GL_ARRAY_BUFFER or %GL_UNIFORM_BUFFER
 * @element_size: the size of elements within the buffer
 *
 * Creates a new #GskGLBuffer which can be used to deliver data to shaders
 * within a GLSL program. You can use this to store vertices such as with
 * %GL_ARRAY_BUFFER or uniform data with %GL_UNIFORM_BUFFER.
 *
 * Note that only writing to this buffer is allowed (see %GL_WRITE_ONLY for
 * more details).
 *
 * The buffer will be bound to target upon returning from this function.
 */
GskGLBuffer *
gsk_gl_buffer_new (GLenum target,
                   guint  element_size)
{
  GskGLBuffer *buffer;
  GLuint id = 0;

  glGenBuffers (1, &id);

  buffer = g_new0 (GskGLBuffer, 1);
  buffer->buffer = g_array_sized_new (FALSE, FALSE, element_size, RESERVED_SIZE);
  buffer->target = target;
  buffer->current = 0;

  for (guint i = 0; i < N_BUFFERS; i++)
    gsk_gl_buffer_shadow_init (&buffer->shadows[i],
                               target,
                               element_size,
                               RESERVED_SIZE);

  return g_steal_pointer (&buffer);
}

void
gsk_gl_buffer_submit (GskGLBuffer *buffer)
{
  gsk_gl_buffer_shadow_submit (&buffer->shadows[buffer->current],
                               buffer->target,
                               buffer->buffer);
  buffer->current = (buffer->current + 1) % N_BUFFERS;
  buffer->buffer->len = 0;
}

void
gsk_gl_buffer_free (GskGLBuffer *buffer)
{
  buffer->target = 0;
  buffer->current = 0;
  for (guint i = 0; i < N_BUFFERS; i++)
    gsk_gl_buffer_shadow_destroy (&buffer->shadows[i]);
  g_free (buffer);
}

gpointer
gsk_gl_buffer_advance (GskGLBuffer *buffer,
                       guint        count,
                       guint       *offset)
{

  *offset = buffer->buffer->len;
  g_array_set_size (buffer->buffer, buffer->buffer->len + count);
  return (guint8 *)buffer->buffer->data + (*offset * g_array_get_element_size (buffer->buffer));
}
