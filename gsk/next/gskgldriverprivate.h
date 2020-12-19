/* gskgldriverprivate.h
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

#ifndef __GSK_GL_DRIVER_PRIVATE_H__
#define __GSK_GL_DRIVER_PRIVATE_H__

#include "gskgltypes.h"

G_BEGIN_DECLS

enum {
  UNIFORM_SHARED_ALPHA,
  UNIFORM_SHARED_SOURCE,
  UNIFORM_SHARED_CLIP_RECT,
  UNIFORM_SHARED_VIEWPORT,
  UNIFORM_SHARED_PROJECTION,
  UNIFORM_SHARED_MODELVIEW,

  UNIFORM_SHARED_LAST
};

#define GSL_GK_NO_UNIFORMS UNIFORM_INVALID_##__COUNTER__
#define GSK_GL_ADD_UNIFORM(pos, KEY, name) UNIFORM_##KEY = UNIFORM_SHARED_LAST + pos,
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) enum { uniforms };
# include "gskglprograms.defs"
#undef GSK_GL_DEFINE_PROGRAM
#undef GSK_GL_ADD_UNIFORM
#undef GSL_GK_NO_UNIFORMS

#define GSK_TYPE_NEXT_DRIVER (gsk_next_driver_get_type())

G_DECLARE_FINAL_TYPE (GskNextDriver, gsk_next_driver, GSK, NEXT_DRIVER, GObject)

struct _GskNextDriver
{
  GObject parent_instance;

  GskGLCommandQueue *command_queue;

  GskGLGlyphLibrary  *glyphs;
  GskGLIconLibrary   *icons;
  GskGLShadowLibrary *shadows;

#define GSK_GL_NO_UNIFORMS
#define GSK_GL_ADD_UNIFORM(pos, KEY, name)
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) GskGLProgram *name;
# include "gskglprograms.defs"
#undef GSK_GL_NO_UNIFORMS
#undef GSK_GL_ADD_UNIFORM
#undef GSK_GL_DEFINE_PROGRAM

  guint debug : 1;
  guint in_frame : 1;
};

GskNextDriver *gsk_next_driver_new                  (GskGLCommandQueue  *command_queue,
                                                     gboolean            debug,
                                                     GError            **error);
GdkGLContext  *gsk_next_driver_get_context          (GskNextDriver      *self);
gboolean       gsk_next_driver_create_render_target (GskNextDriver      *self,
                                                     int                 width,
                                                     int                 height,
                                                     guint              *out_fbo_id,
                                                     guint              *out_texture_id);
void           gsk_next_driver_begin_frame          (GskNextDriver      *driver);
void           gsk_next_driver_end_frame            (GskNextDriver      *driver);

G_END_DECLS

#endif /* __GSK_GL_DRIVER_PRIVATE_H__ */
