#ifndef MPV_GTK_TRIM_EXPORT_H
#define MPV_GTK_TRIM_EXPORT_H

#include <glib.h>
#include <mpv/client.h>

typedef struct TrimExport TrimExport;
typedef struct {
    const char *ffmpeg_path, *ffprobe_path;
    const char *expected_input; /* Optional mpv path guard for the selection's source. */
    gboolean replace, open_result;
    gint64 start_ms, end_ms;
    /* Source-wide ff-indices of internal audio streams to omit. The start
     * call validates and copies this array; NULL/zero preserves all audio. */
    const gint64 *excluded_audio;
    gsize excluded_audio_count;
} TrimExportRequest;

TrimExport *trim_export_new(mpv_handle *mpv);
gboolean trim_export_start(TrimExport *exporter, const TrimExportRequest *request, GError **error);
gboolean trim_export_handle_event(TrimExport *exporter, const mpv_event *event);
gboolean trim_export_busy(const TrimExport *exporter);
void trim_export_shutdown(TrimExport *exporter);
void trim_export_free(TrimExport *exporter);

#endif
