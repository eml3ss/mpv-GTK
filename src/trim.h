#ifndef MPV_GTK_TRIM_H
#define MPV_GTK_TRIM_H

#include <gtk/gtk.h>
#include <mpv/client.h>
#include "seek_preview.h"

typedef struct TrimMode TrimMode;
typedef struct {
    mpv_handle *mpv;
    GtkWindow *window;
    GtkBox *controls, *transport;
    GtkWidget *seek, *playback, *audio_time, *status;
    GtkLabel *time;
    GtkOverlay *video;
    const char *config_path;
    void (*seek_to)(double seconds, gpointer data);
    void (*message)(const char *text, gboolean error, gpointer data);
    gpointer data;
    SeekPreview *previews;
} TrimHost;

TrimMode *trim_mode_new(const TrimHost *host);
GtkWidget *trim_mode_button(TrimMode *trim);
void trim_mode_toggle(TrimMode *trim);
gboolean trim_mode_handle_event(TrimMode *trim, const mpv_event *event);
gboolean trim_mode_active(const TrimMode *trim);
gboolean trim_mode_audio_menu_open(const TrimMode *trim);
gboolean trim_mode_holding(const TrimMode *trim);
void trim_mode_position(TrimMode *trim, double seconds, gboolean paused);
void trim_mode_translate(TrimMode *trim);
void trim_mode_shutdown(TrimMode *trim);
void trim_mode_free(TrimMode *trim);

#endif
