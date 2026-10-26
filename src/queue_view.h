#ifndef MPV_GTK_QUEUE_VIEW_H
#define MPV_GTK_QUEUE_VIEW_H

#include <gtk/gtk.h>
#include "queue_model.h"

typedef struct QueueView QueueView;

/* The model remains owned by the host. The view keeps a reference to its root
 * widget; unparent it before freeing the view. All calls use the main thread. */
QueueView *queue_view_new(QueueModel *model);
GtkWidget *queue_view_widget(QueueView *view);
GtkWidget *queue_view_list(QueueView *view);
GtkWidget *queue_view_count_label(QueueView *view);
void queue_view_refresh(QueueView *view);
void queue_view_translate(QueueView *view);
/* Let queue navigation and editing reach GTK instead of mpv keybindings. */
gboolean queue_view_wants_key(QueueView *view, GtkWidget *focus, guint key,
                              GdkModifierType modifiers);
void queue_view_free(QueueView *view);

#endif
