/* gskglrenderer.c
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

#include <gsk/gskdebugprivate.h>
#include <gsk/gskrendererprivate.h>

#include "gskglcommandqueueprivate.h"
#include "gskgldriverprivate.h"
#include "gskglprogramprivate.h"
#include "gskglrenderjobprivate.h"
#include "gskglrenderer.h"

struct _GskNextRendererClass
{
  GskRendererClass parent_class;
};

struct _GskNextRenderer
{
  GskRenderer parent_instance;

  /* The GskGLCommandQueue manages how we send all drawing operations,
   * uniform changes, and other texture related operations to the GPU.
   * It maintains a cache of state to help reduce the number of state
   * changes we send to the GPU. Furthermore, it can reorder batches so
   * that we switch programs and uniforms less frequently by verifying
   * what batches can be reordered based on clipping and stacking.
   */
  GskGLCommandQueue *command_queue;

  /* The driver manages our program state and command queues. It gives
   * us a single place to manage loading all the programs as well which
   * unfortunately cannot be shared across all renderers for a display.
   * (Context sharing explicitly does not name program/uniform state as
   * shareable even though on some implementations it is).
   */
  GskNextDriver *driver;
};

G_DEFINE_TYPE (GskNextRenderer, gsk_next_renderer, GSK_TYPE_RENDERER)

GskRenderer *
gsk_next_renderer_new (void)
{
  return g_object_new (GSK_TYPE_NEXT_RENDERER, NULL);
}

static gboolean
gsk_next_renderer_realize (GskRenderer  *renderer,
                           GdkSurface   *surface,
                           GError      **error)
{
  GskNextRenderer *self = (GskNextRenderer *)renderer;
  GskGLCommandQueue *command_queue = NULL;
  GdkGLContext *context = NULL;
  GskNextDriver *driver = NULL;
  gboolean ret = FALSE;
  gboolean debug = FALSE;

  g_assert (GSK_IS_NEXT_RENDERER (self));
  g_assert (GDK_IS_SURFACE (surface));

  if (!(context = gdk_surface_create_gl_context (surface, error)) ||
      !gdk_gl_context_realize (context, error))
    goto failure;

  gdk_gl_context_make_current (context);

  command_queue = gsk_gl_command_queue_new (context);

#ifdef G_ENABLE_DEBUG
  if (GSK_RENDERER_DEBUG_CHECK (GSK_RENDERER (self), SHADERS))
    debug = TRUE;
#endif

  if (!(driver = gsk_next_driver_new (command_queue, debug, error)))
    goto failure;

  self->command_queue = g_steal_pointer (&command_queue);
  self->driver = g_steal_pointer (&driver);

  ret = TRUE;

failure:
  g_clear_object (&driver);
  g_clear_object (&context);
  g_clear_object (&command_queue);

  return ret;
}

static void
gsk_next_renderer_unrealize (GskRenderer *renderer)
{
  GskNextRenderer *self = (GskNextRenderer *)renderer;

  g_assert (GSK_IS_NEXT_RENDERER (renderer));

  g_clear_object (&self->driver);
  g_clear_object (&self->command_queue);
}

typedef struct _GskGLTextureState
{
  GdkGLContext *context;
  GLuint        texture_id;
} GskGLTextureState;

static void
create_texture_from_texture_destroy (gpointer data)
{
  GskGLTextureState *state = data;

  g_assert (state != NULL);
  g_assert (GDK_IS_GL_CONTEXT (state->context));

  gdk_gl_context_make_current (state->context);
  glDeleteTextures (1, &state->texture_id);
  g_clear_object (&state->context);
  g_slice_free (GskGLTextureState, state);
}

static GdkTexture *
create_texture_from_texture (GdkGLContext *context,
                             GLuint        texture_id,
                             int           width,
                             int           height)
{
  GskGLTextureState *state;

  g_assert (GDK_IS_GL_CONTEXT (context));

  state = g_slice_new0 (GskGLTextureState);
  state->texture_id = texture_id;
  state->context = g_object_ref (context);

  return gdk_gl_texture_new (context,
                             texture_id,
                             width,
                             height,
                             create_texture_from_texture_destroy,
                             state);
}

static cairo_region_t *
get_render_region (GdkSurface   *surface,
                   GdkGLContext *context)
{
  const cairo_region_t *damage;
  GdkRectangle whole_surface;
  GdkRectangle extents;
  float scale_factor;

  g_assert (GDK_IS_SURFACE (surface));
  g_assert (GDK_IS_GL_CONTEXT (context));

  scale_factor = gdk_surface_get_scale_factor (surface);

  whole_surface.x = 0;
  whole_surface.y = 0;
  whole_surface.width = gdk_surface_get_width (surface) * scale_factor;
  whole_surface.height = gdk_surface_get_height (surface) * scale_factor;

  damage = gdk_draw_context_get_frame_region (GDK_DRAW_CONTEXT (context));

  /* NULL means everything in this case, and ensures that we
   * don't setup any complicated clips for full scene redraw.
   */
  if (damage == NULL ||
      cairo_region_contains_rectangle (damage, &whole_surface) == CAIRO_REGION_OVERLAP_IN)
    return NULL;

  /* If the extents match the full-scene, do the same as above */
  cairo_region_get_extents (damage, &extents);
  if (gdk_rectangle_equal (&extents, &whole_surface))
    return NULL;

  /* Draw clipped to the bounding-box of the region */
  return cairo_region_create_rectangle (&extents);
}

static void
gsk_gl_renderer_render (GskRenderer          *renderer,
                        GskRenderNode        *root,
                        const cairo_region_t *update_area)
{
  GskNextRenderer *self = (GskNextRenderer *)renderer;
  cairo_region_t *render_region;
  graphene_rect_t viewport;
  GskGLRenderJob *job;
  GdkGLContext *context;
  GdkSurface *surface;
  float scale_factor;

  g_assert (GSK_IS_NEXT_RENDERER (renderer));
  g_assert (root != NULL);

  context = gsk_gl_command_queue_get_context (self->command_queue);
  surface = gdk_draw_context_get_surface (GDK_DRAW_CONTEXT (context));
  scale_factor = gdk_surface_get_scale_factor (surface);
  render_region = get_render_region (surface, context);

  viewport.origin.x = 0;
  viewport.origin.y = 0;
  viewport.size.width = gdk_surface_get_width (surface) * scale_factor;
  viewport.size.height = gdk_surface_get_height (surface) * scale_factor;

  gdk_draw_context_begin_frame (GDK_DRAW_CONTEXT (context), update_area);
  job = gsk_gl_render_job_new (self->driver,
                               &viewport,
                               scale_factor,
                               render_region,
                               0,
                               FALSE);
  gsk_gl_render_job_prepare (job, root);
  gsk_gl_render_job_render (job);
  gdk_draw_context_end_frame (GDK_DRAW_CONTEXT (context));

  gsk_gl_render_job_free (job);

  cairo_region_destroy (render_region);
}

static GdkTexture *
gsk_gl_renderer_render_texture (GskRenderer           *renderer,
                                GskRenderNode         *root,
                                const graphene_rect_t *viewport)
{
  GskNextRenderer *self = (GskNextRenderer *)renderer;
  GskGLRenderJob *job;
  GdkGLContext *context;
  GLuint fbo_id;
  GLuint texture_id;
  int width;
  int height;

  g_assert (GSK_IS_NEXT_RENDERER (renderer));
  g_assert (root != NULL);

  context = gsk_gl_command_queue_get_context (self->command_queue);
  width = ceilf (viewport->size.width);
  height = ceilf (viewport->size.height);

  if (!gsk_next_driver_create_render_target (self->driver,
                                             width,
                                             height,
                                             &fbo_id,
                                             &texture_id))
    return NULL;

  gsk_gl_command_queue_autorelease_framebuffer (self->command_queue, fbo_id);

  job = gsk_gl_render_job_new (self->driver, viewport, 1, NULL, fbo_id, TRUE);
  gsk_gl_render_job_prepare (job, root);
  gsk_gl_render_job_render (job);
  gsk_gl_render_job_free (job);

  return create_texture_from_texture (context, texture_id, width, height);
}

static void
gsk_next_renderer_dispose (GObject *object)
{
#ifdef G_ENABLE_DEBUG
  GskNextRenderer *self = (GskNextRenderer *)object;

  g_assert (self->command_queue == NULL);
  g_assert (self->driver == NULL);
#endif

  G_OBJECT_CLASS (gsk_next_renderer_parent_class)->dispose (object);
}

static void
gsk_next_renderer_class_init (GskNextRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GskRendererClass *renderer_class = GSK_RENDERER_CLASS (klass);

  object_class->dispose = gsk_next_renderer_dispose;

  renderer_class->realize = gsk_next_renderer_realize;
  renderer_class->unrealize = gsk_next_renderer_unrealize;
  renderer_class->render = gsk_gl_renderer_render;
  renderer_class->render_texture = gsk_gl_renderer_render_texture;
}

static void
gsk_next_renderer_init (GskNextRenderer *self)
{
}
