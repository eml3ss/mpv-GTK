#define _GNU_SOURCE
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#include <glib-unix.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <dlfcn.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "timeline.h"
#include "config.h"
#include "controls.h"
#include "preferences.h"
#include "transport.h"
#include "i18n.h"
#include "input.h"
#include "keybindings.h"
#include "key_commands.h"
#include "about.h"
#include "startup_trace.h"
#include "gl_api.h"
#include "chapters.h"
#include "chapter_marks.h"
#include "track_notice.h"
#include "trim.h"
#include "seek_preview.h"
#include "mpris.h"
#include "mpris_art.h"
#include "audio_scrub.h"

#define APP_ID "io.github.emless.mpvgtk"
#define APP_VERSION "0.21.6"
#define APP_ICON_NAME APP_ID
#define HWDEC_REFRESH_ID UINT64_C(10001)
#define HWDEC_SET_ID UINT64_C(10002)
#define TRACK_REFRESH_ID UINT64_C(10003)
#define PLAYLIST_REFRESH_ID UINT64_C(10004)
#define VIDEO_TRACK_REFRESH_ID UINT64_C(10005)
#define PLAYBACK_REFRESH_BASE UINT64_C(11000)
#define INPUT_BINDINGS_REFRESH_ID UINT64_C(10006)
#define COMMAND_LIST_REFRESH_ID UINT64_C(10007)
#define INPUT_RELOAD_CLEAR_ID UINT64_C(10008)
#define INPUT_RELOAD_FILE_ID UINT64_C(10009)
#define INPUT_EDIT_DEFINE_ID UINT64_C(10010)
#define INPUT_EDIT_ENABLE_ID UINT64_C(10011)
#define RENDER_LOAD_HOOK_ID UINT64_C(10012)
#define AUDIO_OUTPUTS_REFRESH_ID UINT64_C(10013)
#define CHAPTERS_REFRESH_BASE (UINT64_C(1) << 32)
#define CHAPTER_CURRENT_REFRESH_BASE (UINT64_C(1) << 33)
#define INITIAL_SUBTITLE_REFRESH_BASE (UINT64_C(1) << 34)
#define TRACK_NOTICE_REFRESH_BASE (UINT64_C(1) << 35)
#define PREFERENCES_OBSERVE_BASE UINT64_C(20000)
#define DECODER_OBSERVE_BASE UINT64_C(20100)
#define PLAYLIST_OBSERVE_ID UINT64_C(20200)
#define AUDIO_OBSERVE_BASE UINT64_C(20300)
#define PLAYLIST_PREFS_OBSERVE_BASE UINT64_C(20400)

static const KeyBindingDefault app_key_defaults[] = {
    {"SPACE", "cycle pause", "Пауза / воспроизведение"},
    {"k", "cycle pause", "Пауза / воспроизведение"},
    {"PLAY", "cycle pause", "Пауза / воспроизведение"},
    {"PAUSE", "set pause yes", "Пауза"},
    {"STOP", "stop keep-playlist", "Остановить воспроизведение"},
    {"PREV", "playlist-prev", "Предыдущий файл"},
    {"NEXT", "playlist-next", "Следующий файл"},
    {"f", "cycle fullscreen", "Полный экран"},
    {"F11", "cycle fullscreen", "Полный экран"},
    {"ESC", "script-message mpv-gtk escape", "Закрыть информацию или выйти из полного экрана"},
    {"LEFT", "seek -5", "Назад на 5 секунд"},
    {"RIGHT", "seek 5", "Вперёд на 5 секунд"},
    {"Shift+LEFT", "seek -30", "Назад на 30 секунд"},
    {"Shift+RIGHT", "seek 30", "Вперёд на 30 секунд"},
    {"UP", "add volume 5", "Увеличить громкость"},
    {"DOWN", "add volume -5", "Уменьшить громкость"},
    {"m", "cycle mute", "Выключить / включить звук"},
    {",", "frame-back-step", "Предыдущий кадр"},
    {".", "frame-step", "Следующий кадр"},
    {"PGUP", "playlist-prev", "Предыдущий файл"},
    {"PGDWN", "playlist-next", "Следующий файл"},
    {"Ctrl+PGUP", "script-message mpv-gtk chapter-previous", "Предыдущая глава"},
    {"Ctrl+PGDWN", "script-message mpv-gtk chapter-next", "Следующая глава"},
    {"i", "script-message mpv-gtk info", "Информация о воспроизведении"},
    {"Ctrl+o", "script-message mpv-gtk open-file", "Открыть файл"},
    {"Ctrl+l", "script-message mpv-gtk open-url", "Открыть ссылку"},
    {"Ctrl+q", "quit", "Закрыть проигрыватель"},
    {"Ctrl+p", "script-message mpv-gtk queue", "Показать / скрыть очередь"},
    {"Ctrl+t", "script-message mpv-gtk trim", "Режим обрезки"},
    {"Ctrl+,", "script-message mpv-gtk settings", "Настройки"},
    {"TAB", "script-message mpv-gtk focus-next", "Следующий элемент интерфейса"},
    {"Shift+TAB", "script-message mpv-gtk focus-previous", "Предыдущий элемент интерфейса"},
    {"ENTER", "script-message mpv-gtk activate", "Нажать выбранную кнопку"},
    {"MENU", "script-message mpv-gtk context-menu", "Контекстное меню выбранной кнопки"},
    {"Shift+F10", "script-message mpv-gtk context-menu", "Контекстное меню выбранной кнопки"},
    {"MBTN_LEFT", "script-message mpv-gtk focus-video", "Выбрать область видео"},
    {"MBTN_RIGHT", "cycle pause", "Пауза / воспроизведение"},
    {"MBTN_LEFT_DBL", "cycle fullscreen", "Полный экран"}
};

typedef struct { const char *name; mpv_format format; } ObservedProperty;
static const ObservedProperty playback_properties[] = {
    {"video-codec", MPV_FORMAT_STRING}, {"video-format", MPV_FORMAT_STRING},
    {"video-params/w", MPV_FORMAT_INT64}, {"video-params/h", MPV_FORMAT_INT64},
    {"container-fps", MPV_FORMAT_DOUBLE}, {"audio-codec-name", MPV_FORMAT_STRING},
    {"audio-params/samplerate", MPV_FORMAT_INT64},
    {"audio-params/hr-channels", MPV_FORMAT_STRING},
    {"frame-drop-count", MPV_FORMAT_INT64}, {"decoder-frame-drop-count", MPV_FORMAT_INT64},
    {"avsync", MPV_FORMAT_DOUBLE}, {"demuxer-cache-duration", MPV_FORMAT_DOUBLE}
};

static const ObservedProperty preferences_properties[] = {
    {"keepaspect", MPV_FORMAT_FLAG},
    {"deinterlace", MPV_FORMAT_FLAG}, {"deband", MPV_FORMAT_FLAG},
    {"loop-file", MPV_FORMAT_STRING}, {"audio-delay", MPV_FORMAT_DOUBLE},
    {"sub-delay", MPV_FORMAT_DOUBLE}, {"sub-scale", MPV_FORMAT_DOUBLE}
};

static const ObservedProperty decoder_properties[] = {
    {"hwdec", MPV_FORMAT_STRING}, {"hwdec-current", MPV_FORMAT_STRING},
    {"vid", MPV_FORMAT_STRING}
};

static const ObservedProperty audio_properties[] = {
    {"ao", MPV_FORMAT_STRING}, {"audio-device", MPV_FORMAT_STRING},
    {"audio-device-list", MPV_FORMAT_NODE}
};

static const ObservedProperty playlist_preference_properties[] = {
    {"autocreate-playlist", MPV_FORMAT_STRING},
    {"directory-mode", MPV_FORMAT_STRING},
    {"directory-filter-types", MPV_FORMAT_STRING},
    {"video-exts", MPV_FORMAT_STRING}, {"audio-exts", MPV_FORMAT_STRING},
    {"image-exts", MPV_FORMAT_STRING}, {"archive-exts", MPV_FORMAT_STRING},
    {"playlist-exts", MPV_FORMAT_STRING}
};

static const gsize playlist_preference_offsets[] = {
    G_STRUCT_OFFSET(PreferenceValues, autocreate_playlist),
    G_STRUCT_OFFSET(PreferenceValues, directory_mode),
    G_STRUCT_OFFSET(PreferenceValues, directory_filter_types),
    G_STRUCT_OFFSET(PreferenceValues, video_exts),
    G_STRUCT_OFFSET(PreferenceValues, audio_exts),
    G_STRUCT_OFFSET(PreferenceValues, image_exts),
    G_STRUCT_OFFSET(PreferenceValues, archive_exts),
    G_STRUCT_OFFSET(PreferenceValues, playlist_exts)
};
G_STATIC_ASSERT(G_N_ELEMENTS(playlist_preference_offsets) ==
                G_N_ELEMENTS(playlist_preference_properties));

typedef struct {
    char *video_codec, *video_format, *audio_codec, *channels;
    int64_t width, height, sample_rate, dropped, decoder_dropped;
    double fps, avsync, cache;
    gboolean avsync_known, drops_known, decoder_drops_known;
} PlaybackValues;

typedef struct {
    GtkWidget *title, *video, *decoder, *context, *audio, *drops, *sync, *cache, *cache_row;
} PlaybackInfoView;

typedef struct {
    char *actual, *observed;
    uint64_t request;
    gboolean priming;
} TrackNoticeWatch;

typedef struct {
    GtkApplication *application;
    GtkWindow *window;
    GtkGLArea *area;
    GtkWidget *video_widget;
    gboolean default_gsk_renderer;
    const char *active_vo, *gl_api, *pending_gpu_api;
    GtkWidget *header, *controls, *empty, *error_bar, *error_label;
    GtkWidget *title, *time_label, *play_button, *play_icon, *mute_button, *root;
    TrimMode *trim;
    SeekPreview *seek_preview;
    Mpris *mpris;
    MprisArt *mpris_art;
    AudioScrub *audio_scrub;
    guint scrub_release_source;
    guint mpris_source;
    GtkWidget *previous_button, *next_button, *chapter_button;
    GtkWidget *seek, *seek_overlay, *volume, *playlist, *sidebar, *stage, *video_overlay;
    GtkWidget *info_overlay;
    GtkWidget *track_notice;
    GtkWidget *controls_revealer, *audio_context, *subtitle_button, *speed_button;
    GtkWidget *queue_label, *status_label;
    GtkWidget *app_menu_button;
    GtkFileDialog *chooser;
    GCancellable *file_cancel;
    GtkWindow *url_dialog, *preferences, *info_window, *about_window;
    GCancellable *config_launch_cancel;
    GMenu *audio_menu, *subtitle_menu, *speed_menu, *app_menu;
    PlayerConfig config;
    PreferenceValues preference_values;
    char *hwdec, *hwdec_current, *pending_hwdec;
    gboolean cuda_interop;
    char *audio_output, *audio_device;
    char *initial_subtitle;
    char *playlist_values[G_N_ELEMENTS(playlist_preference_properties)];
    ControlsVisibility controls_state;
    CursorVisibility cursor_state;
    PlaybackValues playback;
    PlaybackInfoView popup_info, overlay_info;
    mpv_handle *mpv;
    mpv_render_context *renderer;
    uint64_t render_load_hook;
    void *gl_library;
    int wake_fd;
    guint wake_source, test_source, language_source, cursor_source;
    gboolean engine_started, engine_loading;
    gboolean startup_frame_rendered;
    gboolean startup_waiting_for_media, startup_presented;
    gboolean quit_requested, shutdown_received, close_prepared;
    gboolean closing, updating, dragging, seek_settling;
    gboolean scrub_pending;
    gboolean keys_reload_pending, keys_loaded;
    gboolean preferences_observed, decoder_observed, playlist_observed, info_dirty;
    gboolean audio_observed;
    gboolean playlist_preferences_observed, playlist_extensions_observed;
    gboolean media_loading;
    gboolean initial_subtitle_reset_pending;
    guint initial_subtitle_pending_cycles;
    gboolean muted, ready, fullscreen, idle, pointer_bottom, pointer_present, controls_pressed;
    gboolean cursor_hidden;
    gboolean loaded_once, test_paused, test_seek_sent, test_resumed, seek_ok;
    gboolean test_finished, render_failed;
    double preview, volume_value, test_seconds, max_render_ms, max_tick_gap_ms;
    double play_seconds, paused_seconds;
    gint64 test_started, test_launched, last_tick, settle_until, last_scrub_us;
    gint64 hwdec_refresh_at, info_refresh_at;
    gint64 input_refresh_at, cursor_deadline_us;
    guint64 ui_ticks, play_ticks, paused_ticks, render_calls, frame_updates;
    guint error_count;
    gint64 playlist_position, playlist_count;
    int64_t chapter_count, current_chapter;
    int64_t pending_chapter;
    gboolean chapter_pending;
    uint64_t chapter_request, chapter_current_request, chapter_serial;
    uint64_t initial_subtitle_request, initial_subtitle_serial;
    TrackNoticeWatch track_notices[2]; /* Primary subtitles, then audio. */
    uint64_t track_notice_serial;
    int last_time_second, pointer_height;
    GPtrArray *pending;
    GQueue pending_commands;
    gboolean settings_requested;
    Timeline timeline;
} Player;

static gboolean open_paths(Player *p, GPtrArray *paths);
static void flush_pending_paths(Player *p);
static void show_files(Player *p);
static void show_url(Player *p);
static void update_transport(Player *p);
static void update_track_menu(Player *p, mpv_node *node);
static void set_fullscreen(Player *p, gboolean fullscreen);
static void update_controls_visibility(Player *p, gint64 now);
static void show_preferences(Player *p);
static void show_about(Player *p);
static void update_app_menu(Player *p);
static void open_config(const char *filename, gpointer data);
static void show_playback_info(Player *p);
static void update_playback_info(Player *p);
static void clear_playback_values(PlaybackValues *values);
static void update_metadata_observers(Player *p);
static void toggle_playback_info(Player *p);
static gboolean change_key_binding(const char *old_key, const char *key,
                                   const char *command_text, gboolean reset,
                                   GError **error, gpointer data);
static void app_key_action(Player *p, const char *action);
static gboolean install_key_edits(Player *p, GError **error);
static void continue_render_load(Player *p);
static void prepare_close(Player *p);
static void discard_render_frame(Player *p);
static void clear_track_notices(Player *p);
static void hide_track_notice(Player *p);
static void request_track_notice(Player *p, gboolean audio, gboolean silent);
static gboolean start_mpris_idle(gpointer data);

static GtkWidget *player_video_widget(Player *p)
{
    return p->video_widget ? p->video_widget : GTK_WIDGET(p->area);
}

static gboolean startup_should_present(const Player *p)
{
    return !p->closing && !p->quit_requested && !p->startup_presented && p->engine_started &&
           !p->engine_loading && (!p->startup_waiting_for_media ||
           p->timeline.loaded ||
           p->render_failed || p->error_count > 0);
}

static void present_startup_window(Player *p)
{
    if (!p->window || !startup_should_present(p))
        return;
    p->startup_presented = TRUE;
    startup_trace_mark("window presentation requested");
    gtk_window_present(p->window);
    p->mpris_source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, start_mpris_idle, p, NULL);
}

static gboolean playback_info_visible(Player *p)
{
    return (p->info_window && gtk_widget_get_visible(GTK_WIDGET(p->info_window))) ||
           (p->info_overlay && gtk_widget_get_visible(p->info_overlay));
}

static void observe_group(Player *p, const ObservedProperty *properties, gsize count,
                           uint64_t base, gboolean enabled, gboolean *active)
{
    if (*active == enabled || !p->mpv || p->closing || p->quit_requested)
        return;
    for (gsize i = 0; i < count; ++i) {
        if (enabled)
            mpv_observe_property(p->mpv, base + i, properties[i].name, properties[i].format);
        else
            mpv_unobserve_property(p->mpv, base + i);
    }
    *active = enabled;
}

static void update_metadata_observers(Player *p)
{
    gboolean settings = p->preferences &&
        gtk_widget_get_visible(GTK_WIDGET(p->preferences));
    gboolean decoder = settings || playback_info_visible(p);
    observe_group(p, preferences_properties, G_N_ELEMENTS(preferences_properties),
                  PREFERENCES_OBSERVE_BASE, settings, &p->preferences_observed);
    observe_group(p, decoder_properties, G_N_ELEMENTS(decoder_properties),
                  DECODER_OBSERVE_BASE, decoder, &p->decoder_observed);
    if (!decoder)
        p->hwdec_refresh_at = 0;
    if (!playback_info_visible(p)) {
        clear_playback_values(&p->playback);
        p->info_dirty = FALSE;
    }
}

static void optional_view_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)object;
    (void)spec;
    update_metadata_observers(data);
}

static void translated_tooltip(GtkWidget *widget, const char *key)
{
    g_object_set_data(G_OBJECT(widget), "mpv-tooltip-key", (gpointer)key);
    gtk_widget_set_tooltip_text(widget, tr(key));
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
        GTK_ACCESSIBLE_PROPERTY_LABEL, tr(key), -1);
}

static GtkWidget *translated_label(const char *key)
{
    GtkWidget *widget = gtk_label_new(tr(key));
    g_object_set_data(G_OBJECT(widget), "mpv-label-key", (gpointer)key);
    return widget;
}

static GtkWidget *translated_button(const char *key)
{
    GtkWidget *widget = gtk_button_new_with_label(tr(key));
    g_object_set_data(G_OBJECT(widget), "mpv-button-key", (gpointer)key);
    return widget;
}

static void translate_tree(GtkWidget *widget)
{
    const char *key = g_object_get_data(G_OBJECT(widget), "mpv-label-key");
    if (key && GTK_IS_LABEL(widget))
        gtk_label_set_text(GTK_LABEL(widget), tr(key));
    key = g_object_get_data(G_OBJECT(widget), "mpv-button-key");
    if (key && GTK_IS_BUTTON(widget))
        gtk_button_set_label(GTK_BUTTON(widget), tr(key));
    key = g_object_get_data(G_OBJECT(widget), "mpv-tooltip-key");
    if (key)
        translated_tooltip(widget, key);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child))
        translate_tree(child);
}

static void request_hwdec_state(Player *p)
{
    if (!p->mpv || p->closing || p->quit_requested)
        return;
    mpv_get_property_async(p->mpv, HWDEC_REFRESH_ID, "hwdec-current", MPV_FORMAT_STRING);
    mpv_get_property_async(p->mpv, VIDEO_TRACK_REFRESH_ID, "vid", MPV_FORMAT_STRING);
    p->hwdec_refresh_at = 0;
}

static void request_playback_values(Player *p)
{
    if (!p->mpv || p->closing || p->quit_requested)
        return;
    for (guint i = 0; i < G_N_ELEMENTS(playback_properties); ++i)
        mpv_get_property_async(p->mpv, PLAYBACK_REFRESH_BASE + i,
            playback_properties[i].name, playback_properties[i].format);
}

static void clear_playback_values(PlaybackValues *values)
{
    g_free(values->video_codec);
    g_free(values->video_format);
    g_free(values->audio_codec);
    g_free(values->channels);
    *values = (PlaybackValues){0};
}

static char *media_location(GFile *file)
{
    char *path = g_file_get_path(file);
    return path ? path : g_file_get_uri(file);
}

static void show_error(Player *p, const char *message)
{
    p->error_count++;
    g_printerr("mpv-gtk: %s\n", message);
    if (!p->closing && !p->quit_requested && p->error_label) {
        gtk_label_set_text(GTK_LABEL(p->error_label), message);
        gtk_widget_set_visible(p->error_bar, TRUE);
        present_startup_window(p);
    }
}

/* These are the ONLY functions called by libmpv worker threads. eventfd
 * coalesces notifications and never enters GTK/GL/libmpv recursively. */
static void wake_main(void *data)
{
    Player *p = data;
    uint64_t one = 1;
    ssize_t result;
    do {
        result = write(p->wake_fd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
}

static void command(Player *p, const char *args[])
{
    if (p->closing || p->quit_requested)
        return;
    if (!p->mpv) {
        g_queue_push_tail(&p->pending_commands, g_strdupv((char **)args));
        return;
    }
    int result = mpv_command_async(p->mpv, 0, args);
    if (result < 0)
        show_error(p, mpv_error_string(result));
}

static void clear_chapters(Player *p)
{
    p->chapter_count = 0;
    p->current_chapter = -1;
    p->chapter_pending = FALSE;
    p->chapter_request = 0;
    p->chapter_current_request = 0;
    if (p->chapter_button)
        chapters_button_reset(p->chapter_button);
    if (p->seek_overlay)
        chapter_marks_reset(p->seek_overlay);
}

static void request_chapters(Player *p)
{
    if (!p->mpv || p->closing || p->quit_requested || !p->timeline.loaded)
        return;
    /* Ignore replies from a previous file or superseded metadata refresh. */
    p->chapter_request = CHAPTERS_REFRESH_BASE + ++p->chapter_serial;
    int result = mpv_get_property_async(p->mpv, p->chapter_request,
                                       "chapter-list", MPV_FORMAT_NODE);
    if (result < 0)
        p->chapter_request = 0;
    p->chapter_current_request = CHAPTER_CURRENT_REFRESH_BASE + p->chapter_serial;
    result = mpv_get_property_async(p->mpv, p->chapter_current_request,
                                    "chapter", MPV_FORMAT_INT64);
    if (result < 0)
        p->chapter_current_request = 0;
}

static void accept_chapters(Player *p, const mpv_event_property *property)
{
    if (!p->timeline.loaded || !property || property->format != MPV_FORMAT_NODE ||
        !property->data)
        return;
    const mpv_node *chapters = property->data;
    if (chapters->format != MPV_FORMAT_NODE_ARRAY || !chapters->u.list)
        return;
    p->chapter_count = chapters->u.list->num;
    if (p->chapter_count == 0) {
        clear_chapters(p);
        return;
    }
    if (p->chapter_button)
        chapters_button_update(p->chapter_button, chapters, p->current_chapter);
    if (p->seek_overlay) {
        chapter_marks_update(p->seek_overlay, chapters);
        chapter_marks_set_duration(p->seek_overlay, p->timeline.duration);
    }
}

static void select_chapter(int64_t index, gpointer data)
{
    Player *p = data;
    if (!p->mpv || p->closing || p->quit_requested || !p->timeline.loaded ||
        index < 0 || index >= p->chapter_count)
        return;
    int result = mpv_set_property_async(p->mpv, 0, "chapter", MPV_FORMAT_INT64, &index);
    if (result < 0) {
        show_error(p, mpv_error_string(result));
    } else {
        p->pending_chapter = index;
        p->chapter_pending = TRUE;
        p->chapter_current_request = 0;
    }
}

static void step_chapter(int direction, gpointer data)
{
    Player *p = data;
    int64_t current = p->chapter_pending ? p->pending_chapter : p->current_chapter;
    if (!p->timeline.loaded || p->chapter_count <= 0 ||
        (direction != -1 && direction != 1) ||
        (direction < 0 && current < 0) ||
        (direction > 0 && current >= p->chapter_count - 1))
        return;
    /* mpv can advance the playlist when stepping beyond the last chapter.
     * The chapter controls stop at the file's boundaries instead. */
    if (direction > 0) {
        /* Absolute bounded targets also keep a burst of right-clicks safe
         * while libmpv's chapter observations are still in flight. */
        select_chapter(current + 1, p);
    } else {
        p->chapter_pending = FALSE;
        const char *args[] = {"add", "chapter", "-1", NULL};
        command(p, args);
    }
}

static void chapter_secondary_click(GtkGestureClick *gesture, int presses,
                                     double x, double y, gpointer data)
{
    (void)presses; (void)x; (void)y;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    step_chapter(1, data);
}

static void set_flag(Player *p, const char *name, gboolean value)
{
    if (!p->mpv) {
        const char *args[] = {"set", name, value ? "yes" : "no", NULL};
        command(p, args);
        return;
    }
    int flag = !!value;
    int result = mpv_set_property_async(p->mpv, 0, name, MPV_FORMAT_FLAG, &flag);
    if (result < 0)
        show_error(p, mpv_error_string(result));
}

static void set_number(Player *p, const char *name, double value)
{
    if (!p->mpv) {
        char number[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(number, sizeof(number), value);
        const char *args[] = {"set", name, number, NULL};
        command(p, args);
        return;
    }
    int result = mpv_set_property_async(p->mpv, 0, name, MPV_FORMAT_DOUBLE, &value);
    if (result < 0)
        show_error(p, mpv_error_string(result));
}

static void set_string(Player *p, const char *name, const char *value)
{
    if (!p->mpv) {
        const char *args[] = {"set", name, value, NULL};
        command(p, args);
        return;
    }
    const char *copy = value;
    int result = mpv_set_property_async(p->mpv, 0, name, MPV_FORMAT_STRING, &copy);
    if (result < 0)
        show_error(p, mpv_error_string(result));
}

static void clear_initial_subtitle(Player *p)
{
    g_clear_pointer(&p->initial_subtitle, g_free);
    p->initial_subtitle_request = 0;
    p->initial_subtitle_reset_pending = FALSE;
    p->initial_subtitle_pending_cycles = 0;
}

static void request_initial_subtitle(Player *p)
{
    clear_initial_subtitle(p);
    if (!p->mpv || p->closing || p->quit_requested || !p->timeline.loaded)
        return;
    /* Track selection is complete at FILE_LOADED. Queue this read before
     * subsequent button commands and reject late replies from older files. */
    p->initial_subtitle_request = INITIAL_SUBTITLE_REFRESH_BASE + ++p->initial_subtitle_serial;
    if (mpv_get_property_async(p->mpv, p->initial_subtitle_request,
                              "sid", MPV_FORMAT_STRING) < 0)
        p->initial_subtitle_request = 0;
}

static void reset_initial_subtitle(Player *p)
{
    if (p->closing || p->quit_requested || !p->timeline.loaded)
        return;
    hide_track_notice(p);
    if (p->initial_subtitle) {
        set_string(p, "sid", p->initial_subtitle);
        request_track_notice(p, FALSE, TRUE);
    } else if (p->initial_subtitle_request) {
        p->initial_subtitle_reset_pending = TRUE;
        p->initial_subtitle_pending_cycles = 0;
    }
}

static void quick_control_action(Player *p, gboolean speed, guint button)
{
    if (p->closing || p->quit_requested)
        return;
    if (speed && button == GDK_BUTTON_MIDDLE) {
        set_number(p, "speed", 1.0);
    } else if (speed && button == GDK_BUTTON_SECONDARY) {
        const char *args[] = {"add", "speed", "0.25", NULL};
        command(p, args);
    } else if (!speed && button == GDK_BUTTON_MIDDLE) {
        reset_initial_subtitle(p);
    } else if (!speed && button == GDK_BUTTON_SECONDARY && p->timeline.loaded) {
        /* Preserve click order if a reset is waiting for the initial SID. */
        if (p->initial_subtitle_reset_pending) {
            ++p->initial_subtitle_pending_cycles;
            return;
        }
        const char *args[] = {"cycle", "sid", NULL};
        command(p, args);
        request_track_notice(p, FALSE, FALSE);
    }
}

static void quick_audio_action(Player *p, guint button)
{
    if (p->closing || p->quit_requested)
        return;
    if (button == GDK_BUTTON_MIDDLE) {
        if (p->audio_context)
            gtk_popover_bin_popup(GTK_POPOVER_BIN(p->audio_context));
    } else if (button == GDK_BUTTON_SECONDARY && p->timeline.loaded) {
        const char *args[] = {"cycle", "aid", NULL};
        command(p, args);
        request_track_notice(p, TRUE, FALSE);
    }
}

static gboolean quick_control_contains(GtkWidget *widget, double x, double y)
{
    return x >= 0 && y >= 0 && x < gtk_widget_get_width(widget) &&
           y < gtk_widget_get_height(widget);
}

static void quick_control_pressed(GtkGestureClick *gesture, int presses,
                                   double x, double y, gpointer data)
{
    (void)presses; (void)data;
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (quick_control_contains(widget, x, y))
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void quick_control_released(GtkGestureClick *gesture, int presses,
                                    double x, double y, gpointer data)
{
    (void)presses;
    Player *p = data;
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!quick_control_contains(widget, x, y) ||
        gtk_gesture_get_sequence_state(GTK_GESTURE(gesture), NULL) != GTK_EVENT_SEQUENCE_CLAIMED)
        return;
    guint button = gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(gesture));
    if (widget == p->audio_context)
        quick_audio_action(p, button);
    else
        quick_control_action(p, widget == p->speed_button, button);
}

static void add_quick_control_gestures(GtkWidget *widget, Player *p)
{
    const guint buttons[] = {GDK_BUTTON_MIDDLE, GDK_BUTTON_SECONDARY};
    for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i) {
        GtkGesture *gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), buttons[i]);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
        g_signal_connect(gesture, "pressed", G_CALLBACK(quick_control_pressed), p);
        g_signal_connect(gesture, "released", G_CALLBACK(quick_control_released), p);
        gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));
    }
}

static void simple_command(Player *p, const char *name)
{
    const char *args[] = {name, NULL};
    command(p, args);
}

static void cycle_pause(Player *p)
{
    if (p->timeline.loaded) {
        const char *args[] = {"cycle", "pause", NULL};
        command(p, args);
    }
}

static void seek_absolute(Player *p, double seconds)
{
    if (!p->timeline.loaded || p->timeline.duration <= 0)
        return;
    char number[G_ASCII_DTOSTR_BUF_SIZE];
    p->preview = CLAMP(seconds, 0, p->timeline.duration);
    g_ascii_dtostr(number, sizeof(number), p->preview);
    const char *args[] = {"seek", number, "absolute+exact", NULL};
    command(p, args);
    p->seek_settling = TRUE;
    p->settle_until = g_get_monotonic_time() + 1500000;
}

static void scrub_cancel(Player *p)
{
    if (p->scrub_release_source) { g_source_remove(p->scrub_release_source); p->scrub_release_source = 0; }
    p->dragging = p->scrub_pending = FALSE;
    if (p->audio_scrub) audio_scrub_cancel(p->audio_scrub);
}

static void scrub_begin(Player *p)
{
    if (p->closing || p->quit_requested || !p->mpv || !p->timeline.loaded || p->timeline.duration <= 0) return;
    if (p->scrub_release_source) { g_source_remove(p->scrub_release_source); p->scrub_release_source = 0; }
    p->dragging = TRUE;
    p->seek_settling = p->scrub_pending = FALSE;
    p->last_scrub_us = 0;
    if (!p->audio_scrub) p->audio_scrub = audio_scrub_new(p->mpv);
    audio_scrub_begin(p->audio_scrub);
}

static void scrub_end(Player *p, double position)
{
    if (p->scrub_release_source) { g_source_remove(p->scrub_release_source); p->scrub_release_source = 0; }
    p->dragging = p->scrub_pending = FALSE;
    if (p->closing || p->quit_requested || !p->timeline.loaded || p->timeline.duration <= 0) {
        if (p->audio_scrub) audio_scrub_cancel(p->audio_scrub);
        return;
    }
    p->preview = CLAMP(position, 0, p->timeline.duration);
    if (p->audio_scrub && audio_scrub_active(p->audio_scrub)) {
        audio_scrub_end(p->audio_scrub, p->preview);
        p->seek_settling = TRUE;
        p->settle_until = g_get_monotonic_time() + 1500000;
    } else seek_absolute(p, p->preview);
}

static void scrub_flush(Player *p, gint64 now)
{
    if (!p->scrub_pending || !p->dragging || p->closing || p->quit_requested ||
        !p->timeline.loaded || p->timeline.duration <= 0)
        return;
    if (p->last_scrub_us && now - p->last_scrub_us < 33333)
        return;
    p->scrub_pending = FALSE;
    p->last_scrub_us = now;
    seek_absolute(p, p->preview);
}

static void scrub_queue(Player *p, double position, gint64 now)
{
    if (!p->timeline.loaded || p->timeline.duration <= 0)
        return;
    p->preview = CLAMP(position, 0, p->timeline.duration);
    p->scrub_pending = TRUE;
    /* Send the first movement immediately and coalesce subsequent motion
     * to the latest target, avoiding a queue of obsolete exact seeks. */
    scrub_flush(p, now);
}

static char *format_time(double seconds)
{
    gint64 n = isfinite(seconds) ? (gint64)fmax(0, seconds) : 0;
    if (n >= 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT
                               ":%02" G_GINT64_FORMAT, n / 3600, n / 60 % 60, n % 60);
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, n / 60, n % 60);
}

static void update_time(Player *p, double position)
{
    if (trim_mode_active(p->trim)) {
        trim_mode_position(p->trim, position, p->timeline.paused);
        return;
    }
    int second = (int)fmax(0, position);
    if (second == p->last_time_second)
        return;
    p->last_time_second = second;
    g_autofree char *current = format_time(position);
    g_autofree char *duration = p->timeline.duration > 0
                            ? format_time(p->timeline.duration) : g_strdup("--:--");
    g_autofree char *text = g_strdup_printf("%s / %s", current, duration);
    gtk_label_set_text(GTK_LABEL(p->time_label), text);
}

/* GTK's frame clock drives the progress bar, independently of mpv frames.
 * Live scrubbing submits only asynchronous, coalesced seek commands. */
static gboolean tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget;
    Player *p = data;
    if (p->closing || p->quit_requested)
        return G_SOURCE_REMOVE;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    gint64 monotonic = g_get_monotonic_time();
    update_controls_visibility(p, monotonic);
    scrub_flush(p, monotonic);
    if (p->hwdec_refresh_at && monotonic >= p->hwdec_refresh_at)
        request_hwdec_state(p);
    if (p->input_refresh_at && monotonic >= p->input_refresh_at) {
        p->input_refresh_at = 0;
        if (p->preferences && p->keys_loaded)
            mpv_get_property_async(p->mpv, INPUT_BINDINGS_REFRESH_ID, "input-bindings", MPV_FORMAT_NODE);
    }
    if (playback_info_visible(p)) {
        if (monotonic >= p->info_refresh_at) {
            request_playback_values(p);
            p->info_refresh_at = monotonic + 1000000;
        }
        if (p->info_dirty) {
            update_playback_info(p);
            p->info_dirty = FALSE;
        }
    }
    if (p->test_started) {
        p->ui_ticks++;
        if (p->timeline.paused)
            p->paused_ticks++;
        else
            p->play_ticks++;
        if (p->last_tick && now > p->last_tick) {
            double dt = (now - p->last_tick) / 1000000.0;
            p->max_tick_gap_ms = fmax(p->max_tick_gap_ms, dt * 1000);
            if (p->timeline.paused)
                p->paused_seconds += dt;
            else
                p->play_seconds += dt;
        }
        p->last_tick = now;
    }
    if (p->seek_settling && g_get_monotonic_time() > p->settle_until)
        p->seek_settling = FALSE;
    double position = (p->dragging || p->seek_settling) ? p->preview
                         : timeline_position(&p->timeline, now);
    if (!p->dragging && !p->seek_settling) {
        p->updating = TRUE;
        gtk_range_set_value(GTK_RANGE(p->seek), position);
        p->updating = FALSE;
    }
    update_time(p, position);
    return G_SOURCE_CONTINUE;
}

static const mpv_node *node_field(const mpv_node *node, const char *key)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP)
        return NULL;
    for (int i = 0; i < node->u.list->num; ++i)
        if (g_str_equal(node->u.list->keys[i], key))
            return &node->u.list->values[i];
    return NULL;
}

static const char *node_string(const mpv_node *node, const char *key)
{
    const mpv_node *field = node_field(node, key);
    return field && field->format == MPV_FORMAT_STRING ? field->u.string : "";
}

static gint64 node_int(const mpv_node *node, const char *key, gint64 fallback)
{
    const mpv_node *field = node_field(node, key);
    return field && field->format == MPV_FORMAT_INT64 ? field->u.int64 : fallback;
}

static void playlist_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;
    Player *p = data;
    if (!p->mpv || p->closing || p->quit_requested)
        return;
    gint64 position = gtk_list_box_row_get_index(row);
    int result = mpv_set_property_async(p->mpv, 0, "playlist-pos", MPV_FORMAT_INT64,
                                        &position);
    if (result < 0)
        show_error(p, mpv_error_string(result));
    set_flag(p, "pause", FALSE);
}

static void update_playlist(Player *p, mpv_node *node)
{
    if (!p->playlist || !p->playlist_observed)
        return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(p->playlist)))
        gtk_list_box_remove(GTK_LIST_BOX(p->playlist), child);
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY)
        return;
    int count = node->u.list->num;
    for (int i = 0; i < count; ++i) {
        const mpv_node *entry = &node->u.list->values[i];
        const char *title = node_string(entry, "title");
        g_autofree char *fallback = NULL;
        if (!*title) {
            g_autoptr(GFile) file = g_file_new_for_commandline_arg(node_string(entry, "filename"));
            fallback = g_file_get_basename(file);
            title = fallback ? fallback : tr("Медиафайл");
        }
        g_autofree char *text = g_strdup_printf("%02d  %s", i + 1, title);
        GtkWidget *label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_tooltip_text(label, title);
        gtk_widget_set_margin_top(label, 10);
        gtk_widget_set_margin_bottom(label, 10);
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        gtk_list_box_append(GTK_LIST_BOX(p->playlist), label);
    }
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->playlist),
                                                     (int)p->playlist_position);
    if (row)
        gtk_list_box_select_row(GTK_LIST_BOX(p->playlist), row);
    g_autofree char *label = g_strdup_printf(tr("Очередь · %d"), count);
    gtk_label_set_text(GTK_LABEL(p->queue_label), label);
}

static void queue_reveal_changed(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)object;
    (void)spec;
    Player *p = data;
    gboolean visible = gtk_revealer_get_reveal_child(GTK_REVEALER(p->sidebar));
    if (visible && !p->playlist) {
        GtkWidget *sidebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(sidebox, "queue");
        gtk_widget_set_size_request(sidebox, 260, -1);
        p->queue_label = translated_label("Очередь · 0");
        g_autofree char *count = g_strdup_printf(tr("Очередь · %d"), (int)p->playlist_count);
        gtk_label_set_text(GTK_LABEL(p->queue_label), count);
        gtk_widget_add_css_class(p->queue_label, "heading");
        gtk_widget_set_margin_top(p->queue_label, 16);
        gtk_widget_set_margin_bottom(p->queue_label, 16);
        gtk_box_append(GTK_BOX(sidebox), p->queue_label);
        GtkWidget *scroller = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(scroller, TRUE);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                        GTK_POLICY_AUTOMATIC);
        p->playlist = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->playlist), GTK_SELECTION_SINGLE);
        g_signal_connect(p->playlist, "row-activated", G_CALLBACK(playlist_activated), p);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), p->playlist);
        gtk_box_append(GTK_BOX(sidebox), scroller);
        gtk_revealer_set_child(GTK_REVEALER(p->sidebar), sidebox);
    }
    const ObservedProperty property = {"playlist", MPV_FORMAT_NODE};
    observe_group(p, &property, 1, PLAYLIST_OBSERVE_ID, visible, &p->playlist_observed);
    if (!visible && !gtk_revealer_get_child_revealed(GTK_REVEALER(p->sidebar)) && p->playlist) {
        p->playlist = NULL;
        p->queue_label = NULL;
        gtk_revealer_set_child(GTK_REVEALER(p->sidebar), NULL);
    }
}

static void add_target_item(GMenu *menu, const char *label, const char *action,
                            const char *target)
{
    g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target(item, action, "s", target);
    g_menu_append_item(menu, item);
}

static void update_track_menu(Player *p, mpv_node *node)
{
    g_menu_remove_all(p->audio_menu);
    g_menu_remove_all(p->subtitle_menu);
    add_target_item(p->audio_menu, tr("Автоматически"), "app.audio", "auto");
    add_target_item(p->audio_menu, tr("Без звука"), "app.audio", "no");
    add_target_item(p->subtitle_menu, tr("Выключены"), "app.subtitle", "no");
    if (node && node->format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node->u.list->num; ++i) {
            const mpv_node *track = &node->u.list->values[i];
            const char *type = node_string(track, "type");
            GMenu *menu = g_str_equal(type, "audio") ? p->audio_menu
                         : g_str_equal(type, "sub") ? p->subtitle_menu : NULL;
            if (!menu)
                continue;
            gint64 id = node_int(track, "id", 0);
            g_autofree char *target = g_strdup_printf("%" G_GINT64_FORMAT, id);
            const char *title = node_string(track, "title");
            const char *lang = node_string(track, "lang");
            const char *codec = node_string(track, "codec");
            g_autofree char *label = g_strdup_printf("%" G_GINT64_FORMAT " · %s%s%s%s%s",
                id, *title ? title : (*lang ? lang : tr("Дорожка")),
                *title && *lang ? " · " : "", *title && *lang ? lang : "",
                *codec ? " · " : "", codec);
            add_target_item(menu, label, g_str_equal(type, "audio")
                            ? "app.audio" : "app.subtitle", target);
        }
    }
}

static char *track_notice_text(Player *p, gboolean audio, const char *id)
{
    if (g_strcmp0(id, "no") == 0)
        return g_strdup(tr(audio ? "Без звука" : "Выключены"));
    if (g_strcmp0(id, "auto") == 0)
        return g_strdup(tr("Автоматически"));
    GMenu *menu = audio ? p->audio_menu : p->subtitle_menu;
    GMenuModel *model = menu ? G_MENU_MODEL(menu) : NULL;
    for (int i = 0; model && i < g_menu_model_get_n_items(model); ++i) {
        g_autoptr(GVariant) target = g_menu_model_get_item_attribute_value(
            model, i, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE_STRING);
        if (target && g_strcmp0(id, g_variant_get_string(target, NULL)) == 0) {
            char *label = NULL;
            if (g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label))
                return label;
        }
    }
    return g_strdup_printf(tr("Дорожка %s"), id ? id : "—");
}

static void hide_track_notice(Player *p)
{
    if (p->track_notice)
        track_notice_clear(p->track_notice);
}

static void clear_track_notices(Player *p)
{
    for (guint i = 0; i < G_N_ELEMENTS(p->track_notices); ++i) {
        TrackNoticeWatch *watch = &p->track_notices[i];
        g_clear_pointer(&watch->actual, g_free);
        g_clear_pointer(&watch->observed, g_free);
        watch->request = 0;
        watch->priming = FALSE;
    }
    hide_track_notice(p);
}

static void request_track_notice(Player *p, gboolean audio, gboolean silent)
{
    if (!p->mpv || p->closing || p->quit_requested || !p->timeline.loaded)
        return;
    TrackNoticeWatch *watch = &p->track_notices[!!audio];
    /* Initial load and explicit menu/reset setters establish a quiet
     * baseline. Observations during that read are verified afterwards. */
    if (silent) {
        watch->priming = TRUE;
        g_clear_pointer(&watch->observed, g_free);
    } else if (watch->priming) {
        return;
    }
    watch->request = TRACK_NOTICE_REFRESH_BASE + ++p->track_notice_serial;
    if (mpv_get_property_async(p->mpv, watch->request,
                              audio ? "aid" : "sid", MPV_FORMAT_STRING) < 0) {
        watch->request = 0;
        watch->priming = FALSE;
    }
}

static void position_track_notice(Player *p)
{
    if (!p->track_notice)
        return;
    int panel_height = 0;
    if (p->fullscreen && p->controls) {
        /* Keep the notice above the full panel, including during autohide,
         * without reserving any space in the video itself. */
        panel_height = gtk_widget_get_height(p->controls);
        if (panel_height <= 0)
            gtk_widget_measure(p->controls, GTK_ORIENTATION_VERTICAL,
                gtk_widget_get_width(p->video_overlay), NULL, &panel_height, NULL, NULL);
    }
    gtk_widget_set_margin_bottom(p->track_notice, 14 + panel_height);
}

static void show_track_notice(Player *p, gboolean audio, const char *id)
{
    if (!p->video_overlay || p->closing || p->quit_requested || !p->timeline.loaded || !id)
        return;
    if (!p->track_notice) {
        p->track_notice = track_notice_new(GTK_OVERLAY(p->video_overlay));
        g_object_add_weak_pointer(G_OBJECT(p->track_notice), (gpointer *)&p->track_notice);
    }
    position_track_notice(p);
    g_autofree char *text = track_notice_text(p, audio, id);
    track_notice_show(p->track_notice, tr(audio ? "Звук" : "Субтитры"), text);
}

static void observe_track_notice(Player *p, gboolean audio, const char *id)
{
    if (!p->timeline.loaded || p->closing || p->quit_requested || !id)
        return;
    TrackNoticeWatch *watch = &p->track_notices[!!audio];
    g_free(watch->observed);
    watch->observed = g_strdup(id);
    if (watch->priming)
        return;
    if (!watch->actual)
        request_track_notice(p, audio, TRUE);
    else if (watch->request || g_strcmp0(watch->actual, id) != 0)
        request_track_notice(p, audio, FALSE);
}

static gboolean accept_track_notice(Player *p, mpv_event *event)
{
    for (guint i = 0; i < G_N_ELEMENTS(p->track_notices); ++i) {
        TrackNoticeWatch *watch = &p->track_notices[i];
        if (!watch->request || event->reply_userdata != watch->request)
            continue;
        watch->request = 0;
        gboolean silent = watch->priming;
        watch->priming = FALSE;
        g_autofree char *observed = g_steal_pointer(&watch->observed);
        mpv_event_property *property = event->data;
        if (p->closing || p->quit_requested || !p->timeline.loaded || event->error < 0 || !property ||
            property->format != MPV_FORMAT_STRING || !property->data || !*(char **)property->data)
            return TRUE;
        g_autofree char *previous = g_steal_pointer(&watch->actual);
        watch->actual = g_strdup(*(char **)property->data);
        if (!silent && previous && g_strcmp0(previous, watch->actual) != 0)
            show_track_notice(p, i == 1, watch->actual);
        /* A coalesced observation can precede its initial/silent read's
         * reply. Verify the current value rather than announcing that hint. */
        if (observed && g_strcmp0(observed, watch->actual) != 0)
            request_track_notice(p, i == 1, FALSE);
        return TRUE;
    }
    return FALSE;
}

static void action_state(Player *p, const char *action, const char *state)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(p->application), action);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_string(state));
}

static void update_playlist_navigation(Player *p)
{
    gboolean visible = p->playlist_count > 1;
    if (p->previous_button)
        gtk_widget_set_visible(p->previous_button, visible);
    if (p->next_button)
        gtk_widget_set_visible(p->next_button, visible);
}

static void update_transport(Player *p)
{
    if (p->quit_requested)
        return;
    update_playlist_navigation(p);
    transport_icon_set(p->play_icon, p->timeline.paused || !p->timeline.loaded
                        ? TRANSPORT_PLAY : TRANSPORT_PAUSE);
    gtk_widget_set_tooltip_text(p->play_button,
        tr(p->timeline.paused ? "Продолжить · Пробел" : "Пауза · Пробел"));
    gtk_button_set_icon_name(GTK_BUTTON(p->mute_button),
        p->muted || p->volume_value == 0 ? "audio-volume-muted-symbolic"
                                        : "audio-volume-high-symbolic");
    gtk_widget_set_sensitive(p->play_button, p->timeline.loaded);
    gtk_widget_set_sensitive(p->seek, p->timeline.loaded && p->timeline.duration > 0);
    gtk_widget_set_visible(p->empty, p->idle);
    const char *status = p->timeline.buffering ? tr("Буферизация…") : "";
    gtk_label_set_text(GTK_LABEL(p->status_label), status);
}

static gboolean playback_property_changed(Player *p, mpv_event_property *property)
{
    const char *name = property->name;
    void *data = property->data;
    gboolean available = data && property->format != MPV_FORMAT_NONE;
    char **text = NULL;
    int64_t *integer = NULL;
    double *number = NULL;
    if (g_str_equal(name, "video-codec")) text = &p->playback.video_codec;
    else if (g_str_equal(name, "video-format")) text = &p->playback.video_format;
    else if (g_str_equal(name, "audio-codec-name")) text = &p->playback.audio_codec;
    else if (g_str_equal(name, "audio-params/hr-channels")) text = &p->playback.channels;
    else if (g_str_equal(name, "video-params/w")) integer = &p->playback.width;
    else if (g_str_equal(name, "video-params/h")) integer = &p->playback.height;
    else if (g_str_equal(name, "audio-params/samplerate")) integer = &p->playback.sample_rate;
    else if (g_str_equal(name, "frame-drop-count")) {
        integer = &p->playback.dropped;
        p->playback.drops_known = available;
    } else if (g_str_equal(name, "decoder-frame-drop-count")) {
        integer = &p->playback.decoder_dropped;
        p->playback.decoder_drops_known = available;
    } else if (g_str_equal(name, "container-fps")) number = &p->playback.fps;
    else if (g_str_equal(name, "avsync")) {
        number = &p->playback.avsync;
        p->playback.avsync_known = available;
    } else if (g_str_equal(name, "demuxer-cache-duration")) number = &p->playback.cache;
    else return FALSE;
    if (text) {
        g_free(*text);
        *text = available ? g_strdup(*(char **)data) : NULL;
    } else if (integer) {
        *integer = available ? *(int64_t *)data : 0;
    } else if (number) {
        *number = available && isfinite(*(double *)data) ? *(double *)data : 0;
    }
    return TRUE;
}

static void cache_playlist_preference(Player *p, guint index, const char *value)
{
    char *copy = g_strdup(value);
    g_free(p->playlist_values[index]);
    p->playlist_values[index] = copy;
    const char **slot = G_STRUCT_MEMBER_P(&p->preference_values,
                                         playlist_preference_offsets[index]);
    *slot = copy;
    if (p->preferences)
        preferences_window_update(p->preferences, &p->preference_values);
}

static void property_changed(Player *p, mpv_event_property *property)
{
    const char *name = property->name;
    void *value = property->data;
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < G_N_ELEMENTS(playlist_preference_properties); ++i) {
        if (g_str_equal(name, playlist_preference_properties[i].name)) {
            cache_playlist_preference(p, i, value && property->format == MPV_FORMAT_STRING
                ? *(const char *const *)value : NULL);
            return;
        }
    }
    if (g_str_equal(name, "chapter")) {
        p->chapter_pending = FALSE;
        p->current_chapter = value && property->format == MPV_FORMAT_INT64
            ? *(int64_t *)value : -1;
        if (p->chapter_button)
            chapters_button_set_current(p->chapter_button, p->current_chapter);
        return;
    }
    if (g_str_equal(name, "chapters")) {
        p->chapter_count = value && property->format == MPV_FORMAT_INT64
            ? MAX(INT64_C(0), *(int64_t *)value) : 0;
        if (p->chapter_count == 0)
            clear_chapters(p);
        else
            request_chapters(p);
        return;
    }
    if (playback_property_changed(p, property))
        return;
    gboolean preference_property = g_str_has_prefix(name, "hwdec") ||
        g_str_equal(name, "keepaspect") || g_str_equal(name, "deinterlace") ||
        g_str_equal(name, "deband") || g_str_equal(name, "loop-file") ||
        g_str_equal(name, "audio-delay") || g_str_equal(name, "sub-delay") ||
        g_str_equal(name, "sub-scale") || g_str_equal(name, "vid") ||
        g_str_equal(name, "ao") || g_str_equal(name, "audio-device");
    if (!value || property->format == MPV_FORMAT_NONE) {
        if (g_str_equal(name, "duration")) {
            p->timeline.duration = 0;
            if (p->seek_overlay)
                chapter_marks_set_duration(p->seek_overlay, 0);
            p->last_time_second = -1;
            update_transport(p);
        }
        if (g_str_equal(name, "hwdec-current")) {
            g_free(p->hwdec_current);
            p->hwdec_current = NULL;
            p->preference_values.hwdec_current = p->hwdec_current;
            p->preference_values.hwdec_known = FALSE;
            if (p->preferences)
                preferences_window_update(p->preferences, &p->preference_values);
        }
        if (g_str_equal(name, "vid"))
            p->preference_values.has_video = FALSE;
        return;
    }
    if (g_str_equal(name, "audio-device-list")) {
        if (p->preferences && p->audio_observed && property->format == MPV_FORMAT_NODE)
            preferences_window_update_audio_outputs(p->preferences, NULL, value);
    } else if (g_str_equal(name, "ao")) {
        g_free(p->audio_output);
        const char *output = *(char **)value;
        p->audio_output = g_strdup(*output ? output : "auto");
        p->preference_values.audio_output = p->audio_output;
    } else if (g_str_equal(name, "audio-device")) {
        g_free(p->audio_device);
        p->audio_device = g_strdup(*(char **)value);
        p->preference_values.audio_device = p->audio_device;
    } else if (g_str_equal(name, "time-pos")) {
        double position = *(double *)value;
        if (!isfinite(position))
            return;
        p->timeline.position = position;
        p->timeline.sampled_at = now;
        if (p->seek_settling && fabs(position - p->preview) < 0.75) {
            p->seek_settling = FALSE;
            if (p->test_seek_sent)
                p->seek_ok = TRUE;
        }
    } else if (g_str_equal(name, "duration")) {
        double duration = *(double *)value;
        p->timeline.duration = isfinite(duration) ? fmax(0, duration) : 0;
        if (p->seek_overlay)
            chapter_marks_set_duration(p->seek_overlay, p->timeline.duration);
        p->updating = TRUE;
        gtk_range_set_range(GTK_RANGE(p->seek), 0, fmax(1, p->timeline.duration));
        p->updating = FALSE;
        p->last_time_second = -1;
        update_transport(p);
    } else if (g_str_equal(name, "pause")) {
        timeline_rebase(&p->timeline, now);
        p->timeline.paused = !!*(int *)value;
        update_transport(p);
    } else if (g_str_equal(name, "paused-for-cache")) {
        timeline_rebase(&p->timeline, now);
        p->timeline.buffering = !!*(int *)value;
        update_transport(p);
    } else if (g_str_equal(name, "seeking")) {
        timeline_rebase(&p->timeline, now);
        p->timeline.seeking = !!*(int *)value;
    } else if (g_str_equal(name, "speed")) {
        timeline_rebase(&p->timeline, now);
        p->timeline.speed = *(double *)value;
        char state[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(state, sizeof(state), p->timeline.speed);
        action_state(p, "speed", state);
        if (p->speed_button) {
            g_autofree char *label = g_strdup_printf("%s×", state);
            gtk_label_set_text(GTK_LABEL(gtk_menu_button_get_child(
                GTK_MENU_BUTTON(p->speed_button))), label);
            if (fabs(p->timeline.speed - 1.0) > 1e-6)
                gtk_widget_add_css_class(p->speed_button, "active-track");
            else
                gtk_widget_remove_css_class(p->speed_button, "active-track");
        }
    } else if (g_str_equal(name, "volume")) {
        p->volume_value = *(double *)value;
        p->updating = TRUE;
        gtk_range_set_value(GTK_RANGE(p->volume), p->volume_value);
        p->updating = FALSE;
        update_transport(p);
    } else if (g_str_equal(name, "mute")) {
        p->muted = !!*(int *)value;
        update_transport(p);
    } else if (g_str_equal(name, "fullscreen")) {
        gboolean fullscreen = !!*(int *)value;
        if (p->window && fullscreen != p->fullscreen)
            set_fullscreen(p, fullscreen);
    } else if (g_str_equal(name, "idle-active")) {
        p->idle = !!*(int *)value;
        if (p->area)
            gtk_gl_area_queue_render(p->area);
        update_transport(p);
    } else if (g_str_equal(name, "media-title")) {
        const char *title = *(char **)value;
        gtk_label_set_text(GTK_LABEL(p->title), title);
        gtk_window_set_title(p->window, title);
    } else if (g_str_equal(name, "playlist")) {
        update_playlist(p, value);
    } else if (g_str_equal(name, "track-list")) {
        update_track_menu(p, value);
    } else if (g_str_equal(name, "playlist-pos")) {
        p->playlist_position = *(int64_t *)value;
        GtkListBoxRow *row = p->playlist ? gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(p->playlist), (int)p->playlist_position) : NULL;
        if (row)
            gtk_list_box_select_row(GTK_LIST_BOX(p->playlist), row);
    } else if (g_str_equal(name, "playlist-count")) {
        p->playlist_count = *(int64_t *)value;
        update_playlist_navigation(p);
    } else if (g_str_equal(name, "aid") || g_str_equal(name, "sid")) {
        observe_track_notice(p, g_str_equal(name, "aid"), *(char **)value);
        action_state(p, g_str_equal(name, "aid") ? "audio" : "subtitle", *(char **)value);
        if (g_str_equal(name, "sid") && p->subtitle_button) {
            if (g_str_equal(*(char **)value, "no"))
                gtk_widget_remove_css_class(p->subtitle_button, "active-track");
            else
                gtk_widget_add_css_class(p->subtitle_button, "active-track");
        }
    } else if (g_str_equal(name, "hwdec")) {
        g_free(p->hwdec);
        p->hwdec = g_strdup(*(char **)value);
        p->preference_values.hwdec = p->pending_hwdec ? p->pending_hwdec : p->hwdec;
        p->preference_values.hwdec_restart_required = p->pending_hwdec != NULL;
    } else if (g_str_equal(name, "hwdec-current")) {
        g_free(p->hwdec_current);
        p->hwdec_current = g_strdup(*(char **)value);
        p->preference_values.hwdec_current = p->hwdec_current;
        p->preference_values.hwdec_known = TRUE;
        p->preference_values.hwdec_pending = FALSE;
    } else if (g_str_equal(name, "vid")) {
        const char *video = *(char **)value;
        p->preference_values.has_video = *video && !g_str_equal(video, "no") &&
                                         !g_str_equal(video, "0");
    } else if (g_str_equal(name, "keepaspect")) {
        p->preference_values.keepaspect = !!*(int *)value;
    } else if (g_str_equal(name, "deinterlace")) {
        p->preference_values.deinterlace = !!*(int *)value;
    } else if (g_str_equal(name, "deband")) {
        p->preference_values.deband = !!*(int *)value;
    } else if (g_str_equal(name, "loop-file")) {
        const char *loop = *(char **)value;
        p->preference_values.loop = !g_str_equal(loop, "no") && !g_str_equal(loop, "0");
    } else if (g_str_equal(name, "audio-delay")) {
        p->preference_values.audio_delay = *(double *)value;
    } else if (g_str_equal(name, "sub-delay")) {
        p->preference_values.sub_delay = *(double *)value;
    } else if (g_str_equal(name, "sub-scale")) {
        p->preference_values.sub_scale = *(double *)value;
    }
    if (p->preferences && preference_property)
        preferences_window_update(p->preferences, &p->preference_values);
    if (preference_property)
        p->info_dirty = TRUE;
}

static void handle_event(Player *p, mpv_event *event)
{
    if (audio_scrub_handle_event(p->audio_scrub, event)) return;
    /* Once hidden, only drain lifecycle events and the render queue. Late
     * snapshots/command replies must not reopen UI or schedule more work. */
    if (p->quit_requested && event->event_id != MPV_EVENT_HOOK &&
        event->event_id != MPV_EVENT_END_FILE && event->event_id != MPV_EVENT_SHUTDOWN)
        return;
    if (mpris_handle_event(p->mpris, event)) return;
    if (mpris_art_handle_event(p->mpris_art, event)) return;
    if (seek_preview_handle_event(p->seek_preview, event)) return;
    gboolean trimming = trim_mode_active(p->trim);
    if (trim_mode_handle_event(p->trim, event)) {
        if (trimming != trim_mode_active(p->trim)) p->last_time_second = -1;
        return;
    }
    switch (event->event_id) {
    case MPV_EVENT_HOOK: {
        mpv_event_hook *hook = event->data;
        if (event->reply_userdata == RENDER_LOAD_HOOK_ID) {
            startup_trace_mark("media preloaded");
            p->render_load_hook = hook->id;
            continue_render_load(p);
        } else {
            mpv_hook_continue(p->mpv, hook->id);
        }
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE:
        property_changed(p, event->data);
        break;
    case MPV_EVENT_START_FILE:
        p->media_loading = TRUE;
        p->idle = FALSE;
        p->timeline.loaded = FALSE;
        p->preference_values.media_loaded = FALSE;
        p->preference_values.has_video = FALSE;
        p->preference_values.hwdec_known = FALSE;
        p->preference_values.hwdec_pending = FALSE;
        clear_playback_values(&p->playback);
        clear_chapters(p);
        clear_initial_subtitle(p);
        clear_track_notices(p);
        p->info_dirty = TRUE;
        p->timeline.position = 0;
        p->timeline.duration = 0;
        p->seek_settling = p->dragging = FALSE;
        p->scrub_pending = FALSE;
        p->last_scrub_us = 0;
        p->last_time_second = -1;
        if (p->area)
            gtk_gl_area_queue_render(p->area);
        gtk_widget_set_visible(p->error_bar, FALSE);
        update_transport(p);
        if (p->preferences)
            preferences_window_update(p->preferences, &p->preference_values);
        break;
    case MPV_EVENT_FILE_LOADED:
        p->media_loading = FALSE;
        p->idle = FALSE;
        p->timeline.loaded = TRUE;
        startup_trace_mark("media loaded");
        request_initial_subtitle(p);
        request_track_notice(p, FALSE, TRUE);
        request_track_notice(p, TRUE, TRUE);
        p->preference_values.media_loaded = TRUE;
        request_chapters(p);
        if (p->decoder_observed)
            p->hwdec_refresh_at = g_get_monotonic_time() + 200000;
        if (playback_info_visible(p))
            request_playback_values(p);
        p->loaded_once = TRUE;
        p->timeline.sampled_at = g_get_monotonic_time();
        if (p->test_seconds > 0 && !p->test_started)
            p->test_started = g_get_monotonic_time();
        if (p->area)
            gtk_gl_area_queue_render(p->area);
        update_transport(p);
        if (p->preferences)
            preferences_window_update(p->preferences, &p->preference_values);
        present_startup_window(p);
        break;
    case MPV_EVENT_END_FILE: {
        p->media_loading = FALSE;
        mpv_event_end_file *end = event->data;
        if (end->reason == MPV_END_FILE_REASON_QUIT) {
            startup_trace_mark("mpv quit playback ended");
            prepare_close(p);
            discard_render_frame(p);
            break;
        }
        if (p->quit_requested) {
            discard_render_frame(p);
            break;
        }
        p->timeline.loaded = FALSE;
        clear_chapters(p);
        clear_initial_subtitle(p);
        clear_track_notices(p);
        p->preference_values.media_loaded = FALSE;
        p->preference_values.hwdec_known = FALSE;
        p->preference_values.hwdec_pending = FALSE;
        p->info_dirty = TRUE;
        p->timeline.position = 0;
        p->timeline.sampled_at = 0;
        if (p->area)
            gtk_gl_area_queue_render(p->area);
        if (end->reason == MPV_END_FILE_REASON_ERROR)
            show_error(p, mpv_error_string(end->error));
        update_transport(p);
        if (p->preferences)
            preferences_window_update(p->preferences, &p->preference_values);
        /* Failed or empty media must leave an accessible error/empty window.
         * Playlist redirects, however, can still load a real entry next. */
        if (end->reason != MPV_END_FILE_REASON_REDIRECT ||
            end->playlist_insert_num_entries == 0) {
            p->startup_waiting_for_media = FALSE;
            present_startup_window(p);
        }
        break;
    }
    case MPV_EVENT_VIDEO_RECONFIG:
        if (playback_info_visible(p))
            request_playback_values(p);
        if (p->decoder_observed)
            request_hwdec_state(p);
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        if (p->decoder_observed)
            request_hwdec_state(p);
        break;
    case MPV_EVENT_GET_PROPERTY_REPLY:
        if (accept_track_notice(p, event)) {
            /* Actual aid/sid reads also handle changes from native bindings. */
        } else if (p->initial_subtitle_request &&
            event->reply_userdata == p->initial_subtitle_request) {
            p->initial_subtitle_request = 0;
            mpv_event_property *property = event->data;
            if (!p->closing && !p->quit_requested && p->timeline.loaded && event->error >= 0 && property &&
                property->format == MPV_FORMAT_STRING && property->data) {
                g_free(p->initial_subtitle);
                p->initial_subtitle = g_strdup(*(char **)property->data);
            }
            if (p->initial_subtitle_reset_pending) {
                guint cycles = p->initial_subtitle_pending_cycles;
                p->initial_subtitle_reset_pending = FALSE;
                p->initial_subtitle_pending_cycles = 0;
                reset_initial_subtitle(p);
                for (guint i = 0; i < cycles; ++i)
                    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
            }
        } else if (p->chapter_request && event->reply_userdata == p->chapter_request) {
            if (event->error >= 0)
                accept_chapters(p, event->data);
            p->chapter_request = 0;
        } else if (p->chapter_current_request &&
                   event->reply_userdata == p->chapter_current_request) {
            if (p->timeline.loaded && event->error >= 0)
                property_changed(p, event->data);
            p->chapter_current_request = 0;
        } else if (event->reply_userdata == AUDIO_OUTPUTS_REFRESH_ID) {
            if (event->error >= 0 && p->preferences && p->audio_observed) {
                mpv_event_property *property = event->data;
                if (property->format == MPV_FORMAT_NODE)
                    preferences_window_update_audio_outputs(p->preferences, property->data, NULL);
            }
        } else if (event->reply_userdata == HWDEC_REFRESH_ID) {
            p->preference_values.hwdec_pending = FALSE;
            if (event->error >= 0)
                property_changed(p, event->data);
            else
                p->preference_values.hwdec_known = FALSE;
            if (p->preferences)
                preferences_window_update(p->preferences, &p->preference_values);
        } else if (event->error >= 0 && event->reply_userdata == VIDEO_TRACK_REFRESH_ID) {
            property_changed(p, event->data);
        } else if (event->reply_userdata >= PLAYBACK_REFRESH_BASE &&
                   event->reply_userdata < PLAYBACK_REFRESH_BASE + G_N_ELEMENTS(playback_properties)) {
            if (playback_info_visible(p) && event->error >= 0) {
                playback_property_changed(p, event->data);
                p->info_dirty = TRUE;
            }
        } else if (event->error >= 0 && event->reply_userdata == TRACK_REFRESH_ID) {
            mpv_event_property *property = event->data;
            update_track_menu(p, property->format == MPV_FORMAT_NODE ? property->data : NULL);
        } else if (event->error >= 0 && event->reply_userdata == PLAYLIST_REFRESH_ID) {
            mpv_event_property *property = event->data;
            update_playlist(p, property->format == MPV_FORMAT_NODE ? property->data : NULL);
        } else if (event->error >= 0 && event->reply_userdata == INPUT_BINDINGS_REFRESH_ID &&
                   p->preferences && p->keys_loaded) {
            mpv_event_property *property = event->data;
            preferences_window_update_keys(p->preferences,
                property->format == MPV_FORMAT_NODE ? property->data : NULL, NULL);
        } else if (event->reply_userdata == COMMAND_LIST_REFRESH_ID &&
                   p->preferences && p->keys_loaded) {
            if (event->error >= 0) {
                mpv_event_property *property = event->data;
                preferences_window_update_keys(p->preferences, NULL,
                    property->format == MPV_FORMAT_NODE ? property->data : NULL);
            } else {
                preferences_window_keys_error(p->preferences, mpv_error_string(event->error));
            }
        }
        break;
    case MPV_EVENT_COMMAND_REPLY:
    case MPV_EVENT_SET_PROPERTY_REPLY:
        if (event->reply_userdata == INPUT_RELOAD_CLEAR_ID && event->error < 0 && p->preferences)
            preferences_window_keys_error(p->preferences, mpv_error_string(event->error));
        if (event->reply_userdata == INPUT_RELOAD_FILE_ID) {
            g_autoptr(GError) error = NULL;
            if (event->error < 0 || !install_key_edits(p, &error)) {
                if (p->preferences)
                    preferences_window_keys_error(p->preferences, error ? error->message
                        : mpv_error_string(event->error));
                p->keys_reload_pending = FALSE;
                if (p->preferences && p->keys_loaded)
                    mpv_get_property_async(p->mpv, INPUT_BINDINGS_REFRESH_ID, "input-bindings", MPV_FORMAT_NODE);
            }
        }
        if (event->reply_userdata == INPUT_EDIT_DEFINE_ID && event->error < 0 && p->preferences)
            preferences_window_keys_error(p->preferences, mpv_error_string(event->error));
        if (event->reply_userdata == INPUT_EDIT_ENABLE_ID) {
            if (event->error < 0 && p->preferences)
                preferences_window_keys_error(p->preferences, mpv_error_string(event->error));
            p->keys_reload_pending = FALSE;
            if (p->preferences && p->keys_loaded)
                mpv_get_property_async(p->mpv, INPUT_BINDINGS_REFRESH_ID, "input-bindings", MPV_FORMAT_NODE);
        }
        if (event->reply_userdata == HWDEC_SET_ID)
            p->hwdec_refresh_at = event->error >= 0 && p->decoder_observed
                ? g_get_monotonic_time() + 200000 : 0;
        if (event->error < 0)
            show_error(p, mpv_error_string(event->error));
        break;
    case MPV_EVENT_CLIENT_MESSAGE: {
        mpv_event_client_message *message = event->data;
        if (message->num_args >= 2 && g_str_equal(message->args[0], "mpv-gtk"))
            app_key_action(p, message->args[1]);
        break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *log = event->data;
        g_printerr("mpv [%s]: %s", log->prefix, log->text);
        if (p->keys_reload_pending && p->preferences &&
            (strstr(log->prefix, "input") || strstr(log->prefix, "command")))
            preferences_window_keys_error(p->preferences, log->text);
        break;
    }
    case MPV_EVENT_SHUTDOWN:
        startup_trace_mark("mpv shutdown received");
        p->shutdown_received = TRUE;
        if (p->window) {
            gtk_window_close(p->window);
        } else {
            prepare_close(p);
            p->closing = TRUE;
            continue_render_load(p);
        }
        break;
    default:
        break;
    }
}

static gboolean dispatch_mpv(gint fd, GIOCondition condition, gpointer data)
{
    Player *p = data;
    (void)condition;
    uint64_t count;
    ssize_t result;
    do {
        result = read(fd, &count, sizeof(count));
    } while (result < 0 && errno == EINTR);
    if (p->closing)
        return G_SOURCE_CONTINUE;
    /* Both client and render wakeups use this fd. Always service the render
     * context even if no GTK snapshot is pending (e.g. a hidden window). */
    if (p->renderer) {
        if (p->quit_requested) {
            discard_render_frame(p);
        } else {
            gtk_gl_area_make_current(p->area);
            if (!gtk_gl_area_get_error(p->area)) {
                uint64_t flags = mpv_render_context_update(p->renderer);
                if (flags & MPV_RENDER_UPDATE_FRAME) {
                    p->frame_updates++;
                    gtk_gl_area_queue_render(p->area);
                }
            }
        }
    }
    /* Bound a dispatch so a busy stream cannot starve input/GTK animation. */
    for (int i = 0; i < 128; ++i) {
        mpv_event *event = mpv_wait_event(p->mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            return G_SOURCE_CONTINUE;
        handle_event(p, event);
        if (p->closing)
            return G_SOURCE_CONTINUE;
    }
    wake_main(p);
    return G_SOURCE_CONTINUE;
}

static void *get_proc_address(void *context, const char *name)
{
    /* Resolve against EGL for both desktop GL and GLES under Wayland. A
     * desktop-only symbol table must not decide which API GTK can use. */
    __eglMustCastToProperFunctionPointerType function = eglGetProcAddress(name);
    void *address = NULL;
    G_STATIC_ASSERT(sizeof(function) == sizeof(address));
    memcpy(&address, &function, sizeof(address));
    return address ? address : dlsym(context, name);
}

/* The GL context is already current. Keeping renderer allocation separate
 * from GtkGLArea readiness lets the empty window avoid libmpv's GL setup. */
static gboolean cuda_interop_handles(const char *hwdec)
{
    if (!hwdec)
        return FALSE;
    g_auto(GStrv) choices = g_strsplit(hwdec, ",", -1);
    for (guint i = 0; choices[i]; ++i) {
        const char *choice = g_strstrip(choices[i]);
        if (g_str_equal(choice, "no"))
            return TRUE; /* mpv stops considering the remaining alternatives. */
        if (!*choice || g_str_equal(choice, "auto") || g_str_equal(choice, "auto-safe") ||
            g_str_equal(choice, "auto-unsafe") || g_str_equal(choice, "yes") ||
            g_str_equal(choice, "auto-copy") || g_str_equal(choice, "auto-copy-safe") ||
            g_str_equal(choice, "auto-copy-unsafe") || g_str_equal(choice, "nvdec") ||
            g_str_equal(choice, "cuda") || g_str_has_suffix(choice, "-nvdec") ||
            g_str_has_suffix(choice, "-cuda") || g_str_has_suffix(choice, "-copy"))
            continue;
        return FALSE;
    }
    return TRUE;
}

static gboolean prefer_cuda_interop(const char *vendor, const char *interop, const char *hwdec)
{
    return vendor && g_ascii_strncasecmp(vendor, "NVIDIA", 6) == 0 &&
        interop && (!*interop || g_str_equal(interop, "auto")) && cuda_interop_handles(hwdec);
}

static gboolean create_renderer(Player *p)
{
    if (p->renderer)
        return TRUE;
    /* libmpv's default preloads every interop. On NVIDIA, prepare CUDA for
     * NVDEC and CPU/copy modes rather than probing unrelated driver stacks.
     * Preserve explicit interop choices and the user's effective option. */
    char *interop = mpv_get_property_string(p->mpv, "options/gpu-hwdec-interop");
    char *hwdec = mpv_get_property_string(p->mpv, "options/hwdec");
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    gboolean narrowed = prefer_cuda_interop(vendor, interop, hwdec) &&
        mpv_set_option_string(p->mpv, "gpu-hwdec-interop", "cuda") >= 0;
    if (narrowed)
        startup_trace_mark("CUDA interop selected for NVIDIA");
    mpv_opengl_init_params gl_init = {
        .get_proc_address = get_proc_address,
        .get_proc_address_ctx = p->gl_library
    };
    void *wayland_display = NULL;
#ifdef GDK_WINDOWING_WAYLAND
    if (p->area) {
        GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(p->area));
        if (GDK_IS_WAYLAND_DISPLAY(display))
            wayland_display = gdk_wayland_display_get_wl_display(display);
    }
#endif
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {wayland_display ? MPV_RENDER_PARAM_WL_DISPLAY : MPV_RENDER_PARAM_INVALID,
         wayland_display},
        {MPV_RENDER_PARAM_INVALID, NULL}
    };
    int result = mpv_render_context_create(&p->renderer, p->mpv, params);
    if (narrowed)
        mpv_set_option_string(p->mpv, "gpu-hwdec-interop", interop ? interop : "auto");
    mpv_free(hwdec);
    mpv_free(interop);
    p->cuda_interop = narrowed && result >= 0;
    if (result < 0) {
        show_error(p, mpv_error_string(result));
        p->render_failed = TRUE;
        return FALSE;
    }
    mpv_render_context_set_update_callback(p->renderer, wake_main, p);
    startup_trace_mark("mpv renderer ready");
    return TRUE;
}

static void gl_context_failed(Player *p, GtkGLArea *area, const GError *error)
{
    /* GtkGLArea draws its error in the middle of the video. Our error bar is
     * the sole presentation; copy before clearing the error owned by GTK. */
    g_autofree char *message = g_strdup(error->message);
    p->ready = FALSE;
    p->render_failed = TRUE;
    gtk_gl_area_set_error(area, NULL);
    show_error(p, message);
}

static gboolean ensure_renderer(Player *p)
{
    if (p->quit_requested)
        return FALSE;
    if (p->renderer)
        return TRUE;
    if (!p->mpv || !p->ready || !p->area)
        return FALSE;
    gtk_gl_area_make_current(p->area);
    const GError *error = gtk_gl_area_get_error(p->area);
    if (error) {
        gl_context_failed(p, p->area, error);
        return FALSE;
    }
    return create_renderer(p);
}

static void continue_render_load(Player *p)
{
    if (!p->render_load_hook)
        return;
    if (!p->closing && !p->quit_requested && !p->render_failed) {
        if (!p->ready && p->area) {
            gtk_widget_set_visible(GTK_WIDGET(p->area), TRUE);
            if (!p->render_load_hook)
                return;
        }
        if (!p->ready)
            return;
        ensure_renderer(p);
    }
    /* START_FILE is only a notification: the on_preloaded hook is the barrier
     * that prevents libmpv from creating its VO before our renderer exists.
     * Always release it on failure and shutdown so the core cannot stall. */
    uint64_t hook = p->render_load_hook;
    p->render_load_hook = 0;
    /* Window output is owned by GtkGLArea, including after file profiles. */
    if (!p->closing && !p->quit_requested)
        mpv_set_option_string(p->mpv, "vo", "libmpv");
    int result = mpv_hook_continue(p->mpv, hook);
    if (result < 0 && !p->closing && !p->quit_requested)
        show_error(p, mpv_error_string(result));
}

static void realize(GtkGLArea *area, gpointer data)
{
    Player *p = data;
    gtk_gl_area_make_current(area);
    const GError *error = gtk_gl_area_get_error(area);
    if (error) {
        gl_context_failed(p, area, error);
        continue_render_load(p);
        return;
    }
    p->ready = TRUE;
    startup_trace_mark("GTK GL context ready");
    /* An active file may outlive a GtkGLArea unrealize/realize cycle. Its
     * on_preloaded hook has already completed, so restore that renderer here. */
    if (p->timeline.loaded || !p->idle)
        ensure_renderer(p);
    continue_render_load(p);
    flush_pending_paths(p);
}

static void clear_render_target(GLint target)
{
    GLint framebuffer;
    GLfloat clear_color[4];
    GLboolean color_mask[4];
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)target);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)framebuffer);
}

static gboolean render(GtkGLArea *area, GdkGLContext *context, gpointer data)
{
    (void)area;
    (void)context;
    Player *p = data;
    if (p->quit_requested) {
        discard_render_frame(p);
        return TRUE;
    }
    GLint fbo = 0, viewport[4] = {0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0)
        return TRUE;
    if (!p->renderer) {
        clear_render_target(fbo);
        return TRUE;
    }
    mpv_opengl_fbo target = {.fbo = fbo, .w = viewport[2], .h = viewport[3]};
    int flip = 1, block = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        /* Waiting for a low-FPS video frame here would freeze GTK too. */
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {MPV_RENDER_PARAM_INVALID, NULL}
    };
    mpv_render_frame_info frame_info = {0};
    gboolean trace_frame = !p->startup_frame_rendered && startup_trace_enabled() &&
        p->timeline.loaded && !p->idle;
    if (trace_frame)
        mpv_render_context_get_info(p->renderer,
            (mpv_render_param){MPV_RENDER_PARAM_NEXT_FRAME_INFO, &frame_info});
    gint64 started = g_get_monotonic_time();
    int result = mpv_render_context_render(p->renderer, params);
    if (result >= 0 && trace_frame && (frame_info.flags & MPV_RENDER_FRAME_INFO_PRESENT)) {
        p->startup_frame_rendered = TRUE;
        startup_trace_mark("first video frame rendered");
    }
    p->render_calls++;
    p->max_render_ms = fmax(p->max_render_ms,
                           (g_get_monotonic_time() - started) / 1000.0);
    if (result < 0 && !p->render_failed) {
        p->render_failed = TRUE;
        show_error(p, mpv_error_string(result));
    }
    /* The render API may redraw its last frame after EOF. Service its frame
     * queue, then replace that cached image before GTK shows the welcome UI. */
    if (p->idle || !p->timeline.loaded)
        clear_render_target(fbo);
    /* GTK presents this texture later; report_swap here would be inaccurate. */
    return TRUE;
}

static void discard_render_frame(Player *p)
{
    if (!p->renderer)
        return;
    if (p->area) {
        gtk_gl_area_make_current(p->area);
        if (gtk_gl_area_get_error(p->area))
            return;
    }
    /* A hidden window will not get another GtkGLArea render. Explicitly
     * consume its queued frame before libmpv waits for VO/decoder teardown. */
    mpv_render_context_update(p->renderer);
    int skip = 1, block = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {MPV_RENDER_PARAM_INVALID, NULL}
    };
    mpv_render_context_render(p->renderer, params);
}

static void unrealize(GtkGLArea *area, gpointer data)
{
    Player *p = data;
    p->ready = FALSE;
    if (p->renderer) {
        gtk_gl_area_make_current(area);
        discard_render_frame(p);
        mpv_render_context_set_update_callback(p->renderer, NULL, NULL);
        startup_trace_mark("mpv renderer cleanup started");
        mpv_render_context_free(p->renderer);
        p->renderer = NULL;
        startup_trace_mark("mpv renderer cleanup finished");
    }
}

static void flush_pending_paths(Player *p)
{
    if (!p->mpv || p->closing || p->quit_requested)
        return;
    while (!g_queue_is_empty(&p->pending_commands)) {
        g_auto(GStrv) args = g_queue_pop_head(&p->pending_commands);
        command(p, (const char **)args);
    }
    if (p->settings_requested) {
        p->settings_requested = FALSE;
        show_preferences(p);
    }
    if (!p->pending || !p->pending->len)
        return;
    if (p->area)
        gtk_widget_set_visible(GTK_WIDGET(p->area), TRUE);
    if (!p->ready || !p->pending->len)
        return;
    g_autoptr(GPtrArray) paths = p->pending;
    p->pending = g_ptr_array_new_with_free_func(g_free);
    open_paths(p, paths);
}

/* The preloaded hook holds VO/decoder setup until GtkGLArea is ready. File
 * opening and demuxing can already run while GTK prepares that context. */
static void queue_media_paths(Player *p, GPtrArray *paths, gboolean append)
{
    if (!append)
        p->media_loading = TRUE;
    if (!append && paths->len > 1) {
        /* Build the explicit selection before playback starts, so native
         * autocreate-playlist cannot expand the first entry mid-batch. */
        const char *stop[] = {"stop", NULL};
        command(p, stop);
        for (guint i = 0; i < paths->len; ++i) {
            const char *args[] = {"loadfile", g_ptr_array_index(paths, i), "append", NULL};
            command(p, args);
        }
        const char *play[] = {"playlist-play-index", "0", NULL};
        command(p, play);
    } else {
        for (guint i = 0; i < paths->len; ++i) {
            const char *args[] = {"loadfile", g_ptr_array_index(paths, i),
                                 append || i > 0 ? "append" : "replace", NULL};
            command(p, args);
        }
    }
    if (!append)
        set_flag(p, "pause", FALSE);
}

static void queue_startup_media(Player *p)
{
    if (p->closing || p->quit_requested || !p->mpv || !p->pending || !p->pending->len)
        return;
    p->startup_waiting_for_media = TRUE;
    g_autoptr(GPtrArray) paths = p->pending;
    p->pending = g_ptr_array_new_with_free_func(g_free);
    /* A new player is empty regardless of its incoming-file policy. */
    queue_media_paths(p, paths, FALSE);
    startup_trace_mark("initial media queued");
}

static gboolean open_paths(Player *p, GPtrArray *paths)
{
    if (p->closing || p->quit_requested || !paths || !paths->len)
        return FALSE;
    gboolean current = p->timeline.loaded || p->media_loading ||
        (p->pending && p->pending->len > 0);
    gboolean append = current && i18n_add_to_queue();
    if (current && !append && !i18n_allow_replace())
        return FALSE;
    if (!p->mpv || !p->ready) {
        if (!p->pending)
            p->pending = g_ptr_array_new_with_free_func(g_free);
        if (paths != p->pending) {
            if (!append)
                g_ptr_array_set_size(p->pending, 0);
            for (guint i = 0; i < paths->len; ++i)
                g_ptr_array_add(p->pending, g_strdup(g_ptr_array_index(paths, i)));
        }
        flush_pending_paths(p);
        return TRUE;
    }
    queue_media_paths(p, paths, append);
    if (player_video_widget(p))
        gtk_widget_grab_focus(player_video_widget(p));
    return TRUE;
}

static void mpris_raise_window(gpointer data)
{
    Player *p = data;
    if (p->startup_presented && !p->closing && !p->quit_requested)
        gtk_window_present(p->window);
}

static void mpris_quit_player(gpointer data)
{
    Player *p = data;
    if (!p->closing && !p->quit_requested) gtk_window_close(p->window);
}

static void mpris_fullscreen_window(gboolean fullscreen, gpointer data)
{
    Player *p = data;
    if (!p->closing && !p->quit_requested) set_fullscreen(p, fullscreen);
}

static gboolean mpris_open_media(const char *uri, gpointer data)
{
    Player *p = data;
    if (!p->mpv || p->closing || p->quit_requested) return FALSE;
    g_autoptr(GFile) file = g_file_new_for_uri(uri);
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(paths, media_location(file));
    /* MPRIS OpenUri explicitly requests playback of this item, rather than
     * the configurable append/replace behavior of ordinary file opening. */
    queue_media_paths(p, paths, FALSE);
    return TRUE;
}

static void mpris_art_changed(const char *uri, gpointer data)
{
    Player *p = data;
    if (!p->closing && !p->quit_requested && p->mpris) mpris_set_art_url(p->mpris, uri);
}

static gboolean start_mpris_idle(gpointer data)
{
    Player *p = data;
    p->mpris_source = 0;
    if (p->closing || p->quit_requested || !p->mpv) return G_SOURCE_REMOVE;
    MprisHost host = {.raise = mpris_raise_window, .quit = mpris_quit_player,
        .set_fullscreen = mpris_fullscreen_window, .open_uri = mpris_open_media, .data = p};
    p->mpris = mpris_new(p->mpv, &host);
    p->mpris_art = mpris_art_new(p->mpv, mpris_art_changed, p);
    return G_SOURCE_REMOVE;
}

static void file_response(GObject *source, GAsyncResult *result, gpointer data)
{
    Player *p = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(
        GTK_FILE_DIALOG(source), result, &error);
    if (files && !p->closing && !p->quit_requested) {
        g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < g_list_model_get_n_items(files); ++i) {
            g_autoptr(GFile) file = g_list_model_get_item(files, i);
            g_ptr_array_add(paths, media_location(file));
        }
        open_paths(p, paths);
    } else if (error && !p->closing && !p->quit_requested &&
               !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
               !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED)) {
        show_error(p, error->message);
    }
    g_clear_object(&p->chooser);
    g_clear_object(&p->file_cancel);
    g_application_release(G_APPLICATION(p->application));
}

static void show_files(Player *p)
{
    if (p->chooser)
        return;
    p->chooser = gtk_file_dialog_new();
    p->file_cancel = g_cancellable_new();
    gtk_file_dialog_set_title(p->chooser, tr("Открыть медиафайл"));
    gtk_file_dialog_set_accept_label(p->chooser, tr("Открыть"));
    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_autoptr(GtkFileFilter) media = gtk_file_filter_new();
    gtk_file_filter_set_name(media, tr("Видео и аудио"));
    gtk_file_filter_add_mime_type(media, "video/*");
    gtk_file_filter_add_mime_type(media, "audio/*");
    gtk_file_filter_add_pattern(media, "*.m3u");
    gtk_file_filter_add_pattern(media, "*.m3u8");
    g_list_store_append(filters, media);
    g_autoptr(GtkFileFilter) all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, tr("Все файлы"));
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(p->chooser, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(p->chooser, media);
    /* Keep Player alive until a cancelled portal request completes at close. */
    g_application_hold(G_APPLICATION(p->application));
    gtk_file_dialog_open_multiple(p->chooser, p->window, p->file_cancel, file_response, p);
}

static void url_accept(GtkButton *button, gpointer data)
{
    (void)button;
    Player *p = data;
    GtkWindow *dialog = p->url_dialog;
    if (!p->closing && !p->quit_requested) {
        GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "entry");
        g_autofree char *url = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
        g_strstrip(url);
        if (*url) {
            g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(paths, g_strdup(url));
            open_paths(p, paths);
        }
    }
    p->url_dialog = NULL;
    gtk_window_destroy(dialog);
}

static void url_cancel(GtkButton *button, gpointer data)
{
    (void)button;
    Player *p = data;
    GtkWindow *dialog = p->url_dialog;
    p->url_dialog = NULL;
    gtk_window_destroy(dialog);
}

static gboolean url_closed(GtkWindow *dialog, gpointer data)
{
    (void)dialog;
    Player *p = data;
    p->url_dialog = NULL;
    return FALSE;
}

static void show_url(Player *p)
{
    if (p->url_dialog) {
        gtk_window_present(p->url_dialog);
        return;
    }
    GtkWidget *dialog = gtk_window_new();
    p->url_dialog = GTK_WINDOW(dialog);
    gtk_window_set_title(GTK_WINDOW(dialog), tr("Открыть ссылку"));
    gtk_window_set_transient_for(GTK_WINDOW(dialog), p->window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "https://…");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(box), entry);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = translated_button("Отмена");
    GtkWidget *open = translated_button("Открыть");
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), open);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_default_widget(GTK_WINDOW(dialog), open);
    g_object_set_data(G_OBJECT(dialog), "entry", entry);
    g_signal_connect(cancel, "clicked", G_CALLBACK(url_cancel), p);
    g_signal_connect(open, "clicked", G_CALLBACK(url_accept), p);
    g_signal_connect(dialog, "close-request", G_CALLBACK(url_closed), p);
    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(entry);
}

static gboolean drop_files(GtkDropTarget *target, const GValue *value,
                           double x, double y, gpointer data)
{
    (void)target; (void)x; (void)y;
    Player *p = data;
    GdkFileList *list = g_value_get_boxed(value);
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    for (GSList *it = gdk_file_list_get_files(list); it; it = it->next)
        g_ptr_array_add(paths, media_location(G_FILE(it->data)));
    if (!paths->len)
        return FALSE;
    return open_paths(p, paths);
}

static gboolean popover_visible(GtkWidget *popover)
{
    return popover && gtk_widget_get_visible(popover);
}

static void center_audio_popover(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    if (!gtk_widget_get_visible(GTK_WIDGET(object)))
        return;
    /* Context menus normally point at the click. Use the entire sound
     * button as the anchor so every click opens at the same center. */
    gtk_popover_set_pointing_to(GTK_POPOVER(object), NULL);
    gtk_popover_set_position(GTK_POPOVER(object), GTK_POS_TOP);
    gtk_popover_set_offset(GTK_POPOVER(object), 0, 0);
    gtk_widget_set_halign(GTK_WIDGET(object), GTK_ALIGN_CENTER);
}

static void cursor_timeout_cancel(Player *p)
{
    if (p->cursor_source)
        g_source_remove(p->cursor_source);
    p->cursor_source = 0;
    p->cursor_deadline_us = 0;
}

static gboolean cursor_timeout(gpointer data)
{
    Player *p = data;
    p->cursor_source = 0;
    p->cursor_deadline_us = 0;
    update_controls_visibility(p, g_get_monotonic_time());
    return G_SOURCE_REMOVE;
}

static void cursor_timeout_arm(Player *p, gint64 deadline_us)
{
    if (p->cursor_source && p->cursor_deadline_us == deadline_us)
        return;
    cursor_timeout_cancel(p);
    /* An absolute GLib deadline also runs when GTK's frame clock is idle.
     * Repainting the timeline must neither postpone nor dispatch this timer. */
    GSource *source = g_timeout_source_new(0);
    g_source_set_ready_time(source, deadline_us);
    g_source_set_callback(source, cursor_timeout, p, NULL);
    p->cursor_deadline_us = deadline_us;
    p->cursor_source = g_source_attach(source, NULL);
    g_source_unref(source);
}

static void controls_interaction_changed(GObject *object, GParamSpec *property,
                                         gpointer data)
{
    (void)object; (void)property;
    update_controls_visibility(data, g_get_monotonic_time());
}

static void update_controls_visibility(Player *p, gint64 now)
{
    if (!p->controls_revealer || !p->audio_context || !p->subtitle_button ||
        !p->speed_button || p->closing || p->quit_requested)
        return;
    int height = gtk_widget_get_height(p->stage);
    if (height != p->pointer_height) {
        p->pointer_height = height;
        p->pointer_bottom = FALSE;
    }
    GtkWidget *focus = gtk_window_get_focus(p->window);
    gboolean keyboard_focus = focus && gtk_widget_has_visible_focus(focus) &&
        (focus == p->controls || gtk_widget_is_ancestor(focus, p->controls));
    gboolean menu_open = popover_visible(gtk_popover_bin_get_popover(
        GTK_POPOVER_BIN(p->audio_context))) ||
        gtk_menu_button_get_active(GTK_MENU_BUTTON(p->subtitle_button)) ||
        gtk_menu_button_get_active(GTK_MENU_BUTTON(p->speed_button)) ||
        (p->chapter_button && gtk_menu_button_get_active(GTK_MENU_BUTTON(p->chapter_button))) ||
        (p->app_menu_button && gtk_menu_button_get_active(GTK_MENU_BUTTON(p->app_menu_button)));
    gboolean hold = p->pointer_bottom || p->dragging || p->controls_pressed || keyboard_focus || menu_open ||
                    trim_mode_holding(p->trim) ||
                    gtk_revealer_get_reveal_child(GTK_REVEALER(p->sidebar)) ||
                    p->chooser || p->url_dialog || p->preferences || p->info_window;
    gboolean visible = controls_update(&p->controls_state, hold, now);
    if (gtk_revealer_get_reveal_child(GTK_REVEALER(p->controls_revealer)) != visible) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->controls_revealer), visible);
        /* A hidden overlay must not swallow the bottom-edge motion events. */
        gtk_widget_set_can_target(p->controls_revealer, visible);
    }
    /* Auxiliary windows have their own cursors and must not extend the
     * video's inactivity timer. Only an active operation here holds it. */
    gboolean cursor_hold = p->dragging || p->controls_pressed || menu_open || trim_mode_holding(p->trim);
    /* mpv's deadline does not depend on pointer-position queries. The cursor
     * belongs to our widgets, so hiding it never affects another window. */
    gboolean pointer_present = p->fullscreen || p->pointer_present;
    gboolean hide_cursor = !cursor_update(&p->cursor_state, pointer_present, cursor_hold, now);
    if (hide_cursor != p->cursor_hidden) {
        p->cursor_hidden = hide_cursor;
        gtk_widget_set_cursor_from_name(p->stage, hide_cursor ? "none" : NULL);
        if (player_video_widget(p))
            gtk_widget_set_cursor_from_name(player_video_widget(p), hide_cursor ? "none" : NULL);
    }
    if (p->fullscreen && !cursor_hold && !hide_cursor)
        cursor_timeout_arm(p, p->cursor_state.hide_at_us);
    else
        cursor_timeout_cancel(p);
}

static void pointer_enter(GtkEventControllerMotion *controller, double x, double y,
                           gpointer data)
{
    (void)x;
    Player *p = data;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    double surface_x, surface_y;
    if (event && gdk_event_get_event_type(event) == GDK_ENTER_NOTIFY &&
        gdk_event_get_position(event, &surface_x, &surface_y)) {
        /* A compositor crossing after fullscreen/layout is not physical
         * motion. Rebase coordinates without restarting the deadline. */
        cursor_position(&p->cursor_state, floor(surface_x), floor(surface_y),
                        TRUE, g_get_monotonic_time());
    }
    p->pointer_present = TRUE;
    p->pointer_height = gtk_widget_get_height(p->stage);
    int zone = MAX(144, gtk_widget_get_height(p->controls) + 48);
    p->pointer_bottom = y >= p->pointer_height - zone;
    update_controls_visibility(p, g_get_monotonic_time());
}

static void pointer_motion(GtkEventControllerMotion *controller, double x, double y,
                            gpointer data)
{
    Player *p = data;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    double surface_x, surface_y;
    if (event && gdk_event_get_event_type(event) == GDK_MOTION_NOTIFY &&
        gdk_event_get_position(event, &surface_x, &surface_y)) {
        /* GTK's layout-generated motion has time 0. Use surface coordinates:
         * local widget coordinates can change while the mouse stays still. */
        cursor_position(&p->cursor_state, floor(surface_x), floor(surface_y),
            gdk_event_get_time(event) == GDK_CURRENT_TIME, g_get_monotonic_time());
    }
    pointer_enter(controller, x, y, data);
}

static void pointer_leave(GtkEventControllerMotion *controller, gpointer data)
{
    (void)controller;
    Player *p = data;
    p->pointer_present = p->pointer_bottom = FALSE;
    update_controls_visibility(p, g_get_monotonic_time());
}

static gboolean controls_event(GtkEventControllerLegacy *controller, GdkEvent *event,
                                gpointer data)
{
    Player *p = data;
    GdkEventType type = gdk_event_get_event_type(event);
    if ((type == GDK_BUTTON_PRESS || type == GDK_TOUCH_BEGIN) &&
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)) == p->controls)
        p->controls_pressed = TRUE;
    else if (type == GDK_BUTTON_RELEASE || type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL)
        p->controls_pressed = FALSE;
    if (gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)) == p->stage &&
        (type == GDK_BUTTON_PRESS || type == GDK_TOUCH_BEGIN || type == GDK_SCROLL)) {
        cursor_motion(&p->cursor_state, g_get_monotonic_time());
    }
    if (type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE ||
        type == GDK_TOUCH_BEGIN || type == GDK_TOUCH_END ||
        type == GDK_TOUCH_CANCEL || type == GDK_SCROLL) {
        update_controls_visibility(p, g_get_monotonic_time());
    }
    return FALSE;
}

static void config_launched(GObject *source, GAsyncResult *result, gpointer data)
{
    Player *p = data;
    g_autoptr(GError) error = NULL;
    if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error) &&
        !p->closing && !p->quit_requested && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_error(p, error->message);
    g_clear_object(&p->config_launch_cancel);
    g_application_release(G_APPLICATION(p->application));
}

static void open_config(const char *filename, gpointer data)
{
    Player *p = data;
    if (p->config_launch_cancel || p->closing || p->quit_requested)
        return;
    const char *path = g_str_equal(filename, "input.conf")
                     ? config_input_path(&p->config) : config_path(&p->config);
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_set_writable(launcher, TRUE);
    p->config_launch_cancel = g_cancellable_new();
    g_application_hold(G_APPLICATION(p->application));
    gtk_file_launcher_launch(launcher, p->preferences ? p->preferences : p->window,
                             p->config_launch_cancel, config_launched, p);
}

static gboolean language_changed_idle(gpointer data)
{
    Player *p = data;
    p->language_source = 0;
    if (p->closing || p->quit_requested)
        return G_SOURCE_REMOVE;
    translate_tree(p->root);
    translate_tree(p->header);
    trim_mode_translate(p->trim);
    update_app_menu(p);
    if (p->url_dialog) {
        gtk_window_set_title(p->url_dialog, tr("Открыть ссылку"));
        translate_tree(GTK_WIDGET(p->url_dialog));
    }
    update_transport(p);
    g_autofree char *queue = g_strdup_printf(tr("Очередь · %d"), (int)p->playlist_count);
    if (p->queue_label)
        gtk_label_set_text(GTK_LABEL(p->queue_label), queue);
    if (p->mpv)
        mpv_get_property_async(p->mpv, TRACK_REFRESH_ID, "track-list", MPV_FORMAT_NODE);
    if (p->mpv && p->playlist_observed)
        mpv_get_property_async(p->mpv, PLAYLIST_REFRESH_ID, "playlist", MPV_FORMAT_NODE);
    if (p->preferences) {
        GtkWindow *old = p->preferences;
        g_object_remove_weak_pointer(G_OBJECT(old), (gpointer *)&p->preferences);
        p->preferences = NULL;
        gtk_window_destroy(old);
        show_preferences(p);
        preferences_window_show_interface(p->preferences);
    }
    if (p->info_window) {
        GtkWindow *old = p->info_window;
        g_object_remove_weak_pointer(G_OBJECT(old), (gpointer *)&p->info_window);
        p->info_window = NULL;
        gtk_window_destroy(old);
        show_playback_info(p);
    }
    if (p->about_window) {
        GtkWindow *old = p->about_window;
        g_object_remove_weak_pointer(G_OBJECT(old), (gpointer *)&p->about_window);
        p->about_window = NULL;
        gtk_window_destroy(old);
        show_about(p);
    }
    if (p->info_overlay) {
        translate_tree(p->info_overlay);
        update_playback_info(p);
    }
    return G_SOURCE_REMOVE;
}

static gboolean audio_device_matches_output(const char *device, const char *output)
{
    if (!device || !*device || g_str_equal(device, "auto"))
        return TRUE;
    const char *separator = strchr(device, '/');
    if (!separator)
        return g_str_equal(device, output);
    return separator && strlen(output) == (gsize)(separator - device) &&
        strncmp(device, output, separator - device) == 0;
}

static void change_audio_preferences(Player *p, const char *name, const char *choice)
{
    /* Read evaluated options only on demand. Opening the player never scans
     * audio devices or constructs settings models. An explicit device carries
     * its AO prefix, so keep both options compatible when changing the AO. */
    char *old_output = mpv_get_property_string(p->mpv, "ao");
    char *old_device = mpv_get_property_string(p->mpv, "audio-device");
    g_autofree char *output = g_strdup(g_str_equal(name, "ao")
        ? (g_str_equal(choice, "auto") ? "" : choice) : old_output ? old_output : "");
    g_autofree char *device = g_strdup(g_str_equal(name, "audio-device")
        ? choice : old_device ? old_device : "auto");
    mpv_free(old_output);
    mpv_free(old_device);
    const char *names[2], *values[2];
    gsize count = 0;
    if (g_str_equal(name, "ao")) {
        if (!audio_device_matches_output(device, output)) {
            g_free(device);
            device = g_strdup("auto");
            names[count] = "audio-device";
            values[count++] = device;
        }
        names[count] = "ao";
        values[count++] = output;
    } else {
        if (*output && !audio_device_matches_output(device, output)) {
            g_free(output);
            output = g_strdup("");
            names[count] = "ao";
            values[count++] = output;
        }
        names[count] = "audio-device";
        values[count++] = device;
    }
    g_autoptr(GError) error = NULL;
    if (!config_set_options(&p->config, names, values, count, &error)) {
        show_error(p, error->message);
        if (p->preferences)
            preferences_window_update(p->preferences, &p->preference_values);
        return;
    }
    char *recover_track = NULL;
    if (p->timeline.loaded) {
        char *active_output = mpv_get_property_string(p->mpv, "current-ao");
        if (!active_output || !*active_output) {
            char *active_track = mpv_get_property_string(p->mpv, "aid");
            char *requested_track = mpv_get_property_string(p->mpv, "options/aid");
            /* AO initialization failure deselects the actual track while
             * retaining its requested option. Respect an explicit No audio. */
            if (g_strcmp0(active_track, "no") == 0 && requested_track &&
                !g_str_equal(requested_track, "no"))
                recover_track = g_strdup(requested_track);
            mpv_free(active_track);
            mpv_free(requested_track);
        }
        mpv_free(active_output);
    }
    for (gsize i = 0; i < count; ++i) {
        const char *value = values[i];
        int result = mpv_set_property_async(p->mpv, 0, names[i], MPV_FORMAT_STRING, &value);
        if (result < 0)
            show_error(p, mpv_error_string(result));
    }
    if (recover_track) {
        /* Setting the same requested aid is a no-op in mpv. Reselect it
         * after the new AO/device options, without reopening the file. */
        const char *disabled = "no";
        mpv_set_property_async(p->mpv, 0, "aid", MPV_FORMAT_STRING, &disabled);
        mpv_set_property_async(p->mpv, 0, "aid", MPV_FORMAT_STRING, &recover_track);
        g_free(recover_track);
    }
    g_free(p->audio_output);
    g_free(p->audio_device);
    p->audio_output = g_strdup(*output ? output : "auto");
    p->audio_device = g_strdup(device);
    p->preference_values.audio_output = p->audio_output;
    p->preference_values.audio_device = p->audio_device;
    if (p->preferences)
        preferences_window_update(p->preferences, &p->preference_values);
}

static void preference_changed(const char *name, mpv_format format, const void *value,
                                gpointer data)
{
    Player *p = data;
    if (p->closing)
        return;
    if (g_str_equal(name, "timeline-previews")) {
        g_autoptr(GError) error = NULL;
        if (!i18n_save_timeline_previews(!!*(const int *)value, &error))
            show_error(p, error->message);
        p->preference_values.timeline_previews = i18n_timeline_previews();
        seek_preview_set_enabled(p->seek_preview, p->preference_values.timeline_previews);
        if (p->preferences) preferences_window_update(p->preferences, &p->preference_values);
        return;
    }
    if (g_str_equal(name, "ui-allow-replace") || g_str_equal(name, "ui-add-to-queue")) {
        gboolean allow = g_str_equal(name, "ui-allow-replace");
        gboolean enabled = !!*(const int *)value;
        g_autoptr(GError) error = NULL;
        gboolean saved = allow ? i18n_save_allow_replace(enabled, &error)
                               : i18n_save_add_to_queue(enabled, &error);
        if (!saved)
            show_error(p, error->message);
        p->preference_values.allow_replace = i18n_allow_replace();
        p->preference_values.add_to_queue = i18n_add_to_queue();
        if (p->preferences)
            preferences_window_update(p->preferences, &p->preference_values);
        return;
    }
    for (guint i = 0; i < G_N_ELEMENTS(playlist_preference_properties); ++i) {
        if (!g_str_equal(name, playlist_preference_properties[i].name))
            continue;
        if (!p->mpv || format != MPV_FORMAT_STRING || !value) {
            show_error(p, mpv_error_string(MPV_ERROR_INVALID_PARAMETER));
            return;
        }
        const char *text = *(const char *const *)value;
        if (!text || (i == 0 && !g_str_equal(text, "no") && !g_str_equal(text, "filter") &&
            !g_str_equal(text, "same")) || (i == 1 && !g_str_equal(text, "auto") &&
            !g_str_equal(text, "lazy") && !g_str_equal(text, "recursive") &&
            !g_str_equal(text, "ignore"))) {
            show_error(p, mpv_error_string(MPV_ERROR_INVALID_PARAMETER));
            return;
        }
        g_autofree char *choice = g_strdup(text);
        g_autoptr(GError) error = NULL;
        gboolean saved = i >= 2 ? config_set_list(&p->config, name, choice, &error)
                               : config_set(&p->config, name, choice, &error);
        if (!saved) {
            show_error(p, error->message);
            if (p->preferences)
                preferences_window_update(p->preferences, &p->preference_values);
            return;
        }
        int result;
        if (i >= 2 && !*choice) {
            /* An empty STRING represents one empty element in mpv. Only an
             * empty NODE array clears the list (all files for the filter). */
            mpv_node_list empty = {0};
            mpv_node node = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &empty};
            result = mpv_set_property_async(p->mpv, 0, name, MPV_FORMAT_NODE, &node);
        } else {
            const char *selected = choice;
            result = mpv_set_property_async(p->mpv, 0, name, MPV_FORMAT_STRING, &selected);
        }
        if (result < 0)
            show_error(p, mpv_error_string(result));
        else
            cache_playlist_preference(p, i, choice);
        return;
    }
    if (g_str_equal(name, "ao") || g_str_equal(name, "audio-device")) {
        change_audio_preferences(p, name, *(const char *const *)value);
        return;
    }
    if (g_str_equal(name, "ui-cursor-hide-delay")) {
        g_autoptr(GError) error = NULL;
        double seconds = *(const double *)value;
        if (!i18n_save_cursor_hide_delay(seconds, &error)) {
            show_error(p, error->message);
            if (p->preferences)
                preferences_window_update(p->preferences, &p->preference_values);
            return;
        }
        p->preference_values.cursor_hide_delay = i18n_cursor_hide_delay();
        gint64 now = g_get_monotonic_time();
        cursor_set_delay(&p->cursor_state,
            (int64_t)llround(p->preference_values.cursor_hide_delay * 1000000.0), now);
        update_controls_visibility(p, now);
        return;
    }
    if (g_str_equal(name, "ui-language")) {
        const char *language = *(const char *const *)value;
        g_autoptr(GError) error = NULL;
        if (!i18n_save_language(language, &error)) {
            show_error(p, error->message);
            return;
        }
        i18n_set_language(language);
        p->preference_values.language = i18n_language();
        if (!p->language_source)
            p->language_source = g_idle_add(language_changed_idle, p);
        return;
    }
    if (g_str_equal(name, "gpu-api")) {
        p->pending_gpu_api = *(const char *const *)value;
        return;
    }
    if (g_str_equal(name, "opengl-es")) {
        g_autoptr(GError) error = NULL;
        const char *es = *(const char *const *)value;
        const char *api = p->pending_gpu_api ? p->pending_gpu_api : "auto";
        const char *names[] = {"gpu-api", "opengl-es"};
        const char *values[] = {api, es};
        if (!g_str_equal(api, "auto") && !g_str_equal(api, "opengl")) {
            show_error(p, tr("Встроенный видеовывод поддерживает OpenGL и OpenGL ES."));
        } else if (!config_set_options(&p->config, names, values, G_N_ELEMENTS(names), &error)) {
            show_error(p, error->message);
        } else {
            p->preference_values.gpu_api = g_str_equal(es, "yes") ? "opengl-es"
                : g_str_equal(api, "opengl") ? "opengl" : "auto";
        }
        p->pending_gpu_api = NULL;
        if (p->preferences)
            preferences_window_update(p->preferences, &p->preference_values);
        return;
    }
    gboolean hardware = g_str_equal(name, "hwdec");
    if (hardware) {
        g_autofree char *choice = g_strdup(*(const char *const *)value);
        g_autoptr(GError) error = NULL;
        if (!config_set(&p->config, name, choice, &error)) {
            show_error(p, error->message);
            if (p->preferences)
                preferences_window_update(p->preferences, &p->preference_values);
            return;
        }
        if (p->cuda_interop && !cuda_interop_handles(choice)) {
            g_free(p->pending_hwdec);
            p->pending_hwdec = g_strdup(choice);
            p->preference_values.hwdec = p->pending_hwdec;
            p->preference_values.hwdec_restart_required = TRUE;
            p->preference_values.hwdec_pending = FALSE;
        } else {
            const char *selected = choice;
            int result = mpv_set_property_async(p->mpv, HWDEC_SET_ID, name, MPV_FORMAT_STRING, &selected);
            if (result < 0) {
                show_error(p, mpv_error_string(result));
                return;
            }
            g_clear_pointer(&p->pending_hwdec, g_free);
            g_free(p->hwdec);
            p->hwdec = g_strdup(choice);
            p->preference_values.hwdec = p->hwdec;
            p->preference_values.hwdec_restart_required = FALSE;
            p->preference_values.hwdec_pending = p->timeline.loaded;
            p->preference_values.hwdec_known = FALSE;
            p->hwdec_refresh_at = g_get_monotonic_time() + 200000;
        }
        if (p->preferences)
            preferences_window_update(p->preferences, &p->preference_values);
        return;
    }
    int result = mpv_set_property_async(p->mpv, 0, name, format, (void *)value);
    if (result < 0) {
        show_error(p, mpv_error_string(result));
        return;
    }
    char number[G_ASCII_DTOSTR_BUF_SIZE];
    const char *text = NULL;
    if (format == MPV_FORMAT_STRING)
        text = *(const char *const *)value;
    else if (format == MPV_FORMAT_FLAG)
        text = *(const int *)value ? "yes" : "no";
    else if (format == MPV_FORMAT_DOUBLE)
        text = g_ascii_dtostr(number, sizeof(number), *(const double *)value);
    if (text) {
        g_autoptr(GError) error = NULL;
        if (!config_set(&p->config, name, text, &error))
            show_error(p, error->message);
    }
}

static void load_preference_keys(GtkWindow *window, gpointer data)
{
    Player *p = data;
    p->keys_loaded = TRUE;
    preferences_window_configure_keys(window, config_input_path(&p->config),
        app_key_defaults, G_N_ELEMENTS(app_key_defaults), change_key_binding, p);
    if (!p->mpv || p->closing || p->quit_requested)
        return;
    mpv_get_property_async(p->mpv, INPUT_BINDINGS_REFRESH_ID, "input-bindings", MPV_FORMAT_NODE);
    mpv_get_property_async(p->mpv, COMMAND_LIST_REFRESH_ID, "command-list", MPV_FORMAT_NODE);
}

static void load_preference_audio(GtkWindow *window, gboolean active, gpointer data)
{
    (void)window;
    Player *p = data;
    gboolean was_active = p->audio_observed;
    observe_group(p, audio_properties, G_N_ELEMENTS(audio_properties),
                  AUDIO_OBSERVE_BASE, active, &p->audio_observed);
    if (p->audio_observed && !was_active)
        mpv_get_property_async(p->mpv, AUDIO_OUTPUTS_REFRESH_ID,
                              "option-info/ao/choices", MPV_FORMAT_NODE);
}

static void load_preference_playlist(GtkWindow *window, guint groups, gpointer data)
{
    (void)window;
    Player *p = data;
    observe_group(p, playlist_preference_properties, 3, PLAYLIST_PREFS_OBSERVE_BASE,
                  (groups & PREFERENCES_PLAYLIST_BASIC) != 0,
                  &p->playlist_preferences_observed);
    observe_group(p, playlist_preference_properties + 3,
                  G_N_ELEMENTS(playlist_preference_properties) - 3,
                  PLAYLIST_PREFS_OBSERVE_BASE + 3,
                  (groups & PREFERENCES_PLAYLIST_EXTENSIONS) != 0,
                  &p->playlist_extensions_observed);
}

static void show_preferences(Player *p)
{
    if (!p->mpv) {
        p->settings_requested = TRUE;
        return;
    }
    if (!p->preferences) {
        p->keys_loaded = FALSE;
        p->input_refresh_at = 0;
        p->preferences = preferences_window_new(p->window, &p->preference_values,
            config_path(&p->config), preference_changed, open_config, p);
        preferences_window_set_keys_loader(p->preferences, load_preference_keys, p);
        preferences_window_set_audio_loader(p->preferences, load_preference_audio, p);
        preferences_window_set_playlist_loader(p->preferences, load_preference_playlist, p);
        g_signal_connect(p->preferences, "notify::visible", G_CALLBACK(optional_view_changed), p);
        g_object_add_weak_pointer(G_OBJECT(p->preferences), (gpointer *)&p->preferences);
    }
    gtk_window_present(p->preferences);
    update_controls_visibility(p, g_get_monotonic_time());
}

static gboolean install_key_edits(Player *p, GError **error)
{
    g_autofree char *contents = keybindings_managed_contents(config_input_path(&p->config), error);
    if (!contents)
        return FALSE;
    const char *define[] = {"define-section", KEYBINDINGS_EDIT_SECTION, contents, "force", NULL};
    int result = mpv_command_async(p->mpv, INPUT_EDIT_DEFINE_ID, define);
    if (result >= 0) {
        const char *enable[] = {"enable-section", KEYBINDINGS_EDIT_SECTION, NULL};
        result = mpv_command_async(p->mpv, INPUT_EDIT_ENABLE_ID, enable);
    }
    if (result < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", mpv_error_string(result));
        return FALSE;
    }
    return TRUE;
}

static gboolean change_key_binding(const char *old_key, const char *key,
                                   const char *command_text, gboolean reset,
                                   GError **error, gpointer data)
{
    Player *p = data;
    if (p->closing || p->quit_requested) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, tr("Проигрыватель закрывается."));
        return FALSE;
    }
    const char *path = config_input_path(&p->config);
    gboolean saved;
    if (reset)
        saved = keybindings_reset(path, key, error);
    else if (old_key && !g_str_equal(old_key, key))
        saved = keybindings_rename(path, old_key, key, command_text, error);
    else
        saved = keybindings_set(path, key, command_text, error);
    if (!saved)
        return FALSE;
    /* Keep the default section in the enabled stack while replacing its
     * user definitions. Named app fallbacks and other sections survive. */
    const char *clear[] = {"define-section", "default", "# mpv-gtk reload\n", "force", NULL};
    p->keys_reload_pending = TRUE;
    int result = mpv_command_async(p->mpv, INPUT_RELOAD_CLEAR_ID, clear);
    if (result >= 0) {
        const char *reload[] = {"load-input-conf", path, NULL};
        result = mpv_command_async(p->mpv, INPUT_RELOAD_FILE_ID, reload);
    }
    if (result < 0) {
        p->keys_reload_pending = FALSE;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", mpv_error_string(result));
        return FALSE;
    }
    return TRUE;
}

static GtkWidget *info_row(GtkBox *content, const char *name, GtkWidget **value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_margin_top(row, 9);
    gtk_widget_set_margin_bottom(row, 9);
    GtkWidget *label = translated_label(name);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_widget_set_size_request(label, 128, -1);
    gtk_box_append(GTK_BOX(row), label);
    *value = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(*value), 1);
    gtk_label_set_wrap(GTK_LABEL(*value), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(*value), 42);
    gtk_label_set_selectable(GTK_LABEL(*value), TRUE);
    gtk_widget_set_hexpand(*value, TRUE);
    gtk_box_append(GTK_BOX(row), *value);
    gtk_box_append(content, row);
    return row;
}

static void update_info_view(Player *p, PlaybackInfoView *view)
{
    gboolean loaded = p->timeline.loaded;
    PlaybackValues *v = &p->playback;
    gtk_label_set_text(GTK_LABEL(view->title), loaded
        ? gtk_label_get_text(GTK_LABEL(p->title)) : tr("Нет открытого файла"));
    g_autofree char *codec = v->video_format ? g_ascii_strup(v->video_format, -1) : g_strdup("—");
    g_autofree char *video = NULL;
    if (loaded && v->width > 0 && v->height > 0) {
        if (v->fps > 0)
            video = g_strdup_printf("%s · %" G_GINT64_FORMAT " × %" G_GINT64_FORMAT " · %.3f FPS",
                                    codec, v->width, v->height, v->fps);
        else
            video = g_strdup_printf("%s · %" G_GINT64_FORMAT " × %" G_GINT64_FORMAT,
                                    codec, v->width, v->height);
    }
    gtk_label_set_text(GTK_LABEL(view->video), video ? video : !loaded ? "—"
        : p->preference_values.has_video ? tr("Определяется…") : tr("Нет видеодорожки"));
    g_autofree char *decoder = preferences_decoder_status(&p->preference_values);
    gtk_label_set_text(GTK_LABEL(view->decoder), decoder);
    gtk_widget_set_tooltip_text(view->decoder, v->video_codec);
    const char *context = "—";
    if (loaded && p->area) {
        GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(p->area));
        if (g_str_equal(G_OBJECT_TYPE_NAME(display), "GdkWaylandDisplay"))
            context = "wayland";
    }
    gtk_label_set_text(GTK_LABEL(view->context), context);
    g_autofree char *audio = NULL;
    if (loaded && v->audio_codec) {
        if (v->sample_rate > 0)
            audio = g_strdup_printf("%s · %.1f kHz%s%s", v->audio_codec, v->sample_rate / 1000.0,
                                     v->channels ? " · " : "", v->channels ? v->channels : "");
        else
            audio = g_strdup(v->audio_codec);
    }
    gtk_label_set_text(GTK_LABEL(view->audio), audio ? audio
                        : loaded ? tr("Нет аудиодорожки") : "—");
    g_autofree char *dropped = loaded && v->drops_known
        ? g_strdup_printf("%" G_GINT64_FORMAT, v->dropped) : g_strdup("—");
    g_autofree char *decoder_dropped = loaded && v->decoder_drops_known
        ? g_strdup_printf("%" G_GINT64_FORMAT, v->decoder_dropped) : g_strdup("—");
    g_autofree char *drops = g_strdup_printf(tr("Вывод: %s · декодер: %s"), dropped, decoder_dropped);
    gtk_label_set_text(GTK_LABEL(view->drops), drops);
    g_autofree char *sync = loaded && v->avsync_known
        ? g_strdup_printf("%+.1f %s", v->avsync * 1000, tr("мс")) : g_strdup("—");
    gtk_label_set_text(GTK_LABEL(view->sync), sync);
    gtk_widget_set_visible(view->cache_row, loaded && v->cache > 0);
    g_autofree char *cache = g_strdup_printf("%.1f %s", v->cache, tr("с"));
    gtk_label_set_text(GTK_LABEL(view->cache), cache);
}

static void update_playback_info(Player *p)
{
    if (p->info_window)
        update_info_view(p, &p->popup_info);
    if (p->info_overlay && gtk_widget_get_visible(p->info_overlay))
        update_info_view(p, &p->overlay_info);
}

static GtkWidget *build_info_view(PlaybackInfoView *view, gboolean overlay)
{
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    view->title = gtk_label_new("");
    gtk_widget_add_css_class(view->title, "heading");
    gtk_label_set_xalign(GTK_LABEL(view->title), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(view->title), 48);
    gtk_label_set_ellipsize(GTK_LABEL(view->title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_bottom(view->title, 10);
    gtk_box_append(GTK_BOX(content), view->title);
    info_row(GTK_BOX(content), "Видео", &view->video);
    info_row(GTK_BOX(content), "Декодер", &view->decoder);
    info_row(GTK_BOX(content), "Контекст", &view->context);
    info_row(GTK_BOX(content), "Звук", &view->audio);
    info_row(GTK_BOX(content), "Пропуски кадров", &view->drops);
    info_row(GTK_BOX(content), "Синхронизация A/V", &view->sync);
    view->cache_row = info_row(GTK_BOX(content), "Буфер", &view->cache);
    if (overlay) {
        gtk_widget_set_can_target(content, FALSE);
        GtkWidget *values[] = {view->video, view->decoder, view->context, view->audio,
                              view->drops, view->sync, view->cache};
        for (guint i = 0; i < G_N_ELEMENTS(values); ++i)
            gtk_label_set_selectable(GTK_LABEL(values[i]), FALSE);
        gtk_widget_add_css_class(content, "playback-info-overlay");
    }
    return content;
}

static void toggle_playback_info(Player *p)
{
    if (!p->info_overlay) {
        p->info_overlay = build_info_view(&p->overlay_info, TRUE);
        gtk_widget_set_halign(p->info_overlay, GTK_ALIGN_START);
        gtk_widget_set_valign(p->info_overlay, GTK_ALIGN_START);
        gtk_widget_set_margin_start(p->info_overlay, 14);
        gtk_widget_set_margin_top(p->info_overlay, 14);
        gtk_widget_set_margin_end(p->info_overlay, 14);
        gtk_overlay_add_overlay(GTK_OVERLAY(p->video_overlay), p->info_overlay);
        gtk_overlay_set_measure_overlay(GTK_OVERLAY(p->video_overlay), p->info_overlay, FALSE);
        gtk_widget_set_visible(p->info_overlay, FALSE);
        g_signal_connect(p->info_overlay, "notify::visible", G_CALLBACK(optional_view_changed), p);
    }
    gtk_widget_set_visible(p->info_overlay, !gtk_widget_get_visible(p->info_overlay));
    update_metadata_observers(p);
    if (playback_info_visible(p))
        request_playback_values(p);
    update_playback_info(p);
    p->info_refresh_at = g_get_monotonic_time() + 1000000;
}

static void info_secondary_click(GtkGestureClick *gesture, int presses,
                                  double x, double y, gpointer data)
{
    (void)presses; (void)x; (void)y;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    show_playback_info(data);
}

static void show_playback_info(Player *p)
{
    if (!p->info_window) {
        p->info_window = GTK_WINDOW(gtk_window_new());
        g_object_add_weak_pointer(G_OBJECT(p->info_window), (gpointer *)&p->info_window);
        gtk_window_set_title(p->info_window, tr("Информация о воспроизведении"));
        gtk_window_set_transient_for(p->info_window, p->window);
        gtk_window_set_destroy_with_parent(p->info_window, TRUE);
        g_signal_connect(p->info_window, "notify::visible", G_CALLBACK(optional_view_changed), p);
        gtk_window_set_default_size(p->info_window, 560, 420);
        GtkWidget *header = gtk_header_bar_new();
        gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header),
            translated_label("Информация о воспроизведении"));
        gtk_window_set_titlebar(p->info_window, header);
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                        GTK_POLICY_AUTOMATIC);
        GtkWidget *content = build_info_view(&p->popup_info, FALSE);
        gtk_widget_set_margin_start(content, 22);
        gtk_widget_set_margin_end(content, 22);
        gtk_widget_set_margin_top(content, 16);
        gtk_widget_set_margin_bottom(content, 18);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
        gtk_window_set_child(p->info_window, scroll);
    }
    update_playback_info(p);
    p->info_refresh_at = g_get_monotonic_time() + 1000000;
    gtk_window_present(p->info_window);
    update_metadata_observers(p);
    request_playback_values(p);
    update_controls_visibility(p, g_get_monotonic_time());
}

static void set_fullscreen(Player *p, gboolean fullscreen)
{
    if (p->closing || p->quit_requested)
        return;
    if (fullscreen)
        gtk_window_fullscreen(p->window);
    else
        gtk_window_unfullscreen(p->window);
}

static void fullscreen_changed(GObject *window, GParamSpec *spec, gpointer data)
{
    (void)spec;
    Player *p = data;
    if (p->closing || p->quit_requested)
        return;
    p->fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(window));
    set_flag(p, "fullscreen", p->fullscreen);
    gtk_widget_set_visible(p->header, !p->fullscreen);
    /* Window coordinates change on this transition; an old hover near the
     * window's bottom must not hold the fullscreen panel open forever. */
    p->pointer_bottom = FALSE;
    p->controls_pressed = FALSE;
    controls_set_fullscreen(&p->controls_state, p->fullscreen, g_get_monotonic_time());
    cursor_set_fullscreen(&p->cursor_state, p->fullscreen, g_get_monotonic_time());
    /* Keep the windowed transport docked below the video. Only the HUD is
     * moved into an overlay in fullscreen; the GLArea stays realized. */
    GtkWidget *parent = gtk_widget_get_parent(p->controls_revealer);
    if ((p->fullscreen && parent != p->stage) || (!p->fullscreen && parent != p->root)) {
        g_object_ref(p->controls_revealer);
        if (parent == p->stage)
            gtk_overlay_remove_overlay(GTK_OVERLAY(p->stage), p->controls_revealer);
        else if (parent == p->root)
            gtk_box_remove(GTK_BOX(p->root), p->controls_revealer);
        if (p->fullscreen) {
            gtk_widget_set_valign(p->controls_revealer, GTK_ALIGN_END);
            gtk_overlay_add_overlay(GTK_OVERLAY(p->stage), p->controls_revealer);
            gtk_overlay_set_measure_overlay(GTK_OVERLAY(p->stage), p->controls_revealer, FALSE);
        } else {
            gtk_widget_set_valign(p->controls_revealer, GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(p->root), p->controls_revealer);
        }
        g_object_unref(p->controls_revealer);
    }
    if (p->fullscreen) {
        gtk_widget_add_css_class(p->controls, "fullscreen-controls");
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->sidebar), FALSE);
    } else {
        gtk_widget_remove_css_class(p->controls, "fullscreen-controls");
    }
    position_track_notice(p);
    update_controls_visibility(p, g_get_monotonic_time());
}

static void update_app_menu(Player *p)
{
    if (!p->app_menu)
        return;
    g_menu_remove_all(p->app_menu);
    g_menu_append(p->app_menu, tr("Новое окно"), "app.new-window");
    g_menu_append(p->app_menu, tr("О программе"), "app.about");
}

static void show_about(Player *p)
{
    if (!p->about_window) {
        p->about_window = about_window_new(p->window, APP_VERSION);
        g_object_add_weak_pointer(G_OBJECT(p->about_window), (gpointer *)&p->about_window);
    }
    gtk_window_present(p->about_window);
}

static void new_window(Player *p)
{
    g_autoptr(GError) error = NULL;
    /* Resolve the real executable, so launches through PATH or a symlink keep
     * using the same player and its sibling configuration files. */
    g_autofree char *executable = g_file_read_link("/proc/self/exe", &error);
    if (!executable) {
        show_error(p, error->message);
        return;
    }
    /* An upgrade may replace the running executable while this window is
     * still open. Launch the current installed file at the original path. */
    const char deleted_suffix[] = " (deleted)";
    if (g_str_has_suffix(executable, deleted_suffix))
        executable[strlen(executable) - strlen(deleted_suffix)] = '\0';
    /* A separate process owns the new window and libmpv instance. No command
     * line media or test arguments are inherited, and closing this window
     * does not terminate the child. */
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    if (p->default_gsk_renderer)
        g_subprocess_launcher_unsetenv(launcher, "GSK_RENDERER");
    g_autoptr(GSubprocess) process = g_subprocess_launcher_spawn(launcher, &error,
                                                               executable, NULL);
    if (!process)
        show_error(p, error->message);
}

static void action_activated(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    Player *p = data;
    if (p->closing || p->quit_requested)
        return;
    const char *name = g_action_get_name(G_ACTION(action));
    if (g_str_equal(name, "new-window")) new_window(p);
    else if (g_str_equal(name, "about")) show_about(p);
    else if (g_str_equal(name, "open")) show_files(p);
    else if (g_str_equal(name, "url")) show_url(p);
    else if (g_str_equal(name, "play")) cycle_pause(p);
    else if (g_str_equal(name, "previous")) simple_command(p, "playlist-prev");
    else if (g_str_equal(name, "next")) simple_command(p, "playlist-next");
    else if (g_str_equal(name, "settings")) show_preferences(p);
    else if (g_str_equal(name, "info")) toggle_playback_info(p);
    else if (g_str_equal(name, "trim")) trim_mode_toggle(p->trim);
    else if (g_str_equal(name, "config")) open_config("mpv.conf", p);
    else if (g_str_equal(name, "fullscreen"))
        set_fullscreen(p, !p->fullscreen);
    else if (g_str_equal(name, "mute")) {
        const char *args[] = {"cycle", "mute", NULL};
        command(p, args);
    } else if (g_str_equal(name, "queue")) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->sidebar),
            !gtk_revealer_get_reveal_child(GTK_REVEALER(p->sidebar)));
    } else if (g_str_equal(name, "quit")) gtk_window_close(p->window);
    else if (g_str_equal(name, "audio") || g_str_equal(name, "subtitle")) {
        if (g_str_equal(name, "subtitle")) {
            p->initial_subtitle_reset_pending = FALSE;
            p->initial_subtitle_pending_cycles = 0;
        }
        hide_track_notice(p);
        set_string(p, g_str_equal(name, "audio") ? "aid" : "sid",
                   g_variant_get_string(parameter, NULL));
        request_track_notice(p, g_str_equal(name, "audio"), TRUE);
    } else if (g_str_equal(name, "speed")) {
        set_number(p, "speed", g_ascii_strtod(g_variant_get_string(parameter, NULL), NULL));
    }
}

static void app_key_action(Player *p, const char *action)
{
    if (p->closing || p->quit_requested)
        return;
    if (g_str_equal(action, "chapter-previous")) {
        step_chapter(-1, p);
        return;
    }
    if (g_str_equal(action, "chapter-next")) {
        step_chapter(1, p);
        return;
    }
    if (!p->window)
        return;
    if (g_str_equal(action, "open-file")) show_files(p);
    else if (g_str_equal(action, "open-url")) show_url(p);
    else if (g_str_equal(action, "settings")) show_preferences(p);
    else if (g_str_equal(action, "info")) toggle_playback_info(p);
    else if (g_str_equal(action, "trim")) trim_mode_toggle(p->trim);
    else if (g_str_equal(action, "fullscreen"))
        set_fullscreen(p, !p->fullscreen);
    else if (g_str_equal(action, "quit")) gtk_window_close(p->window);
    else if (g_str_equal(action, "focus-video")) gtk_widget_grab_focus(player_video_widget(p));
    else if (g_str_equal(action, "queue")) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->sidebar),
            !gtk_revealer_get_reveal_child(GTK_REVEALER(p->sidebar)));
    } else if (g_str_equal(action, "escape")) {
        if (p->info_overlay && gtk_widget_get_visible(p->info_overlay))
            toggle_playback_info(p);
        else if (p->fullscreen)
            set_fullscreen(p, FALSE);
    } else if (g_str_equal(action, "focus-next") || g_str_equal(action, "focus-previous")) {
        if (p->fullscreen && !p->controls_state.visible) {
            controls_update(&p->controls_state, TRUE, g_get_monotonic_time());
            gtk_revealer_set_reveal_child(GTK_REVEALER(p->controls_revealer), TRUE);
            gtk_widget_set_can_target(p->controls_revealer, TRUE);
            gtk_widget_grab_focus(p->play_button);
        } else {
            gtk_widget_child_focus(p->root, g_str_equal(action, "focus-next")
                                    ? GTK_DIR_TAB_FORWARD : GTK_DIR_TAB_BACKWARD);
        }
    } else if (g_str_equal(action, "activate")) {
        GtkWidget *focus = gtk_window_get_focus(p->window);
        if (focus)
            gtk_widget_activate(focus);
    } else if (g_str_equal(action, "context-menu")) {
        GtkWidget *focus = gtk_window_get_focus(p->window);
        if (focus && (focus == p->mute_button || gtk_widget_is_ancestor(focus, p->audio_context)))
            gtk_popover_bin_popup(GTK_POPOVER_BIN(p->audio_context));
        else {
            while (focus && !GTK_IS_MENU_BUTTON(focus))
                focus = gtk_widget_get_parent(focus);
            if (focus)
                gtk_menu_button_popup(GTK_MENU_BUTTON(focus));
        }
    }
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint key,
                             guint keycode, GdkModifierType state, gpointer data)
{
    (void)controller; (void)keycode;
    Player *p = data;
    if (p->closing || p->quit_requested)
        return TRUE;
    if (popover_visible(gtk_popover_bin_get_popover(GTK_POPOVER_BIN(p->audio_context))) ||
        gtk_menu_button_get_active(GTK_MENU_BUTTON(p->subtitle_button)) ||
        gtk_menu_button_get_active(GTK_MENU_BUTTON(p->speed_button)) ||
        (p->chapter_button && gtk_menu_button_get_active(GTK_MENU_BUTTON(p->chapter_button))) ||
        (p->app_menu_button && gtk_menu_button_get_active(GTK_MENU_BUTTON(p->app_menu_button))) ||
        trim_mode_audio_menu_open(p->trim))
        return FALSE;
    GtkWidget *focus = gtk_window_get_focus(p->window);
    if (focus && GTK_IS_EDITABLE(focus))
        return FALSE;
    g_autofree char *input_key = input_key_name(key, state);
    if (!input_key)
        return FALSE;
    /* mpv resolves both single keys and sequences. Frontend defaults are a
     * lower-priority input section, so edited bindings can replace any key. */
    const char *args[] = {"keypress", input_key, NULL};
    startup_trace_mark("keyboard input queued");
    command(p, args);
    if (p->preferences && p->keys_loaded && !p->input_refresh_at)
        p->input_refresh_at = g_get_monotonic_time() + 200000;
    return TRUE;
}

static char *pointer_key_name(GtkEventController *controller, const char *name)
{
    GdkModifierType modifiers = gtk_event_controller_get_current_event_state(controller);
    GString *key = g_string_new(NULL);
    if (modifiers & GDK_SHIFT_MASK) g_string_append(key, "Shift+");
    if (modifiers & GDK_CONTROL_MASK) g_string_append(key, "Ctrl+");
    if (modifiers & GDK_ALT_MASK) g_string_append(key, "Alt+");
    if (modifiers & (GDK_SUPER_MASK | GDK_META_MASK)) g_string_append(key, "Meta+");
    g_string_append(key, name);
    return g_string_free(key, FALSE);
}

static void video_clicked(GtkGestureClick *gesture, int n, double x, double y, gpointer data)
{
    (void)x; (void)y;
    Player *p = data;
    if (p->closing || p->quit_requested)
        return;
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    const char *name = button == GDK_BUTTON_PRIMARY ? (n == 2 ? "MBTN_LEFT_DBL" : "MBTN_LEFT")
                     : button == GDK_BUTTON_MIDDLE ? (n == 2 ? "MBTN_MID_DBL" : "MBTN_MID")
                     : button == GDK_BUTTON_SECONDARY ? (n == 2 ? "MBTN_RIGHT_DBL" : "MBTN_RIGHT")
                     : button == 8 ? "MBTN_BACK" : button == 9 ? "MBTN_FORWARD" : NULL;
    g_autofree char *extra = !name && button >= 10 && button <= 20
                          ? g_strdup_printf("MBTN%u", button - 1) : NULL;
    if (!name)
        name = extra;
    if (!name)
        return;
    gtk_widget_grab_focus(player_video_widget(p));
    g_autofree char *key = pointer_key_name(GTK_EVENT_CONTROLLER(gesture), name);
    const char *args[] = {"keypress", key, NULL};
    command(p, args);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static gboolean video_scrolled(GtkEventControllerScroll *controller, double dx, double dy,
                                gpointer data)
{
    Player *p = data;
    if (p->closing || p->quit_requested)
        return TRUE;
    const char *name = fabs(dy) >= fabs(dx) ? (dy < 0 ? "WHEEL_UP" : "WHEEL_DOWN")
                                           : (dx < 0 ? "WHEEL_LEFT" : "WHEEL_RIGHT");
    if (dx == 0 && dy == 0)
        return FALSE;
    g_autofree char *key = pointer_key_name(GTK_EVENT_CONTROLLER(controller), name);
    const char *args[] = {"keypress", key, NULL};
    command(p, args);
    return TRUE;
}

static gboolean scrub_release_idle(gpointer data)
{
    Player *p = data;
    p->scrub_release_source = 0;
    /* GtkRange may apply its final value after the capture-phase release. */
    if (p->dragging) scrub_end(p, gtk_range_get_value(GTK_RANGE(p->seek)));
    return G_SOURCE_REMOVE;
}

static void seek_unmapped(GtkWidget *widget, gpointer data)
{
    (void)widget;
    Player *p = data;
    if (p->dragging) scrub_cancel(p);
}

static gboolean seek_event(GtkEventControllerLegacy *controller, GdkEvent *event,
                            gpointer data)
{
    (void)controller;
    Player *p = data;
    GdkEventType type = gdk_event_get_event_type(event);
    if ((type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE) &&
        gdk_button_event_get_button(event) != GDK_BUTTON_PRIMARY) return FALSE;
    if (type == GDK_BUTTON_PRESS || type == GDK_TOUCH_BEGIN) {
        scrub_begin(p);
        p->preview = gtk_range_get_value(GTK_RANGE(p->seek));
    } else if (type == GDK_TOUCH_CANCEL || type == GDK_GRAB_BROKEN) {
        scrub_cancel(p);
    } else if ((type == GDK_BUTTON_RELEASE || type == GDK_TOUCH_END) && p->dragging) {
        if (!p->scrub_release_source)
            p->scrub_release_source = g_idle_add(scrub_release_idle, p);
    }
    if (type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE ||
        type == GDK_TOUCH_BEGIN || type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL)
        update_controls_visibility(p, g_get_monotonic_time());
    return FALSE;
}

static void seek_changed(GtkRange *range, gpointer data)
{
    Player *p = data;
    if (p->updating || p->closing || p->quit_requested)
        return;
    p->preview = gtk_range_get_value(range);
    update_time(p, p->preview);
    if (p->dragging)
        scrub_queue(p, p->preview, g_get_monotonic_time());
    else
        seek_absolute(p, p->preview);
}

static void volume_changed(GtkRange *range, gpointer data)
{
    Player *p = data;
    if (!p->updating && !p->closing && !p->quit_requested)
        set_number(p, "volume", gtk_range_get_value(range));
}

static void dismiss_error(GtkButton *button, gpointer data)
{
    (void)button;
    Player *p = data;
    gtk_widget_set_visible(p->error_bar, FALSE);
}

static void prepare_close(Player *p)
{
    if (p->close_prepared)
        return;
    p->close_prepared = TRUE;
    scrub_cancel(p);
    p->quit_requested = TRUE;
    if (p->mpris_source) { g_source_remove(p->mpris_source); p->mpris_source = 0; }
    seek_preview_shutdown(p->seek_preview);
    trim_mode_shutdown(p->trim);
    startup_trace_mark("window close requested");
    /* Hide without unrealizing: libmpv still needs this GL context to return
     * queued frames while its native quit command saves and unloads media. */
    GdkDisplay *display = NULL;
    GtkWindow *windows[] = {p->window, p->preferences, p->info_window,
                           p->about_window, p->url_dialog};
    for (guint i = 0; i < G_N_ELEMENTS(windows); ++i) {
        if (windows[i]) {
            if (!display)
                display = gtk_widget_get_display(GTK_WIDGET(windows[i]));
            gtk_widget_set_visible(GTK_WIDGET(windows[i]), FALSE);
        }
    }
    if (display)
        gdk_display_flush(display);
    startup_trace_mark("windows hidden and flushed");
    mpris_art_free(p->mpris_art);
    p->mpris_art = NULL;
    mpris_free(p->mpris);
    p->mpris = NULL;
    /* Keep closing false until the final shutdown acknowledgement so pending
     * preference edits can still be persisted before a WM-initiated quit. */
    if (p->preferences)
        preferences_window_commit(p->preferences);
    clear_track_notices(p);
    cursor_timeout_cancel(p);
    p->dragging = p->scrub_pending = FALSE;
    p->hwdec_refresh_at = p->input_refresh_at = p->info_refresh_at = 0;
    if (p->language_source) {
        g_source_remove(p->language_source);
        p->language_source = 0;
    }
    if (p->test_source) {
        g_source_remove(p->test_source);
        p->test_source = 0;
    }
    if (p->file_cancel)
        g_cancellable_cancel(p->file_cancel);
    if (p->config_launch_cancel)
        g_cancellable_cancel(p->config_launch_cancel);
}

static gboolean close_window(GtkWindow *window, gpointer data)
{
    (void)window;
    Player *p = data;
    if (p->closing)
        return FALSE;
    gboolean already_requested = p->quit_requested;
    prepare_close(p);
    if (p->mpv && !p->shutdown_received) {
        if (already_requested) {
            continue_render_load(p);
            return TRUE;
        }
        const char *args[] = {"quit", NULL};
        /* Bypass the guarded command helper only for this one native quit.
         * Native quit-watch-later reaches us via END_FILE/SHUTDOWN instead. */
        if (mpv_command_async(p->mpv, 0, args) >= 0) {
            startup_trace_mark("mpv quit queued");
            continue_render_load(p);
            return TRUE;
        }
        startup_trace_mark("mpv quit request failed; final cleanup");
    }
    p->closing = TRUE;
    continue_render_load(p);
    if (p->preferences)
        gtk_window_destroy(p->preferences);
    if (p->info_window)
        gtk_window_destroy(p->info_window);
    if (p->about_window)
        gtk_window_destroy(p->about_window);
    if (p->url_dialog) {
        gtk_window_destroy(p->url_dialog);
        p->url_dialog = NULL;
    }
    return FALSE;
}

static GtkWidget *icon_button(const char *icon, const char *tooltip, const char *action)
{
    GtkWidget *button = icon ? gtk_button_new_from_icon_name(icon) : gtk_button_new();
    translated_tooltip(button, tooltip);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "control-button");
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    return button;
}

static void install_actions(Player *p)
{
    const char *names[] = {"open", "url", "play", "previous", "next", "fullscreen",
                          "mute", "queue", "quit", "audio", "subtitle", "speed",
                          "settings", "config", "info", "new-window", "about", "trim", NULL};
    for (int i = 0; names[i]; ++i) {
        gboolean stateful = i >= 9 && i <= 11;
        GSimpleAction *action = stateful
            ? g_simple_action_new_stateful(names[i], G_VARIANT_TYPE_STRING,
                g_variant_new_string(i == 9 ? "auto" : i == 10 ? "no" : "1"))
            : g_simple_action_new(names[i], NULL);
        g_signal_connect(action, "activate", G_CALLBACK(action_activated), p);
        g_action_map_add_action(G_ACTION_MAP(p->application), G_ACTION(action));
        g_object_unref(action);
    }
    /* Keyboard shortcuts are defined through mpv below, so user edits also
     * replace Ctrl+O/Ctrl+P rather than competing with GTK accelerators. */
}

static gboolean init_mpv_core(Player *p)
{
    if (!p->config.path || !p->config.input_path) {
        g_autoptr(GError) error = NULL;
        if (!config_init(&p->config, &error)) {
            g_printerr("mpv-gtk: %s\n", error->message);
            return FALSE;
        }
    }
    p->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (p->wake_fd < 0) {
        g_printerr("eventfd: %s\n", g_strerror(errno));
        return FALSE;
    }
    p->mpv = mpv_create();
    if (!p->mpv)
        return FALSE;
    mpv_set_option_string(p->mpv, "config", "no");
    /* These are defaults. The player's own config, includes and profiles
     * select the actual stock VO and graphics API below. */
    mpv_set_option_string(p->mpv, "vo", "libmpv");
    mpv_set_option_string(p->mpv, "hwdec", "auto-safe");
    mpv_set_option_string(p->mpv, "volume", "50");
    mpv_set_option_string(p->mpv, "autocreate-playlist", "no");
    g_autofree char *config_directory = g_path_get_dirname(config_path(&p->config));
    mpv_set_option_string(p->mpv, "config-dir", config_directory);
    /* config=no also disables mpv's automatic state-directory lookup. Keep
     * native watch-later saving/resume available without global configs;
     * an explicit path in mpv.conf is loaded afterwards and takes priority. */
    g_autofree char *watch_later_directory = g_build_filename(config_directory, "watch_later", NULL);
    mpv_set_option_string(p->mpv, "watch-later-dir", watch_later_directory);
    /* The same isolated configuration also removes mpv's default cache path.
     * Restore its persistent shader cache instead of recompiling at launch. */
    g_autofree char *shader_cache_directory = g_build_filename(
        g_get_user_cache_dir(), "mpv-gtk", "shaders", NULL);
    mpv_set_option_string(p->mpv, "gpu-shader-cache-dir", shader_cache_directory);
    const char *config = config_path(&p->config);
    if (g_file_test(config, G_FILE_TEST_IS_REGULAR)) {
        int result = mpv_load_config_file(p->mpv, config);
        if (result < 0)
            g_printerr("mpv-gtk: %s: %s\n", config, mpv_error_string(result));
    }
    const char *options[][2] = {
        {"idle", "yes"}, {"keep-open", "no"},
        /* Open the selected stock VO when a file is loaded, after the
         * controller is ready. libmpv also needs its GL context first. */
        {"force-window", "no"},
        {"load-scripts", "no"}, {"osc", "no"}, {"osd-level", "0"},
        {"input-default-bindings", "no"}, {"input-terminal", "no"},
        {"terminal", "no"}, {"input-vo-keyboard", "yes"},
        {"ytdl", "yes"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i) {
        int result = mpv_set_option_string(p->mpv, options[i][0], options[i][1]);
        if (result < 0) {
            g_printerr("mpv-gtk: %s: %s\n", options[i][0], mpv_error_string(result));
            return FALSE;
        }
    }
    int input_result = mpv_set_option_string(p->mpv, "input-conf", config_input_path(&p->config));
    if (input_result < 0) {
        g_printerr("mpv-gtk: input.conf: %s\n", mpv_error_string(input_result));
        return FALSE;
    }
    if (p->test_seconds > 0)
        mpv_set_option_string(p->mpv, "ao", "null");
    int result = mpv_initialize(p->mpv);
    if (result < 0) {
        g_printerr("mpv-gtk: %s\n", mpv_error_string(result));
        return FALSE;
    }
    /* The standard libmpv Render API renders into our GTK widget.
     * Finalize this after mpv has evaluated includes and profiles:
     * a native gpu/gpu-next VO would otherwise create a second window. */
    result = mpv_set_option_string(p->mpv, "vo", "libmpv");
    if (result < 0)
        return FALSE;
    mpv_set_option_string(p->mpv, "force-window", "no");
    mpv_set_option_string(p->mpv, "video-timing-offset", "0");
    p->active_vo = "libmpv";
    char *api = NULL, *es = NULL;
    mpv_get_property(p->mpv, "options/gpu-api", MPV_FORMAT_STRING, &api);
    mpv_get_property(p->mpv, "options/opengl-es", MPV_FORMAT_STRING, &es);
    p->gl_api = g_strcmp0(es, "yes") == 0 ? "opengl-es"
        : g_strcmp0(api, "opengl") == 0 || g_strcmp0(es, "no") == 0 ? "opengl" : "auto";
    p->preference_values.gpu_api = p->gl_api;
    mpv_free(api);
    mpv_free(es);
    p->gl_library = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!p->gl_library) {
        g_printerr("mpv-gtk: OpenGL: %s\n", dlerror());
        return FALSE;
    }
    result = mpv_hook_add(p->mpv, RENDER_LOAD_HOOK_ID, "on_preloaded", 0);
    if (result < 0) {
        g_printerr("mpv-gtk: %s\n", mpv_error_string(result));
        return FALSE;
    }
    GString *defaults = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(app_key_defaults); ++i)
        g_string_append_printf(defaults, "%s %s # %s\n", app_key_defaults[i].key,
                               app_key_defaults[i].command, app_key_defaults[i].comment);
    const char *define[] = {"define-section", KEYBINDINGS_APP_SECTION, defaults->str, "force", NULL};
    result = mpv_command_async(p->mpv, 0, define);
    g_string_free(defaults, TRUE);
    if (result >= 0) {
        const char *enable[] = {"enable-section", KEYBINDINGS_APP_SECTION, NULL};
        result = mpv_command_async(p->mpv, 0, enable);
    }
    if (result >= 0) {
        const char *user_first[] = {"enable-section", "default", NULL};
        result = mpv_command_async(p->mpv, 0, user_first);
    }
    if (result < 0) {
        g_printerr("mpv-gtk: %s\n", mpv_error_string(result));
        return FALSE;
    }
    g_autoptr(GError) edits_error = NULL;
    if (!install_key_edits(p, &edits_error))
        g_printerr("mpv-gtk: %s\n", edits_error->message);
    mpv_request_log_messages(p->mpv, "error");
    const struct { const char *name; mpv_format format; } properties[] = {
        {"time-pos", MPV_FORMAT_DOUBLE}, {"duration", MPV_FORMAT_DOUBLE},
        {"pause", MPV_FORMAT_FLAG}, {"paused-for-cache", MPV_FORMAT_FLAG},
        {"seeking", MPV_FORMAT_FLAG}, {"speed", MPV_FORMAT_DOUBLE},
        {"volume", MPV_FORMAT_DOUBLE}, {"mute", MPV_FORMAT_FLAG},
        {"idle-active", MPV_FORMAT_FLAG}, {"media-title", MPV_FORMAT_STRING},
        {"playlist-pos", MPV_FORMAT_INT64},
        {"playlist-count", MPV_FORMAT_INT64}, {"track-list", MPV_FORMAT_NODE},
        {"aid", MPV_FORMAT_STRING}, {"sid", MPV_FORMAT_STRING},
        {"fullscreen", MPV_FORMAT_FLAG},
        {"chapter", MPV_FORMAT_INT64}, {"chapters", MPV_FORMAT_INT64}
    };
    for (guint i = 0; i < G_N_ELEMENTS(properties); ++i)
        mpv_observe_property(p->mpv, i + 1, properties[i].name, properties[i].format);
    return TRUE;
}

/* Synchronous entry point retained for engine integration tests. Production
 * evaluates the stock engine's configuration before initializing GTK. */
static gboolean G_GNUC_UNUSED init_mpv(Player *p)
{
    setlocale(LC_NUMERIC, "C");
    if (!init_mpv_core(p))
        return FALSE;
    mpv_set_wakeup_callback(p->mpv, wake_main, p);
    return TRUE;
}

static void free_initializing_player(gpointer data)
{
    Player *engine = data;
    if (engine->mpv)
        mpv_terminate_destroy(engine->mpv);
    if (engine->gl_library)
        dlclose(engine->gl_library);
    if (engine->wake_fd >= 0)
        close(engine->wake_fd);
    config_clear(&engine->config);
    g_free(engine);
}

static void initialize_mpv_worker(GTask *task, gpointer source,
                                  gpointer task_data, GCancellable *cancel)
{
    (void)source;
    (void)cancel;
    Player *engine = task_data;
    startup_trace_mark("mpv initialization started");
    if (!init_mpv_core(engine)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Unable to initialize mpv");
        return;
    }
    startup_trace_mark("mpv initialized");
    g_task_return_boolean(task, TRUE);
}

static void initialize_mpv_finished(GObject *source, GAsyncResult *result,
                                    gpointer data)
{
    Player *p = data;
    g_autoptr(GError) error = NULL;
    gboolean initialized = g_task_propagate_boolean(G_TASK(result), &error);
    p->engine_loading = FALSE;
    if (!initialized)
        p->render_failed = TRUE;
    if (!p->closing && !p->quit_requested && initialized) {
        Player *engine = g_task_get_task_data(G_TASK(result));
        p->mpv = engine->mpv;
        p->gl_library = engine->gl_library;
        p->wake_fd = engine->wake_fd;
        p->active_vo = engine->active_vo;
        p->gl_api = engine->gl_api;
        p->preference_values.gpu_api = engine->preference_values.gpu_api;
        p->ready = engine->ready;
        engine->mpv = NULL;
        engine->gl_library = NULL;
        engine->wake_fd = -1;
        p->wake_source = g_unix_fd_add(p->wake_fd, G_IO_IN, dispatch_mpv, p);
        mpv_set_wakeup_callback(p->mpv, wake_main, p);
        update_metadata_observers(p);
        queue_reveal_changed(NULL, NULL, p);
        if (p->preferences && p->keys_loaded)
            load_preference_keys(p->preferences, p);
        if (playback_info_visible(p))
            request_playback_values(p);
        if (p->startup_waiting_for_media && p->area && p->window) {
            /* Realization creates the native surface and GL context without
             * mapping the window. That lets the on_preloaded hook prepare libmpv
             * before FILE_LOADED, without flashing the welcome screen first. */
            startup_trace_mark("GTK native window realization started");
            gtk_widget_realize(GTK_WIDGET(p->window));
            startup_trace_mark("GTK native window realized");
            startup_trace_mark("GTK video context realization started");
            gtk_widget_set_visible(GTK_WIDGET(p->area), TRUE);
            gtk_widget_realize(GTK_WIDGET(p->area));
            startup_trace_mark("GTK video context realized");
        }
        continue_render_load(p);
        flush_pending_paths(p);
        present_startup_window(p);
        /* Initial property events predate the wakeup callback installation. */
        wake_main(p);
    } else if (!p->closing && !p->quit_requested) {
        show_error(p, error->message);
    }
    g_application_release(G_APPLICATION(source));
}

static void start_mpv_async(Player *p)
{
    if (p->engine_started || p->closing || p->quit_requested)
        return;
    p->engine_started = TRUE;
    if (p->mpv) {
        /* Configuration has already been evaluated by stock mpv before GTK
         * chooses its shared GL API. Attach the prepared engine once. */
        p->wake_source = g_unix_fd_add(p->wake_fd, G_IO_IN, dispatch_mpv, p);
        mpv_set_wakeup_callback(p->mpv, wake_main, p);
        update_metadata_observers(p);
        queue_reveal_changed(NULL, NULL, p);
        if (p->startup_waiting_for_media && p->area) {
            startup_trace_mark("GTK native window realization started");
            gtk_widget_realize(GTK_WIDGET(p->window));
            startup_trace_mark("GTK native window realized");
            startup_trace_mark("GTK video context realization started");
            gtk_widget_set_visible(GTK_WIDGET(p->area), TRUE);
            gtk_widget_realize(GTK_WIDGET(p->area));
            startup_trace_mark("GTK video context realized");
        }
        continue_render_load(p);
        flush_pending_paths(p);
        present_startup_window(p);
        wake_main(p);
        return;
    }
    p->engine_loading = TRUE;
    Player *engine = g_new0(Player, 1);
    engine->wake_fd = -1;
    engine->test_seconds = p->test_seconds;
    engine->config.path = g_strdup(p->config.path);
    engine->config.input_path = g_strdup(p->config.input_path);
    /* Keep Player's stack and the main context alive if its window closes
     * while mpv_initialize is running. Completion then only frees the engine. */
    g_application_hold(G_APPLICATION(p->application));
    GTask *task = g_task_new(p->application, NULL, initialize_mpv_finished, p);
    g_task_set_task_data(task, engine, free_initializing_player);
    g_task_run_in_thread(task, initialize_mpv_worker);
    g_object_unref(task);
}

static gboolean self_test(gpointer data)
{
    Player *p = data;
    gint64 now = g_get_monotonic_time();
    double elapsed = p->test_started ? (now - p->test_started) / 1000000.0 : 0;
    gboolean timed_out = !p->test_started && now - p->test_launched > 15000000;
    if (p->test_started && elapsed >= p->test_seconds * .30 && !p->test_paused) {
        p->test_paused = TRUE;
        set_flag(p, "pause", TRUE);
    }
    if (p->test_started && elapsed >= p->test_seconds * .60 && !p->test_seek_sent) {
        p->test_seek_sent = TRUE;
        seek_absolute(p, 3);
        set_number(p, "volume", 35);
        set_number(p, "speed", 1.25);
    }
    if (p->test_started && elapsed >= p->test_seconds * .70 && !p->test_resumed) {
        p->test_resumed = TRUE;
        set_flag(p, "pause", FALSE);
    }
    if (timed_out || (p->test_started && elapsed >= p->test_seconds)) {
        p->test_finished = TRUE;
        char play_time[G_ASCII_DTOSTR_BUF_SIZE], pause_time[G_ASCII_DTOSTR_BUF_SIZE];
        char tick_gap[G_ASCII_DTOSTR_BUF_SIZE], render_time[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(play_time, sizeof(play_time), "%.6f", p->play_seconds);
        g_ascii_formatd(pause_time, sizeof(pause_time), "%.6f", p->paused_seconds);
        g_ascii_formatd(tick_gap, sizeof(tick_gap), "%.3f", p->max_tick_gap_ms);
        g_ascii_formatd(render_time, sizeof(render_time), "%.3f", p->max_render_ms);
        char *actual_vo = mpv_get_property_string(p->mpv, "current-vo");
        g_print("{\"loaded\":%s,\"native_window\":%s,\"vo\":\"%s\",\"ui_ticks\":%" G_GUINT64_FORMAT
            ",\"play_ticks\":%" G_GUINT64_FORMAT ",\"paused_ticks\":%" G_GUINT64_FORMAT
            ",\"play_seconds\":%s,\"paused_seconds\":%s"
            ",\"render_calls\":%" G_GUINT64_FORMAT ",\"frame_updates\":%" G_GUINT64_FORMAT
            ",\"max_tick_gap_ms\":%s,\"max_render_ms\":%s,\"seek_ok\":%s,\"error_count\":%u}\n",
            p->loaded_once ? "true" : "false", "false",
            actual_vo ? actual_vo : "", p->ui_ticks, p->play_ticks, p->paused_ticks,
            play_time, pause_time, p->render_calls, p->frame_updates,
            tick_gap, render_time, p->seek_ok ? "true" : "false", p->error_count);
        mpv_free(actual_vo);
        p->test_source = 0;
        gtk_window_close(p->window);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void use_bundled_icons(void)
{
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    /* Configure the shared database before GTK creates any widgets. Adding
     * resources alone still indexes the user's entire filesystem icon theme.
     * An empty search path keeps all icon lookups inside our executable and
     * GTK's own resources, including icons used by native GTK controls. */
    const char *search_paths[] = {NULL};
    const char *resource_paths[] = {
        "/io/github/emless/mpvgtk/icons",
        "/org/gtk/libgtk/icons/",
        NULL
    };
    gtk_icon_theme_set_search_path(theme, search_paths);
    gtk_icon_theme_set_resource_path(theme, resource_paths);
    /* Symbolic icons still inherit GTK's CSS colors and the user's accent.
     * Neither GtkSettings nor the desktop's icon theme is changed. */
    gtk_window_set_default_icon_name(APP_ICON_NAME);
    startup_trace_mark("GTK bundled icons configured");
}

static void application_started(GApplication *application, gpointer data)
{
    (void)application;
    (void)data;
    /* Connected after GtkApplication's startup class closure: registration,
     * desktop integration and automatic resources have finished by now. */
    startup_trace_mark("GTK application startup completed");
}

static void trim_seek_to(double seconds, gpointer data)
{
    seek_absolute(data, seconds);
}

static void trim_scrub_begin(gpointer data) { scrub_begin(data); }
static void trim_scrub_end(double seconds, gpointer data) { scrub_end(data, seconds); }
static void trim_scrub_cancel(gpointer data) { scrub_cancel(data); }

static void trim_message(const char *message, gboolean error, gpointer data)
{
    Player *p = data;
    if (p->closing || p->quit_requested) return;
    if (error) {
        show_error(p, message);
        return;
    }
    if (!p->track_notice) {
        p->track_notice = track_notice_new(GTK_OVERLAY(p->video_overlay));
        g_object_add_weak_pointer(G_OBJECT(p->track_notice), (gpointer *)&p->track_notice);
    }
    position_track_notice(p);
    track_notice_show(p->track_notice, tr("Обрезка"), message);
}

static void activate(GtkApplication *application, gpointer data)
{
    Player *p = data;
    if (p->window) {
        if (p->startup_presented)
            gtk_window_present(p->window);
        return;
    }
    startup_trace_mark("GTK activation entered");
    p->application = application;
    install_actions(p);
    startup_trace_mark("GTK actions installed");
    p->window = GTK_WINDOW(gtk_application_window_new(application));
    startup_trace_mark("GTK window constructed");
    gtk_window_set_title(p->window, "mpv GTK");
    gtk_window_set_icon_name(p->window, APP_ICON_NAME);
    gtk_window_set_default_size(p->window, 1080, 700);
    g_signal_connect(p->window, "close-request", G_CALLBACK(close_window), p);
    g_signal_connect(p->window, "notify::fullscreened", G_CALLBACK(fullscreen_changed), p);
    startup_trace_mark("GTK window configured");

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    p->root = root;
    gtk_window_set_child(p->window, root);
    p->header = gtk_header_bar_new();
    p->title = gtk_label_new("mpv GTK");
    gtk_label_set_ellipsize(GTK_LABEL(p->title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(p->title), 60);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(p->header), p->title);
    gtk_window_set_titlebar(p->window, p->header);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(p->header), icon_button(
        "document-open-symbolic", "Открыть файл · Ctrl+O", "app.open"));
    GtkWidget *url_button = icon_button(
        "insert-link-symbolic", "Открыть ссылку · Ctrl+L", "app.url");
    gtk_widget_add_css_class(url_button, "open-url-button");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(p->header), url_button);
    p->app_menu = g_menu_new();
    update_app_menu(p);
    p->app_menu_button = gtk_menu_button_new();
    GtkWidget *app_menu_icon = gtk_image_new_from_icon_name("view-more-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(app_menu_icon), 20);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(p->app_menu_button), app_menu_icon);
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(p->app_menu_button), FALSE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(p->app_menu_button), G_MENU_MODEL(p->app_menu));
    gtk_widget_add_css_class(p->app_menu_button, "flat");
    gtk_widget_add_css_class(p->app_menu_button, "header-app-menu");
    gtk_widget_set_focus_on_click(p->app_menu_button, FALSE);
    translated_tooltip(p->app_menu_button, "Главное меню");
    g_signal_connect(p->app_menu_button, "notify::active", G_CALLBACK(controls_interaction_changed), p);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(p->header), p->app_menu_button);
    GtkWidget *settings_button = icon_button(
        "emblem-system-symbolic", "Настройки · Ctrl+,", "app.settings");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(p->header), settings_button);
    g_autoptr(GtkSizeGroup) header_button_size = gtk_size_group_new(GTK_SIZE_GROUP_BOTH);
    gtk_size_group_add_widget(header_button_size, p->app_menu_button);
    gtk_size_group_add_widget(header_button_size, settings_button);
    startup_trace_mark("GTK header constructed");

    p->error_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(p->error_bar, "error-bar");
    p->error_label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(p->error_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(p->error_label), 0);
    gtk_widget_set_hexpand(p->error_label, TRUE);
    gtk_box_append(GTK_BOX(p->error_bar), p->error_label);
    GtkWidget *dismiss = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(dismiss, "flat");
    g_signal_connect(dismiss, "clicked", G_CALLBACK(dismiss_error), p);
    gtk_box_append(GTK_BOX(p->error_bar), dismiss);
    gtk_widget_set_visible(p->error_bar, FALSE);
    gtk_box_append(GTK_BOX(root), p->error_bar);

    p->stage = gtk_overlay_new();
    gtk_widget_set_hexpand(p->stage, TRUE);
    gtk_widget_set_vexpand(p->stage, TRUE);
    gtk_box_append(GTK_BOX(root), p->stage);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(p->stage), body);
    GtkWidget *overlay = gtk_overlay_new();
    p->video_overlay = overlay;
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_box_append(GTK_BOX(body), overlay);
    p->area = GTK_GL_AREA(gtk_gl_area_new());
    p->video_widget = GTK_WIDGET(p->area);
    gtk_widget_set_visible(p->video_widget, FALSE);
    /* Match the API chosen for GTK's shared display context before init. */
    gtk_gl_area_set_allowed_apis(p->area, gl_api_allowed_apis(p->gl_api));
    gtk_gl_area_set_auto_render(p->area, FALSE);
    g_signal_connect(p->area, "realize", G_CALLBACK(realize), p);
    g_signal_connect(p->area, "render", G_CALLBACK(render), p);
    g_signal_connect(p->area, "unrealize", G_CALLBACK(unrealize), p);
    gtk_widget_set_focusable(p->video_widget, TRUE);
    gtk_widget_set_hexpand(p->video_widget, TRUE);
    gtk_widget_set_vexpand(p->video_widget, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), p->video_widget);
    GtkGesture *video_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(video_click), 0);
    g_signal_connect(video_click, "pressed", G_CALLBACK(video_clicked), p);
    gtk_widget_add_controller(p->video_widget, GTK_EVENT_CONTROLLER(video_click));
    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(video_scrolled), p);
    gtk_widget_add_controller(p->video_widget, scroll);

    p->empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_halign(p->empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->empty, GTK_ALIGN_CENTER);
    GtkWidget *logo = gtk_image_new_from_icon_name(APP_ICON_NAME);
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 64);
    gtk_box_append(GTK_BOX(p->empty), logo);
    GtkWidget *heading = gtk_label_new("mpv GTK");
    gtk_widget_add_css_class(heading, "title-1");
    gtk_box_append(GTK_BOX(p->empty), heading);
    GtkWidget *hint = translated_label("Перетащите видео сюда или откройте файл");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(p->empty), hint);
    GtkWidget *open = translated_button("Открыть файл");
    gtk_widget_add_css_class(open, "suggested-action");
    gtk_widget_set_halign(open, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "app.open");
    gtk_box_append(GTK_BOX(p->empty), open);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->empty);

    p->sidebar = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(p->sidebar),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    g_signal_connect(p->sidebar, "notify::reveal-child", G_CALLBACK(queue_reveal_changed), p);
    g_signal_connect(p->sidebar, "notify::child-revealed", G_CALLBACK(queue_reveal_changed), p);
    gtk_box_append(GTK_BOX(body), p->sidebar);
    startup_trace_mark("GTK video widgets constructed");

    p->controls_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(p->controls_revealer),
                                      GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(p->controls_revealer), 80);
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->controls_revealer), TRUE);
    gtk_widget_set_valign(p->controls_revealer, GTK_ALIGN_FILL);
    gtk_widget_set_halign(p->controls_revealer, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(root), p->controls_revealer);
    p->controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(p->controls, "controls");
    gtk_revealer_set_child(GTK_REVEALER(p->controls_revealer), p->controls);
    p->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, .01);
    gtk_widget_add_css_class(p->seek, "seek-scale");
    gtk_scale_set_draw_value(GTK_SCALE(p->seek), FALSE);
    gtk_widget_set_hexpand(p->seek, TRUE);
    p->seek_overlay = chapter_marks_new(GTK_RANGE(p->seek));
    gtk_box_append(GTK_BOX(p->controls), p->seek_overlay);
    p->seek_preview = seek_preview_new(p->mpv);
    seek_preview_set_enabled(p->seek_preview, p->preference_values.timeline_previews);
    seek_preview_attach_area(p->seek_preview, p->seek, TRUE, GTK_WIDGET(p->window));
    g_signal_connect(p->seek, "value-changed", G_CALLBACK(seek_changed), p);
    GtkEventController *release = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(release, GTK_PHASE_CAPTURE);
    g_signal_connect(release, "event", G_CALLBACK(seek_event), p);
    gtk_widget_add_controller(p->seek, release);
    g_signal_connect(p->seek, "unmap", G_CALLBACK(seek_unmapped), p);

    GtkWidget *transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(p->controls), transport);
    GtkWidget *playback = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_valign(playback, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(transport), playback);
    GtkWidget *previous = icon_button(NULL,
        "Предыдущий · Page Up", "app.previous");
    p->previous_button = previous;
    gtk_button_set_child(GTK_BUTTON(previous), transport_icon_new(TRANSPORT_PREVIOUS));
    gtk_widget_add_css_class(previous, "skip-button");
    gtk_box_append(GTK_BOX(playback), previous);
    p->play_button = icon_button(NULL, "Пауза · Пробел", "app.play");
    p->play_icon = transport_icon_new(TRANSPORT_PLAY);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->play_icon), 28);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->play_icon), 28);
    gtk_button_set_child(GTK_BUTTON(p->play_button), p->play_icon);
    gtk_widget_set_size_request(p->play_button, 40, 40);
    gtk_widget_add_css_class(p->play_button, "play-button");
    gtk_box_append(GTK_BOX(playback), p->play_button);
    GtkWidget *next = icon_button(NULL,
        "Следующий · Page Down", "app.next");
    p->next_button = next;
    gtk_button_set_child(GTK_BUTTON(next), transport_icon_new(TRANSPORT_NEXT));
    gtk_widget_add_css_class(next, "skip-button");
    gtk_box_append(GTK_BOX(playback), next);
    GtkWidget *audio_time = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(transport), audio_time);
    p->time_label = gtk_label_new("00:00 / --:--");
    gtk_widget_add_css_class(p->time_label, "timecode");
    gtk_widget_set_margin_start(p->time_label, 5);
    gtk_box_append(GTK_BOX(audio_time), p->time_label);
    p->status_label = gtk_label_new("");
    gtk_widget_add_css_class(p->status_label, "dim-label");
    gtk_widget_set_hexpand(p->status_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(p->status_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->status_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(p->status_label, 12);
    gtk_box_append(GTK_BOX(transport), p->status_label);
    p->audio_menu = g_menu_new();
    p->subtitle_menu = g_menu_new();
    p->speed_menu = g_menu_new();
    update_track_menu(p, NULL);
    const char *speeds[] = {"0.5", "0.75", "1", "1.25", "1.5", "1.75", "2", NULL};
    for (int i = 0; speeds[i]; ++i) {
        g_autofree char *label = g_strdup_printf("%s×", speeds[i]);
        add_target_item(p->speed_menu, label, "app.speed", speeds[i]);
    }
    p->mute_button = icon_button("audio-volume-high-symbolic",
        "Звук · M\nПКМ — следующая аудиодорожка\nСредняя кнопка — аудиодорожки", "app.mute");
    p->audio_context = gtk_popover_bin_new();
    gtk_widget_set_valign(p->audio_context, GTK_ALIGN_CENTER);
    gtk_popover_bin_set_child(GTK_POPOVER_BIN(p->audio_context), p->mute_button);
    gtk_popover_bin_set_menu_model(GTK_POPOVER_BIN(p->audio_context), G_MENU_MODEL(p->audio_menu));
    gtk_popover_bin_set_handle_input(GTK_POPOVER_BIN(p->audio_context), FALSE);
    add_quick_control_gestures(p->audio_context, p);
    GtkWidget *audio_popover = gtk_popover_bin_get_popover(GTK_POPOVER_BIN(p->audio_context));
    gtk_popover_set_position(GTK_POPOVER(audio_popover), GTK_POS_TOP);
    gtk_widget_set_halign(audio_popover, GTK_ALIGN_CENTER);
    g_signal_connect(audio_popover, "notify::visible", G_CALLBACK(center_audio_popover), NULL);
    g_signal_connect(audio_popover, "notify::visible", G_CALLBACK(controls_interaction_changed), p);
    gtk_box_append(GTK_BOX(audio_time), p->audio_context);
    p->volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(p->volume), FALSE);
    gtk_range_set_value(GTK_RANGE(p->volume), 50);
    /* Remove half the side padding without changing the visible track's
     * length: 90 - 2*12 == 78 - 2*6. */
    gtk_widget_set_size_request(p->volume, 78, -1);
    gtk_widget_set_valign(p->volume, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(p->volume, "volume-scale");
    gtk_widget_set_focus_on_click(p->volume, FALSE);
    translated_tooltip(p->volume, "Громкость · ↑ / ↓");
    g_signal_connect(p->volume, "value-changed", G_CALLBACK(volume_changed), p);
    gtk_box_append(GTK_BOX(audio_time), p->volume);
    gtk_box_reorder_child_after(GTK_BOX(audio_time), p->audio_context, NULL);
    gtk_box_reorder_child_after(GTK_BOX(audio_time), p->volume, p->audio_context);
    p->subtitle_button = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(p->subtitle_button), "CC");
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(p->subtitle_button), FALSE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(p->subtitle_button), G_MENU_MODEL(p->subtitle_menu));
    gtk_widget_add_css_class(p->subtitle_button, "compact-menu");
    gtk_widget_set_size_request(p->subtitle_button, 64, 32);
    gtk_widget_set_valign(p->subtitle_button, GTK_ALIGN_CENTER);
    translated_tooltip(p->subtitle_button,
        "Субтитры\nПКМ — следующие\nСредняя кнопка — исходный выбор");
    add_quick_control_gestures(p->subtitle_button, p);
    gtk_box_append(GTK_BOX(transport), p->subtitle_button);
    p->speed_button = gtk_menu_button_new();
    GtkWidget *speed_label = gtk_label_new("1×");
    gtk_label_set_xalign(GTK_LABEL(speed_label), 0);
    gtk_widget_set_hexpand(speed_label, TRUE);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(p->speed_button), speed_label);
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(p->speed_button), TRUE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(p->speed_button), G_MENU_MODEL(p->speed_menu));
    gtk_widget_add_css_class(p->speed_button, "compact-menu");
    gtk_widget_add_css_class(p->speed_button, "speed-menu");
    gtk_widget_set_size_request(p->speed_button, 64, 32);
    gtk_widget_set_valign(p->speed_button, GTK_ALIGN_CENTER);
    translated_tooltip(p->speed_button,
        "Скорость воспроизведения\nПКМ — +0.25×\nСредняя кнопка — 1×");
    add_quick_control_gestures(p->speed_button, p);
    gtk_box_append(GTK_BOX(transport), p->speed_button);
    g_signal_connect(p->subtitle_button, "notify::active", G_CALLBACK(controls_interaction_changed), p);
    g_signal_connect(p->speed_button, "notify::active", G_CALLBACK(controls_interaction_changed), p);
    gtk_box_append(GTK_BOX(transport), icon_button("view-list-symbolic",
        "Очередь · Ctrl+P", "app.queue"));
    p->chapter_button = chapters_button_new(select_chapter, step_chapter, p);
    gtk_widget_set_valign(p->chapter_button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(p->chapter_button, "chapter-menu");
    translated_tooltip(p->chapter_button,
        "Главы · Ctrl+Page Up / Ctrl+Page Down\nПКМ — следующая глава");
    g_signal_connect(p->chapter_button, "notify::active",
                     G_CALLBACK(controls_interaction_changed), p);
    GtkGesture *chapter_context = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(chapter_context), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(chapter_context), GTK_PHASE_CAPTURE);
    g_signal_connect(chapter_context, "released", G_CALLBACK(chapter_secondary_click), p);
    gtk_widget_add_controller(p->chapter_button, GTK_EVENT_CONTROLLER(chapter_context));
    gtk_box_append(GTK_BOX(transport), p->chapter_button);
    TrimHost trim_host = {
        .mpv = p->mpv, .window = p->window,
        .controls = GTK_BOX(p->controls), .transport = GTK_BOX(transport),
        .seek = p->seek_overlay, .playback = playback, .audio_time = audio_time,
        .status = p->status_label, .time = GTK_LABEL(p->time_label),
        .video = GTK_OVERLAY(p->video_overlay), .config_path = config_path(&p->config),
        .seek_to = trim_seek_to, .message = trim_message, .data = p,
        .scrub_begin = trim_scrub_begin, .scrub_end = trim_scrub_end, .scrub_cancel = trim_scrub_cancel,
        .previews = p->seek_preview
    };
    p->trim = trim_mode_new(&trim_host);
    gtk_box_append(GTK_BOX(transport), trim_mode_button(p->trim));
    GtkWidget *info = icon_button("dialog-information-symbolic",
        "Информация · I\nЛКМ — на видео, ПКМ — отдельное окно", "app.info");
    GtkGesture *info_context = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(info_context), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(info_context), GTK_PHASE_CAPTURE);
    g_signal_connect(info_context, "released", G_CALLBACK(info_secondary_click), p);
    gtk_widget_add_controller(info, GTK_EVENT_CONTROLLER(info_context));
    gtk_box_append(GTK_BOX(transport), info);
    GtkWidget *fullscreen_button = icon_button("view-fullscreen-symbolic",
        "Полный экран · F", "app.fullscreen");
    gtk_image_set_pixel_size(GTK_IMAGE(gtk_button_get_child(GTK_BUTTON(fullscreen_button))), 18);
    gtk_widget_set_size_request(fullscreen_button, 36, 36);
    gtk_widget_set_valign(fullscreen_button, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(transport), fullscreen_button);
    startup_trace_mark("GTK controls constructed");

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), p);
    gtk_widget_add_controller(GTK_WIDGET(p->window), keys);
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(drop_files), p);
    gtk_widget_add_controller(GTK_WIDGET(p->window), GTK_EVENT_CONTROLLER(drop));
    GtkEventController *motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "enter", G_CALLBACK(pointer_enter), p);
    g_signal_connect(motion, "motion", G_CALLBACK(pointer_motion), p);
    g_signal_connect(motion, "leave", G_CALLBACK(pointer_leave), p);
    gtk_widget_add_controller(p->stage, motion);
    GtkEventController *panel_events = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(panel_events, GTK_PHASE_CAPTURE);
    g_signal_connect(panel_events, "event", G_CALLBACK(controls_event), p);
    gtk_widget_add_controller(p->controls, panel_events);
    GtkEventController *stage_events = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(stage_events, GTK_PHASE_CAPTURE);
    g_signal_connect(stage_events, "event", G_CALLBACK(controls_event), p);
    gtk_widget_add_controller(p->stage, stage_events);
    gtk_widget_add_tick_callback(root, tick, p, NULL);

    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { background: @window_bg_color; color: @window_fg_color; }"
        "headerbar .open-url-button > image { transform: translate(0, 1px); }"
        ".controls { background: @headerbar_bg_color; color: @headerbar_fg_color;"
        "padding: 2px 12px 6px; border: none; border-top: 1px solid alpha(@window_fg_color,.08);"
        "border-radius: 0; box-shadow: none; }"
        ".controls button { min-width: 24px; min-height: 24px; border-radius: 9px;"
        "border: none; box-shadow: none; transition: background-color 100ms ease; }"
        ".control-button { padding: 4px; }"
        ".controls .control-button:hover { background: alpha(@window_fg_color,.08); }"
        ".controls .control-button:active { background: alpha(@window_fg_color,.14); }"
        ".controls .skip-button { min-width: 24px; min-height: 24px; }"
        ".controls .play-button { min-width: 28px; min-height: 28px; padding: 6px;"
        "background: alpha(@window_fg_color,.08); color: @window_fg_color; border-radius: 50%; }"
        ".controls .play-button:hover { background: alpha(@window_fg_color,.14); }"
        ".compact-menu > button { background: transparent; padding: 4px 8px;"
        "font-weight: 600; font-variant-numeric: tabular-nums; }"
        ".compact-menu > button:hover { background: alpha(@window_fg_color,.08); }"
        ".active-track > button { color: @accent_color; background: alpha(@accent_bg_color,.12); }"
        ".timecode { font-variant-numeric: tabular-nums; font-family: 'Adwaita Sans', sans-serif; }"
        ".controls button.trim-active { color: @accent_color; background: alpha(@accent_bg_color,.14); }"
        ".trim-time { min-height: 26px; padding: 2px 8px; border-radius: 7px; }"
        ".trim-controls { padding: 2px 0; }"
        ".trim-timeline { color: @accent_bg_color; }"
        ".queue { background: @view_bg_color; color: @view_fg_color;"
        "border-left: 1px solid alpha(@window_fg_color,.08); }"
        ".queue list { background: transparent; }"
        ".error-bar { background: @error_bg_color; color: @error_fg_color; padding: 8px 12px; }"
        ".controls scale.seek-scale { padding: 12px; }"
        ".controls scale.seek-scale trough { min-height: 4px; background: alpha(@window_fg_color,.13); border: none; }"
        ".controls scale.seek-scale highlight { min-height: 4px; background: @accent_bg_color; border: none; }"
        ".controls scale.seek-scale slider { min-width: 20px; min-height: 20px; background: @window_fg_color;"
        "border: none; box-shadow: none; }"
        ".controls scale.volume-scale slider { min-width: 13px; min-height: 13px; padding: 0; }"
        ".playback-info-overlay { background: alpha(@window_bg_color,.92); color: @window_fg_color;"
        "padding: 12px 16px; border-radius: 10px; border: 1px solid alpha(@window_fg_color,.12); }"
        ".preferences-card { background: @card_bg_color; border: 1px solid alpha(@window_fg_color,.06);"
        "border-radius: 12px; }"
        "frame.keybinds-panel { border: none; padding: 0; border-radius: 12px; box-shadow: none; }"
        ".keybind-keycap { padding: 4px 7px; border: 1px solid alpha(@window_fg_color,.12);"
        "border-bottom-width: 2px; border-radius: 6px; background: alpha(@window_fg_color,.06); }"
        ".mpv-about .about-version { background: alpha(@accent_bg_color,.15); color: @accent_color;"
        "padding: 4px 12px; border-radius: 999px; font-weight: bold; }"
        ".preferences-sidebar { background: @sidebar_bg_color; color: @sidebar_fg_color;"
        "border-right: 1px solid alpha(@window_fg_color,.08); }"
        ".title-1 { font-size: 28px; font-weight: bold; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(p->window)),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    startup_trace_mark("GTK application CSS loaded");
    /* Preserve the theme's original seek-bar size. A smaller volume thumb
     * needs matching margins so it cannot collapse the trough to zero. */
    g_autoptr(GtkCssProvider) size_css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(size_css,
        "headerbar .header-app-menu > button {"
        "min-width: 24px; min-height: 24px; padding: 4px; }"
        ".controls .control-button, .controls .compact-menu > button {"
        "min-width: 24px; min-height: 24px; padding: 4px; }"
        ".controls .compact-menu > button { min-width: 56px; border-radius: 6px;"
        "background: alpha(@window_fg_color,.05); }"
        ".controls .compact-menu.speed-menu > button { min-width: 0; padding: 4px 12px; }"
        ".controls .compact-menu > button:hover { background: alpha(@window_fg_color,.10); }"
        ".controls .compact-menu.active-track > button { color: @accent_color;"
        "background: alpha(@accent_bg_color,.12); }"
        ".controls .trim-audio-menu > button, .controls .trim-audio-menu > button:checked {"
        "min-width: 24px; min-height: 24px; padding: 4px; border-radius: 9px;"
        "background: transparent; background-image: none; border: none; box-shadow: none; outline: none; }"
        ".controls .trim-audio-menu > button:hover, .controls .trim-audio-menu > button:focus-visible {"
        "background: alpha(@window_fg_color,.08); }"
        ".controls .trim-audio-menu > button:active { background: alpha(@window_fg_color,.14); }"
        /* A horizontal margin on the native popover also shifts GTK's arrow.
         * Keep that inset on its contents so the arrow stays on its anchor. */
        "popover.trim-audio-popover { margin: 8px 0; }"
        "popover.trim-audio-popover > contents { margin: 0 8px; }"
        ".controls .chapter-menu > button, .controls .chapter-menu > button:checked {"
        "background: transparent; box-shadow: none; color: @window_fg_color;"
        "border-color: transparent; padding: 4px; min-width: 24px; min-height: 24px; border-radius: 8px; }"
        ".controls .chapter-menu > button:hover, .controls .chapter-menu > button:checked:hover {"
        "background: alpha(@window_fg_color,.08); }"
        "popover.chapters-popover > contents { padding: 4px; }"
        "list.chapters-list { padding: 0; }"
        "list.chapters-list > row { min-height: 0; padding: 2px 6px; margin: 1px 0; border-radius: 5px; }"
        ".controls .play-button { min-width: 28px; min-height: 28px; padding: 6px; }"
        ".controls scale.seek-scale { min-height: 10px; padding: 12px; margin: 0; }"
        ".controls scale.seek-scale > trough > slider { min-width: 20px; min-height: 20px;"
        "padding: 0; margin: -8px; }"
        ".controls scale.seek-scale > trough, scale.volume-scale > trough { min-height: 4px;"
        "background: alpha(@window_fg_color,.15); }"
        ".controls scale.seek-scale > trough > highlight, scale.volume-scale > trough > highlight {"
        "min-height: 4px; background: @accent_bg_color; }"
        "scale.volume-scale { padding: 12px 6px; }"
        "scale.volume-scale > trough > slider { min-width: 13px; min-height: 13px;"
        "padding: 0; margin: -4.5px; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(p->window)),
        GTK_STYLE_PROVIDER(size_css), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    startup_trace_mark("GTK sizing CSS loaded");
    update_transport(p);
    p->startup_waiting_for_media = p->startup_waiting_for_media ||
        (p->pending && p->pending->len > 0);
    startup_trace_mark("GTK UI constructed");
    start_mpv_async(p);
    if (p->test_seconds > 0) {
        p->test_launched = g_get_monotonic_time();
        p->test_source = g_timeout_add(50, self_test, p);
    }
}

int main(int argc, char **argv)
{
    startup_trace_begin();
    Player p = {.wake_fd = -1, .idle = TRUE, .volume_value = 50,
                .current_chapter = -1,
                .last_time_second = -1, .playlist_position = -1,
                .timeline = {.speed = 1}, .controls_state = {.visible = TRUE},
                .preference_values = {.hwdec = "auto-safe",
                                      .keepaspect = TRUE, .sub_scale = 1}};
    gchar **files = NULL;
    gboolean version = FALSE;
    GOptionEntry options[] = {
        {"self-test", 0, 0, G_OPTION_ARG_DOUBLE, &p.test_seconds,
         "Run playback/pause/seek responsiveness test and emit JSON (6–120 seconds)", "SECONDS"},
        {"version", 0, 0, G_OPTION_ARG_NONE, &version, "Print version", NULL},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &files,
         "Media files or URLs", "FILE…"},
        {NULL, 0, 0, 0, NULL, NULL, NULL}
    };
    g_autoptr(GOptionContext) context = g_option_context_new("— a responsive GTK interface for mpv");
    g_option_context_add_main_entries(context, options, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        g_strfreev(files);
        return 2;
    }
    if (version) {
        g_print("mpv GTK %s\n", APP_VERSION);
        g_strfreev(files);
        return 0;
    }
    if (!isfinite(p.test_seconds) || (p.test_seconds != 0 &&
        (p.test_seconds < 6 || p.test_seconds > 120 || !files || !files[0]))) {
        g_printerr("--self-test requires 6–120 seconds and a media file\n");
        g_strfreev(files);
        return 2;
    }
    /* GTK fixes its shared GL API while initializing the display. Read the
     * saved choice first, before it can create a GLES context for the UI. */
    if (!config_init(&p.config, &error)) {
        g_printerr("mpv-gtk: %s\n", error->message);
        config_clear(&p.config);
        g_strfreev(files);
        return 1;
    }
    if (!config_migrate_renderer(&p.config, &error)) {
        g_printerr("mpv-gtk: %s\n", error->message);
        config_clear(&p.config);
        g_strfreev(files);
        return 1;
    }
    if (!i18n_init(config_path(&p.config), &error)) {
        g_printerr("mpv-gtk: %s\n", error->message);
        g_clear_error(&error);
        i18n_set_language("ru");
    }
    p.preference_values.language = i18n_language();
    p.preference_values.cursor_hide_delay = i18n_cursor_hide_delay();
    p.preference_values.allow_replace = i18n_allow_replace();
    p.preference_values.add_to_queue = i18n_add_to_queue();
    p.preference_values.timeline_previews = i18n_timeline_previews();
    /* GTK must not change the numeric locale while early demuxing runs on
     * libmpv's threads. Keep the desktop's other locale categories intact. */
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    gtk_disable_setlocale();
    /* Let the unmodified engine evaluate mpv.conf, includes and profiles.
     * The resulting options determine GTK's shared GL API. */
    startup_trace_mark("mpv initialization started");
    if (!init_mpv_core(&p)) {
        if (p.mpv) mpv_terminate_destroy(p.mpv);
        if (p.gl_library) dlclose(p.gl_library);
        if (p.wake_fd >= 0) close(p.wake_fd);
        config_clear(&p.config);
        g_strfreev(files);
        return 1;
    }
    startup_trace_mark("configuration loaded");
    p.pending = g_ptr_array_new_with_free_func(g_free);
    if (files) {
        for (int i = 0; files[i]; ++i) {
            g_autoptr(GFile) file = g_file_new_for_commandline_arg(files[i]);
            g_ptr_array_add(p.pending, media_location(file));
        }
    }
    g_clear_pointer(&files, g_strfreev);
    queue_startup_media(&p);
    GlApiEnvironment gl_environment = {0};
    gl_api_environment_begin(&gl_environment, p.gl_api);
    /* Video and GTK share the standard OpenGL context. Keep this process
     * on the native Wayland backend; no external video window is created. */
    gdk_set_allowed_backends("wayland");
    p.default_gsk_renderer = g_getenv("GSK_RENDERER") == NULL;
    g_setenv("GSK_RENDERER", "opengl", FALSE);
    startup_trace_mark("GTK initialization started");
    gboolean gtk_initialized = gtk_init_check();
    gl_api_environment_end(&gl_environment);
    if (!gtk_initialized) {
        g_printerr("mpv-gtk: could not connect to the Wayland session\n");
        mpv_terminate_destroy(p.mpv);
        if (p.gl_library) dlclose(p.gl_library);
        if (p.wake_fd >= 0) close(p.wake_fd);
        g_ptr_array_unref(p.pending);
        config_clear(&p.config);
        return 77;
    }
    startup_trace_mark("GTK initialized");
    cursor_set_delay(&p.cursor_state,
        (int64_t)llround(p.preference_values.cursor_hide_delay * 1000000.0),
        g_get_monotonic_time());
    g_set_application_name("mpv GTK");
    use_bundled_icons();
    startup_trace_mark("GTK application creation started");
    p.application = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    /* Icon resources are registered by the generated resource constructor and
     * configured above. Avoid adding a duplicate default resource path. */
    g_application_set_resource_base_path(G_APPLICATION(p.application), NULL);
    startup_trace_mark("GTK application created");
    g_signal_connect_after(p.application, "startup", G_CALLBACK(application_started), &p);
    g_signal_connect(p.application, "activate", G_CALLBACK(activate), &p);
    startup_trace_mark("GTK application run entered");
    int status = g_application_run(G_APPLICATION(p.application), 1, argv);
    startup_trace_mark("GTK application loop finished");
    p.closing = TRUE;
    if (p.scrub_release_source) g_source_remove(p.scrub_release_source);
    audio_scrub_free(p.audio_scrub);
    p.audio_scrub = NULL;
    if (p.mpris_source) g_source_remove(p.mpris_source);
    mpris_art_free(p.mpris_art);
    p.mpris_art = NULL;
    mpris_free(p.mpris);
    p.mpris = NULL;
    clear_track_notices(&p);
    cursor_timeout_cancel(&p);
    if (p.wake_source) g_source_remove(p.wake_source);
    if (p.test_source) g_source_remove(p.test_source);
    if (p.language_source) g_source_remove(p.language_source);
    continue_render_load(&p);
    /* Render context was freed by GLArea::unrealize with its context current.
     * Only now may we synchronously wait for the mpv core to terminate. */
    seek_preview_free(p.seek_preview);
    p.seek_preview = NULL;
    trim_mode_free(p.trim);
    p.trim = NULL;
    if (p.mpv) {
        mpv_set_wakeup_callback(p.mpv, NULL, NULL);
        startup_trace_mark("mpv core cleanup started");
        mpv_terminate_destroy(p.mpv);
        startup_trace_mark("mpv core cleanup finished");
    }
    if (p.gl_library) dlclose(p.gl_library);
    if (p.wake_fd >= 0) close(p.wake_fd);
    g_clear_object(&p.audio_menu);
    g_clear_object(&p.subtitle_menu);
    g_clear_object(&p.speed_menu);
    g_clear_object(&p.app_menu);
    config_clear(&p.config);
    g_free(p.hwdec);
    g_free(p.hwdec_current);
    g_free(p.pending_hwdec);
    g_free(p.audio_output);
    g_free(p.audio_device);
    clear_initial_subtitle(&p);
    for (guint i = 0; i < G_N_ELEMENTS(p.playlist_values); ++i)
        g_free(p.playlist_values[i]);
    clear_playback_values(&p.playback);
    g_object_unref(p.application);
    g_ptr_array_unref(p.pending);
    g_queue_clear_full(&p.pending_commands, (GDestroyNotify)g_strfreev);
    if (p.test_seconds && (!p.test_finished || !p.loaded_once || !p.seek_ok ||
                          p.error_count || p.render_failed))
        return 1;
    return status;
}
