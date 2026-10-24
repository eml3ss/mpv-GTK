#ifndef MPV_GTK_PREVIEW_DECODER_H
#define MPV_GTK_PREVIEW_DECODER_H

#include <glib.h>

typedef struct PreviewDecoder PreviewDecoder;
typedef struct {
    guint64 token;
    GBytes *pixels;
    int width, height, stride;
    double seconds;
    gboolean refined;
    const char *error;
} PreviewDecoderFrame;

typedef void (*PreviewDecoderCallback)(const PreviewDecoderFrame *frame, gpointer data);

/* Call on the creating main context. Pixels are packed, straight-alpha BGRA8;
 * seconds uses absolute source PTS (the decoder disables start-time rebasing).
 * Frames and error strings are borrowed for the callback; retain pixels with
 * g_bytes_ref() when needed. Cancel keeps the paused decoder warm briefly. */
PreviewDecoder *preview_decoder_new(PreviewDecoderCallback callback, gpointer data);
void preview_decoder_request(PreviewDecoder *decoder, const char *source_key,
                              const char *filename, gint64 ff_index,
                              double seconds, guint64 token);
void preview_decoder_cancel(PreviewDecoder *decoder);
void preview_decoder_set_enabled(PreviewDecoder *decoder, gboolean enabled);
void preview_decoder_free(PreviewDecoder *decoder);

#endif
