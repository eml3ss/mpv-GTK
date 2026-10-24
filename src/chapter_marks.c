#include "chapter_marks.h"

#include <math.h>
#include <string.h>

typedef struct {
    GtkRange *scale;
    GtkWidget *canvas;
    GtkAdjustment *adjustment;
    gulong adjustment_handler;
    GArray *times;
    double duration;
} ChapterMarks;

typedef struct {
    double origin, travel;
    gboolean inverted;
} MarkTrack;

static const mpv_node *mark_time_field(const mpv_node *entry)
{
    if (!entry || entry->format != MPV_FORMAT_NODE_MAP || !entry->u.list)
        return NULL;
    for (int i = 0; i < entry->u.list->num; i++)
        if (g_strcmp0(entry->u.list->keys[i], "time") == 0)
            return &entry->u.list->values[i];
    return NULL;
}

static gint compare_times(gconstpointer a, gconstpointer b)
{
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}

static GArray *mark_times_copy(const mpv_node *node)
{
    GArray *times = g_array_new(FALSE, FALSE, sizeof(double));
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return times;
    for (int i = 0; i < node->u.list->num; i++) {
        const mpv_node *time = mark_time_field(&node->u.list->values[i]);
        double seconds = time && time->format == MPV_FORMAT_DOUBLE ? time->u.double_
            : time && time->format == MPV_FORMAT_INT64 ? (double)time->u.int64 : NAN;
        if (isfinite(seconds) && seconds >= 0)
            g_array_append_val(times, seconds);
    }
    g_array_sort(times, compare_times);
    guint unique = 0;
    for (guint i = 0; i < times->len; i++) {
        double time = g_array_index(times, double, i);
        if (!unique || time != g_array_index(times, double, unique - 1))
            g_array_index(times, double, unique++) = time;
    }
    g_array_set_size(times, unique);
    return times;
}

static double track_offset(double travel, double fraction, gboolean inverted)
{
    /* GtkRange assigns the positive offset to an int before inversion.
     * Mirroring that order prevents a stationary mark drifting by one pixel
     * when we calibrate it against a newly allocated current slider. */
    double offset = floor(travel * CLAMP(fraction, 0, 1));
    return inverted ? travel - offset : offset;
}

static gboolean track_calibrate(double center, double trough_length, double slider_length,
                                 double lower, double upper, double page, double value,
                                 gboolean inverted, MarkTrack *track)
{
    double span = upper - lower - page;
    if (!isfinite(center) || !isfinite(trough_length) || !isfinite(slider_length) ||
        !isfinite(span) || !isfinite(value) || trough_length <= 0 || span <= 0)
        return FALSE;
    double travel = MAX(0, trough_length - CLAMP(slider_length, 0, trough_length));
    track->travel = travel;
    track->inverted = inverted;
    track->origin = center - track_offset(travel, (value - lower) / span, inverted);
    return TRUE;
}

static double mark_position(const MarkTrack *track, double fraction)
{
    return track->origin + track_offset(track->travel, fraction, track->inverted);
}

static GtkWidget *css_child(GtkWidget *parent, const char *name)
{
    for (GtkWidget *child = gtk_widget_get_first_child(parent); child;
         child = gtk_widget_get_next_sibling(child))
        if (g_str_equal(gtk_widget_get_css_name(child), name))
            return child;
    return NULL;
}

static gboolean get_track(ChapterMarks *p, MarkTrack *track, graphene_rect_t *clip)
{
    GtkWidget *scale = GTK_WIDGET(p->scale);
    GtkWidget *trough = css_child(scale, "trough");
    GtkWidget *slider = trough ? css_child(trough, "slider") : NULL;
    if (!trough || !slider)
        return FALSE;
    GdkRectangle range_rect;
    int start, end, slider_minimum;
    gtk_range_get_range_rect(p->scale, &range_rect);
    gtk_range_get_slider_range(p->scale, &start, &end);
    if (end <= start || range_rect.width <= 0 || range_rect.height <= 0)
        return FALSE;
    graphene_point_t origin;
    if (!gtk_widget_compute_point(scale, p->canvas, &GRAPHENE_POINT_INIT(0, 0), &origin))
        return FALSE;
    double center = origin.x + (start + end) / 2.0;
    /* The public int getter truncates fractional CSS margins. Preserve the
     * actual border-box center when GTK supplies its floating-point bounds. */
    graphene_rect_t thumb;
    if (gtk_widget_compute_bounds(slider, p->canvas, &thumb))
        center = thumb.origin.x + thumb.size.width / 2.0;
    gtk_widget_measure(slider, GTK_ORIENTATION_HORIZONTAL, -1,
                       &slider_minimum, NULL, NULL, NULL);
    double length = gtk_widget_get_width(trough);
    double lower = gtk_adjustment_get_lower(p->adjustment);
    double upper = gtk_adjustment_get_upper(p->adjustment);
    double page = gtk_adjustment_get_page_size(p->adjustment);
    double slider_length = slider_minimum;
    if (!gtk_range_get_slider_size_fixed(p->scale) && upper > lower)
        slider_length = MAX(slider_length, floor(length * page / (upper - lower)));
    gboolean inverted = gtk_range_get_inverted(p->scale) ^
        (gtk_range_get_flippable(p->scale) && gtk_widget_get_direction(scale) == GTK_TEXT_DIR_RTL);
    if (!track_calibrate(center, length, slider_length, lower, upper, page,
                          gtk_adjustment_get_value(p->adjustment), inverted, track))
        return FALSE;
    double height = MIN(4, range_rect.height);
    graphene_rect_init(clip, origin.x + range_rect.x,
                        origin.y + range_rect.y + (range_rect.height - height) / 2.0,
                        range_rect.width, height);
    return TRUE;
}

static void draw_marks(GtkDrawingArea *area, cairo_t *cr,
                        int width, int height, gpointer data)
{
    (void)area; (void)width; (void)height;
    ChapterMarks *p = data;
    if (p->duration <= 0 || !p->times->len)
        return;
    MarkTrack track;
    graphene_rect_t clip;
    if (!get_track(p, &track, &clip))
        return;
    GdkRGBA foreground;
    gtk_widget_get_color(GTK_WIDGET(p->scale), &foreground);
    GdkRGBA dark = {.red = foreground.red * .15, .green = foreground.green * .15,
        .blue = foreground.blue * .15, .alpha = foreground.alpha * .9};
    GdkRGBA light = {.red = foreground.red + (1 - foreground.red) * .75,
        .green = foreground.green + (1 - foreground.green) * .75,
        .blue = foreground.blue + (1 - foreground.blue) * .75, .alpha = foreground.alpha * .75};
    double pixel = 1.0 / MAX(1, gtk_widget_get_scale_factor(p->canvas));
    cairo_save(cr);
    cairo_rectangle(cr, clip.origin.x, clip.origin.y, clip.size.width, clip.size.height);
    cairo_clip(cr);
    double previous = NAN;
    for (guint i = 0; i < p->times->len; i++) {
        double time = g_array_index(p->times, double, i);
        if (time > p->duration)
            break;
        double x = round(mark_position(&track, time / p->duration) / pixel) * pixel;
        if (x == previous)
            continue;
        previous = x;
        gdk_cairo_set_source_rgba(cr, &dark);
        cairo_rectangle(cr, x - pixel, clip.origin.y, pixel, clip.size.height);
        cairo_fill(cr);
        gdk_cairo_set_source_rgba(cr, &light);
        cairo_rectangle(cr, x, clip.origin.y, pixel, clip.size.height);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static void marks_free(gpointer data)
{
    ChapterMarks *p = data;
    if (p->adjustment_handler)
        g_signal_handler_disconnect(p->adjustment, p->adjustment_handler);
    g_clear_object(&p->adjustment);
    g_clear_object(&p->scale);
    g_array_unref(p->times);
    g_free(p);
}

static void adjustment_changed(GtkAdjustment *adjustment, gpointer data)
{
    (void)adjustment;
    ChapterMarks *p = data;
    gtk_widget_queue_draw(p->canvas);
}

static void bind_adjustment(ChapterMarks *p)
{
    if (p->adjustment_handler)
        g_signal_handler_disconnect(p->adjustment, p->adjustment_handler);
    g_set_object(&p->adjustment, gtk_range_get_adjustment(p->scale));
    p->adjustment_handler = g_signal_connect(p->adjustment, "changed",
                                             G_CALLBACK(adjustment_changed), p);
}

static void range_notify(GObject *object, GParamSpec *property, gpointer data)
{
    (void)object;
    GtkWidget *canvas = data;
    ChapterMarks *p = g_object_get_data(G_OBJECT(canvas), "mpv-chapter-marks");
    if (g_str_equal(property->name, "adjustment"))
        bind_adjustment(p);
    gtk_widget_queue_draw(canvas);
}

static void direction_changed(GtkWidget *scale, GtkTextDirection previous, gpointer data)
{
    (void)scale; (void)previous;
    gtk_widget_queue_draw(data);
}

static void state_changed(GtkWidget *scale, GtkStateFlags previous, gpointer data)
{
    (void)scale; (void)previous;
    gtk_widget_queue_draw(data);
}

static void marks_changed(ChapterMarks *p)
{
    gtk_widget_set_visible(p->canvas, p->duration > 0 && p->times->len > 0);
    gtk_widget_queue_draw(p->canvas);
}

GtkWidget *chapter_marks_new(GtkRange *scale)
{
    g_return_val_if_fail(GTK_IS_RANGE(scale), NULL);
    g_return_val_if_fail(gtk_orientable_get_orientation(GTK_ORIENTABLE(scale)) ==
                        GTK_ORIENTATION_HORIZONTAL, NULL);
    g_return_val_if_fail(gtk_widget_get_parent(GTK_WIDGET(scale)) == NULL, NULL);
    GtkWidget *overlay = gtk_overlay_new();
    ChapterMarks *p = g_new0(ChapterMarks, 1);
    p->scale = g_object_ref(scale);
    p->times = g_array_new(FALSE, FALSE, sizeof(double));
    p->canvas = gtk_drawing_area_new();
    gtk_widget_set_can_target(p->canvas, FALSE);
    gtk_widget_set_focusable(p->canvas, FALSE);
    gtk_accessible_update_state(GTK_ACCESSIBLE(p->canvas), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
    gtk_widget_set_hexpand(overlay, gtk_widget_get_hexpand(GTK_WIDGET(scale)));
    gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(scale));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->canvas);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(overlay), p->canvas, FALSE);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay), p->canvas, TRUE);
    g_object_set_data_full(G_OBJECT(p->canvas), "mpv-chapter-marks", p, marks_free);
    g_object_set_data(G_OBJECT(overlay), "mpv-chapter-marks", p);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->canvas), draw_marks, p, NULL);
    bind_adjustment(p);
    g_signal_connect_object(scale, "notify", G_CALLBACK(range_notify), p->canvas, 0);
    g_signal_connect_object(scale, "direction-changed", G_CALLBACK(direction_changed), p->canvas, 0);
    g_signal_connect_object(scale, "state-flags-changed", G_CALLBACK(state_changed), p->canvas, 0);
    marks_changed(p);
    return overlay;
}

void chapter_marks_update(GtkWidget *overlay, const mpv_node *chapter_list)
{
    ChapterMarks *p = g_object_get_data(G_OBJECT(overlay), "mpv-chapter-marks");
    g_return_if_fail(p != NULL);
    GArray *times = mark_times_copy(chapter_list);
    if (times->len == p->times->len &&
        (!times->len || memcmp(times->data, p->times->data, times->len * sizeof(double)) == 0)) {
        g_array_unref(times);
        return;
    }
    g_array_unref(p->times);
    p->times = times;
    marks_changed(p);
}

void chapter_marks_set_duration(GtkWidget *overlay, double duration)
{
    ChapterMarks *p = g_object_get_data(G_OBJECT(overlay), "mpv-chapter-marks");
    g_return_if_fail(p != NULL);
    duration = isfinite(duration) && duration > 0 ? duration : 0;
    if (p->duration == duration)
        return;
    p->duration = duration;
    marks_changed(p);
}

void chapter_marks_reset(GtkWidget *overlay)
{
    ChapterMarks *p = g_object_get_data(G_OBJECT(overlay), "mpv-chapter-marks");
    g_return_if_fail(p != NULL);
    g_array_set_size(p->times, 0);
    p->duration = 0;
    marks_changed(p);
}
