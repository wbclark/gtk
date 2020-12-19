/* gskglbufferprivate.h
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

#ifndef __GSK_GL_BUFFER_PRIVATE_H__
#define __GSK_GL_BUFFER_PRIVATE_H__

#include <glib.h>
#include <epoxy/gl.h>

G_BEGIN_DECLS

typedef struct _GskGLBuffer GskGLBuffer;

GskGLBuffer *gsk_gl_buffer_new     (GLenum       target,
                                    guint        element_size);
void         gsk_gl_buffer_free    (GskGLBuffer *buffer);
void         gsk_gl_buffer_submit  (GskGLBuffer *buffer);
gpointer     gsk_gl_buffer_advance (GskGLBuffer *buffer,
                                    guint        count,
                                    guint       *offset);

G_END_DECLS

#endif /* __GSK_GL_BUFFER_PRIVATE_H__ */
