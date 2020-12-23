/* gskglprogramprivate.h
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

#ifndef __GSK_GL_PROGRAM_H__
#define __GSK_GL_PROGRAM_H__

#include "gskgltypes.h"

G_BEGIN_DECLS

#define GSK_TYPE_GL_PROGRAM (gsk_gl_program_get_type())

G_DECLARE_FINAL_TYPE (GskGLProgram, gsk_gl_program, GSK, GL_PROGRAM, GObject)

GskGLProgram    *gsk_gl_program_new                      (GskGLCommandQueue     *command_queue,
                                                          const char            *name,
                                                          int                    program_id);
gboolean         gsk_gl_program_add_uniform              (GskGLProgram          *self,
                                                          const char            *name,
                                                          guint                  key);
void             gsk_gl_program_delete                   (GskGLProgram          *self);
void             gsk_gl_program_set_uniform1i            (GskGLProgram          *self,
                                                          guint                  key,
                                                          int                    value0);
void             gsk_gl_program_set_uniform2i            (GskGLProgram          *self,
                                                          guint                  key,
                                                          int                    value0,
                                                          int                    value1);
void             gsk_gl_program_set_uniform3i            (GskGLProgram          *self,
                                                          guint                  key,
                                                          int                    value0,
                                                          int                    value1,
                                                          int                    value2);
void             gsk_gl_program_set_uniform4i            (GskGLProgram          *self,
                                                          guint                  key,
                                                          int                    value0,
                                                          int                    value1,
                                                          int                    value2,
                                                          int                    value3);
void             gsk_gl_program_set_uniform1f            (GskGLProgram          *self,
                                                          guint                  key,
                                                          float                  value0);
void             gsk_gl_program_set_uniform2f            (GskGLProgram          *self,
                                                          guint                  key,
                                                          float                  value0,
                                                          float                  value1);
void             gsk_gl_program_set_uniform3f            (GskGLProgram          *self,
                                                          guint                  key,
                                                          float                  value0,
                                                          float                  value1,
                                                          float                  value2);
void             gsk_gl_program_set_uniform4f            (GskGLProgram          *self,
                                                          guint                  key,
                                                          float                  value0,
                                                          float                  value1,
                                                          float                  value2,
                                                          float                  value3);
void             gsk_gl_program_set_uniform_color        (GskGLProgram          *self,
                                                          guint                  key,
                                                          const GdkRGBA         *color);
void             gsk_gl_program_set_uniform_texture      (GskGLProgram          *self,
                                                          guint                  key,
                                                          GLenum                 texture_target,
                                                          GLenum                 texture_slot,
                                                          guint                  texture_id);
void             gsk_gl_program_set_uniform_rounded_rect (GskGLProgram          *self,
                                                          guint                  key,
                                                          const GskRoundedRect  *rounded_rect);
void             gsk_gl_program_begin_draw               (GskGLProgram          *self);
void             gsk_gl_program_end_draw                 (GskGLProgram          *self);
GskGLDrawVertex *gsk_gl_program_add_vertices             (GskGLProgram          *self,
                                                          const GskGLDrawVertex  vertices[GSK_GL_N_VERTICES]);

G_END_DECLS

#endif /* __GSK_GL_PROGRAM_H__ */
