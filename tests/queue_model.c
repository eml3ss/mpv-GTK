/* Exercise playlist edits against the real stock core, without a display or
 * the user's configuration. Duplicate paths must remain distinct entries. */
#define _GNU_SOURCE
#include "../src/queue_model.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    mpv_handle *mpv;
    QueueModel *model;
    guint loaded, restarted, changed;
    char *error;
} Host;

typedef struct {
    int64_t id;
    char *filename, *title;
} Entry;

static const mpv_node *field(const mpv_node *map, const char *name)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], name) == 0) return &map->u.list->values[i];
    return NULL;
}

static void entry_free(gpointer data)
{
    Entry *entry = data;
    g_free(entry->filename); g_free(entry->title); g_free(entry);
}

static GPtrArray *snapshot(mpv_handle *mpv)
{
    mpv_node playlist = {0};
    g_assert_cmpint(mpv_get_property(mpv, "playlist", MPV_FORMAT_NODE, &playlist), >=, 0);
    g_assert_cmpint(playlist.format, ==, MPV_FORMAT_NODE_ARRAY);
    GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);
    for (int i = 0; i < playlist.u.list->num; ++i) {
        const mpv_node *item = &playlist.u.list->values[i];
        const mpv_node *id = field(item, "id"), *path = field(item, "filename"), *title = field(item, "title");
        g_assert_nonnull(id); g_assert_cmpint(id->format, ==, MPV_FORMAT_INT64);
        g_assert_nonnull(path); g_assert_cmpint(path->format, ==, MPV_FORMAT_STRING);
        Entry *entry = g_new0(Entry, 1);
        entry->id = id->u.int64; entry->filename = g_strdup(path->u.string);
        entry->title = g_strdup(title && title->format == MPV_FORMAT_STRING ? title->u.string : "");
        g_ptr_array_add(entries, entry);
    }
    mpv_free_node_contents(&playlist);
    return entries;
}

static double number(mpv_handle *mpv, const char *name)
{
    double value = NAN;
    g_assert_cmpint(mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &value), >=, 0);
    return value;
}

static int64_t integer(mpv_handle *mpv, const char *name)
{
    int64_t value = -1;
    g_assert_cmpint(mpv_get_property(mpv, name, MPV_FORMAT_INT64, &value), >=, 0);
    return value;
}

static gboolean paused(mpv_handle *mpv)
{
    int value = 0;
    g_assert_cmpint(mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &value), >=, 0);
    return value;
}

static int64_t playing_id(mpv_handle *mpv)
{
    g_autoptr(GPtrArray) entries = snapshot(mpv);
    int64_t index = integer(mpv, "playlist-playing-pos");
    return index >= 0 && index < (int64_t)entries->len ? ((Entry *)entries->pdata[index])->id : -1;
}

static void changed(gpointer data) { ((Host *)data)->changed++; }
static void failed(const char *message, gpointer data)
{
    Host *host = data;
    g_free(host->error); host->error = g_strdup(message);
}

static void dispatch_event(Host *host, mpv_event *event)
{
    if (host->model) queue_model_handle_event(host->model, event);
    if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && host->model) {
        mpv_event_property *property = event->data;
        if (g_str_equal(property->name, "playlist"))
            queue_model_update(host->model, property->format == MPV_FORMAT_NODE ? property->data : NULL);
        else if (g_str_equal(property->name, "playlist-pos") && property->format == MPV_FORMAT_INT64 && property->data)
            queue_model_set_position(host->model, *(int64_t *)property->data);
        else if (g_str_equal(property->name, "loop-playlist") && property->format == MPV_FORMAT_STRING && property->data)
            queue_model_update_repeat(host->model, *(char **)property->data);
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) host->loaded++;
    else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) host->restarted++;
    else if (event->event_id == MPV_EVENT_END_FILE) {
        mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->reason, !=, MPV_END_FILE_REASON_ERROR);
    }
}

static void poll_host(Host *host)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    dispatch_event(host, mpv_wait_event(host->mpv, .005));
}

static void settle(Host *host)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    do {
        poll_host(host);
        if (g_get_monotonic_time() > deadline) g_error("Playlist operation timed out: %s", host->error ? host->error : "busy");
    } while (queue_model_busy(host->model));
    /* Drain observations queued alongside the final command reply. */
    gint64 drain = g_get_monotonic_time() + 20000;
    while (g_get_monotonic_time() < drain) poll_host(host);
}

static void assert_ok(Host *host)
{
    if (host->error) g_error("Playlist operation failed: %s", host->error);
}

static void command(Host *host, const char **arguments)
{
    g_assert_cmpint(mpv_command_async(host->mpv, 0, arguments), >=, 0);
}

static void load_playlist(Host *host, const char *path, guint expected)
{
    guint loaded = host->loaded;
    const char *args[] = {"loadlist", path, "replace", NULL};
    command(host, args);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (host->loaded == loaded || queue_model_entries(host->model)->len != expected || host->restarted == 0) {
        if (g_get_monotonic_time() > deadline) g_error("Playlist fixture did not load.");
        poll_host(host);
    }
    settle(host); assert_ok(host);
}

static void assert_order(Host *host, const int64_t *ids, guint count)
{
    g_autoptr(GPtrArray) entries = snapshot(host->mpv);
    g_assert_cmpuint(entries->len, ==, count);
    for (guint i = 0; i < count; ++i)
        g_assert_cmpint(((Entry *)entries->pdata[i])->id, ==, ids[i]);
    const GPtrArray *model = queue_model_entries(host->model);
    g_assert_cmpuint(model->len, ==, count);
    for (guint i = 0; i < count; ++i)
        g_assert_cmpint(((QueueEntry *)model->pdata[i])->id, ==, ids[i]);
}

static void assert_preserved(Host *host, const GPtrArray *original, int64_t current, double position, guint loaded)
{
    g_autoptr(GPtrArray) entries = snapshot(host->mpv);
    g_assert_cmpuint(entries->len, ==, original->len);
    for (guint i = 0; i < entries->len; ++i) {
        Entry *entry = entries->pdata[i], *old = NULL;
        for (guint j = 0; j < original->len; ++j) {
            Entry *candidate = original->pdata[j];
            if (candidate->id == entry->id) old = candidate;
        }
        g_assert_nonnull(old);
        g_assert_cmpstr(entry->filename, ==, old->filename);
        g_assert_cmpstr(entry->title, ==, old->title);
    }
    g_assert_cmpint(playing_id(host->mpv), ==, current);
    g_assert_true(paused(host->mpv));
    g_assert_cmpfloat(fabs(number(host->mpv, "time-pos") - position), <, .08);
    g_assert_cmpuint(host->loaded, ==, loaded);
    assert_ok(host);
}

static void check_dates(Host *host, const GPtrArray *original, const int64_t *ids,
                        double position, guint loaded, gboolean birthtime)
{
    queue_model_sort(host->model, QUEUE_SORT_MODIFIED_ASC); settle(host);
    int64_t modified_asc[] = {ids[1], ids[4], ids[0], ids[3], ids[2]};
    assert_order(host, modified_asc, 5); assert_preserved(host, original, ids[0], position, loaded);
    queue_model_sort(host->model, QUEUE_SORT_MODIFIED_DESC); settle(host);
    int64_t modified_desc[] = {ids[2], ids[0], ids[3], ids[4], ids[1]};
    assert_order(host, modified_desc, 5); assert_preserved(host, original, ids[0], position, loaded);
    queue_model_sort(host->model, QUEUE_SORT_CREATED_ASC); settle(host);
    int64_t created_asc[] = {ids[0], ids[3], ids[1], ids[2], ids[4]};
    assert_order(host, birthtime ? created_asc : modified_desc, 5);
    assert_preserved(host, original, ids[0], position, loaded);
    queue_model_sort(host->model, QUEUE_SORT_CREATED_DESC); settle(host);
    int64_t created_desc[] = {ids[4], ids[2], ids[1], ids[0], ids[3]};
    assert_order(host, birthtime ? created_desc : modified_desc, 5);
    assert_preserved(host, original, ids[0], position, loaded);
}

static void check_date_paths(Host *host, const char *directory, const char *path,
                             const GPtrArray *original, const int64_t *ids,
                             double position, guint loaded, gboolean birthtime)
{
    g_autofree char *cwd = g_get_current_dir();
    g_autofree char *pwd = g_strdup(g_getenv("PWD"));
    g_assert_cmpint(g_chdir(directory), ==, 0);
    /* mpv's working-directory property prefers PWD to getcwd(). */
    g_assert_cmpint(setenv("PWD", directory, 1), ==, 0);
    char *working = mpv_get_property_string(host->mpv, "working-directory");
    g_assert_cmpstr(working, ==, directory); mpv_free(working);
    g_autofree char *uri = g_filename_to_uri(path, NULL, NULL);
    const char *inputs[] = {"/tmp/mpv-gtk-deliberately-missing-date.mp4",
        "https://example.invalid/queue-date.mp4", uri, "dirA/a2.mp4"};
    for (guint i = 0; i < G_N_ELEMENTS(inputs); ++i) {
        const char *append[] = {"loadfile", inputs[i], "append", NULL};
        g_assert_cmpint(mpv_command(host->mpv, append), >=, 0);
    }
    g_autoptr(GPtrArray) full = snapshot(host->mpv);
    g_assert_cmpuint(full->len, ==, 9);
    int64_t extra[4];
    for (guint i = 0; i < 4; ++i) extra[i] = ((Entry *)full->pdata[i + 5])->id;
    queue_model_sort(host->model, QUEUE_SORT_MODIFIED_ASC); settle(host);
    int64_t ascending[] = {ids[1], extra[3], ids[4], ids[0], ids[3], extra[2], ids[2], extra[0], extra[1]};
    assert_order(host, ascending, 9); assert_preserved(host, full, ids[0], position, loaded);
    queue_model_sort(host->model, QUEUE_SORT_MODIFIED_DESC); settle(host);
    int64_t descending[] = {ids[2], ids[0], ids[3], extra[2], ids[4], ids[1], extra[3], extra[0], extra[1]};
    assert_order(host, descending, 9); assert_preserved(host, full, ids[0], position, loaded);
    if (birthtime) {
        queue_model_sort(host->model, QUEUE_SORT_CREATED_ASC); settle(host);
        int64_t created_asc[] = {ids[0], ids[3], extra[2], ids[1], extra[3], ids[2], ids[4], extra[0], extra[1]};
        assert_order(host, created_asc, 9); assert_preserved(host, full, ids[0], position, loaded);
        queue_model_sort(host->model, QUEUE_SORT_CREATED_DESC); settle(host);
        int64_t created_desc[] = {ids[4], ids[2], ids[1], extra[3], ids[0], ids[3], extra[2], extra[0], extra[1]};
        assert_order(host, created_desc, 9); assert_preserved(host, full, ids[0], position, loaded);
    }
    queue_model_remove(host->model, extra, G_N_ELEMENTS(extra)); settle(host);
    g_assert_cmpint(g_chdir(cwd), ==, 0);
    if (pwd) g_assert_cmpint(setenv("PWD", pwd, 1), ==, 0);
    else g_assert_cmpint(unsetenv("PWD"), ==, 0);
    assert_preserved(host, original, ids[0], position, loaded);
}

/* Do not dispatch GLib completion sources after the native directory reply:
 * the file worker is running or its result is queued, giving a deterministic
 * point for membership changes and destruction without adding test hooks. */
static void launch_date_worker(Host *host)
{
    queue_model_sort(host->model, QUEUE_SORT_MODIFIED_ASC);
    gint64 deadline = g_get_monotonic_time() + 3000000;
    for (;;) {
        mpv_event *event = mpv_wait_event(host->mpv, .005);
        mpv_event_property *property = event->event_id == MPV_EVENT_GET_PROPERTY_REPLY ? event->data : NULL;
        gboolean directory = property && g_str_equal(property->name, "working-directory");
        dispatch_event(host, event);
        if (directory) { g_assert_true(queue_model_busy(host->model)); return; }
        if (g_get_monotonic_time() > deadline) g_error("Date metadata worker did not launch.");
    }
}

static void check_date_worker_lifetime(Host *host, const char *playlist, const char *base)
{
    const char *load[] = {"loadlist", playlist, "append", NULL};
    g_assert_cmpint(mpv_command(host->mpv, load), >=, 0);
    launch_date_worker(host);
    const char *append[] = {"loadfile", base, "append", NULL};
    g_assert_cmpint(mpv_command(host->mpv, append), >=, 0);
    g_autoptr(GPtrArray) expected = snapshot(host->mpv);
    g_assert_cmpuint(expected->len, ==, 6);
    settle(host);
    g_assert_nonnull(host->error); /* Membership changes abort before any move. */
    g_clear_pointer(&host->error, g_free);
    g_autoptr(GPtrArray) actual = snapshot(host->mpv);
    g_assert_cmpuint(actual->len, ==, expected->len);
    for (guint i = 0; i < actual->len; ++i)
        g_assert_cmpint(((Entry *)actual->pdata[i])->id, ==, ((Entry *)expected->pdata[i])->id);
    launch_date_worker(host);
    queue_model_free(host->model); host->model = NULL;
    guint changes = host->changed;
    gint64 deadline = g_get_monotonic_time() + 200000;
    while (g_get_monotonic_time() < deadline) poll_host(host);
    g_assert_cmpuint(host->changed, ==, changes);
    g_assert_null(host->error);
}

static void remove_tree(const char *path)
{
    GStatBuf status;
    if (g_lstat(path, &status) != 0) return;
    if (!S_ISDIR(status.st_mode)) { g_assert_cmpint(g_unlink(path), ==, 0); return; }
    g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        g_autofree char *child = g_build_filename(path, name, NULL); remove_tree(child);
    }
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

int main(void)
{
    setlocale(LC_ALL, "C.UTF-8"); setlocale(LC_NUMERIC, "C");
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    if (!ffmpeg) { puts("SKIP: FFmpeg is unavailable."); return 77; }
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-queue-model-XXXXXX", NULL);
    g_autofree char *base = g_build_filename(directory, "fixture.mp4", NULL);
    char *generate[] = {ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
        "color=c=black:size=64x36:rate=10:duration=30", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=8000:duration=30", "-c:v", "mpeg4", "-q:v", "8",
        "-c:a", "aac", "-threads", "1", "-shortest", base, NULL};
    int status = -1;
    g_assert_true(g_spawn_sync(NULL, generate, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL));
    g_assert_true(g_spawn_check_wait_status(status, NULL));
    g_autofree char *dir_a = g_build_filename(directory, "dirA", NULL);
    g_autofree char *dir_b = g_build_filename(directory, "dirB", NULL);
    g_assert_cmpint(g_mkdir(dir_a, 0700), ==, 0); g_assert_cmpint(g_mkdir(dir_b, 0700), ==, 0);
    char *paths[] = {g_build_filename(dir_b, "z10 Ω.mp4", NULL),
        g_build_filename(dir_a, "a2.mp4", NULL), g_build_filename(dir_a, "a1.mp4", NULL),
        NULL, g_build_filename(dir_b, "z2.mp4", NULL)};
    paths[3] = g_strdup(paths[0]);
    const char *titles[] = {"track 10", "Track 2", "Alpha 3", "Same title", "track 1"};
    g_autoptr(GString) list = g_string_new("#EXTM3U\n");
    g_autofree char *bytes = NULL; gsize size = 0;
    g_assert_true(g_file_get_contents(base, &bytes, &size, NULL));
    const time_t modified[] = {3000, 1000, 4000, 3000, 2000};
    gboolean birthtime = TRUE;
    struct statx_timestamp previous_birth = {0};
    for (guint i = 0; i < G_N_ELEMENTS(paths); ++i) {
        if (i != 3) {
            g_assert_true(g_file_set_contents(paths[i], bytes, size, NULL));
            struct timespec times[] = {{.tv_sec = modified[i]}, {.tv_sec = modified[i]}};
            g_assert_cmpint(utimensat(AT_FDCWD, paths[i], times, 0), ==, 0);
            struct statx status = {0};
            if (statx(AT_FDCWD, paths[i], 0, STATX_BTIME, &status) != 0 || !(status.stx_mask & STATX_BTIME))
                birthtime = FALSE;
            else if (birthtime) {
                g_assert_true(status.stx_btime.tv_sec > previous_birth.tv_sec ||
                    (status.stx_btime.tv_sec == previous_birth.tv_sec && status.stx_btime.tv_nsec > previous_birth.tv_nsec));
                previous_birth = status.stx_btime;
            }
            g_usleep(5000);
        }
        g_string_append_printf(list, "#EXTINF:-1,%s\n%s\n", titles[i], paths[i]);
    }
    /* Make the oldest file's ctime newest: creation sorting must still use
     * birth time, and must leave unavailable creation dates unknown. */
    g_assert_cmpint(g_chmod(paths[0], 0660), ==, 0);
    g_assert_cmpint(g_chmod(paths[0], 0600), ==, 0);
    g_autofree char *playlist = g_build_filename(directory, "queue.m3u", NULL);
    g_assert_true(g_file_set_contents(playlist, list->str, list->len, NULL));

    Host host = {.mpv = mpv_create()};
    g_assert_nonnull(host.mpv);
    const char *options[][2] = {{"config", "no"}, {"vo", "null"}, {"ao", "null"},
        {"hwdec", "no"}, {"idle", "yes"}, {"pause", "yes"}, {"terminal", "no"},
        {"load-scripts", "no"}, {"osc", "no"}, {"ytdl", "no"}};
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(host.mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(host.mpv), >=, 0);
    host.model = queue_model_new(host.mpv, changed, failed, &host);
    g_assert_cmpint(mpv_observe_property(host.mpv, 1, "playlist", MPV_FORMAT_NODE), >=, 0);
    g_assert_cmpint(mpv_observe_property(host.mpv, 2, "playlist-pos", MPV_FORMAT_INT64), >=, 0);
    g_assert_cmpint(mpv_observe_property(host.mpv, 3, "loop-playlist", MPV_FORMAT_STRING), >=, 0);
    load_playlist(&host, playlist, 5);
    g_autoptr(GPtrArray) original = snapshot(host.mpv);
    int64_t ids[5];
    for (guint i = 0; i < 5; ++i) {
        Entry *entry = original->pdata[i]; ids[i] = entry->id;
        g_assert_cmpstr(entry->title, ==, titles[i]);
    }
    g_assert_cmpint(ids[0], !=, ids[3]);
    const char *seek[] = {"seek", "2.0", "absolute+exact", NULL};
    guint restarted = host.restarted; command(&host, seek);
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (host.restarted == restarted) {
        if (g_get_monotonic_time() > deadline) g_error("Fixture seek did not settle.");
        poll_host(&host);
    }
    double position = number(host.mpv, "time-pos"); guint loaded = host.loaded;
    queue_model_move(host.model, ids[0], -1); settle(&host);
    int64_t moved[] = {ids[1], ids[2], ids[3], ids[4], ids[0]};
    assert_order(&host, moved, 5); assert_preserved(&host, original, ids[0], position, loaded);
    queue_model_move(host.model, ids[0], ids[3]); settle(&host);
    int64_t before_duplicate[] = {ids[1], ids[2], ids[0], ids[3], ids[4]};
    assert_order(&host, before_duplicate, 5); assert_preserved(&host, original, ids[0], position, loaded);

    queue_model_sort(host.model, QUEUE_SORT_TITLE_ASC); settle(&host);
    int64_t ascending[] = {ids[2], ids[3], ids[4], ids[1], ids[0]};
    assert_order(&host, ascending, 5); assert_preserved(&host, original, ids[0], position, loaded);
    queue_model_sort(host.model, QUEUE_SORT_TITLE_DESC); settle(&host);
    int64_t descending[] = {ids[0], ids[1], ids[4], ids[3], ids[2]};
    assert_order(&host, descending, 5); assert_preserved(&host, original, ids[0], position, loaded);
    queue_model_sort(host.model, QUEUE_SORT_PATH_ASC); settle(&host);
    g_autoptr(GPtrArray) path_ascending = snapshot(host.mpv);
    g_assert_cmpint(((Entry *)path_ascending->pdata[0])->id, ==, ids[2]);
    g_assert_cmpint(((Entry *)path_ascending->pdata[1])->id, ==, ids[1]);
    g_assert_cmpint(((Entry *)path_ascending->pdata[2])->id, ==, ids[4]);
    assert_preserved(&host, original, ids[0], position, loaded);
    queue_model_sort(host.model, QUEUE_SORT_PATH_DESC); settle(&host);
    g_autoptr(GPtrArray) path_descending = snapshot(host.mpv);
    g_assert_cmpint(((Entry *)path_descending->pdata[2])->id, ==, ids[4]);
    g_assert_cmpint(((Entry *)path_descending->pdata[3])->id, ==, ids[1]);
    g_assert_cmpint(((Entry *)path_descending->pdata[4])->id, ==, ids[2]);
    assert_preserved(&host, original, ids[0], position, loaded);
    check_dates(&host, original, ids, position, loaded, birthtime);
    check_date_paths(&host, directory, paths[0], original, ids, position, loaded, birthtime);
    queue_model_shuffle(host.model); settle(&host);
    assert_preserved(&host, original, ids[0], position, loaded);

    queue_model_set_repeat(host.model, TRUE); settle(&host);
    g_assert_cmpstr(queue_model_repeat(host.model), ==, "inf");
    char *repeat = mpv_get_property_string(host.mpv, "loop-playlist");
    g_assert_cmpstr(repeat, ==, "inf"); mpv_free(repeat);
    queue_model_set_repeat(host.model, FALSE); settle(&host);
    g_assert_cmpstr(queue_model_repeat(host.model), ==, "no");

    /* Simulate a keybinding/plugin changing native order while the view still
     * holds its previous snapshot. IDs must resolve against the fresh core. */
    const char *external_move[] = {"playlist-move", "0", "5", NULL};
    g_assert_cmpint(mpv_command(host.mpv, external_move), >=, 0);
    int64_t remove[] = {ids[1], ids[3]};
    queue_model_remove(host.model, remove, 2); settle(&host); assert_ok(&host);
    g_autoptr(GPtrArray) remaining = snapshot(host.mpv);
    g_assert_cmpuint(remaining->len, ==, 3);
    for (guint i = 0; i < remaining->len; ++i) {
        Entry *entry = remaining->pdata[i]; g_assert_true(entry->id != ids[1] && entry->id != ids[3]);
    }
    queue_model_remove(host.model, remove, 1); settle(&host);
    g_clear_pointer(&host.error, g_free);
    g_autoptr(GPtrArray) after_stale = snapshot(host.mpv);
    g_assert_cmpuint(after_stale->len, ==, remaining->len);
    for (guint i = 0; i < remaining->len; ++i)
        g_assert_cmpint(((Entry *)after_stale->pdata[i])->id, ==, ((Entry *)remaining->pdata[i])->id);

    int64_t following = ((Entry *)remaining->pdata[0])->id;
    if (following == ids[2]) following = ((Entry *)remaining->pdata[1])->id;
    queue_model_move(host.model, ids[2], following); settle(&host); assert_ok(&host);
    queue_model_play(host.model, ids[2]); settle(&host); assert_ok(&host);
    deadline = g_get_monotonic_time() + 3000000;
    while (playing_id(host.mpv) != ids[2] || paused(host.mpv)) {
        if (g_get_monotonic_time() > deadline) g_error("Play by stable entry ID failed.");
        poll_host(&host);
    }
    loaded = host.loaded;
    queue_model_remove(host.model, &ids[2], 1); settle(&host); assert_ok(&host);
    deadline = g_get_monotonic_time() + 3000000;
    while (host.loaded == loaded || playing_id(host.mpv) != following) {
        if (g_get_monotonic_time() > deadline) g_error("Removing the active entry did not advance to its successor.");
        poll_host(&host);
    }
    g_assert_false(paused(host.mpv));
    queue_model_clear(host.model); settle(&host); assert_ok(&host);
    g_assert_cmpint(integer(host.mpv, "playlist-count"), ==, 0);
    g_assert_cmpuint(queue_model_entries(host.model)->len, ==, 0);
    for (guint i = 0; i < 5; ++i) g_assert_true(g_file_test(paths[i], G_FILE_TEST_IS_REGULAR));

    check_date_worker_lifetime(&host, playlist, base);
    mpv_terminate_destroy(host.mpv); g_free(host.error);
    for (guint i = 0; i < 5; ++i) g_free(paths[i]);
    remove_tree(directory);
    puts("PASS queue model: stable duplicate IDs, reorder, natural/date sorting, real birthtime versus ctime, URI/relative/missing/network dates, shuffle, repeat, multi-remove, stale IDs, clear, playback preservation and date-worker races/teardown.");
    return 0;
}
