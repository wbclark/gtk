/* gskglrenderjob.c
 *
 * Copyright 2017 Timm Bäder <mail@baedert.org>
 * Copyright 2018 Matthias Clasen <mclasen@redhat.com>
 * Copyright 2018 Alexander Larsson <alexl@redhat.com>
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

#include <gdk/gdkglcontextprivate.h>
#include <gsk/gskrendernodeprivate.h>
#include <math.h>
#include <string.h>

#include "gskglcommandqueueprivate.h"
#include "gskgldriverprivate.h"
#include "gskglrenderjobprivate.h"

#define ORTHO_NEAR_PLANE -10000
#define ORTHO_FAR_PLANE   10000

struct _GskGLRenderJob
{
  GskNextDriver     *driver;
  GskGLCommandQueue *command_queue;
  cairo_region_t    *region;
  guint              framebuffer;
  graphene_rect_t    viewport;
  graphene_matrix_t  projection;
  GArray            *modelview;
  GArray            *clip;
  float              offset_x;
  float              offset_y;
  float              scale_x;
  float              scale_y;
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
  float scale_x;
  float scale_y;
  float offset_x_before;
  float offset_y_before;
} GskGLRenderModelview;

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

static inline GskGLRenderModelview *
gsk_gl_render_job_get_modelview (GskGLRenderJob *job)
{
  return &g_array_index (job->modelview,
                         GskGLRenderModelview,
                         job->modelview->len - 1);
}

static void
extract_matrix_metadata (GskGLRenderModelview *modelview)
{
  float dummy;

  switch (gsk_transform_get_category (modelview->transform))
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      modelview->scale_x = 1;
      modelview->scale_y = 1;
      break;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      gsk_transform_to_affine (modelview->transform,
                               &modelview->scale_x, &modelview->scale_y,
                               &dummy, &dummy);
      break;

    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_2D:
      {
        graphene_vec3_t col1;
        graphene_vec3_t col2;
        graphene_matrix_t m;

        gsk_transform_to_matrix (modelview->transform, &m);

        /* TODO: 90% sure this is incorrect. But we should never hit this code
         * path anyway. */
        graphene_vec3_init (&col1,
                            graphene_matrix_get_value (&m, 0, 0),
                            graphene_matrix_get_value (&m, 1, 0),
                            graphene_matrix_get_value (&m, 2, 0));

        graphene_vec3_init (&col2,
                            graphene_matrix_get_value (&m, 0, 1),
                            graphene_matrix_get_value (&m, 1, 1),
                            graphene_matrix_get_value (&m, 2, 1));

        modelview->scale_x = graphene_vec3_length (&col1);
        modelview->scale_y = graphene_vec3_length (&col2);
      }
      break;

    default:
      break;
    }
}

static void
gsk_gl_render_job_set_modelview (GskGLRenderJob *job,
                                 GskTransform   *transform)
{
  GskGLRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskGLRenderModelview,
                              job->modelview->len - 1);

  modelview->transform = transform;

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = modelview->scale_x;
  job->scale_y = modelview->scale_y;
}

static void
gsk_gl_render_job_push_modelview (GskGLRenderJob *job,
                                  GskTransform   *transform)
{
  GskGLRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);
  g_assert (transform != NULL);

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskGLRenderModelview,
                              job->modelview->len - 1);

  if G_LIKELY (job->modelview->len > 1)
    {
      GskGLRenderModelview *last;
      GskTransform *t = NULL;

      last = &g_array_index (job->modelview,
                             GskGLRenderModelview,
                             job->modelview->len - 2);

      /* Multiply given matrix with our previews modelview */
      t = gsk_transform_translate (gsk_transform_ref (last->transform),
                                   &(graphene_point_t) {
                                     job->offset_x,
                                     job->offset_y
                                   });
      t = gsk_transform_transform (t, transform);
      modelview->transform = t;
    }
  else
    {
      modelview->transform = gsk_transform_ref (transform);
    }

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = job->scale_x;
  job->scale_y = job->scale_y;
}

static void
gsk_gl_render_job_pop_modelview (GskGLRenderJob *job)
{
  const GskGLRenderModelview *head;

  g_assert (job != NULL);
  g_assert (job->modelview);
  g_assert (job->modelview->len > 0);

  head = gsk_gl_render_job_get_modelview (job);

  job->offset_x = head->offset_x_before;
  job->offset_y = head->offset_y_before;

  gsk_transform_unref (head->transform);

  job->modelview->len--;

  if (job->modelview->len >= 1)
    {
      head = &g_array_index (job->modelview, GskGLRenderModelview, job->modelview->len - 1);

      job->scale_x = head->scale_x;
      job->scale_y = head->scale_y;
    }
}

static inline GskGLRenderClip *
gsk_gl_render_job_get_clip (GskGLRenderJob *job)
{
  if (job->clip->len == 0)
    return NULL;
  else
    return &g_array_index (job->clip, GskGLRenderClip, job->clip->len - 1);
}

static void
gsk_gl_render_job_push_clip (GskGLRenderJob       *job,
                             const GskRoundedRect *rect)
{
  GskGLRenderClip clip;

  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (rect != NULL);

  clip.rect = *rect;
  clip.is_rectilinear = gsk_rounded_rect_is_rectilinear (rect);

  g_array_append_val (job->clip, clip);
}

static void
gsk_gl_render_job_pop_clip (GskGLRenderJob *job)
{
  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (job->clip->len > 0);

  job->clip->len--;
}

static void
gsk_gl_render_job_offset (GskGLRenderJob *job,
                          float           offset_x,
                          float           offset_y)
{
  g_assert (job != NULL);

  job->offset_x += offset_x;
  job->offset_y += offset_y;
}

static void
gsk_gl_render_job_transform_bounds (GskGLRenderJob        *job,
                                    const graphene_rect_t *rect,
                                    graphene_rect_t       *out_rect)
{
  GskGLRenderModelview *modelview;
  graphene_rect_t r;

  g_assert (job != NULL);
  g_assert (rect != NULL);
  g_assert (out_rect != NULL);

  r.origin.x = rect->origin.x + job->offset_x;
  r.origin.y = rect->origin.y + job->offset_y;
  r.size.width = rect->size.width;
  r.size.height = rect->size.width;

  modelview = gsk_gl_render_job_get_modelview (job);

  gsk_transform_transform_bounds (modelview->transform, &r, out_rect);
}

GskGLRenderJob *
gsk_gl_render_job_new (GskNextDriver         *driver,
                       const graphene_rect_t *viewport,
                       float                  scale_factor,
                       const cairo_region_t  *region,
                       guint                  framebuffer,
                       gboolean               flip_y)
{
  const graphene_rect_t *clip_rect = viewport;
  graphene_rect_t transformed_extents;
  GskGLRenderJob *job;

  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (driver), NULL);
  g_return_val_if_fail (viewport != NULL, NULL);
  g_return_val_if_fail (scale_factor > 0, NULL);

  job = g_slice_new0 (GskGLRenderJob);
  job->driver = g_object_ref (driver);
  job->command_queue = driver->command_queue;
  job->clip = g_array_new (FALSE, FALSE, sizeof (GskGLRenderClip));
  job->modelview = g_array_new (FALSE, FALSE, sizeof (GskGLRenderModelview));
  job->framebuffer = framebuffer;
  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = scale_factor;
  job->scale_y = scale_factor;
  job->viewport = *viewport;
  job->region = region ? cairo_region_copy (region) : NULL;
  job->flip_y = !!flip_y;

  init_projection_matrix (&job->projection, viewport, flip_y);
  gsk_gl_render_job_set_modelview (job, gsk_transform_scale (NULL, scale_factor, scale_factor));

  /* Setup our initial clip. If region is NULL then we are drawing the
   * whole viewport. Otherwise, we need to convert the region to a
   * bounding box and clip based on that.
   */

  if (region != NULL)
    {
      cairo_rectangle_int_t extents;

      cairo_region_get_extents (region, &extents);
      gsk_gl_render_job_transform_bounds (job,
                                          &GRAPHENE_RECT_INIT (extents.x,
                                                               extents.y,
                                                               extents.width,
                                                               extents.height),
                                          &transformed_extents);
      clip_rect = &transformed_extents;
    }

  gsk_gl_render_job_push_clip (job,
                               &GSK_ROUNDED_RECT_INIT (clip_rect->origin.x,
                                                       clip_rect->origin.y,
                                                       clip_rect->size.width,
                                                       clip_rect->size.height));

  return job;
}

void
gsk_gl_render_job_free (GskGLRenderJob *job)
{
  while (job->modelview->len > 0)
    {
      GskGLRenderModelview *modelview = gsk_gl_render_job_get_modelview (job);
      g_clear_pointer (&modelview->transform, gsk_transform_unref);
      job->modelview->len--;
    }

  g_clear_object (&job->driver);
  g_clear_pointer (&job->region, cairo_region_destroy);
  g_clear_pointer (&job->modelview, g_array_unref);
  g_clear_pointer (&job->clip, g_array_unref);
  g_slice_free (GskGLRenderJob, job);
}

static inline gboolean G_GNUC_PURE
node_is_invisible (const GskRenderNode *node)
{
  return node->bounds.size.width == 0.0f ||
         node->bounds.size.height == 0.0f ||
         isnan (node->bounds.size.width) ||
         isnan (node->bounds.size.height);
}

static inline gboolean G_GNUC_PURE
rect_intersects (const graphene_rect_t *r1,
                 const graphene_rect_t *r2)
{
  /* Assume both rects are already normalized, as they usually are */
  if (r1->origin.x > (r2->origin.x + r2->size.width) ||
      (r1->origin.x + r1->size.width) < r2->origin.x)
    return FALSE;

  if (r1->origin.y > (r2->origin.y + r2->size.height) ||
      (r1->origin.y + r1->size.height) < r2->origin.y)
    return FALSE;

  return TRUE;
}

static gboolean
gsk_gl_render_job_node_overlaps_clip (GskGLRenderJob *job,
                                      GskRenderNode  *node)
{
  GskGLRenderClip *clip;
  graphene_rect_t transformed_bounds;

  g_assert (job != NULL);
  g_assert (node != NULL);

  if (!(clip = gsk_gl_render_job_get_clip (job)))
    return TRUE;

  gsk_gl_render_job_transform_bounds (job, &node->bounds, &transformed_bounds);

  return rect_intersects (&clip->rect.bounds, &transformed_bounds);
}

static void
gsk_gl_render_job_visit_node (GskGLRenderJob *job,
                              GskRenderNode  *node)
{
  g_assert (job != NULL);
  g_assert (node != NULL);
  g_assert (GSK_IS_NEXT_DRIVER (job->driver));
  g_assert (GSK_IS_GL_COMMAND_QUEUE (job->command_queue));

  if (node_is_invisible (node) ||
      !gsk_gl_render_job_node_overlaps_clip (job, node))
    return;

  switch (gsk_render_node_get_node_type (node))
    {
    case GSK_CONTAINER_NODE:
      {
        guint i;
        guint p;

        for (i = 0, p = gsk_container_node_get_n_children (node);
             i < p;
             i ++)
          {
            GskRenderNode *child = gsk_container_node_get_child (node, i);
            gsk_gl_render_job_visit_node (job, child);
          }
      }
    break;

    case GSK_DEBUG_NODE:
      {
        const char *message = gsk_debug_node_get_message (node);

        if (message != NULL)
          gsk_gl_command_queue_push_debug_group (job->command_queue, message);

        gsk_gl_render_job_visit_node (job, gsk_debug_node_get_child (node));

        if (message != NULL)
          gsk_gl_command_queue_pop_debug_group (job->command_queue);
      }
    break;

    case GSK_CAIRO_NODE:
    case GSK_COLOR_NODE:
    case GSK_LINEAR_GRADIENT_NODE:
    case GSK_REPEATING_LINEAR_GRADIENT_NODE:
    case GSK_RADIAL_GRADIENT_NODE:
    case GSK_REPEATING_RADIAL_GRADIENT_NODE:
    case GSK_CONIC_GRADIENT_NODE:
    case GSK_BORDER_NODE:
    case GSK_TEXTURE_NODE:
    case GSK_INSET_SHADOW_NODE:
    case GSK_OUTSET_SHADOW_NODE:
    case GSK_TRANSFORM_NODE:
    case GSK_OPACITY_NODE:
    case GSK_COLOR_MATRIX_NODE:
    case GSK_REPEAT_NODE:
    case GSK_CLIP_NODE:
    case GSK_ROUNDED_CLIP_NODE:
    case GSK_SHADOW_NODE:
    case GSK_BLEND_NODE:
    case GSK_CROSS_FADE_NODE:
    case GSK_TEXT_NODE:
    case GSK_BLUR_NODE:
    case GSK_GL_SHADER_NODE:
    break;

    case GSK_NOT_A_RENDER_NODE:
    default:
      g_assert_not_reached ();
    break;
    }


}

void
gsk_gl_render_job_render (GskGLRenderJob *job,
                          GskRenderNode  *root)
{
  GdkGLContext *context;

  g_return_if_fail (job != NULL);
  g_return_if_fail (root != NULL);
  g_return_if_fail (GSK_IS_NEXT_DRIVER (job->driver));

  context = gsk_next_driver_get_context (job->driver);

  gsk_next_driver_begin_frame (job->driver);

  gsk_gl_command_queue_bind_framebuffer (job->command_queue, job->framebuffer);
  gsk_gl_command_queue_change_viewport (job->command_queue, &job->viewport);
  gsk_gl_command_queue_clear (job->command_queue, 0);

  gdk_gl_context_push_debug_group (context, "Building command queue");
  gsk_gl_render_job_visit_node (job, root);
  gdk_gl_context_pop_debug_group (context);

  gdk_gl_context_push_debug_group (context, "Executing command queue");
  gsk_gl_command_queue_execute (job->command_queue);
  gdk_gl_context_pop_debug_group (context);

  gsk_next_driver_end_frame (job->driver);
}
