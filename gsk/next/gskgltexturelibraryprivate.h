/* gskgltexturelibraryprivate.h
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

#ifndef __GSK_GL_TEXTURE_LIBRARY_PRIVATE_H__
#define __GSK_GL_TEXTURE_LIBRARY_PRIVATE_H__

#include "gskgltypes.h"

G_BEGIN_DECLS

#define GSK_TYPE_GL_TEXTURE_LIBRARY (gsk_gl_texture_library_get_type())

G_DECLARE_DERIVABLE_TYPE (GskGLTextureLibrary, gsk_gl_texture_library, GSK, GL_TEXTURE_LIBRARY, GObject)

struct _GskGLTextureLibraryClass
{
  GObjectClass parent_class;

  void (*begin_frame) (GskGLTextureLibrary *library);
  void (*end_frame)   (GskGLTextureLibrary *library);
};

GdkGLContext *gsk_gl_texture_library_get_context (GskGLTextureLibrary  *self);
void          gsk_gl_texture_library_set_funcs   (GHashFunc             hash_func,
                                                  GEqualFunc            equal_func);
void          gsk_gl_texture_library_begin_frame (GskGLTextureLibrary  *self);
void          gsk_gl_texture_library_end_frame   (GskGLTextureLibrary  *self);
gboolean      gsk_gl_texture_library_pack        (GskGLTextureLibrary  *self,
                                                  gconstpointer         key,
                                                  gsize                 keylen,
                                                  int                   width,
                                                  int                   height,
                                                  GskGLTextureAtlas   **atlas,
                                                  int                  *atlas_x,
                                                  int                  *atlas_y);
gboolean      gsk_gl_texture_library_lookup      (GskGLTextureLibrary  *library,
                                                  gconstpointer         key,
                                                  GskGLTextureAtlas   **atlas,
                                                  int                  *atlas_x,
                                                  int                  *atlas_y);

G_END_DECLS

#endif /* __GSK_GL_TEXTURE_LIBRARY_PRIVATE_H__ */
