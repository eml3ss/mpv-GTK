#ifndef MPV_GTK_MPRIS_H
#define MPV_GTK_MPRIS_H

#include <glib.h>
#include <mpv/client.h>

typedef struct Mpris Mpris;
typedef struct {
    void (*raise)(gpointer data);
    void (*quit)(gpointer data);
    void (*set_fullscreen)(gboolean fullscreen, gpointer data);
    gboolean (*open_uri)(const char *uri, gpointer data);
    gpointer data;
} MprisHost;

/* Call on the GTK/main context after initial media presentation. Session-bus
 * setup and mpv property sampling are asynchronous. Lifecycle events are
 * observed without consuming them from the host. No session bus is required. */
Mpris *mpris_new(mpv_handle *mpv, const MprisHost *host);
gboolean mpris_handle_event(Mpris *mpris, const mpv_event *event);
const char *mpris_bus_name(const Mpris *mpris);
/* Publish already available artwork for the current media. The caller owns
 * extraction/source-generation checks; this call does not fetch the URI.
 * NULL clears artwork. Loading or ending a file clears it automatically. */
void mpris_set_art_url(Mpris *mpris, const char *uri);
void mpris_free(Mpris *mpris);

#endif
