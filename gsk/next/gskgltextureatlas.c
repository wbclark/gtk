/* gskgltextureatlas.c
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

#include "gskgltextureatlasprivate.h"

struct _GskGLTextureAtlas
{
  GObject parent_instance;
};

G_DEFINE_TYPE (GskGLTextureAtlas, gsk_gl_texture_atlas, G_TYPE_OBJECT)

GskGLTextureAtlas *
gsk_gl_texture_atlas_new (void)
{
  return g_object_new (GSK_TYPE_GL_TEXTURE_ATLAS, NULL);
}

static void
gsk_gl_texture_atlas_class_init (GskGLTextureAtlasClass *klass)
{
}

static void
gsk_gl_texture_atlas_init (GskGLTextureAtlas *self)
{
}
