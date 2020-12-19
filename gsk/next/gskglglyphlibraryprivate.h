/* gskglglyphlibraryprivate.h
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

#ifndef __GSK_GL_GLYPH_LIBRARY_PRIVATE_H__
#define __GSK_GL_GLYPH_LIBRARY_PRIVATE_H__

#include <pango/pango.h>

#include "gskgltexturelibraryprivate.h"

G_BEGIN_DECLS

typedef struct _GskGLGlyphKey
{
  PangoFont *font;
  PangoGlyph glyph;
  guint xshift : 3;
  guint yshift : 3;
  guint scale  : 26; /* times 1024 */
} GskGLGlyphKey;

#if GLIB_SIZEOF_VOID_P == 8
G_STATIC_ASSERT (sizeof (GskGLGlyphKey) == 16);
#elif GLIB_SIZEOF_VOID_P == 4
G_STATIC_ASSERT (sizeof (GskGLGlyphKey) == 12);
#endif

#define GSK_TYPE_GL_GLYPH_LIBRARY (gsk_gl_glyph_library_get_type())

G_DECLARE_FINAL_TYPE (GskGLGlyphLibrary, gsk_gl_glyph_library, GSK, GL_GLYPH_LIBRARY, GskGLTextureLibrary)

GskGLGlyphLibrary *gsk_gl_glyph_library_new (GdkGLContext *context);

G_END_DECLS

#endif /* __GSK_GL_GLYPH_LIBRARY_PRIVATE_H__ */
