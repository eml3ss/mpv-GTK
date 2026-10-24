#ifndef MPV_GTK_TRIM_BACKEND_H
#define MPV_GTK_TRIM_BACKEND_H

#include <glib.h>
#include <mpv/client.h>

typedef struct TrimBackend TrimBackend;

TrimBackend *trim_backend_new(mpv_handle *mpv, const char *config_path);
void trim_backend_set_enabled(TrimBackend *backend, gboolean enabled);
void trim_backend_action(TrimBackend *backend, const char *action,
                         int argc, const char *const *argv);
gboolean trim_backend_handle_event(TrimBackend *backend, const mpv_event *event);
void trim_backend_shutdown(TrimBackend *backend);
void trim_backend_free(TrimBackend *backend);

#endif
