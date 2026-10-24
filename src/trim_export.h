/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef MPV_GTK_TRIM_EXPORT_H
#define MPV_GTK_TRIM_EXPORT_H

#include <glib.h>
#include <mpv/client.h>

typedef struct TrimExport TrimExport;
typedef struct {
    const char *ffmpeg_path, *ffprobe_path;
    gboolean replace, open_result;
    gint64 start_ms, end_ms;
} TrimExportRequest;

TrimExport *trim_export_new(mpv_handle *mpv);
gboolean trim_export_start(TrimExport *exporter, const TrimExportRequest *request, GError **error);
gboolean trim_export_handle_event(TrimExport *exporter, const mpv_event *event);
gboolean trim_export_busy(const TrimExport *exporter);
void trim_export_shutdown(TrimExport *exporter);
void trim_export_free(TrimExport *exporter);

#endif
