#include "transport.h"

#define GLYPH_KEY "mpv-gtk-transport-glyph"

static void rounded_bar(cairo_t *cr, double x, double y,
                         double width, double height, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -G_PI / 2, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, G_PI / 2);
    cairo_arc(cr, x + radius, y + height - radius, radius, G_PI / 2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void play_triangle(cairo_t *cr)
{
    cairo_move_to(cr, 9.0, 5.15);
    cairo_curve_to(cr, 8.35, 4.74, 7.5, 5.21, 7.5, 6.0);
    cairo_line_to(cr, 7.5, 18.0);
    cairo_curve_to(cr, 7.5, 18.79, 8.35, 19.26, 9.0, 18.85);
    cairo_line_to(cr, 18.1, 13.16);
    cairo_curve_to(cr, 19.05, 12.57, 19.05, 11.43, 18.1, 10.84);
    cairo_close_path(cr);
}

static void next_glyph(cairo_t *cr)
{
    /* A single rounded triangle and a separate fine stem. Keep the shape
     * distinct from a double-triangle fast-forward command. */
    cairo_move_to(cr, 6.9, 5.9);
    cairo_curve_to(cr, 6.3, 5.53, 5.5, 5.95, 5.5, 6.65);
    cairo_line_to(cr, 5.5, 17.35);
    cairo_curve_to(cr, 5.5, 18.05, 6.3, 18.47, 6.9, 18.1);
    cairo_line_to(cr, 14.8, 13.15);
    cairo_curve_to(cr, 15.65, 12.62, 15.65, 11.38, 14.8, 10.85);
    cairo_close_path(cr);
    rounded_bar(cr, 17.0, 5.5, 2.25, 13.0, 1.0);
}

void transport_icon_draw(cairo_t *cr, TransportGlyph glyph,
                          double width, double height, const GdkRGBA *color)
{
    g_return_if_fail(cr != NULL && color != NULL);
    g_return_if_fail(glyph >= TRANSPORT_PREVIOUS && glyph <= TRANSPORT_NEXT);
    if (width <= 0 || height <= 0)
        return;
    double size = MIN(width, height);
    cairo_save(cr);
    cairo_translate(cr, (width - size) / 2, (height - size) / 2);
    cairo_scale(cr, size / 24.0, size / 24.0);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
    cairo_new_path(cr);
    switch (glyph) {
    case TRANSPORT_PREVIOUS:
        cairo_translate(cr, 24, 0);
        cairo_scale(cr, -1, 1);
        next_glyph(cr);
        break;
    case TRANSPORT_PLAY:
        play_triangle(cr);
        break;
    case TRANSPORT_PAUSE:
        rounded_bar(cr, 7.25, 5.5, 3.25, 13.0, .8);
        rounded_bar(cr, 13.5, 5.5, 3.25, 13.0, .8);
        break;
    case TRANSPORT_NEXT:
        next_glyph(cr);
        break;
    }
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_icon(GtkDrawingArea *area, cairo_t *cr,
                       int width, int height, gpointer data)
{
    (void)data;
    GtkWidget *widget = GTK_WIDGET(area);
    TransportGlyph glyph = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), GLYPH_KEY)) - 1;
    GdkRGBA color;
    gtk_widget_get_color(widget, &color);
    transport_icon_draw(cr, glyph, width, height, &color);
}

GtkWidget *transport_icon_new(TransportGlyph glyph)
{
    g_return_val_if_fail(glyph >= TRANSPORT_PREVIOUS && glyph <= TRANSPORT_NEXT, NULL);
    GtkWidget *widget = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widget), 24);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widget), 24);
    gtk_widget_set_halign(widget, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(widget, FALSE);
    gtk_widget_add_css_class(widget, "transport-icon");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(widget), draw_icon, NULL, NULL);
    transport_icon_set(widget, glyph);
    return widget;
}

void transport_icon_set(GtkWidget *widget, TransportGlyph glyph)
{
    g_return_if_fail(GTK_IS_DRAWING_AREA(widget));
    g_return_if_fail(glyph >= TRANSPORT_PREVIOUS && glyph <= TRANSPORT_NEXT);
    gpointer encoded = GINT_TO_POINTER(glyph + 1);
    if (g_object_get_data(G_OBJECT(widget), GLYPH_KEY) == encoded)
        return;
    g_object_set_data(G_OBJECT(widget), GLYPH_KEY, encoded);
    gtk_widget_queue_draw(widget);
}
