/* gskgluniformstate.c
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

#include <gsk/gskroundedrectprivate.h>
#include <epoxy/gl.h>
#include <string.h>

#include "gskgluniformstateprivate.h"

typedef struct { float v0; } Uniform1f;
typedef struct { float v0; float v1; } Uniform2f;
typedef struct { float v0; float v1; float v2; } Uniform3f;
typedef struct { float v0; float v1; float v2; float v3; } Uniform4f;

typedef struct { int v0; } Uniform1i;
typedef struct { int v0; int v1; } Uniform2i;
typedef struct { int v0; int v1; int v2; } Uniform3i;
typedef struct { int v0; int v1; int v2; int v3; } Uniform4i;

static guint8 uniform_sizes[] = {
  sizeof (Uniform1f),
  sizeof (Uniform2f),
  sizeof (Uniform3f),
  sizeof (Uniform4f),

  sizeof (Uniform1f),
  sizeof (Uniform2f),
  sizeof (Uniform3f),
  sizeof (Uniform4f),

  sizeof (Uniform1i),
  sizeof (Uniform2i),
  sizeof (Uniform3i),
  sizeof (Uniform4i),

  sizeof (guint),

  sizeof (graphene_matrix_t),
  sizeof (GskRoundedRect),
  sizeof (GdkRGBA),

  0,
};

#define REPLACE_UNIFORM(info, u, format, count)                                          \
  G_STMT_START {                                                                         \
    guint offset;                                                                        \
    u = alloc_uniform_data(state->uniform_data, uniform_sizes[format] * count, &offset); \
    (info)->offset = offset;                                                             \
  } G_STMT_END

typedef struct
{
  GArray *uniform_info;
  guint   n_changed;
} ProgramInfo;

GskGLUniformState *
gsk_gl_uniform_state_new (void)
{
  GskGLUniformState *state;

  state = g_new0 (GskGLUniformState, 1);
  state->program_info = g_array_new (FALSE, TRUE, sizeof (ProgramInfo));
  state->uniform_data = g_byte_array_new ();

  return g_steal_pointer (&state);
}

void
gsk_gl_uniform_state_free (GskGLUniformState *state)
{
  g_clear_pointer (&state->program_info, g_array_unref);
  g_clear_pointer (&state->uniform_data, g_byte_array_unref);
  g_free (state);
}

static inline void
program_changed (GskGLUniformState *state,
                 GskGLUniformInfo  *info,
                 guint              program)
{
  if (!info->changed)
    {
      info->changed = TRUE;
      g_array_index (state->program_info, ProgramInfo, program).n_changed++;
    }
}

void
gsk_gl_uniform_state_clear_program (GskGLUniformState *state,
                                    guint              program)
{
  ProgramInfo *program_info;

  g_assert (state != NULL);

  if (program == 0 || program >= state->program_info->len)
    return;

  program_info = &g_array_index (state->program_info, ProgramInfo, program);
  program_info->n_changed = 0;
  g_clear_pointer (&program_info->uniform_info, g_array_unref);
}

static gpointer
alloc_uniform_data (GByteArray *buffer,
                    guint       size,
                    guint      *offset)
{
  guint align = size > 4 ? GLIB_SIZEOF_VOID_P : 4;
  guint masked = buffer->len & (align - 1);

  /* Try to give a more natural alignment based on the size
   * of the uniform. In case it's greater than 4 try to at least
   * give us an 8-byte alignment to be sure we can dereference
   * without a memcpy().
   */
  if (masked != 0)
    {
      guint prefix_align = align - masked;
      g_byte_array_set_size (buffer, buffer->len + prefix_align);
    }

  *offset = buffer->len;
  g_byte_array_set_size (buffer, buffer->len + size);

  g_assert ((*offset & (align - 1)) == 0);

  return (gpointer)&buffer->data[*offset];
}

static gpointer
get_uniform (GskGLUniformState  *state,
             guint               program,
             GskGLUniformFormat  format,
             guint               array_count,
             guint               location,
             GskGLUniformInfo  **infoptr)
{
  ProgramInfo *program_info;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (array_count < 256);
  g_assert ((int)format >= 0 && format < GSK_GL_UNIFORM_FORMAT_LAST);
  g_assert (location < GL_MAX_UNIFORM_LOCATIONS);

  /* Fast path for common case (state already initialized) */
  if G_LIKELY (program < state->program_info->len &&
               (program_info = &g_array_index (state->program_info, ProgramInfo, program)) &&
               program_info->uniform_info != NULL &&
               location < program_info->uniform_info->len)
    {
      info = &g_array_index (program_info->uniform_info, GskGLUniformInfo, location);

      if G_LIKELY (format == info->format && array_count <= info->array_count)
        {
          *infoptr = info;
          return state->uniform_data->data + info->offset;
        }
      else
        {
          g_critical ("Attempt to access uniform with different type of value "
                      "than it was initialized with. Program %u Location %u.",
                      program, location);
          *infoptr = NULL;
          return NULL;
        }
    }
  else
    {
      guint offset;

      if (program >= state->program_info->len ||
          g_array_index (state->program_info, ProgramInfo, program).uniform_info == NULL)
        {
          if (program >= state->program_info->len)
            g_array_set_size (state->program_info, program + 1);

          program_info = &g_array_index (state->program_info, ProgramInfo, program);
          program_info->uniform_info = g_array_new (FALSE, TRUE, sizeof (GskGLUniformInfo));
          program_info->n_changed = 0;
        }

      g_assert (program_info != NULL);
      g_assert (program_info->uniform_info != NULL);

      if (location >= program_info->uniform_info->len)
        g_array_set_size (program_info->uniform_info, location + 1);

      alloc_uniform_data (state->uniform_data, uniform_sizes[format] * array_count, &offset);

      info = &g_array_index (program_info->uniform_info, GskGLUniformInfo, location);
      info->changed = TRUE;
      info->format = format;
      info->offset = offset;
      info->array_count = 0;

      *infoptr = info;

      return state->uniform_data->data + offset;
    }
}

void
gsk_gl_uniform_state_set1f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0)
{
  Uniform1f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_1F, 1, location, &info)))
    {
      if (u->v0 != value0)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_1F, 1);
          u->v0 = value0;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set2f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0,
                            float              value1)
{
  Uniform2f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_2F, 1, location, &info)))
    {
      if (u->v0 != value0 || u->v1 != value1)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_2F, 1);
          u->v0 = value0;
          u->v1 = value1;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set3f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0,
                            float              value1,
                            float              value2)
{
  Uniform3f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_3F, 1, location, &info)))
    {
      if (u->v0 != value0 || u->v1 != value1 || u->v2 != value2)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_3F, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set4f (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            float              value0,
                            float              value1,
                            float              value2,
                            float              value3)
{
  Uniform4f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_4F, 1, location, &info)))
    {
      if (u->v0 != value0 || u->v1 != value1 || u->v2 != value2 || u->v3 != value3)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_4F, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          u->v3 = value3;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set1i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0)
{
  Uniform1i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_1I, 1, location, &info)))
    {
      if (u->v0 != value0)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_1I, 1);
          u->v0 = value0;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set2i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0,
                            int                value1)
{
  Uniform2i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_2I, 1, location, &info)))
    {
      if (u->v0 != value0 || u->v1 != value1)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_2I, 1);
          u->v0 = value0;
          u->v1 = value1;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set3i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0,
                            int                value1,
                            int                value2)
{
  Uniform3i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_3I, 1, location, &info)))
    {
      if (u->v0 != value0 || u->v1 != value1 || u->v2 != value2)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_3I, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set4i (GskGLUniformState *state,
                            guint              program,
                            guint              location,
                            int                value0,
                            int                value1,
                            int                value2,
                            int                value3)
{
  Uniform4i *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_4I, 1, location, &info)))
    {
      if (u->v0 != value0 || u->v1 != value1 || u->v2 != value2 || u->v3 != value3)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_4I, 1);
          u->v0 = value0;
          u->v1 = value1;
          u->v2 = value2;
          u->v3 = value3;
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set_rounded_rect (GskGLUniformState    *state,
                                       guint                 program,
                                       guint                 location,
                                       const GskRoundedRect *rounded_rect)
{
  GskRoundedRect *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (rounded_rect != NULL);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT, 1, location, &info)))
    {
      if (!gsk_rounded_rect_equal (rounded_rect, u))
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT, 1);

          if (!(info->flags & GSK_GL_UNIFORM_FLAGS_SEND_CORNERS))
            {
              if (!graphene_size_equal (&u->corner[0], &rounded_rect->corner[0]) ||
                  !graphene_size_equal (&u->corner[1], &rounded_rect->corner[1]) ||
                  !graphene_size_equal (&u->corner[2], &rounded_rect->corner[2]) ||
                  !graphene_size_equal (&u->corner[3], &rounded_rect->corner[3]))
                info->flags |= GSK_GL_UNIFORM_FLAGS_SEND_CORNERS;
            }

          memcpy (u, rounded_rect, sizeof *rounded_rect);
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set_matrix (GskGLUniformState       *state,
                                 guint                    program,
                                 guint                    location,
                                 const graphene_matrix_t *matrix)
{
  graphene_matrix_t *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);
  g_assert (matrix != NULL);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_MATRIX, 1, location, &info)))
    {
      if (graphene_matrix_equal_fast (u, matrix))
        return;

      if (!graphene_matrix_equal (u, matrix))
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_MATRIX, 1);
          memcpy (u, matrix, sizeof *matrix);
          program_changed (state, info, program);
        }
    }
}

/**
 * gsk_gl_uniform_state_set_texture:
 * @state: a #GskGLUniformState
 * @program: the program id
 * @location: the location of the texture
 * @texture_slot: a texturing slot such as GL_TEXTURE0
 *
 * Sets the uniform expecting a texture to @texture_slot. This API
 * expects a texture slot such as GL_TEXTURE0 to reduce chances of
 * miss-use by the caller.
 *
 * The value stored to the uniform is in the form of 0 for GL_TEXTURE0,
 * 1 for GL_TEXTURE1, and so on.
 */
void
gsk_gl_uniform_state_set_texture (GskGLUniformState *state,
                                  guint              program,
                                  guint              location,
                                  guint              texture_slot)
{
  GskGLUniformInfo *info;
  guint *u;

  g_assert (texture_slot >= GL_TEXTURE0);
  g_assert (texture_slot < GL_TEXTURE16);

  texture_slot -= GL_TEXTURE0;

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_TEXTURE, 1, location, &info)))
    {
      if (*u != texture_slot)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_TEXTURE, 1);
          *u = texture_slot;
          program_changed (state, info, program);
        }
    }
}

/**
 * gsk_gl_uniform_state_set_color:
 * @state: a #GskGLUniformState
 * @program: a program id > 0
 * @location: the uniform location
 * @color: a color to set or %NULL for transparent
 *
 * Sets a uniform to the color described by @color. This is a convenience
 * function to allow callers to avoid having to translate colors to floats
 * in other portions of the renderer.
 */
void
gsk_gl_uniform_state_set_color (GskGLUniformState *state,
                                guint              program,
                                guint              location,
                                const GdkRGBA     *color)
{
  static const GdkRGBA transparent = {0};
  GskGLUniformInfo *info;
  GdkRGBA *u;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_COLOR, 1, location, &info)))
    {
      if (color == NULL)
        color = &transparent;

      if (!gdk_rgba_equal (u, color))
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_COLOR, 1);
          memcpy (u, color, sizeof *color);
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set1fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform1f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_1F, count, location, &info)))
    {
      gboolean changed = memcmp (u, value, sizeof (Uniform1f) * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_1F, count);
          memcpy (u, value, sizeof (Uniform1f) * count);
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set2fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform2f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_2F, count, location, &info)))
    {
      gboolean changed = memcmp (u, value, sizeof (Uniform2f) * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_2F, count);
          memcpy (u, value, sizeof (Uniform2f) * count);
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set3fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform3f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_3F, count, location, &info)))
    {
      gboolean changed = memcmp (u, value, sizeof (Uniform3f) * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_3F, count);
          memcpy (u, value, sizeof (Uniform3f) * count);
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_set4fv (GskGLUniformState *state,
                             guint              program,
                             guint              location,
                             guint              count,
                             const float       *value)
{
  Uniform4f *u;
  GskGLUniformInfo *info;

  g_assert (state != NULL);
  g_assert (program > 0);

  if ((u = get_uniform (state, program, GSK_GL_UNIFORM_FORMAT_4F, count, location, &info)))
    {
      gboolean changed = memcmp (u, value, sizeof (Uniform4f) * count) != 0;

      if (changed)
        {
          REPLACE_UNIFORM (info, u, GSK_GL_UNIFORM_FORMAT_4F, count);
          memcpy (u, value, sizeof (Uniform4f) * count);
          program_changed (state, info, program);
        }
    }
}

void
gsk_gl_uniform_state_snapshot (GskGLUniformState         *state,
                               guint                      program_id,
                               GskGLUniformStateCallback  callback,
                               gpointer                   user_data)
{
  ProgramInfo *program_info;

  g_assert (state != NULL);
  g_assert (program_id > 0);

  if (program_id >= state->program_info->len)
    return;

  program_info = &g_array_index (state->program_info, ProgramInfo, program_id);
  if (program_info->n_changed == 0 || program_info->uniform_info == NULL)
    return;

  for (guint i = 0; i < program_info->uniform_info->len; i++)
    {
      GskGLUniformInfo *info = &g_array_index (program_info->uniform_info, GskGLUniformInfo, i);

      if (!info->changed)
        continue;

      callback (info, i, user_data);

      info->changed = FALSE;
      info->flags = 0;
    }

  program_info->n_changed = 0;
}

void
gsk_gl_uniform_state_end_frame (GskGLUniformState *state)
{
  GByteArray *buffer;

  g_return_if_fail (state != NULL);

  /* After a frame finishes, we want to remove all our copies of uniform
   * data that isn't needed any longer. We just create a new byte array
   * that contains the new data with the gaps removed.
   */

  buffer = g_byte_array_sized_new (4096);

  for (guint i = 0; i < state->program_info->len; i++)
    {
      ProgramInfo *program_info = &g_array_index (state->program_info, ProgramInfo, i);

      if (program_info->uniform_info == NULL)
        continue;

      for (guint j = 0; j < program_info->uniform_info->len; j++)
        {
          GskGLUniformInfo *info = &g_array_index (program_info->uniform_info, GskGLUniformInfo, j);
          guint size = uniform_sizes[info->format] * info->array_count;
          guint offset;

          alloc_uniform_data (buffer, size, &offset);
          memcpy (&buffer->data[offset], &state->uniform_data->data[info->offset], size);
          info->changed = FALSE;
          info->offset = offset;
        }
    }

  g_clear_pointer (&state->uniform_data, g_byte_array_unref);
  state->uniform_data = buffer;
}
