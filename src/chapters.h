#ifndef MPV_GTK_CHAPTERS_H
#define MPV_GTK_CHAPTERS_H

#include <gtk/gtk.h>
#include <mpv/client.h>
#include <stdint.h>

typedef void (*ChaptersSelectFunc)(int64_t index, gpointer user_data);
typedef void (*ChaptersStepFunc)(int direction, gpointer user_data);

/* The hidden menu button becomes visible when chapter metadata is available.
 * Callbacks run only for user activation; the host owns the mpv commands. */
GtkWidget *chapters_button_new(ChaptersSelectFunc on_select,
                                ChaptersStepFunc on_step, gpointer user_data);

/* Copy the event-owned chapter-list. No mpv_node storage is retained.
 * Updating metadata or selection never invokes either callback. */
void chapters_button_update(GtkWidget *button, const mpv_node *chapter_list,
                            int64_t current);
void chapters_button_set_current(GtkWidget *button, int64_t current);
void chapters_button_reset(GtkWidget *button);

#endif
