/* gskgluniformstateprivate.h
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

#ifndef GSK_GL_UNIFORM_STATE_PRIVATE_H
#define GSK_GL_UNIFORM_STATE_PRIVATE_H

#include "gskgltypes.h"

G_BEGIN_DECLS

typedef struct _GskGLUniformState
{
  GArray     *program_info;
  GByteArray *uniform_data;
} GskGLUniformState;

typedef struct _GskGLUniformInfo
{
  guint changed : 1;
  guint format : 5;
  guint array_count : 6;
  guint flags : 4;
  guint offset : 16;
} GskGLUniformInfo;

G_STATIC_ASSERT (sizeof (GskGLUniformInfo) == 4);

/**
 * GskGLUniformStateCallback:
 * @info: a pointer to the information about the uniform
 * @location: the location of the uniform within the GPU program.
 * @user_data: closure data for the callback
 *
 * This callback can be used to snapshot state of a program which
 * is useful when batching commands so that the state may be compared
 * with future evocations of the program.
 */
typedef void (*GskGLUniformStateCallback) (const GskGLUniformInfo *info,
                                           guint                   location,
                                           gpointer                user_data);

typedef enum _GskGLUniformFlags
{
  GSK_GL_UNIFORM_FLAGS_SEND_CORNERS = 1 << 0,
} GskGLUniformFlags;

typedef enum _GskGLUniformKind
{
  GSK_GL_UNIFORM_FORMAT_1F = 1,
  GSK_GL_UNIFORM_FORMAT_2F,
  GSK_GL_UNIFORM_FORMAT_3F,
  GSK_GL_UNIFORM_FORMAT_4F,

  GSK_GL_UNIFORM_FORMAT_1FV,
  GSK_GL_UNIFORM_FORMAT_2FV,
  GSK_GL_UNIFORM_FORMAT_3FV,
  GSK_GL_UNIFORM_FORMAT_4FV,

  GSK_GL_UNIFORM_FORMAT_1I,
  GSK_GL_UNIFORM_FORMAT_2I,
  GSK_GL_UNIFORM_FORMAT_3I,
  GSK_GL_UNIFORM_FORMAT_4I,

  GSK_GL_UNIFORM_FORMAT_TEXTURE,

  GSK_GL_UNIFORM_FORMAT_MATRIX,
  GSK_GL_UNIFORM_FORMAT_ROUNDED_RECT,
  GSK_GL_UNIFORM_FORMAT_COLOR,

  GSK_GL_UNIFORM_FORMAT_LAST
} GskGLUniformFormat;

GskGLUniformState *gsk_gl_uniform_state_new              (void);
void               gsk_gl_uniform_state_clear_program    (GskGLUniformState         *state,
                                                          guint                      program);
void               gsk_gl_uniform_state_end_frame        (GskGLUniformState         *state);
void               gsk_gl_uniform_state_set1f            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          float                      value0);
void               gsk_gl_uniform_state_set2f            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          float                      value0,
                                                          float                      value1);
void               gsk_gl_uniform_state_set3f            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          float                      value0,
                                                          float                      value1,
                                                          float                      value2);
void               gsk_gl_uniform_state_set4f            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          float                      value0,
                                                          float                      value1,
                                                          float                      value2,
                                                          float                      value3);
void               gsk_gl_uniform_state_set1fv           (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          guint                      count,
                                                          const float               *value);
void               gsk_gl_uniform_state_set2fv           (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          guint                      count,
                                                          const float               *value);
void               gsk_gl_uniform_state_set3fv           (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          guint                      count,
                                                          const float               *value);
void               gsk_gl_uniform_state_set4fv           (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          guint                      count,
                                                          const float               *value);
void               gsk_gl_uniform_state_set1i            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          int                        value0);
void               gsk_gl_uniform_state_set2i            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          int                        value0,
                                                          int                        value1);
void               gsk_gl_uniform_state_set3i            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          int                        value0,
                                                          int                        value1,
                                                          int                        value2);
void               gsk_gl_uniform_state_set4i            (GskGLUniformState         *state,
                                                          guint                      program,
                                                          guint                      location,
                                                          int                        value0,
                                                          int                        value1,
                                                          int                        value2,
                                                          int                        value3);
void               gsk_gl_uniform_state_set_rounded_rect (GskGLUniformState         *self,
                                                          guint                      program,
                                                          guint                      location,
                                                          const GskRoundedRect      *rect);
void               gsk_gl_uniform_state_set_matrix       (GskGLUniformState         *self,
                                                          guint                      program,
                                                          guint                      location,
                                                          const graphene_matrix_t   *value);
void               gsk_gl_uniform_state_set_texture      (GskGLUniformState         *self,
                                                          guint                      program,
                                                          guint                      location,
                                                          guint                      texture_slot);
void               gsk_gl_uniform_state_set_color        (GskGLUniformState         *self,
                                                          guint                      program,
                                                          guint                      location,
                                                          const GdkRGBA             *color);
void               gsk_gl_uniform_state_snapshot         (GskGLUniformState         *self,
                                                          guint                      program_id,
                                                          GskGLUniformStateCallback  callback,
                                                          gpointer                   user_data);
void               gsk_gl_uniform_state_free             (GskGLUniformState         *state);

static inline gconstpointer
gsk_gl_uniform_state_get_uniform_data (GskGLUniformState *state,
                                       guint              offset)
{
  return (gconstpointer)&state->uniform_data->data[offset];
}

G_END_DECLS

#endif /* GSK_GL_UNIFORM_STATE_PRIVATE_H */
