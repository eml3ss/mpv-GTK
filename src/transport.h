#ifndef MPV_GTK_TRANSPORT_H
#define MPV_GTK_TRANSPORT_H

#include <gtk/gtk.h>

typedef enum {
    TRANSPORT_PREVIOUS,
    TRANSPORT_PLAY,
    TRANSPORT_PAUSE,
    TRANSPORT_NEXT
} TransportGlyph;

GtkWidget *transport_icon_new(TransportGlyph glyph);
void transport_icon_set(GtkWidget *widget, TransportGlyph glyph);

/* Render the same vector geometry independently of GTK for visual previews. */
void transport_icon_draw(cairo_t *cr, TransportGlyph glyph,
                          double width, double height, const GdkRGBA *color);

#endif
