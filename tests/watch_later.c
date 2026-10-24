/* Use the real host initialization and stock mpv resume mechanism. Every
 * configuration, media file and state file belongs to this test's temp tree. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <glib/gstdio.h>

typedef struct {
    Player player;
    gboolean loaded, restarted, position_known, shutdown;
    double position;
} WatchHost;

typedef enum {
    WATCH_LATER_KEY,
    NATIVE_QUIT,
    WINDOW_CLOSE_REQUEST
} QuitMode;

static void poll_watch(WatchHost *host)
{
    mpv_event *event = mpv_wait_event(host->player.mpv, .02);
    g_assert_cmpint(event->error, >=, 0);
    switch (event->event_id) {
    case MPV_EVENT_HOOK: {
        /* The normal host rendering barrier is irrelevant for null output;
         * configuration evaluation and native file loading remain unchanged. */
        mpv_event_hook *hook = event->data;
        g_assert_cmpint(mpv_hook_continue(host->player.mpv, hook->id), >=, 0);
        break;
    }
    case MPV_EVENT_FILE_LOADED:
        host->loaded = TRUE;
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        host->restarted = TRUE;
        break;
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *property = event->data;
        if (g_str_equal(property->name, "time-pos") &&
            property->format == MPV_FORMAT_DOUBLE && property->data) {
            host->position = *(double *)property->data;
            host->position_known = TRUE;
        }
        break;
    }
    case MPV_EVENT_END_FILE: {
        mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->error, >=, 0);
        if (end->reason == MPV_END_FILE_REASON_QUIT) {
            /* The production close preparation also supports a headless
             * host: native saving is done before this final file event. */
            handle_event(&host->player, event);
            g_assert_true(host->player.quit_requested);
            g_assert_true(host->player.close_prepared);
            g_assert_false(host->player.closing);
        }
        break;
    }
    case MPV_EVENT_SHUTDOWN:
        handle_event(&host->player, event);
        g_assert_true(host->player.shutdown_received);
        g_assert_true(host->player.closing);
        host->shutdown = TRUE;
        break;
    case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
        break;
    }
    default:
        break;
    }
}

static WatchHost new_host(const char *config)
{
    WatchHost host = {.player = {.wake_fd = -1, .test_seconds = 1}};
    Player *p = &host.player;
    p->config.path = g_strdup(config);
    g_autofree char *directory = g_path_get_dirname(config);
    p->config.input_path = g_build_filename(directory, "input.conf", NULL);
    g_assert_true(init_mpv(p));
    mpv_set_wakeup_callback(p->mpv, NULL, NULL); /* The returned host moves by value. */
    g_assert_cmpint(mpv_set_option_string(p->mpv, "vo", "null"), >=, 0);
    g_assert_cmpint(mpv_set_option_string(p->mpv, "ao", "null"), >=, 0);
    return host;
}

static void free_host(WatchHost *host)
{
    Player *p = &host->player;
    p->closing = TRUE;
    mpv_terminate_destroy(p->mpv);
    close(p->wake_fd);
    if (p->gl_library)
        dlclose(p->gl_library);
    config_clear(&p->config);
    g_free(p->hwdec);
    g_free(p->hwdec_current);
    g_free(p->audio_output);
    g_free(p->audio_device);
    g_free(p->initial_subtitle);
    clear_track_notices(p);
    for (guint i = 0; i < G_N_ELEMENTS(p->playlist_values); ++i)
        g_free(p->playlist_values[i]);
}

static void assert_option(WatchHost *host, const char *name, const char *expected)
{
    char *value = mpv_get_property_string(host->player.mpv, name);
    g_assert_cmpstr(value, ==, expected);
    mpv_free(value);
}

static void wait_position(WatchHost *host, double expected)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host->loaded || !host->restarted || !host->position_known ||
           fabs(host->position - expected) > .05) {
        if (g_get_monotonic_time() >= deadline)
            g_error("Watch-later position timed out: loaded=%d restarted=%d known=%d "
                    "position=%.6f expected=%.6f", host->loaded, host->restarted,
                    host->position_known, host->position, expected);
        poll_watch(host);
    }
}

static void load_clip(WatchHost *host, const char *filename, double expected)
{
    g_autoptr(GFile) file = g_file_new_for_path(filename);
    g_autofree char *location = media_location(file);
    const char *args[] = {"loadfile", location, "replace", NULL};
    g_assert_cmpint(mpv_command_async(host->player.mpv, 1, args), >=, 0);
    wait_position(host, expected);
}

static void quit_and_save(WatchHost *host, QuitMode mode)
{
    const char *key[] = {"keypress", "Q", NULL};
    const char *quit[] = {"quit", NULL};
    if (mode == WINDOW_CLOSE_REQUEST) {
        /* A WM request hides the UI and queues native quit, but must defer
         * destruction until mpv has saved/unloaded the active media. */
        g_assert_true(close_window(NULL, &host->player));
        g_assert_true(host->player.quit_requested);
        g_assert_false(host->player.closing);
        g_assert_false(host->player.shutdown_received);
        g_assert_true(close_window(NULL, &host->player));
        g_assert_false(host->player.closing);
    } else {
        g_assert_cmpint(mpv_command_async(host->player.mpv, 2,
                                          mode == NATIVE_QUIT ? quit : key), >=, 0);
    }
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host->shutdown) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_watch(host);
    }
}

static void check_case(const char *root, const char *clip, const char *name,
                        gboolean custom_directory, gboolean resume, QuitMode mode)
{
    g_autofree char *directory = g_build_filename(root, name, NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    g_autofree char *config = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input = g_build_filename(directory, "input.conf", NULL);
    g_autofree char *expected_directory = g_build_filename(directory,
        custom_directory ? "custom-state" : "watch_later", NULL);
    g_autofree char *custom = custom_directory
        ? g_strdup_printf("watch-later-directory=%s\n", expected_directory) : g_strdup("");
    g_autofree char *contents = g_strdup_printf(
        "hwdec=no\nao=null\npause=yes\nwatch-later-options=start\n"
        "resume-playback=%s\nsave-position-on-quit=%s\n%s",
        resume ? "yes" : "no", mode != WATCH_LATER_KEY ? "yes" : "no", custom);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(config, contents, -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(input, "Q quit-watch-later\n", -1, &error));
    g_assert_no_error(error);

    WatchHost first = new_host(config);
    assert_option(&first, "config", "no");
    assert_option(&first, "watch-later-dir", expected_directory);
    assert_option(&first, "save-position-on-quit", mode != WATCH_LATER_KEY ? "yes" : "no");
    load_clip(&first, clip, 0);
    first.position_known = FALSE;
    const char *seek[] = {"seek", "4", "absolute+exact", NULL};
    g_assert_cmpint(mpv_command_async(first.player.mpv, 3, seek), >=, 0);
    wait_position(&first, 4);
    quit_and_save(&first, mode);
    free_host(&first);

    /* Stock mpv names resume files with the uppercase MD5 of the normalized
     * absolute filename. Check the media entry, not parent-directory markers. */
    g_autofree char *digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, clip, -1);
    g_autofree char *upper = g_ascii_strup(digest, -1);
    g_autofree char *saved_path = g_build_filename(expected_directory, upper, NULL);
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(saved_path, &saved, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(saved, "start=4.000000\n"));
    if (custom_directory) {
        g_autofree char *unused_default = g_build_filename(directory, "watch_later", NULL);
        g_assert_false(g_file_test(unused_default, G_FILE_TEST_EXISTS));
    }

    WatchHost second = new_host(config);
    assert_option(&second, "watch-later-dir", expected_directory);
    assert_option(&second, "resume-playback", resume ? "yes" : "no");
    load_clip(&second, clip, resume ? 4 : 0);
    free_host(&second);
}

static void remove_tree(const char *path)
{
    g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
    g_assert_nonnull(directory);
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR))
            remove_tree(child);
        else
            g_assert_cmpint(g_remove(child), ==, 0);
    }
    g_clear_pointer(&directory, g_dir_close);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    g_autoptr(GError) error = NULL;
    g_autofree char *root = g_dir_make_tmp("mpv-gtk-watch-later-XXXXXX", &error);
    g_assert_no_error(error);
    /* Guard all native fallback paths as well as the explicit app defaults.
     * The test never reads or writes the user's installed watch-later data. */
    g_autofree char *state = g_build_filename(root, "state", NULL);
    g_autofree char *cache = g_build_filename(root, "cache", NULL);
    g_autofree char *configuration = g_build_filename(root, "configuration", NULL);
    g_assert_true(g_setenv("XDG_STATE_HOME", state, TRUE));
    g_assert_true(g_setenv("XDG_CACHE_HOME", cache, TRUE));
    g_assert_true(g_setenv("XDG_CONFIG_HOME", configuration, TRUE));
    g_autofree char *clip = g_build_filename(root, "clip with spaces · Unicode Ω.wav", NULL);
    char *args[] = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=12",
        "-c:a", "pcm_s16le", clip, NULL};
    g_autofree char *messages = NULL;
    int status = -1;
    g_assert_true(g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, &messages, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("ffmpeg: %s; %s", error->message, messages ? messages : "");

    check_case(root, clip, "default", FALSE, TRUE, WATCH_LATER_KEY);
    check_case(root, clip, "custom", TRUE, TRUE, WATCH_LATER_KEY);
    check_case(root, clip, "resume-disabled", FALSE, FALSE, WATCH_LATER_KEY);
    check_case(root, clip, "normal-quit", FALSE, TRUE, NATIVE_QUIT);
    check_case(root, clip, "window-close", FALSE, TRUE, WINDOW_CLOSE_REQUEST);
    remove_tree(root);
    puts("PASS watch later: native Q saves position, portable default/custom paths, "
         "second-host resume, resume opt-out, native quit and deferred WM-close save-position-on-quit");
    return 0;
}
