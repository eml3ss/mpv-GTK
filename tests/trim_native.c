/* Force the first post-commit reload to fail and verify native rollback using
 * only libmpv's C API, null outputs, and generated temporary media. */
#define _GNU_SOURCE
#include "../src/trim_export.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define LOAD_HOOK_ID UINT64_C(7001)

typedef struct {
    mpv_handle *mpv;
    TrimExport *exporter;
    const char *directory, *source, *missing;
    char *original_hash, *backup;
    gboolean loaded, restarted, injected, restored_hook, failed_reload;
    guint load_hooks;
} RollbackHost;

static const mpv_node *node_field(const mpv_node *node, const char *name)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list) return NULL;
    for (int i = 0; i < node->u.list->num; ++i)
        if (g_strcmp0(node->u.list->keys[i], name) == 0) return &node->u.list->values[i];
    return NULL;
}

static char *phase(mpv_handle *mpv)
{
    mpv_node status = {0};
    g_assert_cmpint(mpv_get_property(mpv, "user-data/trim-export", MPV_FORMAT_NODE, &status), >=, 0);
    const mpv_node *value = node_field(&status, "phase");
    char *text = g_strdup(value && value->format == MPV_FORMAT_STRING ? value->u.string : "");
    mpv_free_node_contents(&status);
    return text;
}

static char *file_hash(const char *path)
{
    g_autofree char *bytes = NULL;
    gsize size = 0;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_get_contents(path, &bytes, &size, &error));
    g_assert_no_error(error);
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, size);
}

static void assert_original_bytes(RollbackHost *host, const char *path)
{
    g_autofree char *hash = file_hash(path);
    g_assert_cmpstr(hash, ==, host->original_hash);
}

static char *find_backup(const char *directory)
{
    g_autoptr(GDir) entries = g_dir_open(directory, 0, NULL);
    g_assert_nonnull(entries);
    const char *name;
    while ((name = g_dir_read_name(entries))) {
        if (!g_str_has_prefix(name, ".mpv-trim-")) continue;
        char *backup = g_build_filename(directory, name, "original.mp4", NULL);
        if (g_file_test(backup, G_FILE_TEST_IS_REGULAR)) return backup;
        g_free(backup);
    }
    return NULL;
}

static void poll_host(RollbackHost *host)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    mpv_event *event = mpv_wait_event(host->mpv, .005);
    if (event->event_id == MPV_EVENT_HOOK) {
        g_assert_cmpuint(event->reply_userdata, ==, LOAD_HOOK_ID);
        mpv_event_hook *hook = event->data;
        g_autofree char *current_phase = phase(host->mpv);
        ++host->load_hooks;
        if (g_str_equal(current_phase, "opening") && !host->injected) {
            host->backup = find_backup(host->directory);
            g_assert_nonnull(host->backup);
            assert_original_bytes(host, host->backup);
            g_autofree char *replacement_hash = file_hash(host->source);
            g_assert_cmpstr(replacement_hash, !=, host->original_hash);
            /* on_load explicitly permits changing the stream URL while the
             * playlist entry and its path stay attached to the original. */
            g_assert_cmpint(mpv_set_property_string(host->mpv, "stream-open-filename", host->missing), >=, 0);
            host->injected = TRUE;
        } else if (g_str_equal(current_phase, "restoring")) {
            g_assert_true(host->injected);
            assert_original_bytes(host, host->source);
            host->restored_hook = TRUE;
        }
        g_assert_cmpint(mpv_hook_continue(host->mpv, hook->id), >=, 0);
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        const mpv_event_end_file *end = event->data;
        if (end->reason == MPV_END_FILE_REASON_ERROR) {
            g_assert_true(host->injected);
            g_assert_false(host->restored_hook);
            g_assert_nonnull(host->backup);
            /* This is the destructive-path invariant: the verified original
             * remains recoverable when opening its replacement fails. */
            assert_original_bytes(host, host->backup);
            host->failed_reload = TRUE;
        }
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) host->loaded = TRUE;
    else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) host->restarted = TRUE;
    trim_export_handle_event(host->exporter, event);
}

static void set_option(mpv_handle *mpv, const char *name, const char *value, gboolean optional)
{
    int result = mpv_set_option_string(mpv, name, value);
    g_assert_true(result >= 0 || (optional && result == MPV_ERROR_OPTION_NOT_FOUND));
}

static void generate_media(const char *path)
{
    char *arguments[] = {"ffmpeg", "-nostdin", "-v", "error",
        "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25:duration=4",
        "-f", "lavfi", "-i", "sine=sample_rate=48000:duration=4",
        "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-keyint_min", "25",
        "-sc_threshold", "0", "-threads", "1", "-c:a", "aac", "-shortest", (char *)path, NULL};
    int status = -1;
    g_autofree char *diagnostic = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, &diagnostic, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("Cannot generate rollback fixture: %s; %s", error->message, diagnostic ? diagnostic : "");
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
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
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-native-rollback-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *source = g_build_filename(directory, "rollback Unicode Ω.mp4", NULL);
    g_autofree char *missing = g_build_filename(directory, "intentional-open-failure.mp4", NULL);
    generate_media(source);
    RollbackHost host = {.directory = directory, .source = source, .missing = missing};
    host.original_hash = file_hash(source);
    host.mpv = mpv_create();
    g_assert_nonnull(host.mpv);
    set_option(host.mpv, "config", "no", FALSE);
    set_option(host.mpv, "vo", "null", FALSE);
    set_option(host.mpv, "ao", "null", FALSE);
    set_option(host.mpv, "idle", "yes", FALSE);
    set_option(host.mpv, "pause", "yes", FALSE);
    set_option(host.mpv, "terminal", "no", FALSE);
    set_option(host.mpv, "load-scripts", "no", FALSE);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console", "load-auto-profiles",
        "load-select", "load-positioning", "load-commands", "load-context-menu", NULL};
    for (guint i = 0; scripts[i]; ++i) set_option(host.mpv, scripts[i], "no", TRUE);
    g_assert_cmpint(mpv_initialize(host.mpv), >=, 0);
    host.exporter = trim_export_new(host.mpv);
    g_assert_nonnull(host.exporter);
    g_assert_cmpint(mpv_hook_add(host.mpv, LOAD_HOOK_ID, "on_load", 0), >=, 0);
    const char *load[] = {"loadfile", source, NULL};
    g_assert_cmpint(mpv_command_async(host.mpv, 0, load), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host.loaded || !host.restarted) {
        if (g_get_monotonic_time() > deadline) g_error("Null-output fixture did not load.");
        poll_host(&host);
    }
    int scripts_enabled = 1;
    g_assert_cmpint(mpv_get_property(host.mpv, "options/load-scripts", MPV_FORMAT_FLAG, &scripts_enabled), >=, 0);
    g_assert_false(scripts_enabled);
    mpv_node scripts_option = {0};
    g_assert_cmpint(mpv_get_property(host.mpv, "options/scripts", MPV_FORMAT_NODE, &scripts_option), >=, 0);
    g_assert_cmpint(scripts_option.format, ==, MPV_FORMAT_NODE_ARRAY);
    g_assert_cmpint(scripts_option.u.list->num, ==, 0);
    mpv_free_node_contents(&scripts_option);
    const char *seek[] = {"seek", "0.4", "absolute+exact", NULL};
    host.restarted = FALSE;
    g_assert_cmpint(mpv_command_async(host.mpv, 0, seek), >=, 0);
    deadline = g_get_monotonic_time() + 3000000;
    while (!host.restarted) {
        if (g_get_monotonic_time() > deadline) g_error("Fixture seek did not finish.");
        poll_host(&host);
    }
    TrimExportRequest request = {.ffmpeg_path = ffmpeg, .ffprobe_path = ffprobe,
        .replace = TRUE, .open_result = TRUE, .start_ms = 1000, .end_ms = 2000};
    g_assert_true(trim_export_start(host.exporter, &request, &error));
    g_assert_no_error(error);
    deadline = g_get_monotonic_time() + 12000000;
    while (trim_export_busy(host.exporter)) {
        if (g_get_monotonic_time() > deadline) {
            g_autofree char *current_phase = phase(host.mpv);
            g_error("Rollback timed out in phase %s.", current_phase);
        }
        poll_host(&host);
    }
    g_assert_true(host.injected);
    g_assert_true(host.failed_reload);
    g_assert_true(host.restored_hook);
    g_assert_cmpuint(host.load_hooks, ==, 3);
    assert_original_bytes(&host, source);
    GStatBuf original_status;
    g_assert_cmpint(g_stat(source, &original_status), ==, 0);
    g_assert_cmpuint(original_status.st_mode & 0777, ==, 0640);
    char *current_path = mpv_get_property_string(host.mpv, "path");
    g_assert_cmpstr(current_path, ==, source);
    mpv_free(current_path);
    int paused = 0;
    g_assert_cmpint(mpv_get_property(host.mpv, "pause", MPV_FORMAT_FLAG, &paused), >=, 0);
    g_assert_true(paused);
    double position = 0;
    g_assert_cmpint(mpv_get_property(host.mpv, "time-pos", MPV_FORMAT_DOUBLE, &position), >=, 0);
    g_assert_cmpfloat(fabs(position - .4), <, .08);
    mpv_node final = {0};
    g_assert_cmpint(mpv_get_property(host.mpv, "user-data/trim-export", MPV_FORMAT_NODE, &final), >=, 0);
    const mpv_node *success = node_field(&final, "success");
    g_assert_nonnull(success);
    g_assert_cmpint(success->format, ==, MPV_FORMAT_FLAG);
    g_assert_false(success->u.flag);
    mpv_free_node_contents(&final);
    trim_export_free(host.exporter);
    mpv_terminate_destroy(host.mpv);
    g_free(host.original_hash); g_free(host.backup);
    remove_tree(directory);
    puts("PASS native C rollback: failed replacement reload restored original bytes, permissions, position, and pause; scripting disabled.");
    return 0;
}
