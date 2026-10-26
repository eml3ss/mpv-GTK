/* Exercise keyframe navigation against real, timed libmpv playback. No GTK
 * display, user configuration, or desktop player is involved. */
#define _GNU_SOURCE
#include "../src/trim_backend.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    mpv_handle *mpv;
    TrimBackend *backend;
    gboolean loaded;
    guint restarts;
} KeyframeHost;

static const mpv_node *field(const mpv_node *map, const char *name)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], name) == 0) return &map->u.list->values[i];
    return NULL;
}

static double position(KeyframeHost *host)
{
    double value = NAN;
    mpv_get_property(host->mpv, "time-pos", MPV_FORMAT_DOUBLE, &value);
    return value;
}

static gboolean flag(KeyframeHost *host, const char *name)
{
    int value = FALSE;
    mpv_get_property(host->mpv, name, MPV_FORMAT_FLAG, &value);
    return value != 0;
}

static void poll_host(KeyframeHost *host)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    mpv_event *event = mpv_wait_event(host->mpv, .002);
    if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE)
        host->loaded = FALSE;
    else if (event->event_id == MPV_EVENT_FILE_LOADED) host->loaded = TRUE;
    else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) ++host->restarts;
    if (event->event_id == MPV_EVENT_END_FILE) {
        const mpv_event_end_file *end = event->data;
        if (end && end->reason == MPV_END_FILE_REASON_ERROR)
            g_error("Keyframe fixture failed: %s", mpv_error_string(end->error));
    }
    if (host->backend) trim_backend_handle_event(host->backend, event);
}

static void pump(KeyframeHost *host, gint64 span)
{
    gint64 deadline = g_get_monotonic_time() + span;
    while (g_get_monotonic_time() < deadline) poll_host(host);
}

static void paused(KeyframeHost *host, gboolean pause)
{
    int value = pause;
    g_assert_cmpint(mpv_set_property(host->mpv, "pause", MPV_FORMAT_FLAG, &value), >=, 0);
    pump(host, 10000);
    g_assert_cmpint(flag(host, "pause"), ==, pause);
}

static void await_position(KeyframeHost *host, double target, gboolean playing,
                             const char *operation)
{
    gint64 deadline = g_get_monotonic_time() + 2000000;
    for (;;) {
        double actual = position(host);
        double tolerance = playing ? .4 : .03;
        if (host->loaded && isfinite(actual) && fabs(actual - target) <= tolerance && !flag(host, "seeking")) break;
        if (g_get_monotonic_time() > deadline)
            g_error("%s: expected keyframe %.1f, got %.3f (pause=%d)",
                    operation, target, actual, flag(host, "pause"));
        poll_host(host);
    }
}

static void manual_seek(KeyframeHost *host, double seconds)
{
    guint restarts = host->restarts;
    char time[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(time, sizeof(time), seconds);
    const char *args[] = {"seek", time, "absolute+exact", NULL};
    g_assert_cmpint(mpv_command(host->mpv, args), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 2000000;
    while (host->restarts == restarts) {
        if (g_get_monotonic_time() > deadline) g_error("Manual seek did not restart playback.");
        poll_host(host);
    }
    await_position(host, seconds, FALSE, "manual seek");
    pump(host, 15000);
}

static void keyframe(KeyframeHost *host, const char *direction, double target, gboolean playing)
{
    const char *arguments[] = {direction};
    trim_backend_action(host->backend, "keyframe", G_N_ELEMENTS(arguments), arguments);
    await_position(host, target, playing, direction);
    /* The user clicks again after playback has moved off the exact I-frame.
     * This interval is the regression: the old backend repeatedly chose the
     * same preceding I-frame instead of continuing backward. */
    pump(host, playing ? 100000 : 30000);
    double actual = position(host);
    g_assert_cmpfloat(actual, >=, target - .03);
    g_assert_cmpfloat(actual, <, target + (playing ? .5 : .03));
    g_assert_cmpint(flag(host, "pause"), ==, !playing);
}

static void await_keyframes(KeyframeHost *host)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    for (;;) {
        mpv_node mode = {0};
        mpv_get_property(host->mpv, "user-data/trim-mode", MPV_FORMAT_NODE, &mode);
        const mpv_node *times = field(&mode, "keyframes");
        gboolean ready = times && times->format == MPV_FORMAT_NODE_ARRAY && times->u.list && times->u.list->num == 12;
        if (ready) {
            for (int i = 0; i < times->u.list->num; ++i) {
                const mpv_node *value = &times->u.list->values[i];
                g_assert_cmpint(value->format, ==, MPV_FORMAT_DOUBLE);
                g_assert_cmpfloat(fabs(value->u.double_ - i), <, .001);
            }
        }
        mpv_free_node_contents(&mode);
        if (ready) return;
        if (g_get_monotonic_time() > deadline) g_error("Deterministic keyframe scan did not finish.");
        poll_host(host);
    }
}

static void load(KeyframeHost *host, const char *filename, const char *start)
{
    paused(host, TRUE);
    g_assert_cmpint(mpv_set_option_string(host->mpv, "start", start), >=, 0);
    host->loaded = FALSE;
    guint restarts = host->restarts;
    const char *args[] = {"loadfile", filename, "replace", NULL};
    g_assert_cmpint(mpv_command(host->mpv, args), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (!host->loaded || host->restarts == restarts) {
        if (g_get_monotonic_time() > deadline) g_error("Keyframe fixture did not load.");
        poll_host(host);
    }
    await_position(host, g_ascii_strtod(start, NULL), FALSE, "file load");
    pump(host, 15000);
}

static void set_option(mpv_handle *mpv, const char *name, const char *value, gboolean optional)
{
    int result = mpv_set_option_string(mpv, name, value);
    g_assert_true(result >= 0 || (optional && result == MPV_ERROR_OPTION_NOT_FOUND));
}

static void generate_media(const char *filename)
{
    char *arguments[] = {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
        "testsrc2=size=160x90:rate=20:duration=12", "-f", "lavfi", "-i",
        "anullsrc=r=48000:cl=mono", "-t", "12", "-c:v", "libx264", "-preset", "ultrafast",
        "-g", "20", "-keyint_min", "20", "-sc_threshold", "0", "-threads", "1",
        "-c:a", "aac", "-shortest", (char *)filename, NULL};
    int status = -1;
    g_autofree char *diagnostic = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, &diagnostic, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("Cannot generate keyframe fixture: %s; %s", error->message, diagnostic ? diagnostic : "");
}

static void remove_tree(const char *path)
{
    GStatBuf info;
    if (g_lstat(path, &info) != 0) return;
    if (!S_ISDIR(info.st_mode)) { g_assert_cmpint(g_unlink(path), ==, 0); return; }
    g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
    g_assert_nonnull(directory);
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        remove_tree(child);
    }
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    g_autofree char *ffprobe = g_find_program_in_path("ffprobe");
    if (!ffmpeg || !ffprobe) { puts("SKIP: FFmpeg tools are unavailable."); return 77; }
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-trim-keyframes-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *filename = g_build_filename(directory, "Keyframes Ω.mp4", NULL);
    g_autofree char *config = g_build_filename(directory, "mpv.conf", NULL);
    g_assert_true(g_file_set_contents(config, "", 0, &error));
    g_assert_no_error(error);
    generate_media(filename);

    KeyframeHost host = {0};
    host.mpv = mpv_create();
    g_assert_nonnull(host.mpv);
    const char *options[][2] = {{"config", "no"}, {"vo", "null"}, {"ao", "null"},
        {"hwdec", "no"}, {"pause", "yes"}, {"idle", "yes"}, {"keep-open", "yes"},
        {"terminal", "no"}, {"load-scripts", "no"}, {"untimed", "no"}};
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i)
        set_option(host.mpv, options[i][0], options[i][1], FALSE);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console", "load-auto-profiles",
        "load-select", "load-positioning", "load-commands", "load-context-menu"};
    for (guint i = 0; i < G_N_ELEMENTS(scripts); ++i) set_option(host.mpv, scripts[i], "no", TRUE);
    g_assert_cmpint(mpv_initialize(host.mpv), >=, 0);
    host.backend = trim_backend_new(host.mpv, config);
    load(&host, filename, "8.2");
    trim_backend_set_enabled(host.backend, TRUE);
    await_keyframes(&host);

    paused(&host, FALSE);
    for (int target = 8; target >= 5; --target) keyframe(&host, "previous", target, TRUE);
    puts("PASS repeated previous while playing: 8 -> 7 -> 6 -> 5, without pausing.");

    paused(&host, TRUE);
    manual_seek(&host, 8.2);
    keyframe(&host, "previous", 8, FALSE);
    keyframe(&host, "previous", 7, FALSE);
    keyframe(&host, "next", 8, FALSE);

    manual_seek(&host, 8.2);
    const char *previous[] = {"previous"};
    for (int i = 0; i < 3; ++i)
        trim_backend_action(host.backend, "keyframe", G_N_ELEMENTS(previous), previous);
    await_position(&host, 6, FALSE, "rapid previous burst");
    pump(&host, 50000);
    g_assert_cmpfloat(fabs(position(&host) - 6), <, .03);
    manual_seek(&host, 6.2);
    keyframe(&host, "previous", 6, FALSE);

    paused(&host, FALSE);
    gint64 deadline = g_get_monotonic_time() + 2000000;
    while (position(&host) < 7.15) {
        if (g_get_monotonic_time() > deadline) g_error("Timed playback did not cross the next GOP.");
        poll_host(&host);
    }
    keyframe(&host, "previous", 7, TRUE);
    paused(&host, TRUE);
    trim_backend_set_enabled(host.backend, FALSE);
    trim_backend_set_enabled(host.backend, TRUE);
    await_keyframes(&host);
    keyframe(&host, "previous", 7, FALSE);

    load(&host, filename, "7.2");
    await_keyframes(&host);
    keyframe(&host, "previous", 7, FALSE);
    trim_backend_free(host.backend); host.backend = NULL;
    pump(&host, 30000);
    mpv_terminate_destroy(host.mpv);
    remove_tree(directory);
    puts("PASS native Trim keyframes: paused steps, bursts, manual seeks, GOP crossing, mode/source resets.");
    return 0;
}
