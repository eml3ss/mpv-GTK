#ifndef MPV_GTK_SEEK_PREVIEW_H
#define MPV_GTK_SEEK_PREVIEW_H

#include <gtk/gtk.h>
#include <mpv/client.h>

typedef struct SeekPreview SeekPreview;

SeekPreview *seek_preview_new(mpv_handle *mpv);
void seek_preview_attach(SeekPreview *preview, GtkWidget *timeline, gboolean is_range);
/* Capture pointer motion on a shared ancestor, including an invisible 16px
 * vertical margin around the timeline. The timeline's layout stays unchanged;
 * buttons, entries, other ranges, and popovers keep their normal behavior. */
void seek_preview_attach_area(SeekPreview *preview, GtkWidget *timeline, gboolean is_range,
                              GtkWidget *hover_area);
void seek_preview_set_enabled(SeekPreview *preview, gboolean enabled);
gboolean seek_preview_handle_event(SeekPreview *preview, const mpv_event *event);
void seek_preview_hide(SeekPreview *preview);
void seek_preview_shutdown(SeekPreview *preview);
void seek_preview_free(SeekPreview *preview);

#endif
