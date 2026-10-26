#ifndef MPV_GTK_MPRIS_ART_H
#define MPV_GTK_MPRIS_ART_H

#include <glib.h>
#include <mpv/client.h>

typedef struct MprisArt MprisArt;
typedef void (*MprisArtCallback)(const char *uri, gpointer data);

/* All public calls and callbacks use the creating main context. Callback
 * URIs are borrowed. Source sampling, decoding and image files are async.
 * Artwork fits inside 1024x1024 without enlarging smaller source images. */
MprisArt *mpris_art_new(mpv_handle *mpv, MprisArtCallback callback, gpointer data);
gboolean mpris_art_handle_event(MprisArt *art, const mpv_event *event);
void mpris_art_free(MprisArt *art);

#endif
