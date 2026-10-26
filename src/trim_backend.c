#define _GNU_SOURCE
#include "trim_backend.h"
#include "trim_export.h"
#include "keybindings.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACKEND_OBSERVE_BASE (UINT64_C(1) << 44)
#define BACKEND_SCAN_BASE (BACKEND_OBSERVE_BASE + (UINT64_C(1) << 32))
#define BACKEND_SEEK_ID (BACKEND_OBSERVE_BASE + UINT64_C(1024))
#define BACKEND_SEEK_SAMPLE_BASE (BACKEND_OBSERVE_BASE + (UINT64_C(1) << 30))
#define BACKEND_SEEK_OBSERVE_BASE (BACKEND_OBSERVE_BASE + UINT64_C(16))
#define SEEK_DISPATCH_MS 16
#define SEEK_RECOVERY_MS 750
#define BACKEND_CAPTURE_LIMIT (64 * 1024 * 1024)
#define SHORTCUT_SECTION "trim-mode-shortcuts"

typedef struct {
    char *key;
    GArray *times;
} KeyframeCache;

typedef struct {
    gint64 id, ff_index;
    char *title, *lang, *codec;
    gboolean included;
} AudioTrack;

typedef struct { const char *name; mpv_format format; } SeekProperty;
static const SeekProperty seek_properties[] = {
    {"time-pos", MPV_FORMAT_DOUBLE}, {"pause", MPV_FORMAT_FLAG},
    {"seeking", MPV_FORMAT_FLAG}, {"speed", MPV_FORMAT_DOUBLE},
    {"container-fps", MPV_FORMAT_DOUBLE}, {"paused-for-cache", MPV_FORMAT_FLAG}
};

struct TrimBackend {
    gint references;
    mpv_handle *mpv, *compat;
    TrimExport *exporter;
    int wake_fd;
    guint wake_source;
    gboolean stopped, enabled, ever_enabled, loaded, video_available;
    gboolean gtk_editing, shortcuts_active, follows_duration;
    gboolean remember, open_copy, open_replace;
    char *ffmpeg_path, *ffprobe_path, *state_path, *audio_source;
    gint64 start_ms, end_ms, duration_ms, file_serial;
    GArray *keyframes;
    GPtrArray *audio_tracks;
    GQueue cache;
    char *cached_file, *scan_key;
    guint64 scan_serial, scan_request;
    double scan_offset;
    guint64 parse_request;
    GCancellable *parse_cancel;
    gint keyframe_target, seek_index;
    gboolean seek_active, seek_started, seek_exact, seek_acknowledged;
    guint64 seek_request, seek_serial;
    guint seek_source, seek_recovery;
    guint64 position_refresh, probe_position_request, probe_seeking_request;
    gboolean queued_exact, probe_position_ready, probe_seeking_ready, probe_seeking;
    gboolean position_known, playback_paused, playback_seeking, playback_buffering;
    double playback_position, playback_speed, playback_fps, probe_position;
    gint64 position_sampled;
    double seek_point;
    gint64 seek_issued;
};

static void publish_mode(TrimBackend *b);
static void publish_controls(TrimBackend *b);
static void scan_keyframes(TrimBackend *b);
static void update_shortcuts(TrimBackend *b);
static void backend_unref(TrimBackend *b);
static void publish_audio(TrimBackend *b);

static const mpv_node *map_field(const mpv_node *map, const char *key)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], key) == 0) return &map->u.list->values[i];
    return NULL;
}

static const char *map_text(const mpv_node *map, const char *key)
{
    const mpv_node *value = map_field(map, key);
    return value && value->format == MPV_FORMAT_STRING ? value->u.string : "";
}

static gboolean map_flag(const mpv_node *map, const char *key)
{
    const mpv_node *value = map_field(map, key);
    return value && value->format == MPV_FORMAT_FLAG && value->u.flag;
}

static gint64 map_integer(const mpv_node *map, const char *key)
{
    const mpv_node *value = map_field(map, key);
    return value && value->format == MPV_FORMAT_INT64 ? value->u.int64
        : value && value->format == MPV_FORMAT_DOUBLE && isfinite(value->u.double_)
            ? (gint64)value->u.double_ : -1;
}

static mpv_node node_text(const char *value)
{
    return (mpv_node){.format = MPV_FORMAT_STRING, .u.string = (char *)(value ? value : "")};
}

static mpv_node node_flag(gboolean value)
{
    return (mpv_node){.format = MPV_FORMAT_FLAG, .u.flag = !!value};
}

static mpv_node node_integer(gint64 value)
{
    return (mpv_node){.format = MPV_FORMAT_INT64, .u.int64 = value};
}

static void publish_map(TrimBackend *b, const char *name, int count,
                         char **keys, mpv_node *values)
{
    if (b->stopped) return;
    mpv_node_list list = {.num = count, .keys = keys, .values = values};
    mpv_node node = {.format = MPV_FORMAT_NODE_MAP, .u.list = &list};
    mpv_set_property(b->mpv, name, MPV_FORMAT_NODE, &node);
}

static void command(TrimBackend *b, const char **args)
{
    if (!b->stopped) mpv_command_async(b->mpv, 0, args);
}

static gboolean property_flag(TrimBackend *b, const char *name, gboolean fallback)
{
    int value;
    return mpv_get_property(b->mpv, name, MPV_FORMAT_FLAG, &value) >= 0 ? !!value : fallback;
}

static double property_number(TrimBackend *b, const char *name, double fallback)
{
    double value;
    return mpv_get_property(b->mpv, name, MPV_FORMAT_DOUBLE, &value) >= 0 && isfinite(value)
        ? value : fallback;
}

static gboolean available(const TrimBackend *b)
{
    return b->loaded && b->duration_ms > 0 && b->video_available;
}

static char *format_boundary(gint64 ms)
{
    ms = MAX((gint64)0, ms);
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT
        ":%02" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT,
        ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

static void publish_mode(TrimBackend *b)
{
    update_shortcuts(b);
    g_autofree mpv_node *times = g_new0(mpv_node, b->keyframes->len);
    for (guint i = 0; i < b->keyframes->len; ++i)
        times[i] = (mpv_node){.format = MPV_FORMAT_DOUBLE,
            .u.double_ = g_array_index(b->keyframes, double, i)};
    mpv_node_list list = {.num = (int)b->keyframes->len, .values = times};
    char *keys[] = {"enabled", "keyframes"};
    mpv_node values[] = {node_flag(b->enabled), {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &list}};
    publish_map(b, "user-data/trim-mode", G_N_ELEMENTS(keys), keys, values);
}

static void publish_controls(TrimBackend *b)
{
    update_shortcuts(b);
    g_autofree char *first = format_boundary(b->start_ms);
    g_autofree char *last = format_boundary(b->end_ms);
    char *keys[] = {"available", "start_ms", "end_ms", "duration_ms", "file_serial", "start_text", "end_text"};
    mpv_node values[] = {node_flag(available(b)), node_integer(b->start_ms), node_integer(b->end_ms),
        node_integer(b->duration_ms), node_integer(b->file_serial), node_text(first), node_text(last)};
    publish_map(b, "user-data/trim-controls", G_N_ELEMENTS(keys), keys, values);
}

static void audio_track_free(gpointer data)
{
    AudioTrack *track = data;
    g_free(track->title); g_free(track->lang); g_free(track->codec); g_free(track);
}

static AudioTrack *audio_track_at(TrimBackend *b, gint64 index)
{
    for (guint i = 0; i < b->audio_tracks->len; ++i) {
        AudioTrack *track = g_ptr_array_index(b->audio_tracks, i);
        if (track->ff_index == index) return track;
    }
    return NULL;
}

static void publish_audio(TrimBackend *b)
{
    guint count = b->audio_tracks->len, selected = 0;
    char *track_keys[] = {"id", "ff_index", "title", "lang", "codec", "included"};
    g_autofree mpv_node *tracks = g_new0(mpv_node, count);
    g_autofree mpv_node_list *maps = g_new0(mpv_node_list, count);
    g_autofree mpv_node *fields = g_new0(mpv_node, (gsize)count * G_N_ELEMENTS(track_keys));
    for (guint i = 0; i < count; ++i) {
        AudioTrack *track = g_ptr_array_index(b->audio_tracks, i);
        mpv_node *values = fields + (gsize)i * G_N_ELEMENTS(track_keys);
        values[0] = node_integer(track->id);
        values[1] = node_integer(track->ff_index);
        values[2] = node_text(track->title);
        values[3] = node_text(track->lang);
        values[4] = node_text(track->codec);
        values[5] = node_flag(track->included);
        selected += track->included;
        maps[i] = (mpv_node_list){.num = G_N_ELEMENTS(track_keys), .keys = track_keys, .values = values};
        tracks[i] = (mpv_node){.format = MPV_FORMAT_NODE_MAP, .u.list = &maps[i]};
    }
    mpv_node_list list = {.num = (int)count, .values = tracks};
    char *keys[] = {"file_serial", "tracks", "selected_count", "total_count"};
    mpv_node values[] = {node_integer(b->file_serial),
        {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &list}, node_integer(selected), node_integer(count)};
    publish_map(b, "user-data/trim-audio", G_N_ELEMENTS(keys), keys, values);
}

static gboolean refresh_audio(TrimBackend *b)
{
    if (!b->loaded) return FALSE;
    char *path = mpv_get_property_string(b->mpv, "path");
    if (!path || !*path || (b->audio_source && g_strcmp0(path, b->audio_source) != 0)) {
        mpv_free(path);
        return FALSE;
    }
    mpv_node tracks = {0};
    if (mpv_get_property(b->mpv, "track-list", MPV_FORMAT_NODE, &tracks) < 0 ||
        tracks.format != MPV_FORMAT_NODE_ARRAY || !tracks.u.list) {
        mpv_free_node_contents(&tracks);
        mpv_free(path);
        return FALSE;
    }
    char *current = mpv_get_property_string(b->mpv, "path");
    gboolean same_source = g_strcmp0(path, current) == 0;
    mpv_free(current);
    if (!same_source) { mpv_free(path); mpv_free_node_contents(&tracks); return FALSE; }
    GPtrArray *next = g_ptr_array_new_with_free_func(audio_track_free);
    for (int i = 0; i < tracks.u.list->num; ++i) {
        const mpv_node *value = &tracks.u.list->values[i];
        gint64 index = map_integer(value, "ff-index");
        if (g_strcmp0(map_text(value, "type"), "audio") != 0 || index < 0 ||
            map_flag(value, "external") || *map_text(value, "external-filename")) continue;
        gboolean duplicate = FALSE;
        for (guint n = 0; n < next->len; ++n)
            if (((AudioTrack *)g_ptr_array_index(next, n))->ff_index == index) { duplicate = TRUE; break; }
        if (duplicate) continue;
        AudioTrack *previous = audio_track_at(b, index);
        AudioTrack *track = g_new0(AudioTrack, 1);
        *track = (AudioTrack){.id = map_integer(value, "id"), .ff_index = index,
            .title = g_strdup(map_text(value, "title")), .lang = g_strdup(map_text(value, "lang")),
            .codec = g_strdup(map_text(value, "codec")), .included = previous ? previous->included : TRUE};
        g_ptr_array_add(next, track);
    }
    mpv_free_node_contents(&tracks);
    if (!b->audio_source) b->audio_source = g_strdup(path);
    mpv_free(path);
    g_ptr_array_unref(b->audio_tracks);
    b->audio_tracks = next;
    publish_audio(b);
    return TRUE;
}

static gboolean audio_argument_integer(const char *text, gint64 *value)
{
    if (!text || !*text) return FALSE;
    for (const char *p = text; *p; ++p) if (!g_ascii_isdigit(*p)) return FALSE;
    errno = 0;
    *value = g_ascii_strtoll(text, NULL, 10);
    return errno != ERANGE && *value >= 0;
}

static void select_audio(TrimBackend *b, gboolean all, int argc, const char *const *argv)
{
    if (!b->enabled || !available(b) || trim_export_busy(b->exporter) || !argv ||
        argc != (all ? 2 : 3)) return;
    const char *state = argv[all ? 0 : 1];
    gint64 serial, index = -1;
    if (!state || (!g_str_equal(state, "yes") && !g_str_equal(state, "no")) ||
        !audio_argument_integer(argv[all ? 1 : 2], &serial) || serial != b->file_serial ||
        (!all && !audio_argument_integer(argv[0], &index)) || !refresh_audio(b)) return;
    gboolean included = g_str_equal(state, "yes");
    if (all) {
        for (guint i = 0; i < b->audio_tracks->len; ++i)
            ((AudioTrack *)g_ptr_array_index(b->audio_tracks, i))->included = included;
    } else {
        AudioTrack *track = audio_track_at(b, index);
        if (!track) return;
        track->included = included;
    }
    publish_audio(b);
}

static void publish_field_result(TrimBackend *b, const char *boundary, const char *serial,
                                  gboolean valid)
{
    g_autofree char *text = format_boundary(g_strcmp0(boundary, "end") == 0 ? b->end_ms : b->start_ms);
    char *keys[] = {"boundary", "serial", "valid", "text"};
    mpv_node values[] = {node_text(boundary), node_text(serial), node_flag(valid), node_text(text)};
    publish_map(b, "user-data/trim-field-result", G_N_ELEMENTS(keys), keys, values);
}

static void publish_failure(TrimBackend *b, const char *message)
{
    char *keys[] = {"busy", "success", "error", "serial"};
    mpv_node values[] = {node_flag(FALSE), node_flag(FALSE), node_text(message), node_integer(g_get_monotonic_time())};
    publish_map(b, "user-data/trim-export", G_N_ELEMENTS(keys), keys, values);
}

static void update_shortcuts(TrimBackend *b)
{
    gboolean active = !b->stopped && b->enabled && available(b) && !b->gtk_editing;
    if (active == b->shortcuts_active) return;
    b->shortcuts_active = active;
    if (active) {
        const char *enable[] = {"enable-section", SHORTCUT_SECTION, "allow-hide-cursor+allow-vo-dragging", NULL};
        const char *edits[] = {"enable-section", KEYBINDINGS_EDIT_SECTION, NULL};
        const char *defaults[] = {"enable-section", "default", NULL};
        command(b, enable);
        command(b, edits);
        command(b, defaults);
    } else {
        const char *disable[] = {"disable-section", SHORTCUT_SECTION, NULL};
        command(b, disable);
    }
}

static void load_options(TrimBackend *b, const char *config_path, const char *runtime_directory)
{
    b->ffmpeg_path = g_strdup("ffmpeg");
    b->ffprobe_path = g_strdup("ffprobe");
    b->open_copy = b->open_replace = TRUE;
    g_autofree char *directory = g_path_get_dirname(config_path);
    g_autofree char *filename = g_build_filename(directory, "script-opts", "trim-mode.conf", NULL);
    b->state_path = g_build_filename(runtime_directory ? runtime_directory : directory,
                                   "trim-mode-state.json", NULL);
    g_autofree char *contents = NULL;
    if (g_file_get_contents(filename, &contents, NULL, NULL)) {
        g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
        for (guint i = 0; lines[i]; ++i) {
            char *comment = strchr(lines[i], '#');
            if (comment) *comment = '\0';
            char *equals = strchr(lines[i], '=');
            if (!equals) continue;
            *equals++ = '\0';
            char *key = g_strstrip(lines[i]), *value = g_strstrip(equals);
            gboolean enabled = g_str_equal(value, "yes") || g_str_equal(value, "true");
            if (g_str_equal(key, "remember")) b->remember = enabled;
            else if (g_str_equal(key, "open-copy")) b->open_copy = enabled;
            else if (g_str_equal(key, "open-replace")) b->open_replace = enabled;
            else if (*value && g_str_equal(key, "ffmpeg_path")) {
                g_free(b->ffmpeg_path); b->ffmpeg_path = g_strdup(value);
            } else if (*value && g_str_equal(key, "ffprobe_path")) {
                g_free(b->ffprobe_path); b->ffprobe_path = g_strdup(value);
            }
        }
    }
    if (b->remember) {
        g_autofree char *state = NULL;
        gsize length = 0;
        if (g_file_get_contents(b->state_path, &state, &length, NULL) && length <= 256)
            b->enabled = g_regex_match_simple("\\A\\s*\\{\\s*\"enabled\"\\s*:\\s*true\\s*\\}\\s*\\z", state, 0, 0);
    }
}

static void save_mode(TrimBackend *b)
{
    if (!b->remember) return;
    const char *text = b->enabled ? "{\"enabled\":true}\n" : "{\"enabled\":false}\n";
    GStatBuf status;
    int mode = g_stat(b->state_path, &status) == 0 ? status.st_mode & 0777 : 0600;
    g_autoptr(GError) error = NULL;
    if (!g_file_set_contents_full(b->state_path, text, -1,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, mode, &error))
        g_printerr("mpv-gtk: Could not save Trim Mode: %s\n", error->message);
}

/* Match the supplied parser: decimal fractions are seconds, signs are allowed,
 * negative valid times clamp to zero, and no expression is evaluated. */
static gboolean parse_time(TrimBackend *b, const char *text, double *milliseconds)
{
    if (!text) return FALSE;
    g_autofree char *copy = g_strdup(text);
    char *p = g_strstrip(copy);
    if (!*p || strlen(p) > 256) return FALSE;
    gboolean negative = *p == '-';
    if (*p == '-' || *p == '+') ++p;
    if (!*p) return FALSE;
    for (const char *c = p; *c; ++c)
        if (!g_ascii_isdigit(*c) && *c != ':' && *c != '.') return FALSE;
    g_auto(GStrv) fields = g_strsplit(p, ":", -1);
    guint count = g_strv_length(fields);
    if (count < 1 || count > 3) return FALSE;
    const char *last = fields[count - 1];
    guint digits = 0, dots = 0;
    for (const char *c = last; *c; ++c) {
        if (*c == '.') ++dots; else ++digits;
    }
    if (!digits || dots > 1) return FALSE;
    char *end;
    double total = g_ascii_strtod(last, &end);
    if (*end || !isfinite(total) || total < 0 || (count > 1 && total >= 60)) return FALSE;
    for (guint i = 0; i + 1 < count; ++i) {
        if (!*fields[i]) return FALSE;
        for (const char *c = fields[i]; *c; ++c) if (!g_ascii_isdigit(*c)) return FALSE;
        double value = g_ascii_strtod(fields[i], &end);
        if (*end || !isfinite(value) || (count == 3 && i == 1 && value >= 60)) return FALSE;
        total += value * (count - i == 3 ? 3600 : 60);
    }
    if (!isfinite(total)) return FALSE;
    if (negative) total = 0;
    if (total > (double)b->duration_ms / 1000 + 0.000000001) return FALSE;
    *milliseconds = total * 1000;
    return isfinite(*milliseconds);
}

static gboolean valid_boundary(TrimBackend *b, gboolean end, double milliseconds)
{
    return isfinite(milliseconds) && milliseconds >= 0 && milliseconds <= (double)b->duration_ms + .0000001 &&
        (end ? milliseconds > b->start_ms : milliseconds < b->end_ms);
}

static gboolean apply_boundary(TrimBackend *b, const char *boundary, double milliseconds)
{
    gboolean end = g_strcmp0(boundary, "end") == 0;
    if (g_strcmp0(boundary, "start") != 0 && !end) return FALSE;
    if (!valid_boundary(b, end, milliseconds)) return FALSE;
    double rounded = floor(milliseconds + .5);
    if (!valid_boundary(b, end, rounded)) return FALSE;
    if (end) { b->end_ms = (gint64)rounded; b->follows_duration = FALSE; }
    else b->start_ms = (gint64)rounded;
    return TRUE;
}

static void cancel_keyframe_seek(TrimBackend *b)
{
    if (b->seek_source) { g_source_remove(b->seek_source); b->seek_source = 0; }
    if (b->seek_recovery) { g_source_remove(b->seek_recovery); b->seek_recovery = 0; }
    b->seek_active = b->seek_started = FALSE;
    b->seek_acknowledged = FALSE;
    b->seek_request = 0;
    b->probe_position_request = b->probe_seeking_request = 0;
    b->queued_exact = FALSE;
    b->keyframe_target = -1;
}

static void cache_free(gpointer data)
{
    KeyframeCache *entry = data;
    g_free(entry->key);
    g_array_unref(entry->times);
    g_free(entry);
}

static void stop_scan(TrimBackend *b)
{
    if (b->scan_request) mpv_abort_async_command(b->mpv, b->scan_request);
    b->scan_request = b->parse_request = 0;
    if (b->parse_cancel) g_cancellable_cancel(b->parse_cancel);
    g_clear_object(&b->parse_cancel);
    g_clear_pointer(&b->scan_key, g_free);
}

static void clear_keyframes(TrimBackend *b)
{
    cancel_keyframe_seek(b);
    /* Cache entries share immutable arrays with the active file. */
    g_array_unref(b->keyframes);
    b->keyframes = g_array_new(FALSE, FALSE, sizeof(double));
}

static void reset_file(TrimBackend *b)
{
    cancel_keyframe_seek(b);
    stop_scan(b);
    b->loaded = b->video_available = FALSE;
    b->position_known = FALSE; b->position_sampled = 0; b->position_refresh = 0;
    b->gtk_editing = FALSE;
    b->start_ms = b->end_ms = b->duration_ms = 0;
    b->follows_duration = TRUE;
    ++b->file_serial;
    g_clear_pointer(&b->audio_source, g_free);
    g_ptr_array_set_size(b->audio_tracks, 0);
    g_clear_pointer(&b->cached_file, g_free);
    clear_keyframes(b);
    publish_mode(b);
    publish_controls(b);
    publish_audio(b);
}

static void refresh_duration(TrimBackend *b)
{
    double seconds = property_number(b, "duration", 0);
    gint64 duration = seconds > 0 && seconds < (double)G_MAXINT64 / 1000
        ? (gint64)floor(seconds * 1000 + .000001) : 0;
    if (b->follows_duration) b->end_ms = duration;
    else b->end_ms = MIN(b->end_ms, duration);
    b->duration_ms = duration;
    if (duration > 0 && b->start_ms >= b->end_ms) {
        b->start_ms = 0; b->end_ms = duration;
    } else b->start_ms = MIN(b->start_ms, b->end_ms);
    mpv_node track = {0};
    b->video_available = mpv_get_property(b->mpv, "current-tracks/video", MPV_FORMAT_NODE, &track) >= 0 &&
        track.format == MPV_FORMAT_NODE_MAP && !map_flag(&track, "image") && !map_flag(&track, "albumart");
    mpv_free_node_contents(&track);
    publish_controls(b);
}

static char *local_path(TrimBackend *b, const char *path)
{
    if (!path || !*path) return NULL;
    if (g_str_has_prefix(path, "file://")) return g_filename_from_uri(path, NULL, NULL);
    if (strstr(path, "://")) return NULL;
    char *working = mpv_get_property_string(b->mpv, "working-directory");
    char *absolute = g_canonicalize_filename(path, working && g_path_is_absolute(working) ? working : NULL);
    mpv_free(working);
    char *resolved = realpath(absolute, NULL);
    if (resolved) { g_free(absolute); return resolved; }
    return absolute;
}

static gint compare_times(gconstpointer a, gconstpointer c)
{
    double x = *(const double *)a, y = *(const double *)c;
    return (x > y) - (x < y);
}

static gboolean parse_decimal(const char *text, double *value)
{
    if (!text || !*text) return FALSE;
    char *end;
    double parsed = g_ascii_strtod(text, &end);
    if (end == text || *end || !isfinite(parsed)) return FALSE;
    *value = parsed;
    return TRUE;
}

typedef struct {
    TrimBackend *owner;
    guint64 request;
    char *key, *output;
    double offset;
} ParseTask;

static char *capture_text(const mpv_node *result, const char *name, gsize *size)
{
    const mpv_node *value = map_field(result, name);
    if (value && value->format == MPV_FORMAT_BYTE_ARRAY && value->u.ba) {
        *size = value->u.ba->size;
        return *size && value->u.ba->data ? g_strndup(value->u.ba->data, *size) : g_strdup("");
    }
    const char *text = value && value->format == MPV_FORMAT_STRING ? value->u.string : "";
    *size = strlen(text);
    return g_strdup(text);
}

static void parse_task_free(gpointer data)
{
    ParseTask *task = data;
    backend_unref(task->owner);
    g_free(task->key); g_free(task->output); g_free(task);
}

static void parse_packets(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source;
    ParseTask *input = data;
    GArray *times = g_array_new(FALSE, FALSE, sizeof(double));
    char *line = input->output;
    guint iterations = 0;
    while (line && *line) {
        if (!(iterations++ % 1024) && g_cancellable_is_cancelled(cancel)) {
            g_array_unref(times);
            g_task_return_error_if_cancelled(task);
            return;
        }
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        g_strchomp(line);
        char *second = strchr(line, ',');
        if (second) {
            *second++ = '\0';
            char *flags = strchr(second, ',');
            if (flags) {
                *flags++ = '\0';
                char *trailing = strchr(flags, ',');
                if (trailing) *trailing = '\0';
                double timestamp;
                if (strchr(flags, 'K') && !strchr(flags, 'D') &&
                    (parse_decimal(line, &timestamp) || parse_decimal(second, &timestamp))) {
                    timestamp -= input->offset;
                    if (timestamp >= -.001) {
                        timestamp = MAX(0, timestamp);
                        g_array_append_val(times, timestamp);
                    }
                }
            }
        }
        line = next;
    }
    g_array_sort(times, compare_times);
    guint unique = 0;
    for (guint i = 0; i < times->len; ++i) {
        double time = g_array_index(times, double, i);
        if (!unique || time != g_array_index(times, double, unique - 1))
            g_array_index(times, double, unique++) = time;
    }
    g_array_set_size(times, unique);
    g_task_return_pointer(task, times, (GDestroyNotify)g_array_unref);
}

static void packets_parsed(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)data;
    GTask *task = G_TASK(result);
    ParseTask *input = g_task_get_task_data(task);
    TrimBackend *b = input->owner;
    g_autoptr(GError) error = NULL;
    GArray *times = g_task_propagate_pointer(task, &error);
    if (b->stopped || b->parse_request != input->request) {
        if (times) g_array_unref(times);
        return;
    }
    b->parse_request = 0;
    g_clear_object(&b->parse_cancel);
    if (!times) {
        g_clear_pointer(&b->cached_file, g_free);
        return;
    }
    g_array_unref(b->keyframes);
    b->keyframes = g_array_ref(times);
    KeyframeCache *entry = g_new0(KeyframeCache, 1);
    entry->key = g_strdup(input->key);
    entry->times = times;
    g_queue_push_tail(&b->cache, entry);
    if (g_queue_get_length(&b->cache) > 4) cache_free(g_queue_pop_head(&b->cache));
    publish_mode(b);
}

static void finish_scan(TrimBackend *b, const mpv_event *event)
{
    guint64 request = b->scan_request;
    b->scan_request = 0;
    const mpv_event_command *reply = event->data;
    const mpv_node *result = reply ? &reply->result : NULL;
    gsize size;
    g_autofree char *output = capture_text(result, "stdout", &size);
    if (event->error < 0 || map_integer(result, "status") != 0 || size >= BACKEND_CAPTURE_LIMIT) {
        gsize error_size;
        g_autofree char *detail = capture_text(result, "stderr", &error_size);
        g_printerr("mpv-gtk: Could not read keyframe markers with ffprobe: %s\n",
            *detail ? detail : (size >= BACKEND_CAPTURE_LIMIT ? "capture limit exceeded" : mpv_error_string(event->error)));
        g_clear_pointer(&b->cached_file, g_free);
        g_clear_pointer(&b->scan_key, g_free);
        return;
    }
    ParseTask *input = g_new0(ParseTask, 1);
    g_atomic_int_inc(&b->references);
    input->owner = b;
    input->request = request;
    input->key = g_steal_pointer(&b->scan_key);
    input->output = g_steal_pointer(&output);
    input->offset = b->scan_offset;
    b->parse_request = request;
    b->parse_cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, b->parse_cancel, packets_parsed, NULL);
    g_task_set_task_data(task, input, parse_task_free);
    g_task_run_in_thread(task, parse_packets);
    g_object_unref(task);
}

static void scan_keyframes(TrimBackend *b)
{
    if (b->stopped || !b->loaded || !b->ever_enabled) return;
    mpv_node track = {0};
    if (mpv_get_property(b->mpv, "current-tracks/video", MPV_FORMAT_NODE, &track) < 0 ||
        track.format != MPV_FORMAT_NODE_MAP || map_flag(&track, "image") || map_flag(&track, "albumart")) {
        stop_scan(b);
        g_clear_pointer(&b->cached_file, g_free);
        clear_keyframes(b);
        publish_mode(b);
        mpv_free_node_contents(&track);
        return;
    }
    char *main_path = mpv_get_property_string(b->mpv, "path");
    const char *external = map_text(&track, "external-filename");
    g_autofree char *path = local_path(b, *external ? external : main_path);
    mpv_free(main_path);
    gint64 index = map_integer(&track, "ff-index");
    g_autofree char *stream = index >= 0 ? g_strdup_printf("%" G_GINT64_FORMAT, index) : g_strdup("v:0");
    mpv_free_node_contents(&track);
    GStatBuf status;
    if (!path || g_stat(path, &status) < 0 || !S_ISREG(status.st_mode)) {
        stop_scan(b);
        g_clear_pointer(&b->cached_file, g_free);
        clear_keyframes(b);
        publish_mode(b);
        return;
    }
    double offset = property_flag(b, "rebase-start-time", TRUE)
        ? property_number(b, "demuxer-start-time", 0) : 0;
    char offset_text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(offset_text, sizeof(offset_text), offset);
    g_autofree char *key = g_strdup_printf("%s\n%s\n%s\n%" G_GINT64_FORMAT "\n%" G_GINT64_FORMAT
        ":%ld\n%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT,
        path, stream, offset_text, (gint64)status.st_size, (gint64)status.st_mtim.tv_sec,
        status.st_mtim.tv_nsec, (guint64)status.st_dev, (guint64)status.st_ino);
    if (g_strcmp0(b->cached_file, key) == 0) return;
    stop_scan(b);
    g_free(b->cached_file); b->cached_file = g_strdup(key);
    clear_keyframes(b);
    for (GList *item = b->cache.head; item; item = item->next) {
        KeyframeCache *entry = item->data;
        if (g_str_equal(entry->key, key)) {
            g_array_unref(b->keyframes); b->keyframes = g_array_ref(entry->times);
            publish_mode(b);
            return;
        }
    }
    publish_mode(b);
    const char *argv[] = {b->ffprobe_path, "-v", "error", "-select_streams", stream,
        "-show_packets", "-show_entries", "packet=pts_time,dts_time,flags:packet_side_data=",
        "-of", "csv=p=0", "-i", path, NULL};
    guint count = G_N_ELEMENTS(argv) - 1;
    g_autofree mpv_node *arguments = g_new0(mpv_node, count);
    for (guint i = 0; i < count; ++i) arguments[i] = node_text(argv[i]);
    mpv_node_list argument_list = {.num = (int)count, .values = arguments};
    char *keys[] = {"name", "args", "playback_only", "capture_stdout", "capture_stderr", "capture_size"};
    mpv_node values[] = {node_text("subprocess"), {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &argument_list},
        node_flag(TRUE), node_flag(TRUE), node_flag(TRUE), node_integer(BACKEND_CAPTURE_LIMIT)};
    mpv_node_list command_list = {.num = G_N_ELEMENTS(keys), .keys = keys, .values = values};
    mpv_node request = {.format = MPV_FORMAT_NODE_MAP, .u.list = &command_list};
    b->scan_request = BACKEND_SCAN_BASE + ++b->scan_serial;
    b->scan_offset = offset;
    b->scan_key = g_strdup(key);
    int result = mpv_command_node_async(b->mpv, b->scan_request, &request);
    if (result < 0) {
        b->scan_request = 0;
        g_clear_pointer(&b->scan_key, g_free);
        g_clear_pointer(&b->cached_file, g_free);
        g_printerr("mpv-gtk: Could not start keyframe scan: %s\n", mpv_error_string(result));
    }
}

static void sample_position(TrimBackend *b, double value)
{
    if (!isfinite(value)) return;
    b->playback_position = MAX(0, value);
    b->position_sampled = g_get_monotonic_time();
    b->position_known = TRUE;
}

static double current_position(TrimBackend *b)
{
    if (!b->position_known) return NAN;
    double position = b->playback_position;
    if (!b->playback_paused && !b->playback_seeking && !b->playback_buffering && b->position_sampled)
        position += MAX((gint64)0, g_get_monotonic_time() - b->position_sampled) / 1000000.0 * b->playback_speed;
    return position;
}

static double keyframe_seek_allowance(TrimBackend *b)
{
    double allowance = .002;
    if (!b->playback_paused)
        allowance += (g_get_monotonic_time() - b->seek_issued) / 1000000.0 * b->playback_speed
            + 1 / MAX(1, b->playback_fps);
    return allowance;
}

static void request_seek_probe(TrimBackend *b);
static void queue_keyframe_seek(TrimBackend *b, gboolean exact);

static void finish_keyframe_seek(TrimBackend *b)
{
    if (b->seek_recovery) { g_source_remove(b->seek_recovery); b->seek_recovery = 0; }
    b->seek_active = b->seek_started = b->seek_acknowledged = FALSE;
    b->seek_request = 0;
    b->probe_position_request = b->probe_seeking_request = 0;
}

static gboolean recover_keyframe_seek(gpointer data)
{
    TrimBackend *b = data;
    b->seek_recovery = 0;
    if (!b->stopped && b->seek_active) {
        /* A superseded native seek may omit its restart notification. Probe
         * asynchronously rather than waiting forever or polling the core on
         * the GTK/render thread. Only one probe can be outstanding. */
        request_seek_probe(b);
        if (b->seek_active)
            b->seek_recovery = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, SEEK_RECOVERY_MS,
                                                 recover_keyframe_seek, b, NULL);
    }
    return G_SOURCE_REMOVE;
}

static void seek_keyframe(TrimBackend *b, gint index, gboolean exact)
{
    if (index < 0 || (guint)index >= b->keyframes->len) return;
    double target = g_array_index(b->keyframes, double, index);
    double following = (guint)(index + 1) < b->keyframes->len
        ? g_array_index(b->keyframes, double, index + 1) : (double)b->duration_ms / 1000;
    double point = exact ? target : target + MIN(.01, MAX(0, following - target) / 2);
    b->seek_active = TRUE; b->seek_started = FALSE; b->seek_exact = exact;
    b->seek_acknowledged = FALSE;
    b->seek_request = BACKEND_SEEK_ID + ++b->seek_serial;
    b->seek_index = index; b->seek_point = point; b->seek_issued = g_get_monotonic_time();
    char value[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(value, sizeof(value), point);
    const char *args[] = {"seek", value, exact ? "absolute+exact" : "absolute+keyframes", NULL};
    if (mpv_command_async(b->mpv, b->seek_request, args) < 0) cancel_keyframe_seek(b);
    else b->seek_recovery = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, SEEK_RECOVERY_MS,
                                               recover_keyframe_seek, b, NULL);
}

static gboolean dispatch_keyframe_seek(gpointer data)
{
    TrimBackend *b = data;
    b->seek_source = 0;
    if (!b->stopped && b->enabled && available(b) && !b->seek_active)
        seek_keyframe(b, b->keyframe_target, b->queued_exact);
    return G_SOURCE_REMOVE;
}

static void queue_keyframe_seek(TrimBackend *b, gboolean exact)
{
    b->queued_exact = exact;
    if (!b->seek_source && !b->seek_active)
        b->seek_source = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, SEEK_DISPATCH_MS,
                                            dispatch_keyframe_seek, b, NULL);
}

static void complete_seek_probe(TrimBackend *b)
{
    if (!b->seek_active || !b->probe_position_ready || !b->probe_seeking_ready || b->probe_seeking) return;
    double pos = b->probe_position;
    gboolean exact = b->seek_exact;
    double target = g_array_index(b->keyframes, double, b->seek_index);
    double allowance = keyframe_seek_allowance(b);
    gint completed_index = b->seek_index;
    finish_keyframe_seek(b);
    if (!isfinite(pos)) { cancel_keyframe_seek(b); return; }
    sample_position(b, pos);
    b->playback_seeking = FALSE;
    /* New clicks replace the destination without enqueueing native seeks.
     * Finish this seek first and return to GTK before dispatching the latest
     * target, so both event handling and rendering keep making progress. */
    if (b->keyframe_target != completed_index) queue_keyframe_seek(b, FALSE);
    else if ((pos < target - .002 || pos > target + allowance) && !exact)
        queue_keyframe_seek(b, TRUE);
}

static void request_seek_probe(TrimBackend *b)
{
    if (!b->seek_active || b->probe_position_request || b->probe_seeking_request) return;
    b->probe_position_ready = b->probe_seeking_ready = FALSE;
    b->probe_position = NAN; b->probe_seeking = TRUE;
    b->probe_position_request = BACKEND_SEEK_SAMPLE_BASE + ++b->seek_serial;
    b->probe_seeking_request = BACKEND_SEEK_SAMPLE_BASE + ++b->seek_serial;
    if (mpv_get_property_async(b->mpv, b->probe_position_request, "time-pos", MPV_FORMAT_DOUBLE) < 0) {
        cancel_keyframe_seek(b);
        return;
    }
    if (mpv_get_property_async(b->mpv, b->probe_seeking_request, "seeking", MPV_FORMAT_FLAG) < 0) {
        cancel_keyframe_seek(b);
        return;
    }
}

static void refresh_position_async(TrimBackend *b)
{
    if (b->position_refresh) return;
    b->position_refresh = BACKEND_SEEK_SAMPLE_BASE + ++b->seek_serial;
    if (mpv_get_property_async(b->mpv, b->position_refresh, "time-pos", MPV_FORMAT_DOUBLE) < 0)
        b->position_refresh = 0;
}

static void cache_seek_property(TrimBackend *b, guint index, const mpv_event_property *property)
{
    if (!property || !property->data) {
        if (index == 0) b->position_known = FALSE;
        return;
    }
    if (property->format == MPV_FORMAT_DOUBLE) {
        double value = *(double *)property->data;
        if (!isfinite(value)) return;
        if (index == 0) sample_position(b, value);
        else if (index == 3 && value > 0) {
            sample_position(b, current_position(b));
            b->playback_speed = value;
        } else if (index == 4) b->playback_fps = MAX(1, value);
    } else if (property->format == MPV_FORMAT_FLAG) {
        gboolean value = *(int *)property->data != 0;
        sample_position(b, current_position(b));
        if (index == 1) b->playback_paused = value;
        else if (index == 2) {
            b->playback_seeking = value;
            if (!value && b->seek_active && b->seek_started) request_seek_probe(b);
        } else if (index == 5) b->playback_buffering = value;
    }
}

static void jump_keyframe(TrimBackend *b, const char *direction)
{
    if (!b->enabled || !available(b) || !b->keyframes->len) return;
    gboolean have_target = b->keyframe_target >= 0 && (guint)b->keyframe_target < b->keyframes->len;
    gboolean pending = b->seek_active || b->seek_source;
    double pos = pending && have_target
        ? g_array_index(b->keyframes, double, b->keyframe_target) : current_position(b);
    if (!isfinite(pos)) { refresh_position_async(b); return; }
    double epsilon = MIN(.002, .2 / MAX(1, b->playback_fps));
    if (have_target) {
        double anchor = g_array_index(b->keyframes, double, b->keyframe_target);
        double following = (guint)(b->keyframe_target + 1) < b->keyframes->len
            ? g_array_index(b->keyframes, double, b->keyframe_target + 1) : b->duration_ms / 1000.0;
        if (pending || (pos >= anchor - epsilon && pos < following - epsilon)) pos = anchor;
        else b->keyframe_target = -1;
    }
    gint low = 0, high = (gint)b->keyframes->len - 1;
    if (g_strcmp0(direction, "previous") == 0) {
        while (low <= high) {
            gint mid = low + (high - low) / 2;
            if (g_array_index(b->keyframes, double, mid) < pos - epsilon) low = mid + 1;
            else high = mid - 1;
        }
        if (high < 0) return;
        b->keyframe_target = high;
    } else if (g_strcmp0(direction, "next") == 0) {
        while (low <= high) {
            gint mid = low + (high - low) / 2;
            if (g_array_index(b->keyframes, double, mid) <= pos + epsilon) low = mid + 1;
            else high = mid - 1;
        }
        if ((guint)low >= b->keyframes->len ||
            g_array_index(b->keyframes, double, low) > (double)b->duration_ms / 1000) return;
        b->keyframe_target = low;
    } else return;
    queue_keyframe_seek(b, FALSE);
}

static void playback_restarted(TrimBackend *b)
{
    if (b->seek_active && b->seek_acknowledged && b->seek_started) request_seek_probe(b);
}

void trim_backend_set_enabled(TrimBackend *b, gboolean enabled)
{
    if (!b || b->stopped || b->enabled == enabled) return;
    cancel_keyframe_seek(b);
    b->enabled = enabled;
    b->ever_enabled |= enabled;
    if (!enabled) b->gtk_editing = FALSE;
    save_mode(b);
    publish_mode(b);
    scan_keyframes(b);
}

static gboolean binding_action(TrimBackend *b, const char *binding, const char *state)
{
    if (!binding || !*binding) return FALSE;
    const char *name = g_str_has_prefix(binding, "trim-mode-") ? binding + strlen("trim-mode-") : binding;
    gboolean repeatable = g_str_equal(name, "prev-keyframe") || g_str_equal(name, "previous-keyframe") ||
                          g_str_equal(name, "next-keyframe");
    const char *action = NULL, *argument = NULL;
    if (g_str_equal(name, "toggle")) action = "toggle";
    else if (g_str_equal(name, "prev-keyframe") || g_str_equal(name, "previous-keyframe")) {
        action = "keyframe"; argument = "previous";
    } else if (g_str_equal(name, "next-keyframe")) { action = "keyframe"; argument = "next"; }
    else if (g_str_equal(name, "set-start")) { action = "set-boundary"; argument = "start"; }
    else if (g_str_equal(name, "set-end")) { action = "set-boundary"; argument = "end"; }
    else if (g_str_equal(name, "export-copy")) { action = "export"; argument = "keep"; }
    else if (g_str_equal(name, "export-replace")) { action = "export"; argument = "replace"; }
    if (!action) return FALSE;
    if (state && *state) {
        gboolean mouse = strlen(state) > 1 && state[1] == 'm';
        if ((strlen(state) > 2 && state[2] == 'c') || (state[0] == 'r' && !repeatable) ||
            (mouse ? state[0] != 'u' && state[0] != 'p' : state[0] != 'd' && state[0] != 'r' && state[0] != 'p'))
            return TRUE;
    }
    if (b->gtk_editing && !g_str_equal(action, "toggle")) return TRUE;
    trim_backend_action(b, action, argument ? 1 : 0, &argument);
    return TRUE;
}

static gboolean client_message(TrimBackend *b, const mpv_event_client_message *message)
{
    if (!message || message->num_args < 1) return FALSE;
    const char *name = message->args[0];
    if (g_str_equal(name, "key-binding") && message->num_args >= 2)
        return binding_action(b, message->args[1], message->num_args >= 3 ? message->args[2] : NULL);
    if (binding_action(b, name, NULL)) return TRUE;
    if (g_str_equal(name, "toggle") || g_str_equal(name, "set-enabled") || g_str_equal(name, "set-time") ||
        g_str_equal(name, "gtk-focus") || g_str_equal(name, "set-boundary") ||
        g_str_equal(name, "keyframe") || g_str_equal(name, "export") || g_str_equal(name, "edit-blur") ||
        g_str_equal(name, "set-audio") || g_str_equal(name, "audio-all")) {
        trim_backend_action(b, name, message->num_args - 1, (const char *const *)(message->args + 1));
        return TRUE;
    }
    return FALSE;
}

static void compat_wakeup(void *data)
{
    TrimBackend *b = data;
    uint64_t one = 1;
    ssize_t result;
    do { result = write(b->wake_fd, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
}

static gboolean dispatch_compat(gint fd, GIOCondition condition, gpointer data)
{
    (void)condition;
    TrimBackend *b = data;
    uint64_t count;
    ssize_t result;
    do { result = read(fd, &count, sizeof(count)); } while (result < 0 && errno == EINTR);
    if (b->stopped || !b->compat) return G_SOURCE_CONTINUE;
    for (guint handled = 0; handled < 128; ++handled) {
        mpv_event *event = mpv_wait_event(b->compat, 0);
        if (event->event_id == MPV_EVENT_NONE) return G_SOURCE_CONTINUE;
        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) client_message(b, event->data);
        else if (event->event_id == MPV_EVENT_SHUTDOWN) {
            b->wake_source = 0;
            mpv_set_wakeup_callback(b->compat, NULL, NULL);
            mpv_destroy(b->compat); b->compat = NULL;
            return G_SOURCE_REMOVE;
        }
    }
    /* Leave a render/input turn between bounded compatibility batches. */
    compat_wakeup(b);
    return G_SOURCE_CONTINUE;
}

static void create_compat_client(TrimBackend *b)
{
    b->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (b->wake_fd < 0) return;
    b->compat = mpv_create_weak_client(b->mpv, "trim_mode");
    if (!b->compat || g_strcmp0(mpv_client_name(b->compat), "trim_mode") != 0) {
        if (b->compat) mpv_destroy(b->compat);
        b->compat = NULL; close(b->wake_fd); b->wake_fd = -1;
        g_printerr("mpv-gtk: Could not register the native Trim Mode compatibility client.\n");
        return;
    }
    for (int event = 1; event <= MPV_EVENT_HOOK; ++event)
        if (event != MPV_EVENT_CLIENT_MESSAGE && event != MPV_EVENT_SHUTDOWN)
            mpv_request_event(b->compat, event, 0);
    b->wake_source = g_unix_fd_add_full(G_PRIORITY_DEFAULT_IDLE, b->wake_fd,
        G_IO_IN | G_IO_HUP | G_IO_ERR, dispatch_compat, b, NULL);
    mpv_set_wakeup_callback(b->compat, compat_wakeup, b);
}

TrimBackend *trim_backend_new(mpv_handle *mpv, const char *config_path)
{
    return trim_backend_new_with_runtime(mpv, config_path, NULL);
}

TrimBackend *trim_backend_new_with_runtime(mpv_handle *mpv, const char *config_path,
                                            const char *runtime_directory)
{
    g_return_val_if_fail(mpv != NULL && config_path != NULL, NULL);
    TrimBackend *b = g_new0(TrimBackend, 1);
    b->references = 1;
    b->mpv = mpv;
    b->wake_fd = -1;
    b->follows_duration = TRUE;
    b->keyframe_target = -1;
    b->playback_speed = 1; b->playback_fps = 25;
    b->keyframes = g_array_new(FALSE, FALSE, sizeof(double));
    b->audio_tracks = g_ptr_array_new_with_free_func(audio_track_free);
    load_options(b, config_path, runtime_directory);
    b->ever_enabled = b->enabled;
    b->exporter = trim_export_new(mpv);
    create_compat_client(b);
    const char *prefix = b->compat ? "script-message-to trim_mode" : "script-message mpv-gtk trim";
    g_autofree char *bindings = g_strdup_printf(
        "[ %s keyframe previous\n] %s keyframe next\ni %s set-boundary start\n"
        "o %s set-boundary end\nCtrl+e %s export keep\nCtrl+E %s export replace\n",
        prefix, prefix, prefix, prefix, prefix, prefix);
    const char *define[] = {"define-section", SHORTCUT_SECTION, bindings, "force", NULL};
    command(b, define);
    mpv_observe_property(mpv, BACKEND_OBSERVE_BASE, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, BACKEND_OBSERVE_BASE + 1, "vid", MPV_FORMAT_NODE);
    mpv_observe_property(mpv, BACKEND_OBSERVE_BASE + 2, "track-list", MPV_FORMAT_NODE);
    for (guint i = 0; i < G_N_ELEMENTS(seek_properties); ++i)
        mpv_observe_property(mpv, BACKEND_SEEK_OBSERVE_BASE + i,
            seek_properties[i].name, seek_properties[i].format);
    double position;
    b->loaded = mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &position) >= 0 &&
        !property_flag(b, "idle-active", TRUE);
    if (b->loaded) sample_position(b, position);
    refresh_duration(b);
    if (!refresh_audio(b)) publish_audio(b);
    publish_mode(b);
    if (b->enabled) scan_keyframes(b);
    return b;
}

void trim_backend_action(TrimBackend *b, const char *action, int argc, const char *const *argv)
{
    if (!b || b->stopped || !action) return;
    const char *argument = argc > 0 && argv && argv[0] ? argv[0] : "";
    if (g_str_equal(action, "toggle")) trim_backend_set_enabled(b, !b->enabled);
    else if (g_str_equal(action, "set-enabled")) trim_backend_set_enabled(b, g_str_equal(argument, "yes"));
    else if (g_str_equal(action, "gtk-focus") || g_str_equal(action, "edit-blur")) {
        b->gtk_editing = g_str_equal(action, "gtk-focus") && g_str_equal(argument, "yes");
        update_shortcuts(b);
    } else if (g_str_equal(action, "set-time")) {
        double milliseconds;
        gboolean valid = b->enabled && available(b) && argc >= 2 &&
            parse_time(b, argv[1], &milliseconds) && apply_boundary(b, argument, milliseconds);
        publish_controls(b);
        publish_field_result(b, argument, argc >= 3 ? argv[2] : "", valid);
    } else if (g_str_equal(action, "set-boundary")) {
        if (!b->enabled || !available(b)) return;
        double position = property_number(b, "time-pos", NAN);
        if (isfinite(position) && apply_boundary(b, argument,
            MIN((double)b->duration_ms, MAX(0, floor(position * 1000 + .5))))) publish_controls(b);
    } else if (g_str_equal(action, "keyframe")) jump_keyframe(b, argument);
    else if (g_str_equal(action, "set-audio")) select_audio(b, FALSE, argc, argv);
    else if (g_str_equal(action, "audio-all")) select_audio(b, TRUE, argc, argv);
    else if (g_str_equal(action, "export")) {
        if (!b->enabled || !available(b) || (!g_str_equal(argument, "keep") && !g_str_equal(argument, "replace")) ||
            trim_export_busy(b->exporter)) return;
        cancel_keyframe_seek(b);
        if (!refresh_audio(b)) {
            publish_failure(b, "The export audio selection no longer matches the source file.");
            return;
        }
        g_autoptr(GArray) excluded = g_array_new(FALSE, FALSE, sizeof(gint64));
        for (guint i = 0; i < b->audio_tracks->len; ++i) {
            AudioTrack *track = g_ptr_array_index(b->audio_tracks, i);
            if (!track->included) g_array_append_val(excluded, track->ff_index);
        }
        gboolean replace = g_str_equal(argument, "replace");
        TrimExportRequest request = {.ffmpeg_path = b->ffmpeg_path, .ffprobe_path = b->ffprobe_path,
            .expected_input = b->audio_source,
            .replace = replace, .open_result = replace ? b->open_replace : b->open_copy,
            .start_ms = b->start_ms, .end_ms = b->end_ms,
            .excluded_audio = (const gint64 *)excluded->data, .excluded_audio_count = excluded->len};
        g_autoptr(GError) error = NULL;
        if (!trim_export_start(b->exporter, &request, &error))
            publish_failure(b, error ? error->message : "Could not start the export.");
    }
}

gboolean trim_backend_handle_event(TrimBackend *b, const mpv_event *event)
{
    if (!b || b->stopped || !event) return FALSE;
    if (trim_export_handle_event(b->exporter, event)) return TRUE;
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY &&
        event->reply_userdata >= BACKEND_SEEK_SAMPLE_BASE && event->reply_userdata < BACKEND_SCAN_BASE) {
        const mpv_event_property *property = event->data;
        gboolean valid = event->error >= 0 && property && property->data;
        if (event->reply_userdata == b->position_refresh) {
            b->position_refresh = 0;
            if (valid && property->format == MPV_FORMAT_DOUBLE) sample_position(b, *(double *)property->data);
        } else if (event->reply_userdata == b->probe_position_request) {
            b->probe_position_request = 0; b->probe_position_ready = TRUE;
            b->probe_position = valid && property->format == MPV_FORMAT_DOUBLE ? *(double *)property->data : NAN;
            complete_seek_probe(b);
        } else if (event->reply_userdata == b->probe_seeking_request) {
            b->probe_seeking_request = 0; b->probe_seeking_ready = TRUE;
            b->probe_seeking = !valid || property->format != MPV_FORMAT_FLAG || *(int *)property->data;
            complete_seek_probe(b);
        }
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_PROPERTY_CHANGE &&
        event->reply_userdata >= BACKEND_SEEK_OBSERVE_BASE &&
        event->reply_userdata < BACKEND_SEEK_OBSERVE_BASE + G_N_ELEMENTS(seek_properties)) {
        cache_seek_property(b, (guint)(event->reply_userdata - BACKEND_SEEK_OBSERVE_BASE), event->data);
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
        if (b->scan_request && event->reply_userdata == b->scan_request) { finish_scan(b, event); return TRUE; }
        if (event->reply_userdata >= BACKEND_SCAN_BASE && event->reply_userdata < (UINT64_C(1) << 45)) return TRUE;
        if (event->reply_userdata >= BACKEND_SEEK_ID && event->reply_userdata < BACKEND_SEEK_SAMPLE_BASE) {
            if (event->reply_userdata == b->seek_request) {
                if (event->error < 0) cancel_keyframe_seek(b);
                else b->seek_acknowledged = TRUE;
            }
            return TRUE;
        }
    } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && event->reply_userdata == BACKEND_OBSERVE_BASE + 2) {
        refresh_audio(b);
        return TRUE;
    } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE &&
        (event->reply_userdata == BACKEND_OBSERVE_BASE || event->reply_userdata == BACKEND_OBSERVE_BASE + 1)) {
        if (event->reply_userdata == BACKEND_OBSERVE_BASE + 1) cancel_keyframe_seek(b);
        refresh_duration(b);
        if (event->reply_userdata == BACKEND_OBSERVE_BASE + 1) scan_keyframes(b);
        return TRUE;
    } else if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE) reset_file(b);
    else if (event->event_id == MPV_EVENT_FILE_LOADED) {
        b->loaded = TRUE;
        refresh_position_async(b);
        refresh_duration(b);
        refresh_audio(b);
        scan_keyframes(b);
    } else if (event->event_id == MPV_EVENT_SEEK) {
        if (!b->seek_active) cancel_keyframe_seek(b);
        else if (b->seek_acknowledged) {
            if (b->seek_started) cancel_keyframe_seek(b);
            else b->seek_started = TRUE;
        }
    } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) playback_restarted(b);
    else if (event->event_id == MPV_EVENT_CLIENT_MESSAGE && !b->compat)
        return client_message(b, event->data);
    return FALSE;
}

void trim_backend_shutdown(TrimBackend *b)
{
    if (!b || b->stopped) return;
    stop_scan(b);
    cancel_keyframe_seek(b);
    trim_export_shutdown(b->exporter);
    if (b->shortcuts_active) {
        const char *disable[] = {"disable-section", SHORTCUT_SECTION, NULL};
        command(b, disable);
    }
    b->stopped = TRUE;
    mpv_unobserve_property(b->mpv, BACKEND_OBSERVE_BASE);
    mpv_unobserve_property(b->mpv, BACKEND_OBSERVE_BASE + 1);
    mpv_unobserve_property(b->mpv, BACKEND_OBSERVE_BASE + 2);
    for (guint i = 0; i < G_N_ELEMENTS(seek_properties); ++i)
        mpv_unobserve_property(b->mpv, BACKEND_SEEK_OBSERVE_BASE + i);
    if (b->wake_source) { g_source_remove(b->wake_source); b->wake_source = 0; }
    if (b->compat) {
        mpv_set_wakeup_callback(b->compat, NULL, NULL);
        mpv_destroy(b->compat); b->compat = NULL;
    }
    if (b->wake_fd >= 0) { close(b->wake_fd); b->wake_fd = -1; }
}

static void backend_unref(TrimBackend *b)
{
    if (!g_atomic_int_dec_and_test(&b->references)) return;
    g_array_unref(b->keyframes);
    g_ptr_array_unref(b->audio_tracks);
    g_queue_clear_full(&b->cache, cache_free);
    g_free(b->ffmpeg_path); g_free(b->ffprobe_path); g_free(b->state_path);
    g_free(b->audio_source);
    g_free(b->cached_file); g_free(b->scan_key);
    g_free(b);
}

void trim_backend_free(TrimBackend *b)
{
    if (!b) return;
    trim_backend_shutdown(b);
    trim_export_free(b->exporter);
    b->exporter = NULL;
    backend_unref(b);
}
