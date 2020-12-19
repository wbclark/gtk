/* gskglattachmentstate.c
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

#include "config.h"

#include "gskglattachmentstateprivate.h"

GskGLAttachmentState *
gsk_gl_attachment_state_new (void)
{
  GskGLAttachmentState *self;

  self = g_new0 (GskGLAttachmentState, 1);

  self->fbo.changed = FALSE;
  self->fbo.id = 0;

  /* Initialize textures, assume we are 2D by default since it
   * doesn't really matter until we bind something other than
   * GL_TEXTURE0 to it anyway.
   */
  for (guint i = 0; i < G_N_ELEMENTS (self->textures); i++)
    {
      self->textures[i].target = GL_TEXTURE_2D;
      self->textures[i].texture = GL_TEXTURE0;
      self->textures[i].id = 0;
      self->textures[i].changed = FALSE;
      self->textures[i].initial = TRUE;
    }

  return self;
}

void
gsk_gl_attachment_state_free (GskGLAttachmentState *self)
{
  g_free (self);
}

void
gsk_gl_attachment_state_bind_texture (GskGLAttachmentState *self,
                                      GLenum                target,
                                      GLenum                texture,
                                      guint                 id)
{
  GskGLBindTexture *attach;

  g_assert (self != NULL);
  g_assert (target == GL_TEXTURE_1D ||
            target == GL_TEXTURE_2D ||
            target == GL_TEXTURE_3D);
  g_assert (texture >= GL_TEXTURE0 && texture <= GL_TEXTURE16);

  attach = &self->textures[texture - GL_TEXTURE0];

  if (attach->target != target || attach->texture != texture || attach->id != id)
    {
      attach->target = target;
      attach->texture = texture;
      attach->id = id;
      attach->changed = TRUE;
      attach->initial = FALSE;
      self->has_texture_change = TRUE;
    }
}

void
gsk_gl_attachment_state_bind_framebuffer (GskGLAttachmentState *self,
                                          guint                 id)
{
  g_assert (self != NULL);

  if (self->fbo.id != id)
    {
      self->fbo.id = id;
      self->fbo.changed = TRUE;
    }
}

/**
 * gsk_gl_attachment_state_save:
 * @self: a #GskGLAttachmentState
 *
 * Creates a copy of @self that represents the current attachments
 * as known to @self.
 *
 * This can be used to restore state later, such as after running
 * various GL commands that are external to the GL renderer.
 *
 * This must be freed by calling either gsk_gl_attachment_state_free()
 * or gsk_gl_attachment_state_restore().
 *
 * Returns: (transfer full): a new #GskGLAttachmentState or %NULL
 */
GskGLAttachmentState *
gsk_gl_attachment_state_save (GskGLAttachmentState *self)
{
  GskGLAttachmentState *ret;

  if (self == NULL)
    return NULL;

  ret = g_slice_dup (GskGLAttachmentState, self);
  ret->fbo.changed = FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (ret->textures); i++)
    ret->textures[i].changed = FALSE;

  return g_steal_pointer (&ret);
}

/**
 * gsk_gl_attachment_state_restore:
 * @self: (transfer full): the #GskGLAttachmentState
 *
 * Restores the attachment state and frees @self.
 */
void
gsk_gl_attachment_state_restore (GskGLAttachmentState *self)
{
  if (self == NULL)
    return;

  glBindFramebuffer (GL_FRAMEBUFFER, self->fbo.id);

  for (guint i = 0; i < G_N_ELEMENTS (self->textures); i++)
    {
      if (!self->textures[i].initial)
        {
          glActiveTexture (GL_TEXTURE0 + i);
          glBindTexture (self->textures[i].target,
                         self->textures[i].id);
        }
    }

  g_slice_free (GskGLAttachmentState, self);
}
