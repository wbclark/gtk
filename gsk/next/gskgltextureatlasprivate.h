/* gskgltextureatlasprivate.h
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

#ifndef __GSK_GL_TEXTURE_ATLAS_PRIVATE_H__
#define __GSK_GL_TEXTURE_ATLAS_PRIVATE_H__

#include "gskgltypes.h"

G_BEGIN_DECLS

#define GSK_TYPE_GL_TEXTURE_ATLAS (gsk_gl_texture_atlas_get_type())

G_DECLARE_FINAL_TYPE (GskGLTextureAtlas, gsk_gl_texture_atlas, GSK, GL_TEXTURE_ATLAS, GObject)

GskGLTextureAtlas *gsk_gl_texture_atlas_new (void);

G_END_DECLS

#endif /* __GSK_GL_TEXTURE_ATLAS_PRIVATE_H__ */
