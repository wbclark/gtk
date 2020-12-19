/* gskglrenderjob.c
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

#include <string.h>

#include "gskglcommandqueueprivate.h"
#include "gskgldriverprivate.h"
#include "gskglrenderjobprivate.h"

#define ORTHO_NEAR_PLANE -10000
#define ORTHO_FAR_PLANE   10000

struct _GskGLRenderJob
{
  GskNextDriver     *driver;
  GskRenderNode     *root;
  cairo_region_t    *region;
  guint              framebuffer;
  float              scale_factor;
  graphene_rect_t    viewport;
  graphene_matrix_t  projection;
  GArray            *modelview;
  GArray            *clip;
  guint              flip_y : 1;
};

typedef struct _GskGLRenderClip
{
  GskRoundedRect rect;
  bool           is_rectilinear;
} GskGLRenderClip;

typedef struct _GskGLRenderModelview
{
  GskTransform *transform;
  guint         n_repeated;
} GskGLRenderModelview;

static void
gsk_gl_render_modelview_clear (gpointer data)
{
  GskGLRenderModelview *modelview = data;

  g_clear_pointer (&modelview->transform, gsk_transform_unref);
}

static void
init_projection_matrix (graphene_matrix_t     *projection,
                        const graphene_rect_t *viewport,
                        gboolean               flip_y)
{
  graphene_matrix_init_ortho (projection,
                              viewport->origin.x,
                              viewport->origin.x + viewport->size.width,
                              viewport->origin.y,
                              viewport->origin.y + viewport->size.height,
                              ORTHO_NEAR_PLANE,
                              ORTHO_FAR_PLANE);

  if (!flip_y)
    graphene_matrix_scale (projection, 1, -1, 1);
}

static void
gsk_gl_render_job_push_modelview (GskGLRenderJob *job,
                                  GskTransform   *transform)
{
  GskGLRenderModelview modelview = { transform, 0 };

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);
  g_assert (transform != NULL || job->modelview->len > 0);

  if G_LIKELY (job->modelview->len > 0)
    {
      GskGLRenderModelview *last;

      last = &g_array_index (job->modelview,
                             GskGLRenderModelview,
                             job->modelview->len - 1);

      if (transform == NULL || gsk_transform_equal (transform, last->transform))
        {
          last->n_repeated++;
          return;
        }
    }

  g_assert (transform != NULL);

  g_array_append_val (job->modelview, modelview);
}

static void
gsk_gl_render_job_pop_modelview (GskGLRenderJob *job)
{
  GskGLRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);
  g_assert (job->modelview->len > 0);

  modelview = &g_array_index (job->modelview,
                              GskGLRenderModelview,
                              job->modelview->len - 1);

  if (modelview->n_repeated == 0)
    job->modelview->len--;
  else
    modelview->n_repeated--;
}

GskGLRenderJob *
gsk_gl_render_job_new (GskNextDriver         *driver,
                       GskRenderNode         *root,
                       const graphene_rect_t *viewport,
                       float                  scale_factor,
                       const cairo_region_t  *region,
                       guint                  framebuffer,
                       gboolean               flip_y)
{
  GskGLRenderJob *job;

  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (driver), NULL);
  g_return_val_if_fail (root != NULL, NULL);
  g_return_val_if_fail (viewport != NULL, NULL);
  g_return_val_if_fail (scale_factor > 0, NULL);

  job = g_slice_new0 (GskGLRenderJob);
  job->driver = g_object_ref (driver);
  job->root = gsk_render_node_ref (root);
  job->clip = g_array_new (FALSE, FALSE, sizeof (GskGLRenderClip));
  job->framebuffer = framebuffer;
  job->scale_factor = scale_factor;
  job->viewport = *viewport;
  job->region = region ? cairo_region_copy (region) : NULL;
  job->flip_y = !!flip_y;

  init_projection_matrix (&job->projection, viewport, flip_y);

  job->modelview = g_array_new (FALSE, FALSE, sizeof (GskGLRenderModelview));
  g_array_set_clear_func (job->modelview, gsk_gl_render_modelview_clear);
  gsk_gl_render_job_push_modelview (job, gsk_transform_scale (NULL, scale_factor, scale_factor));

  return job;
}

void
gsk_gl_render_job_free (GskGLRenderJob *job)
{
  g_clear_object (&job->driver);
  g_clear_pointer (&job->root, gsk_render_node_unref);
  g_clear_pointer (&job->region, cairo_region_destroy);
  g_clear_pointer (&job->modelview, g_array_unref);
  g_clear_pointer (&job->clip, g_array_unref);
  g_slice_free (GskGLRenderJob, job);
}

void
gsk_gl_render_job_run (GskGLRenderJob *job)
{
  g_return_if_fail (job != NULL);
  g_return_if_fail (GSK_IS_NEXT_DRIVER (job->driver));

  gsk_next_driver_begin_frame (job->driver);
  gsk_next_driver_end_frame (job->driver);
}
