/* gskgltypes.h
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

#ifndef GSK_GL_TYPES_H
#define GSK_GL_TYPES_H

#include <graphene.h>
#include <epoxy/gl.h>
#include <gdk/gdk.h>
#include <gsk/gsk.h>

G_BEGIN_DECLS

#define GSK_GL_N_VERTICES 6

typedef struct _GskGLAttachmentState GskGLAttachmentState;
typedef struct _GskGLCommandQueue GskGLCommandQueue;
typedef struct _GskGLCompiler GskGLCompiler;
typedef struct _GskGLDrawVertex GskGLDrawVertex;
typedef struct _GskGLGlyphLibrary GskGLGlyphLibrary;
typedef struct _GskGLIconLibrary GskGLIconLibrary;
typedef struct _GskGLProgram GskGLProgram;
typedef struct _GskGLShadowLibrary GskGLShadowLibrary;
typedef struct _GskGLTextureAtlas GskGLTextureAtlas;
typedef struct _GskGLTextureLibrary GskGLTextureLibrary;
typedef struct _GskGLUniformState GskGLUniformState;
typedef struct _GskNextDriver GskNextDriver;
typedef struct _GskGLRenderJob GskGLRenderJob;

struct _GskGLDrawVertex
{
  float position[2];
  float uv[2];
};

G_END_DECLS

#endif /* GSK_GL_TYPES_H */
