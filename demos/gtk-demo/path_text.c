/* Path/Text
 *
 * This demo shows how to use GskPath to animate a path along another path.
 */

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#define GTK_TYPE_PATH_WIDGET (gtk_path_widget_get_type ())
G_DECLARE_FINAL_TYPE (GtkPathWidget, gtk_path_widget, GTK, PATH_WIDGET, GtkWidget)

enum {
  PROP_0,
  PROP_TEXT,
  PROP_EDITABLE,
  N_PROPS
};

struct _GtkPathWidget
{
  GtkWidget parent_instance;

  char *text;
  gboolean editable;

  graphene_point_t points[4];
  GskPath *line_path;
  GskPathMeasure *line_measure;
  GskPath *text_path;

  GdkPaintable *background;
};

struct _GtkPathWidgetClass
{
  GtkWidgetClass parent_class;
};

static GParamSpec *properties[N_PROPS] = { NULL, };

G_DEFINE_TYPE (GtkPathWidget, gtk_path_widget, GTK_TYPE_WIDGET)

static GskPath *
create_path_from_text (GtkWidget  *widget,
                       const char *text)
{
  cairo_surface_t *surface;
  cairo_t *cr;
  cairo_path_t *path;
  PangoLayout *layout;
  PangoFontDescription *desc;
  GskPath *result;

  surface = cairo_recording_surface_create (CAIRO_CONTENT_COLOR_ALPHA, NULL);
  cr = cairo_create (surface);

  layout = gtk_widget_create_pango_layout (widget, text);
  desc = pango_font_description_from_string ("sans bold 36");
  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);

  cairo_move_to (cr, 0, - pango_layout_get_baseline (layout) / (double) PANGO_SCALE);
  pango_cairo_layout_path (cr, layout);
  path = cairo_copy_path_flat (cr);
  result = gsk_path_new_from_cairo (path);

  cairo_path_destroy (path);
  g_object_unref (layout);
  cairo_destroy (cr);
  cairo_surface_destroy (surface);

  return result;
}

typedef struct
{
  GskPathMeasure *measure;
  GskPathBuilder *builder;
  double scale;
} GtkPathTransform;

static void
gtk_path_transform_point (GskPathMeasure         *measure,
                          const graphene_point_t *pt,
                          float                   scale,
                          graphene_point_t       *res)
{
  graphene_vec2_t tangent;

  gsk_path_measure_get_point (measure, pt->x * scale, res, &tangent);

  res->x -= pt->y * scale * graphene_vec2_get_y (&tangent);
  res->y += pt->y * scale * graphene_vec2_get_x (&tangent);
}

static gboolean
gtk_path_transform_op (GskPathOperation        op,
                       const graphene_point_t *pts,
                       gsize                   n_pts,
                       gpointer                data)
{
  GtkPathTransform *transform = data;

  switch (op)
  {
    case GSK_PATH_MOVE:
      {
        graphene_point_t res;
        gtk_path_transform_point (transform->measure, &pts[0], transform->scale, &res);
        gsk_path_builder_move_to (transform->builder, res.x, res.y);
      }
      break;

    case GSK_PATH_LINE:
      {
        graphene_point_t res;
        gtk_path_transform_point (transform->measure, &pts[1], transform->scale, &res);
        gsk_path_builder_line_to (transform->builder, res.x, res.y);
      }
      break;

    case GSK_PATH_CURVE:
      {
        graphene_point_t res[3];
        gtk_path_transform_point (transform->measure, &pts[1], transform->scale, &res[0]);
        gtk_path_transform_point (transform->measure, &pts[2], transform->scale, &res[1]);
        gtk_path_transform_point (transform->measure, &pts[3], transform->scale, &res[2]);
        gsk_path_builder_curve_to (transform->builder, res[0].x, res[0].y, res[1].x, res[1].y, res[2].x, res[2].y);
      }
      break;

    case GSK_PATH_CLOSE:
      gsk_path_builder_close (transform->builder);
      break;

    default:
      g_assert_not_reached();
      return FALSE;
  }

  return TRUE;
}

static GskPath *
gtk_path_transform (GskPathMeasure *measure,
                    GskPath        *path)
{
  GtkPathTransform transform = { measure, gsk_path_builder_new () };
  graphene_rect_t bounds;

  gsk_path_get_bounds (path, &bounds);
  if (bounds.origin.x + bounds.size.width > 0)
    transform.scale = gsk_path_measure_get_length (measure) / (bounds.origin.x + bounds.size.width);
  else
    transform.scale = 1.0f;

  gsk_path_foreach (path, gtk_path_transform_op, &transform);

  return gsk_path_builder_free_to_path (transform.builder);
}

static void
gtk_path_widget_clear_text_path (GtkPathWidget *self)
{
  g_clear_pointer (&self->text_path, gsk_path_unref);
}

static void
gtk_path_widget_clear_paths (GtkPathWidget *self)
{
  gtk_path_widget_clear_text_path (self);

  g_clear_pointer (&self->line_path, gsk_path_unref);
  g_clear_pointer (&self->line_measure, gsk_path_measure_unref);
}

static void
gtk_path_widget_create_text_path (GtkPathWidget *self)
{
  GskPath *path;

  gtk_path_widget_clear_text_path (self);

  if (self->line_measure == NULL)
    return;

  path = create_path_from_text (GTK_WIDGET (self), self->text);
  self->text_path = gtk_path_transform (self->line_measure, path);

  gsk_path_unref (path);
}

static void
gtk_path_widget_create_paths (GtkPathWidget *self)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self));
  double height = gtk_widget_get_height (GTK_WIDGET (self));
  GskPathBuilder *builder;

  gtk_path_widget_clear_paths (self);

  if (width <= 0 || height <= 0)
    return;

  builder = gsk_path_builder_new ();
  gsk_path_builder_move_to (builder,
                            self->points[0].x * width, self->points[0].y * height);
  gsk_path_builder_curve_to (builder,
                             self->points[1].x * width, self->points[1].y * height,
                             self->points[2].x * width, self->points[2].y * height,
                             self->points[3].x * width, self->points[3].y * height);
  self->line_path = gsk_path_builder_free_to_path (builder);

  self->line_measure = gsk_path_measure_new (self->line_path);

  gtk_path_widget_create_text_path (self);
}

static void
gtk_path_widget_allocate (GtkWidget *widget,
                          int        width,
                          int        height,
                          int        baseline)
{
  GtkPathWidget *self = GTK_PATH_WIDGET (widget);

  GTK_WIDGET_CLASS (gtk_path_widget_parent_class)->size_allocate (widget, width, height, baseline);

  gtk_path_widget_create_paths (self);
}

static void
gtk_path_widget_snapshot (GtkWidget   *widget,
                          GtkSnapshot *snapshot)
{
  GtkPathWidget *self = GTK_PATH_WIDGET (widget);
  double width = gtk_widget_get_width (widget);
  double height = gtk_widget_get_height (widget);
  GskPath *path;
  GskStroke *stroke;
  gsize i;

  /* frosted glass the background */
  gtk_snapshot_push_blur (snapshot, 10);
  gdk_paintable_snapshot (self->background, snapshot, width, height);
  gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 1, 1, 1, 0.6 }, &GRAPHENE_RECT_INIT (0, 0, width, height));
  gtk_snapshot_pop (snapshot);

  /* draw the text */
  if (self->text_path)
    {
      gtk_snapshot_push_fill (snapshot, self->text_path, GSK_FILL_RULE_WINDING);
      gdk_paintable_snapshot (self->background, snapshot, width, height);

      /* ... with an emboss effect */
      stroke = gsk_stroke_new (2.0);
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT(1, 1));
      gtk_snapshot_push_stroke (snapshot, self->text_path, stroke);
      gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 0, 0, 0, 0.2 }, &GRAPHENE_RECT_INIT (0, 0, width, height));
      gsk_stroke_free (stroke);
      gtk_snapshot_pop (snapshot);

      gtk_snapshot_pop (snapshot);
    }

  if (self->editable)
    {
      GskPathBuilder *builder;

      /* draw the control line */
      stroke = gsk_stroke_new (1.0);
      gtk_snapshot_push_stroke (snapshot, self->line_path, stroke);
      gsk_stroke_free (stroke);
      gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 0, 0, 0, 1 }, &GRAPHENE_RECT_INIT (0, 0, width, height));
      gtk_snapshot_pop (snapshot);

      /* draw the points */
      builder = gsk_path_builder_new ();
      for (i = 0; i < 4; i++)
        {
          gsk_path_builder_add_circle (builder, &GRAPHENE_POINT_INIT (self->points[i].x * width, self->points[i].y * height), 8);
        }
      path = gsk_path_builder_free_to_path (builder);

      gtk_snapshot_push_fill (snapshot, path, GSK_FILL_RULE_WINDING);
      gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 1, 1, 1, 1 }, &GRAPHENE_RECT_INIT (0, 0, width, height));
      gtk_snapshot_pop (snapshot);

      stroke = gsk_stroke_new (1.0);
      gtk_snapshot_push_stroke (snapshot, path, stroke);
      gsk_stroke_free (stroke);
      gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 0, 0, 0, 1 }, &GRAPHENE_RECT_INIT (0, 0, width, height));
      gtk_snapshot_pop (snapshot);

      gsk_path_unref (path);
    }
}

static void
gtk_path_widget_set_text (GtkPathWidget *self,
                          const char    *text)
{
  if (g_strcmp0 (self->text, text) == 0)
    return;

  g_free (self->text);
  self->text = g_strdup (text);

  gtk_path_widget_create_paths (self);

  gtk_widget_queue_draw (GTK_WIDGET (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TEXT]);
}

static void
gtk_path_widget_set_editable (GtkPathWidget *self,
                              gboolean       editable)
{
  if (self->editable == editable)
    return;

  self->editable = editable;

  gtk_widget_queue_draw (GTK_WIDGET (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_EDITABLE]);
}

static void
gtk_path_widget_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)

{
  GtkPathWidget *self = GTK_PATH_WIDGET (object);

  switch (prop_id)
    {
    case PROP_TEXT:
      gtk_path_widget_set_text (self, g_value_get_string (value));
      break;

    case PROP_EDITABLE:
      gtk_path_widget_set_editable (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gtk_path_widget_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  GtkPathWidget *self = GTK_PATH_WIDGET (object);

  switch (prop_id)
    {
    case PROP_TEXT:
      g_value_set_string (value, self->text);
      break;

    case PROP_EDITABLE:
      g_value_set_boolean (value, self->editable);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gtk_path_widget_dispose (GObject *object)
{
  GtkPathWidget *self = GTK_PATH_WIDGET (object);

  gtk_path_widget_clear_paths (self);

  G_OBJECT_CLASS (gtk_path_widget_parent_class)->dispose (object);
}

static void
gtk_path_widget_class_init (GtkPathWidgetClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gtk_path_widget_dispose;
  object_class->set_property = gtk_path_widget_set_property;
  object_class->get_property = gtk_path_widget_get_property;

  widget_class->size_allocate = gtk_path_widget_allocate;
  widget_class->snapshot = gtk_path_widget_snapshot;

  properties[PROP_TEXT] =
    g_param_spec_string ("text",
                         "text",
                         "Text transformed along a path",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_EDITABLE] =
    g_param_spec_boolean ("editable",
                          "editable",
                          "If the path can be edited by the user",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gtk_path_widget_init (GtkPathWidget *self)
{
  self->points[0] = GRAPHENE_POINT_INIT (0.1, 0.9);
  self->points[1] = GRAPHENE_POINT_INIT (0.3, 0.1);
  self->points[2] = GRAPHENE_POINT_INIT (0.7, 0.1);
  self->points[3] = GRAPHENE_POINT_INIT (0.9, 0.9);

  self->background = GDK_PAINTABLE (gdk_texture_new_from_resource ("/sliding_puzzle/portland-rose.jpg"));

  gtk_path_widget_set_text (self, "It's almost working");
}

GtkWidget *
gtk_path_widget_new (void)
{
  GtkPathWidget *self;

  self = g_object_new (GTK_TYPE_PATH_WIDGET, NULL);

  return GTK_WIDGET (self);
}

GtkWidget *
do_path_text (GtkWidget *do_widget)
{
  static GtkWidget *window = NULL;

  if (!window)
    {
      GtkBuilder *builder;

      g_type_ensure (GTK_TYPE_PATH_WIDGET);

      builder = gtk_builder_new_from_resource ("/path_text/path_text.ui");
      window = GTK_WIDGET (gtk_builder_get_object (builder, "window"));
      gtk_window_set_display (GTK_WINDOW (window),
                              gtk_widget_get_display (do_widget));
      g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &window);
      g_object_unref (builder);
    }

  if (!gtk_widget_get_visible (window))
    gtk_widget_show (window);
  else
    gtk_window_destroy (GTK_WINDOW (window));

  return window;
}