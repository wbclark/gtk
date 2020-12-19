/* gskgltexturelibrary.c
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
#include "gskgltexturelibraryprivate.h"

typedef struct
{
  guint  hash;
  guint  len;
  guint8 key[0];
} Entry;

typedef struct
{
  GdkGLContext *context;
  GHashFunc hash_func;
  GEqualFunc equal_func;
} GskGLTextureLibraryPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GskGLTextureLibrary, gsk_gl_texture_library, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_CONTEXT,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

static void
gsk_gl_texture_library_dispose (GObject *object)
{
  GskGLTextureLibrary *self = (GskGLTextureLibrary *)object;
  GskGLTextureLibraryPrivate *priv = gsk_gl_texture_library_get_instance_private (self);

  g_clear_object (&priv->context);

  G_OBJECT_CLASS (gsk_gl_texture_library_parent_class)->dispose (object);
}

static void
gsk_gl_texture_library_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  GskGLTextureLibrary *self = GSK_GL_TEXTURE_LIBRARY (object);
  GskGLTextureLibraryPrivate *priv = gsk_gl_texture_library_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, priv->context);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gsk_gl_texture_library_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  GskGLTextureLibrary *self = GSK_GL_TEXTURE_LIBRARY (object);
  GskGLTextureLibraryPrivate *priv = gsk_gl_texture_library_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      priv->context = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gsk_gl_texture_library_class_init (GskGLTextureLibraryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsk_gl_texture_library_dispose;
  object_class->get_property = gsk_gl_texture_library_get_property;
  object_class->set_property = gsk_gl_texture_library_set_property;

  properties [PROP_CONTEXT] =
    g_param_spec_object ("context",
                         "Context",
                         "Context",
                         GDK_TYPE_GL_CONTEXT,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gsk_gl_texture_library_init (GskGLTextureLibrary *self)
{
  GskGLTextureLibraryPrivate *priv = gsk_gl_texture_library_get_instance_private (self);

  priv->hash_func = g_direct_hash;
  priv->equal_func = g_direct_equal;
}

GdkGLContext *
gsk_gl_texture_library_get_context (GskGLTextureLibrary *self)
{
  GskGLTextureLibraryPrivate *priv = gsk_gl_texture_library_get_instance_private (self);

  g_return_val_if_fail (GSK_IS_GL_TEXTURE_LIBRARY (self), NULL);
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (priv->context), NULL);

  return priv->context;
}

void
gsk_gl_texture_library_begin_frame (GskGLTextureLibrary *self)
{
  g_return_if_fail (GSK_IS_GL_TEXTURE_LIBRARY (self));

  if (GSK_GL_TEXTURE_LIBRARY_GET_CLASS (self)->begin_frame)
    GSK_GL_TEXTURE_LIBRARY_GET_CLASS (self)->begin_frame (self);
}

void
gsk_gl_texture_library_end_frame (GskGLTextureLibrary *self)
{
  g_return_if_fail (GSK_IS_GL_TEXTURE_LIBRARY (self));

  if (GSK_GL_TEXTURE_LIBRARY_GET_CLASS (self)->end_frame)
    GSK_GL_TEXTURE_LIBRARY_GET_CLASS (self)->end_frame (self);
}
