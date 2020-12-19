/* gskglcommandqueueprivate.h
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

#ifndef __GSK_GL_COMMAND_QUEUE_PRIVATE_H__
#define __GSK_GL_COMMAND_QUEUE_PRIVATE_H__

#include "gskgltypes.h"

G_BEGIN_DECLS

#define GSK_TYPE_GL_COMMAND_QUEUE (gsk_gl_command_queue_get_type())

G_DECLARE_FINAL_TYPE (GskGLCommandQueue, gsk_gl_command_queue, GSK, GL_COMMAND_QUEUE, GObject)

GskGLCommandQueue *gsk_gl_command_queue_new                      (GdkGLContext             *context);
GdkGLContext      *gsk_gl_command_queue_get_context              (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_make_current             (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_begin_frame              (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_end_frame                (GskGLCommandQueue        *self);
GdkTexture        *gsk_gl_command_queue_download                 (GskGLCommandQueue        *self,
                                                                  GError                  **error);
GdkMemoryTexture  *gsk_gl_command_queue_download_texture         (GskGLCommandQueue        *self,
                                                                  guint                     texture_id,
                                                                  GError                  **error);
guint              gsk_gl_command_queue_upload_texture           (GskGLCommandQueue        *self,
                                                                  GdkTexture               *texture,
                                                                  GError                  **error);
int                gsk_gl_command_queue_create_texture           (GskGLCommandQueue        *self,
                                                                  int                       width,
                                                                  int                       height,
                                                                  int                       min_filter,
                                                                  int                       mag_filter);
guint              gsk_gl_command_queue_create_framebuffer       (GskGLCommandQueue        *self);
gboolean           gsk_gl_command_queue_create_render_target     (GskGLCommandQueue        *self,
                                                                  int                       width,
                                                                  int                       height,
                                                                  guint                    *out_fbo_id,
                                                                  guint                    *out_texture_id);
void               gsk_gl_command_queue_delete_program           (GskGLCommandQueue        *self,
                                                                  guint                     program_id);
void               gsk_gl_command_queue_use_program              (GskGLCommandQueue        *self,
                                                                  guint                     program_id);
void               gsk_gl_command_queue_bind_framebuffer         (GskGLCommandQueue        *self,
                                                                  guint                     framebuffer);
void               gsk_gl_command_queue_set_uniform1i            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  int                       value0);
void               gsk_gl_command_queue_set_uniform2i            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  int                       value0,
                                                                  int                       value1);
void               gsk_gl_command_queue_set_uniform3i            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  int                       value0,
                                                                  int                       value1,
                                                                  int                       value2);
void               gsk_gl_command_queue_set_uniform4i            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  int                       value0,
                                                                  int                       value1,
                                                                  int                       value2,
                                                                  int                       value3);
void               gsk_gl_command_queue_set_uniform1f            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  float                     value0);
void               gsk_gl_command_queue_set_uniform2f            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  float                     value0,
                                                                  float                     value1);
void               gsk_gl_command_queue_set_uniform3f            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  float                     value0,
                                                                  float                     value1,
                                                                  float                     value2);
void               gsk_gl_command_queue_set_uniform4f            (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  float                     value0,
                                                                  float                     value1,
                                                                  float                     value2,
                                                                  float                     value3);
void               gsk_gl_command_queue_set_uniform2fv           (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  gsize                     count,
                                                                  const float              *value);
void               gsk_gl_command_queue_set_uniform1fv           (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  gsize                     count,
                                                                  const float              *value);
void               gsk_gl_command_queue_set_uniform4fv           (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  gsize                     count,
                                                                  const float              *value);
void               gsk_gl_command_queue_set_uniform_matrix       (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  const graphene_matrix_t  *matrix);
void               gsk_gl_command_queue_set_uniform_color        (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  const GdkRGBA            *color);
void               gsk_gl_command_queue_set_uniform_texture      (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  GLenum                    texture_target,
                                                                  GLenum                    texture_slot,
                                                                  guint                     texture_id);
void               gsk_gl_command_queue_set_uniform_rounded_rect (GskGLCommandQueue        *self,
                                                                  guint                     program,
                                                                  guint                     location,
                                                                  const GskRoundedRect     *rounded_rect);
void               gsk_gl_command_queue_autorelease_framebuffer  (GskGLCommandQueue        *self,
                                                                  guint                     framebuffer_id);
void               gsk_gl_command_queue_autorelease_texture      (GskGLCommandQueue        *self,
                                                                  guint                     texture_id);
GskGLDrawVertex   *gsk_gl_command_queue_draw                     (GskGLCommandQueue        *self,
                                                                  const GskGLDrawVertex     vertices[6]);

G_END_DECLS

#endif /* __GSK_GL_COMMAND_QUEUE_PRIVATE_H__ */
