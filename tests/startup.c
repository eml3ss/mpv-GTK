/* Real libmpv startup may finish after the last window has closed. Exercise
 * that path with a plain GApplication so it also runs without a GTK display. */
#include <gtk/gtk.h>

#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <glib/gstdio.h>

typedef struct {
    Player player;
    guint main_dispatches;
} StartupTest;

static void check_window_readiness(void)
{
    Player p = {0};
    g_assert_false(startup_should_present(&p));
    p.engine_started = TRUE;
    p.engine_loading = TRUE;
    g_assert_false(startup_should_present(&p));
    p.engine_loading = FALSE;
    /* Empty launches only need the initialized engine. File launches also
     * wait for FILE_LOADED; an unmapped frame clock is never a prerequisite. */
    g_assert_true(startup_should_present(&p));
    p.startup_waiting_for_media = TRUE;
    g_assert_false(startup_should_present(&p));
    p.timeline.loaded = TRUE;
    g_assert_true(startup_should_present(&p));
    /* Presentation can start before the first GTK frame. Waiting for that
     * unmapped frame clock would deadlock an otherwise ready embedded file. */
    p.ready = TRUE;
    g_assert_true(startup_should_present(&p));
    p.timeline.loaded = FALSE;
    g_assert_false(startup_should_present(&p));
    p.error_count = 1;
    g_assert_true(startup_should_present(&p));
    p.error_count = 0;
    p.render_failed = TRUE;
    g_assert_true(startup_should_present(&p));
    p.engine_loading = TRUE;
    g_assert_false(startup_should_present(&p));
    p.engine_loading = FALSE;
    p.render_failed = FALSE;
    p.startup_waiting_for_media = FALSE;
    g_assert_true(startup_should_present(&p));
    p.startup_presented = TRUE;
    g_assert_false(startup_should_present(&p));
    p.startup_presented = FALSE;
    p.closing = TRUE;
    g_assert_false(startup_should_present(&p));
}

static void write_startup_clip(const char *path)
{
    char *arguments[] = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "color=c=black:size=32x32:rate=1:duration=2",
        "-f", "lavfi", "-i", "anullsrc=r=8000:cl=mono", "-t", "2",
        "-c:v", "mpeg4", "-c:a", "aac", "-threads", "1", (char *)path, NULL};
    g_autoptr(GError) error = NULL;
    g_autofree char *messages = NULL;
    int status = 0;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, &messages, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("startup fixture: %s; %s", error->message, messages ? messages : "");
}

static int64_t first_playlist_id(mpv_handle *mpv)
{
    int64_t id = -1;
    g_assert_cmpint(mpv_get_property(mpv, "playlist/0/id", MPV_FORMAT_INT64, &id), >=, 0);
    g_assert_cmpint(id, >=, 0);
    return id;
}

static void check_early_media(const char *configuration, const char *input,
                              const char *first, const char *second)
{
    Player p = {.wake_fd = -1, .test_seconds = 1};
    p.config.path = g_strdup(configuration);
    p.config.input_path = g_strdup(input);
    p.pending = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(p.pending, g_strdup(first));
    if (second)
        g_ptr_array_add(p.pending, g_strdup(second));
    g_assert_true(init_mpv_core(&p));
    g_assert_false(p.ready);
    g_assert_null(p.area);
    g_assert_null(p.renderer);
    queue_startup_media(&p);
    g_assert_true(p.startup_waiting_for_media);
    g_assert_true(p.media_loading);
    g_assert_cmpuint(p.pending->len, ==, 0);
    p.engine_started = TRUE;
    g_assert_false(startup_should_present(&p));

    guint starts = 0;
    gint64 until = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!p.render_load_hook) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        mpv_event *event = mpv_wait_event(p.mpv, .02);
        g_assert_cmpint(event->error, >=, 0);
        g_assert_cmpint(event->event_id, !=, MPV_EVENT_FILE_LOADED);
        g_assert_cmpint(event->event_id, !=, MPV_EVENT_END_FILE);
        if (event->event_id == MPV_EVENT_START_FILE)
            starts++;
        if (event->event_id == MPV_EVENT_HOOK) {
            mpv_event_hook *hook = event->data;
            g_assert_cmpuint(event->reply_userdata, ==, RENDER_LOAD_HOOK_ID);
            g_assert_cmpstr(hook->name, ==, "on_preloaded");
            p.render_load_hook = hook->id;
        }
    }
    g_assert_cmpuint(starts, ==, 1);
    mpv_node tracks = {0};
    g_assert_cmpint(mpv_get_property(p.mpv, "track-list", MPV_FORMAT_NODE, &tracks), >=, 0);
    g_assert_cmpint(tracks.format, ==, MPV_FORMAT_NODE_ARRAY);
    g_assert_cmpint(tracks.u.list->num, ==, 2);
    mpv_free_node_contents(&tracks);
    char *vo = NULL;
    g_assert_cmpint(mpv_get_property(p.mpv, "current-vo", MPV_FORMAT_STRING, &vo), ==,
                    MPV_ERROR_PROPERTY_UNAVAILABLE);
    g_assert_null(vo);
    int64_t count = -1;
    g_assert_cmpint(mpv_get_property(p.mpv, "playlist-count", MPV_FORMAT_INT64, &count), >=, 0);
    g_assert_cmpint(count, ==, second ? 2 : 1);
    int64_t first_id = first_playlist_id(p.mpv);

    /* Attaching the UI later must not redispatch paths already consumed by
     * the pre-GTK queue, even when a startup batch contains multiple files. */
    queue_startup_media(&p);
    p.ready = TRUE;
    flush_pending_paths(&p);
    p.ready = FALSE;
    g_assert_cmpint(first_playlist_id(p.mpv), ==, first_id);
    g_assert_cmpint(mpv_get_property(p.mpv, "playlist-count", MPV_FORMAT_INT64, &count), >=, 0);
    g_assert_cmpint(count, ==, second ? 2 : 1);
    uint64_t held = p.render_load_hook;
    continue_render_load(&p);
    g_assert_cmpuint(p.render_load_hook, ==, held);
    g_assert_false(startup_should_present(&p));
    g_assert_false(p.startup_presented);

    p.closing = TRUE;
    continue_render_load(&p);
    g_assert_cmpuint(p.render_load_hook, ==, 0);
    mpv_terminate_destroy(p.mpv);
    if (p.gl_library)
        dlclose(p.gl_library);
    close(p.wake_fd);
    config_clear(&p.config);
    g_ptr_array_unref(p.pending);
}

static gboolean count_main_dispatch(gpointer data)
{
    StartupTest *test = data;
    test->main_dispatches++;
    return G_SOURCE_REMOVE;
}

static gboolean startup_deadline(gpointer data)
{
    (void)data;
    g_error("asynchronous engine startup did not release the application hold");
    return G_SOURCE_REMOVE;
}

static void begin_and_close(GApplication *application, gpointer data)
{
    StartupTest *test = data;
    Player *p = &test->player;
    p->application = (GtkApplication *)application;
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(paths, g_strdup("/tmp/queued-media-before-engine.mkv"));
    open_paths(p, paths);
    g_assert_cmpuint(p->pending->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(p->pending, 0), ==,
                    "/tmp/queued-media-before-engine.mkv");
    /* Re-entering with the already pending array must not append to itself. */
    open_paths(p, p->pending);
    g_assert_cmpuint(p->pending->len, ==, 1);
    set_flag(p, "pause", TRUE);
    set_number(p, "volume", 35);
    char *subtitle = g_strdup("auto");
    set_string(p, "sid", subtitle);
    g_free(subtitle);
    g_assert_cmpuint(p->pending_commands.length, ==, 3);
    char **number = g_queue_peek_nth(&p->pending_commands, 1);
    g_assert_cmpstr(number[0], ==, "set");
    g_assert_cmpstr(number[1], ==, "volume");
    g_assert_cmpstr(number[2], ==, "35");
    char **string = g_queue_peek_nth(&p->pending_commands, 2);
    g_assert_cmpstr(string[2], ==, "auto");
    show_preferences(p);
    g_assert_true(p->settings_requested);
    g_assert_null(p->preferences);
    start_mpv_async(p);
    g_assert_true(p->engine_started);
    g_assert_true(p->engine_loading);
    /* The worker owns its handle and descriptors until completion, including
     * when it is already running concurrently with this callback. */
    g_assert_null(p->mpv);
    g_assert_null(p->gl_library);
    g_assert_cmpint(p->wake_fd, ==, -1);
    p->closing = TRUE;
    set_number(p, "volume", 75);
    g_assert_cmpuint(p->pending_commands.length, ==, 3);
    g_idle_add_full(G_PRIORITY_HIGH, count_main_dispatch, test, NULL);
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    check_window_readiness();
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-startup-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input_path = g_build_filename(directory, "input.conf", NULL);
    const char *configuration = "hwdec=no\nao=null\n";
    const char *bindings = "SPACE cycle pause\n";
    g_assert_true(g_file_set_contents(path, configuration, -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(input_path, bindings, -1, &error));
    g_assert_no_error(error);
    g_autofree char *first = g_build_filename(directory, "early startup.mp4", NULL);
    g_autofree char *second = g_build_filename(directory, "second.mp4", NULL);
    write_startup_clip(first);
    g_autofree char *clip_data = NULL;
    gsize clip_length = 0;
    g_assert_true(g_file_get_contents(first, &clip_data, &clip_length, &error));
    g_assert_true(g_file_set_contents(second, clip_data, (gssize)clip_length, &error));
    g_assert_no_error(error);
    check_early_media(path, input_path, first, NULL);
    check_early_media(path, input_path, first, second);
    StartupTest test = {.player = {.wake_fd = -1}};
    test.player.pending = g_ptr_array_new_with_free_func(g_free);
    test.player.config.path = g_strdup(path);
    test.player.config.input_path = g_strdup(input_path);
    g_autoptr(GApplication) application = g_application_new(
        APP_ID ".StartupTest", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "activate", G_CALLBACK(begin_and_close), &test);
    guint deadline = g_timeout_add_seconds(15, startup_deadline, NULL);
    char *arguments[] = {"mpv-gtk-startup-test", NULL};
    int status = g_application_run(application, 1, arguments);
    g_source_remove(deadline);
    g_assert_cmpint(status, ==, 0);
    g_assert_cmpuint(test.main_dispatches, ==, 1);
    g_assert_true(test.player.engine_started);
    g_assert_false(test.player.engine_loading);
    g_assert_false(test.player.render_failed);
    g_assert_null(test.player.mpv);
    g_assert_null(test.player.gl_library);
    g_assert_cmpint(test.player.wake_fd, ==, -1);
    g_assert_cmpuint(test.player.wake_source, ==, 0);
    g_assert_cmpuint(test.player.error_count, ==, 0);
    g_assert_cmpuint(test.player.pending->len, ==, 1);
    g_assert_cmpuint(test.player.pending_commands.length, ==, 3);
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(saved, ==, configuration);
    g_clear_pointer(&saved, g_free);
    g_assert_true(g_file_get_contents(input_path, &saved, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(saved, ==, bindings);
    config_clear(&test.player.config);
    g_ptr_array_unref(test.player.pending);
    g_queue_clear_full(&test.player.pending_commands, (GDestroyNotify)g_strfreev);
    g_assert_cmpint(g_remove(second), ==, 0);
    g_assert_cmpint(g_remove(first), ==, 0);
    g_assert_cmpint(g_remove(input_path), ==, 0);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS asynchronous startup: early demux before GTK/GL, held preloaded barrier, single/batch paths consumed once, unchanged media-ready window gate, queued settings, private engine ownership, close during initialization and unchanged configs");
    return 0;
}
