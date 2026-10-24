#ifndef MPV_GTK_PREFERENCES_H
#define MPV_GTK_PREFERENCES_H

#include <gtk/gtk.h>
#include <mpv/client.h>
#include "keys_page.h"

typedef struct {
    const char *hwdec;
    const char *hwdec_current;
    const char *language;
    /* API selection is auto/opengl/opengl-es, with OpenGL ES representing
     * gpu-api=opengl and opengl-es=yes. */
    const char *gpu_api;
    const char *audio_output;
    const char *audio_device;
    const char *autocreate_playlist;
    const char *directory_mode;
    const char *directory_filter_types;
    const char *video_exts;
    const char *audio_exts;
    const char *image_exts;
    const char *archive_exts;
    const char *playlist_exts;
    gboolean media_loaded;
    gboolean has_video;
    gboolean hwdec_known;
    gboolean hwdec_pending;
    /* The saved selection differs from the decoder mode of this engine. */
    gboolean hwdec_restart_required;
    gboolean keepaspect;
    gboolean deinterlace;
    gboolean deband;
    gboolean loop;
    gboolean allow_replace;
    gboolean add_to_queue;
    double audio_delay;
    double sub_delay;
    double sub_scale;
    double cursor_hide_delay;
} PreferenceValues;

/* Values have libmpv's usual representation: int for FLAG, double for
 * DOUBLE, and a pointer to const char * for STRING. They are valid only
 * during the callback; asynchronous libmpv setters copy them immediately.
 * GPU API emits a synchronous pair: gpu-api then opengl-es. The host stages
 * the first value and commits both options together upon the second callback. */
typedef void (*PreferencesChangeFunc)(const char *property, mpv_format format,
                                      const void *value, gpointer user_data);
typedef void (*PreferencesOpenConfigFunc)(const char *filename, gpointer user_data);
typedef void (*PreferencesKeysLoadFunc)(GtkWindow *window, gpointer user_data);
typedef void (*PreferencesAudioLoadFunc)(GtkWindow *window, gboolean active,
                                        gpointer user_data);
enum {
    PREFERENCES_PLAYLIST_BASIC = 1,
    PREFERENCES_PLAYLIST_EXTENSIONS = 2
};
typedef void (*PreferencesPlaylistLoadFunc)(GtkWindow *window, guint groups,
                                           gpointer user_data);

GtkWindow *preferences_window_new(GtkWindow *parent,
                                  const PreferenceValues *values,
                                  const char *config_path,
                                  PreferencesChangeFunc on_change,
                                  PreferencesOpenConfigFunc on_open_config,
                                  gpointer user_data);

/* Reflect observed engine state without issuing another write callback. */
void preferences_window_update(GtkWindow *window, const PreferenceValues *values);

/* Confirm pending numeric text before closing the settings or player. */
void preferences_window_commit(GtkWindow *window);

/* Keep language selection visible after the host recreates translated UI. */
void preferences_window_show_interface(GtkWindow *window);

/* Request the key-binding snapshots only when Keybinds is first opened. */
void preferences_window_set_keys_loader(GtkWindow *window,
                                        PreferencesKeysLoadFunc on_load,
                                        gpointer user_data);

/* Subscribe to audio options/device changes only while the Audio section is
 * selected in a visible window. Leaving, hiding and closing notify FALSE. */
void preferences_window_set_audio_loader(GtkWindow *window,
                                         PreferencesAudioLoadFunc on_load,
                                         gpointer user_data);
/* Copies the provided catalogues without changing saved selections. outputs
 * accepts stock option-info/ao/choices (ARRAY of STRING), or ARRAY of MAP with
 * name/description. devices uses stock audio-device-list (ARRAY of MAP).
 * NULL retains the previous catalogue; an empty ARRAY clears it. */
void preferences_window_update_audio_outputs(GtkWindow *window,
                                              const mpv_node *outputs,
                                              const mpv_node *devices);

/* Request basic playlist options only on a visible Playback page; also request
 * extension lists while its lazy expander is open. Leaving/hiding sends 0.
 * NULL snapshot strings mean unknown/loading and disable their controls;
 * empty strings are known, valid empty lists. */
void preferences_window_set_playlist_loader(GtkWindow *window,
                                            PreferencesPlaylistLoadFunc on_load,
                                            gpointer user_data);

/* Localized active-decoder state; the caller releases it with g_free(). */
char *preferences_decoder_status(const PreferenceValues *values);

void preferences_window_configure_keys(GtkWindow *window, const char *input_path,
                                       const KeyBindingDefault *defaults, gsize count,
                                       KeysPageChangeFunc on_change, gpointer user_data);
/* Snapshots received before Keybinds is first opened are ignored. */
void preferences_window_update_keys(GtkWindow *window, const mpv_node *bindings,
                                    const mpv_node *commands);
void preferences_window_keys_error(GtkWindow *window, const char *message);

#endif
