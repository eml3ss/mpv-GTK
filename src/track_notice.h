#ifndef MPV_GTK_TRACK_NOTICE_H
#define MPV_GTK_TRACK_NOTICE_H

#include <gtk/gtk.h>

/* Add a passive, initially hidden notice. The overlay owns the returned
 * widget; all calls belong on GTK's main thread. */
GtkWidget *track_notice_new(GtkOverlay *overlay);

/* Copy the text and restart the 800 ms display interval. */
void track_notice_show(GtkWidget *notice, const char *title, const char *detail);
void track_notice_clear(GtkWidget *notice);

#endif
