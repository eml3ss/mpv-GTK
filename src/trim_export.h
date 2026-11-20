#ifndef MPV_GTK_TRIM_EXPORT_H
#define MPV_GTK_TRIM_EXPORT_H

#include <glib.h>
#include <mpv/client.h>

typedef struct TrimExport TrimExport;
typedef enum { TRIM_PRECISE_OFF, TRIM_PRECISE_CPU, TRIM_PRECISE_GPU } TrimPreciseMode;
typedef struct {
    const char *ffmpeg_path, *ffprobe_path;
    const char *expected_input; /* Optional mpv path guard for the selection's source. */
    gboolean replace, open_result;
    gint64 start_ms, end_ms;
    /* Source-wide ff-indices of internal audio streams to omit. The start
     * call validates and copies this array; NULL/zero preserves all audio. */
    const gint64 *excluded_audio;
    gsize excluded_audio_count;
    /* Keep only the audio streams, stream-copied into a container named by
     * audio_extension (for example ".opus"). The export is verified by
     * decoding an audio frame. A replacement export with a different
     * extension saves beside the original, then removes the original. */
    gboolean exclude_video;
    const char *audio_extension;
    /* Frame-accurate cuts: re-encode from start_ms to the first keyframe at
     * splice_ms (equal to start_ms when the start is a keyframe, end_ms when
     * no copyable middle remains) and stream-copy from there. -1 means that
     * keyframes are unknown, which exports from the keyframe and reports a
     * fallback. */
    TrimPreciseMode precise;
    gint64 splice_ms;
    /* Last keyframe before end_ms that ends the copied middle; the frames
     * after it are re-encoded. -1 copies to the end of the file. */
    gint64 end_splice_ms;
    gint64 video_index; /* Source-wide index of the video stream, or -1. */
} TrimExportRequest;

TrimExport *trim_export_new(mpv_handle *mpv);
gboolean trim_export_start(TrimExport *exporter, const TrimExportRequest *request, GError **error);
gboolean trim_export_handle_event(TrimExport *exporter, const mpv_event *event);
gboolean trim_export_busy(const TrimExport *exporter);
void trim_export_shutdown(TrimExport *exporter);
void trim_export_free(TrimExport *exporter);

#endif
