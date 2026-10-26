/* Verify MP4 track names across real Trim copy and replacement exports without
 * creating a window or reading the user's configuration. */
#define _GNU_SOURCE
#include "../src/trim_export.h"
#include "../src/mp4_titles.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *titles[] = {
    "All (Game + Mic)", "Microphone 🎙 — Русский", "Discord / Music: \"Mix\""
};

typedef struct {
    mpv_handle *mpv;
    TrimExport *exporter;
    gboolean loaded, restarted;
} Host;

static const mpv_node *field(const mpv_node *node, const char *name)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list) return NULL;
    for (int i = 0; i < node->u.list->num; ++i)
        if (g_strcmp0(node->u.list->keys[i], name) == 0) return &node->u.list->values[i];
    return NULL;
}

static char *run(char **arguments)
{
    g_autofree char *output = NULL, *diagnostic = NULL;
    g_autoptr(GError) error = NULL;
    int status = -1;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, &output, &diagnostic, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("%s failed: %s; %s", arguments[0], error->message, diagnostic ? diagnostic : "");
    return g_steal_pointer(&output);
}

static void option(mpv_handle *mpv, const char *name, const char *value, gboolean optional)
{
    int result = mpv_set_option_string(mpv, name, value);
    g_assert_true(result >= 0 || (optional && result == MPV_ERROR_OPTION_NOT_FOUND));
}

static mpv_handle *new_player(void)
{
    mpv_handle *mpv = mpv_create();
    g_assert_nonnull(mpv);
    option(mpv, "config", "no", FALSE);
    option(mpv, "vo", "null", FALSE);
    option(mpv, "ao", "null", FALSE);
    option(mpv, "hwdec", "no", FALSE);
    option(mpv, "idle", "yes", FALSE);
    option(mpv, "pause", "yes", FALSE);
    option(mpv, "terminal", "no", FALSE);
    option(mpv, "load-scripts", "no", FALSE);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console",
        "load-auto-profiles", "load-select", "load-positioning", "load-commands",
        "load-context-menu", NULL};
    for (guint i = 0; scripts[i]; ++i) option(mpv, scripts[i], "no", TRUE);
    g_assert_cmpint(mpv_initialize(mpv), >=, 0);
    return mpv;
}

static void poll_host(Host *host)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    mpv_event *event = mpv_wait_event(host->mpv, .005);
    if (event->event_id == MPV_EVENT_FILE_LOADED) host->loaded = TRUE;
    else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) host->restarted = TRUE;
    else if (event->event_id == MPV_EVENT_END_FILE) {
        const mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->reason, !=, MPV_END_FILE_REASON_ERROR);
    }
    if (host->exporter) trim_export_handle_event(host->exporter, event);
}

static void load(Host *host, const char *path)
{
    host->loaded = host->restarted = FALSE;
    const char *args[] = {"loadfile", path, NULL};
    g_assert_cmpint(mpv_command_async(host->mpv, 0, args), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host->loaded || !host->restarted) {
        if (g_get_monotonic_time() > deadline) g_error("Media did not load: %s", path);
        poll_host(host);
    }
}

static void assert_player_titles(mpv_handle *mpv, const guint *expected, guint count)
{
    mpv_node tracks = {0};
    g_assert_cmpint(mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &tracks), >=, 0);
    g_assert_cmpint(tracks.format, ==, MPV_FORMAT_NODE_ARRAY);
    guint audio_count = 0, video_count = 0;
    for (int i = 0; i < tracks.u.list->num; ++i) {
        const mpv_node *track = &tracks.u.list->values[i];
        const mpv_node *type = field(track, "type");
        g_assert_nonnull(type);
        g_assert_cmpint(type->format, ==, MPV_FORMAT_STRING);
        if (g_str_equal(type->u.string, "video")) ++video_count;
        if (!g_str_equal(type->u.string, "audio")) continue;
        g_assert_cmpuint(audio_count, <, count);
        const mpv_node *title = field(track, "title");
        g_assert_nonnull(title);
        g_assert_cmpint(title->format, ==, MPV_FORMAT_STRING);
        g_assert_cmpstr(title->u.string, ==, titles[expected[audio_count++]]);
    }
    g_assert_cmpuint(audio_count, ==, count);
    g_assert_cmpuint(video_count, ==, 1);
    mpv_free_node_contents(&tracks);
}

static void assert_titles(const char *path, const guint *expected, guint count)
{
    char *arguments[] = {"ffprobe", "-v", "error", "-select_streams", "a",
        "-show_entries", "stream_tags=title", "-of", "default=nw=1:nk=1", (char *)path, NULL};
    g_autofree char *actual = run(arguments);
    g_autoptr(GString) wanted = g_string_new(NULL);
    for (guint i = 0; i < count; ++i) g_string_append_printf(wanted, "%s\n", titles[expected[i]]);
    g_assert_cmpstr(actual, ==, wanted->str);
    Host reader = {.mpv = new_player()};
    load(&reader, path);
    assert_player_titles(reader.mpv, expected, count);
    mpv_terminate_destroy(reader.mpv);
}

static char *hash(const char *path)
{
    g_autofree char *bytes = NULL;
    gsize size = 0;
    g_assert_true(g_file_get_contents(path, &bytes, &size, NULL));
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, size);
}

static char *export(Host *host, const char *source, const char *ffmpeg, const char *ffprobe,
                    const gint64 *excluded, guint count, gboolean replace)
{
    TrimExportRequest request = {.ffmpeg_path = ffmpeg, .ffprobe_path = ffprobe,
        .expected_input = source, .replace = replace, .open_result = replace,
        .start_ms = 1000, .end_ms = 3000,
        .excluded_audio = excluded, .excluded_audio_count = count};
    g_autoptr(GError) error = NULL;
    g_assert_true(trim_export_start(host->exporter, &request, &error));
    g_assert_no_error(error);
    gint64 deadline = g_get_monotonic_time() + 12000000;
    while (trim_export_busy(host->exporter)) {
        if (g_get_monotonic_time() > deadline) g_error("Trim export timed out.");
        poll_host(host);
    }
    mpv_node result = {0};
    g_assert_cmpint(mpv_get_property(host->mpv, "user-data/trim-export", MPV_FORMAT_NODE, &result), >=, 0);
    const mpv_node *success = field(&result, "success");
    const mpv_node *message = field(&result, "error");
    if (!success || success->format != MPV_FORMAT_FLAG || !success->u.flag)
        g_error("Trim export failed: %s", message && message->format == MPV_FORMAT_STRING ? message->u.string : "unknown error");
    const mpv_node *path = field(&result, "path");
    g_assert_nonnull(path);
    g_assert_cmpint(path->format, ==, MPV_FORMAT_STRING);
    char *output = g_strdup(path->u.string);
    mpv_free_node_contents(&result);
    return output;
}

static void remove_tree(const char *path)
{
    GStatBuf info;
    if (g_lstat(path, &info) != 0) return;
    if (!S_ISDIR(info.st_mode)) { g_assert_cmpint(g_unlink(path), ==, 0); return; }
    g_autoptr(GDir) entries = g_dir_open(path, 0, NULL);
    g_assert_nonnull(entries);
    const char *name;
    while ((name = g_dir_read_name(entries))) {
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
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-trim-titles-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_autofree char *source = g_build_filename(directory, "OBS names Ω.MP4", NULL);
    char *arguments[] = {ffmpeg, "-nostdin", "-v", "error",
        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25:duration=4",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=4",
        "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000:duration=4",
        "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=4",
        "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:a",
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-keyint_min", "25",
        "-sc_threshold", "0", "-threads", "1", "-c:a", "aac",
        "-metadata:s:a:0", "title=All (Game + Mic)",
        "-metadata:s:a:1", "title=Microphone 🎙 — Русский",
        "-metadata:s:a:2", "title=Discord / Music: \"Mix\"", source, NULL};
    g_autofree char *generated = run(arguments);
    char diagnostic[256];
    g_assert_cmpint(mp4_fix_audio_titles(source, diagnostic, sizeof(diagnostic)), ==, 3);
    g_assert_cmpint(g_chmod(source, 0640), ==, 0);
    g_autofree char *original = hash(source);
    guint all[] = {0, 1, 2}, last[] = {2}, first_last[] = {0, 2};
    assert_titles(source, all, G_N_ELEMENTS(all));
    Host host = {.mpv = new_player()};
    host.exporter = trim_export_new(host.mpv);
    load(&host, source);
    g_autofree char *all_copy = export(&host, source, ffmpeg, ffprobe, NULL, 0, FALSE);
    assert_titles(all_copy, all, G_N_ELEMENTS(all));
    gint64 first_middle[] = {1, 2};
    g_autofree char *selected_copy = export(&host, source, ffmpeg, ffprobe,
        first_middle, G_N_ELEMENTS(first_middle), FALSE);
    assert_titles(selected_copy, last, G_N_ELEMENTS(last));
    gint64 every[] = {1, 2, 3};
    g_autofree char *silent_copy = export(&host, source, ffmpeg, ffprobe,
        every, G_N_ELEMENTS(every), FALSE);
    assert_titles(silent_copy, NULL, 0);
    g_autofree char *after_copies = hash(source);
    g_assert_cmpstr(after_copies, ==, original);
    /* The MP4 normalization must leave other export containers alone. */
    g_autofree char *matroska = g_build_filename(directory, "names.mkv", NULL);
    char *remux[] = {ffmpeg, "-nostdin", "-v", "error", "-i", source,
        "-map", "0", "-c", "copy", matroska, NULL};
    g_autofree char *remuxed = run(remux);
    load(&host, matroska);
    g_autofree char *matroska_copy = export(&host, matroska, ffmpeg, ffprobe,
        first_middle, G_N_ELEMENTS(first_middle), FALSE);
    assert_titles(matroska_copy, last, G_N_ELEMENTS(last));
    load(&host, source);
    gint64 middle[] = {2};
    g_autofree char *replacement = export(&host, source, ffmpeg, ffprobe,
        middle, G_N_ELEMENTS(middle), TRUE);
    g_assert_cmpstr(replacement, ==, source);
    assert_titles(source, first_last, G_N_ELEMENTS(first_last));
    assert_player_titles(host.mpv, first_last, G_N_ELEMENTS(first_last));
    GStatBuf info;
    g_assert_cmpint(g_stat(source, &info), ==, 0);
    g_assert_cmpuint(info.st_mode & 0777, ==, 0640);
    trim_export_free(host.exporter);
    mpv_terminate_destroy(host.mpv);
    remove_tree(directory);
    puts("PASS MP4 audio names: ffprobe/mpv titles, Unicode, all/selected/silent copies, stream remapping, replacement reload, Matroska exports, original bytes and permissions.");
    return 0;
}
