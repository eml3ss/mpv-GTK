/* Exercise chapter navigation against stock libmpv without opening a display
 * or audio device. Only the chapter-related host event paths are dispatched. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <glib/gstdio.h>

#define CHAPTER_TEST_BARRIER UINT64_C(60000)

typedef struct {
    Player player;
    guint started, loaded, ended, snapshots, observations, commands, setters, messages;
    int expected_chapters;
    const char *expected_message;
    gboolean restarted, position_known, barrier;
    double position;
} ChapterMedia;

static void run_ffmpeg(char *args[])
{
    g_autoptr(GError) error = NULL;
    g_autofree char *errors = NULL;
    int status = -1;
    g_assert_true(g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, &errors, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("ffmpeg: %s; %s", error->message, errors ? errors : "");
}

static void make_chapter_media(const char *metadata, char *chapter_file, char *plain_file)
{
    static const char text[] =
        ";FFMETADATA1\n"
        "[CHAPTER]\nTIMEBASE=1/1000\nSTART=1000\nEND=8000\ntitle=Opening\n"
        "[CHAPTER]\nTIMEBASE=1/1000\nSTART=8000\nEND=15000\ntitle=Middle · Unicode Ω\n"
        "[CHAPTER]\nTIMEBASE=1/1000\nSTART=15000\nEND=18000\ntitle=Finale\n";
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(metadata, text, -1, &error));
    g_assert_no_error(error);
    char *chapters[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=18",
        "-f", "ffmetadata", "-i", (char *)metadata,
        "-map", "0:a", "-map_metadata", "1", "-map_chapters", "1",
        "-c:a", "flac", chapter_file, NULL
    };
    run_ffmpeg(chapters);
    char *plain[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", chapter_file, "-map", "0:a", "-map_metadata", "-1",
        "-map_chapters", "-1", "-c", "copy", plain_file, NULL
    };
    run_ffmpeg(plain);
}

static const mpv_node *map_value(const mpv_node *map, const char *key)
{
    g_assert_cmpint(map->format, ==, MPV_FORMAT_NODE_MAP);
    g_assert_nonnull(map->u.list);
    for (int i = 0; i < map->u.list->num; ++i) {
        if (g_str_equal(map->u.list->keys[i], key))
            return &map->u.list->values[i];
    }
    g_error("Missing chapter metadata key: %s", key);
    return NULL;
}

static void check_snapshot(const mpv_event_property *property, int expected)
{
    static const char *titles[] = {"Opening", "Middle · Unicode Ω", "Finale"};
    static const double times[] = {1, 8, 15};
    g_assert_nonnull(property);
    g_assert_cmpstr(property->name, ==, "chapter-list");
    g_assert_cmpint(property->format, ==, MPV_FORMAT_NODE);
    const mpv_node *node = property->data;
    g_assert_nonnull(node);
    g_assert_cmpint(node->format, ==, MPV_FORMAT_NODE_ARRAY);
    g_assert_nonnull(node->u.list);
    g_assert_cmpint(node->u.list->num, ==, expected);
    for (int i = 0; i < expected; ++i) {
        const mpv_node *chapter = &node->u.list->values[i];
        const mpv_node *title = map_value(chapter, "title");
        const mpv_node *time = map_value(chapter, "time");
        g_assert_cmpint(title->format, ==, MPV_FORMAT_STRING);
        g_assert_cmpstr(title->u.string, ==, titles[i]);
        g_assert_cmpint(time->format, ==, MPV_FORMAT_DOUBLE);
        g_assert_cmpfloat_with_epsilon(time->u.double_, times[i], 0.000001);
    }
}

static void poll_chapters(ChapterMedia *media)
{
    Player *p = &media->player;
    mpv_event *event = mpv_wait_event(p->mpv, 0.05);
    switch (event->event_id) {
    case MPV_EVENT_START_FILE:
        media->started++;
        media->restarted = media->position_known = FALSE;
        p->timeline.loaded = FALSE;
        clear_chapters(p);
        break;
    case MPV_EVENT_FILE_LOADED:
        media->loaded++;
        p->timeline.loaded = TRUE;
        request_chapters(p);
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        if (p->timeline.loaded)
            media->restarted = TRUE;
        break;
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *property = event->data;
        if (g_str_equal(property->name, "chapter") ||
            g_str_equal(property->name, "chapters")) {
            if (g_str_equal(property->name, "chapter") &&
                property->format == MPV_FORMAT_INT64 && property->data)
                media->observations++;
            property_changed(p, property);
        } else if (g_str_equal(property->name, "time-pos")) {
            media->position_known = property->format == MPV_FORMAT_DOUBLE && property->data;
            if (media->position_known)
                media->position = *(double *)property->data;
        }
        break;
    }
    case MPV_EVENT_GET_PROPERTY_REPLY:
        if (event->reply_userdata == CHAPTER_TEST_BARRIER) {
            g_assert_cmpint(event->error, >=, 0);
            media->barrier = TRUE;
        } else if (event->reply_userdata >= CHAPTERS_REFRESH_BASE) {
            if (event->reply_userdata == p->chapter_request) {
                g_assert_cmpint(event->error, >=, 0);
                check_snapshot(event->data, media->expected_chapters);
                media->snapshots++;
            }
            /* This safe event path also checks the production stale-reply
             * guard rather than calling accept_chapters unconditionally. */
            handle_event(p, event);
        } else {
            g_error("Unexpected asynchronous property reply: %" G_GUINT64_FORMAT,
                    event->reply_userdata);
        }
        break;
    case MPV_EVENT_COMMAND_REPLY:
        g_assert_cmpint(event->error, >=, 0);
        media->commands++;
        break;
    case MPV_EVENT_SET_PROPERTY_REPLY:
        g_assert_cmpint(event->error, >=, 0);
        media->setters++;
        break;
    case MPV_EVENT_CLIENT_MESSAGE: {
        mpv_event_client_message *message = event->data;
        g_assert_cmpint(message->num_args, ==, 2);
        g_assert_cmpstr(message->args[0], ==, "mpv-gtk");
        g_assert_true(g_str_equal(message->args[1], "chapter-previous") ||
                      g_str_equal(message->args[1], "chapter-next"));
        if (media->expected_message)
            g_assert_cmpstr(message->args[1], ==, media->expected_message);
        media->messages++;
        handle_event(p, event);
        break;
    }
    case MPV_EVENT_END_FILE: {
        mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->error, >=, 0);
        g_assert_cmpint(end->reason, ==, MPV_END_FILE_REASON_EOF);
        media->ended++;
        p->timeline.loaded = FALSE;
        clear_chapters(p);
        break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
        break;
    }
    default:
        break;
    }
}

static void load_chapters(ChapterMedia *media, const char *filename, int expected)
{
    guint loaded = media->loaded, snapshots = media->snapshots;
    media->expected_chapters = expected;
    media->restarted = media->position_known = FALSE;
    const char *args[] = {"loadfile", filename, "replace", NULL};
    g_assert_cmpint(mpv_command_async(media->player.mpv, 0, args), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (media->loaded == loaded || (expected > 0 && media->snapshots == snapshots) ||
           !media->restarted || !media->position_known) {
        if (g_get_monotonic_time() >= deadline)
            g_error("Chapter load timed out: loaded=%u/%u snapshots=%u/%u "
                    "restart=%d position-known=%d chapters=%" G_GINT64_FORMAT
                    " current=%" G_GINT64_FORMAT " request=%" G_GUINT64_FORMAT,
                    media->loaded, loaded, media->snapshots, snapshots,
                    media->restarted, media->position_known,
                    media->player.chapter_count, media->player.current_chapter,
                    media->player.chapter_request);
        poll_chapters(media);
    }
    g_assert_cmpint(media->player.chapter_count, ==, expected);
    if (expected == 0) {
        /* The zero-count observation legitimately clears pending requests.
         * Verify the engine's empty schema without requiring that cancelled
         * reply to populate a hidden menu. */
        mpv_node node = {0};
        g_assert_cmpint(mpv_get_property(media->player.mpv, "chapter-list",
                                        MPV_FORMAT_NODE, &node), >=, 0);
        mpv_event_property property = {"chapter-list", MPV_FORMAT_NODE, &node};
        check_snapshot(&property, 0);
        mpv_free_node_contents(&node);
    }
}

static void wait_chapter_position(ChapterMedia *media, int64_t chapter, double position)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!media->position_known || fabs(media->position - position) > 0.15 ||
           media->player.current_chapter != chapter) {
        if (g_get_monotonic_time() >= deadline)
            g_error("Chapter seek timed out: chapter=%" G_GINT64_FORMAT
                    " position=%.3f; expected chapter=%" G_GINT64_FORMAT " position=%.3f",
                    media->player.current_chapter, media->position, chapter, position);
        poll_chapters(media);
    }
}

static void chapter_barrier(ChapterMedia *media)
{
    Player *p = &media->player;
    media->barrier = FALSE;
    g_assert_cmpint(mpv_get_property_async(p->mpv, CHAPTER_TEST_BARRIER,
                                         "chapter-list", MPV_FORMAT_NODE), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!media->barrier) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_chapters(media);
    }
}

static void assert_navigation_ignored(ChapterMedia *media)
{
    Player *p = &media->player;
    chapter_barrier(media); /* Consume replies from the preceding valid seek. */
    guint commands = media->commands, setters = media->setters;
    int64_t chapter = p->current_chapter;
    select_chapter(-1, p);
    select_chapter(p->chapter_count, p);
    step_chapter(0, p);
    step_chapter(2, p);
    if (chapter < 0)
        step_chapter(-1, p);
    if (p->chapter_count == 0 || chapter >= p->chapter_count - 1)
        step_chapter(1, p);
    chapter_barrier(media);
    g_assert_cmpuint(media->commands, ==, commands);
    g_assert_cmpuint(media->setters, ==, setters);
    g_assert_cmpint(p->current_chapter, ==, chapter);
}

static void install_chapter_keys(Player *p)
{
    /* Read the production defaults: copying commands into the test would
     * hide a missing, misspelled, or incorrectly routed default binding. */
    GString *bindings = g_string_new(NULL);
    guint found = 0;
    for (guint i = 0; i < G_N_ELEMENTS(app_key_defaults); ++i) {
        const KeyBindingDefault *binding = &app_key_defaults[i];
        if (g_str_equal(binding->key, "Ctrl+PGUP") ||
            g_str_equal(binding->key, "Ctrl+PGDWN")) {
            g_string_append_printf(bindings, "%s %s\n", binding->key, binding->command);
            found++;
        }
    }
    g_assert_cmpuint(found, ==, 2);
    const char *define[] = {"define-section", KEYBINDINGS_APP_SECTION,
                            bindings->str, "force", NULL};
    g_assert_cmpint(mpv_command(p->mpv, define), >=, 0);
    g_string_free(bindings, TRUE);
    const char *enable[] = {"enable-section", KEYBINDINGS_APP_SECTION, NULL};
    g_assert_cmpint(mpv_command(p->mpv, enable), >=, 0);
}

static void press_chapter_key(ChapterMedia *media, const char *key,
                              const char *message, int64_t chapter, double position)
{
    guint messages = media->messages;
    media->expected_message = message;
    const char *args[] = {"keypress", key, NULL};
    g_assert_cmpint(mpv_command_async(media->player.mpv, 0, args), >=, 0);
    wait_chapter_position(media, chapter, position);
    g_assert_cmpuint(media->messages, ==, messages + 1);
    media->expected_message = NULL;
}

static void assert_next_chapter_burst(ChapterMedia *media, const char *next_file)
{
    Player *p = &media->player;
    g_assert_cmpint(p->current_chapter, ==, 0);
    const char *append[] = {"loadfile", next_file, "append", NULL};
    g_assert_cmpint(mpv_command(p->mpv, append), >=, 0);
    int64_t count = 0;
    g_assert_cmpint(mpv_get_property(p->mpv, "playlist-count", MPV_FORMAT_INT64, &count), >=, 0);
    g_assert_cmpint(count, ==, 2);
    guint started = media->started, loaded = media->loaded, ended = media->ended;

    /* No event polling between clicks: the host still observes chapter zero
     * throughout this burst. Relative add commands could overrun the last
     * chapter and start the queued file before observations catch up. */
    for (int i = 0; i < 5; ++i)
        step_chapter(1, p);
    g_assert_cmpint(p->current_chapter, ==, 0);
    wait_chapter_position(media, 2, 15);
    chapter_barrier(media);
    g_assert_true(p->timeline.loaded);
    g_assert_cmpuint(media->started, ==, started);
    g_assert_cmpuint(media->loaded, ==, loaded);
    g_assert_cmpuint(media->ended, ==, ended);
    int64_t position = -1;
    g_assert_cmpint(mpv_get_property(p->mpv, "playlist-pos", MPV_FORMAT_INT64, &position), >=, 0);
    g_assert_cmpint(position, ==, 0);

    /* Leave the following EOF/no-chapter scenarios with their original
     * single-file playlist, without replacing or reopening the active file. */
    const char *clear[] = {"playlist-clear", NULL};
    g_assert_cmpint(mpv_command(p->mpv, clear), >=, 0);
    g_assert_cmpint(mpv_get_property(p->mpv, "playlist-count", MPV_FORMAT_INT64, &count), >=, 0);
    g_assert_cmpint(count, ==, 1);
    select_chapter(0, p);
    wait_chapter_position(media, 0, 1);
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-chapters-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *metadata = g_build_filename(directory, "chapters.ffmeta", NULL);
    g_autofree char *chapter_file = g_build_filename(directory, "chapters.mka", NULL);
    g_autofree char *plain_file = g_build_filename(directory, "plain.mka", NULL);
    make_chapter_media(metadata, chapter_file, plain_file);

    ChapterMedia media = {0};
    Player *p = &media.player;
    clear_chapters(p);
    p->mpv = mpv_create();
    g_assert_nonnull(p->mpv);
    const char *options[][2] = {
        {"config", "no"}, {"vo", "null"}, {"ao", "null"}, {"hwdec", "no"},
        {"pause", "yes"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"keep-open", "no"}, {"idle", "yes"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(p->mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(p->mpv), >=, 0);
    g_assert_cmpint(mpv_request_log_messages(p->mpv, "error"), >=, 0);
    install_chapter_keys(p);
    g_assert_cmpint(mpv_observe_property(p->mpv, 1, "chapter", MPV_FORMAT_INT64), >=, 0);
    g_assert_cmpint(mpv_observe_property(p->mpv, 2, "chapters", MPV_FORMAT_INT64), >=, 0);
    g_assert_cmpint(mpv_observe_property(p->mpv, 3, "time-pos", MPV_FORMAT_DOUBLE), >=, 0);

    load_chapters(&media, chapter_file, 3);
    wait_chapter_position(&media, -1, 0);
    assert_navigation_ignored(&media);
    guint observed = media.observations;
    step_chapter(1, p);
    g_assert_cmpint(p->current_chapter, ==, -1);
    wait_chapter_position(&media, 0, 1);
    g_assert_cmpuint(media.observations, >, observed);
    assert_navigation_ignored(&media);
    assert_next_chapter_burst(&media, plain_file);

    /* The normal application script message reaches the same navigation
     * callback without requiring a GTK window or synthesizing a key event. */
    const char *message[] = {"script-message", "mpv-gtk", "chapter-next", NULL};
    g_assert_cmpint(mpv_command_async(p->mpv, 0, message), >=, 0);
    wait_chapter_position(&media, 1, 8);
    g_assert_cmpuint(media.messages, ==, 1);
    press_chapter_key(&media, "Ctrl+PGUP", "chapter-previous", 0, 1);
    press_chapter_key(&media, "Ctrl+PGDWN", "chapter-next", 1, 8);

    select_chapter(2, p);
    wait_chapter_position(&media, 2, 15);
    assert_navigation_ignored(&media); /* Native add beyond last can end the file. */
    step_chapter(-1, p);
    wait_chapter_position(&media, 1, 8);

    /* Preserve mpv's ordinary backward-chapter threshold semantics: after
     * five seconds, first restart this chapter, then go to its predecessor. */
    media.position_known = FALSE;
    const char *seek[] = {"seek", "13.5", "absolute+exact", NULL};
    g_assert_cmpint(mpv_command_async(p->mpv, 0, seek), >=, 0);
    wait_chapter_position(&media, 1, 13.5);
    media.position_known = FALSE;
    step_chapter(-1, p);
    wait_chapter_position(&media, 1, 8);
    step_chapter(-1, p);
    wait_chapter_position(&media, 0, 1);

    /* With a first chapter beginning after zero, native previous navigation
     * can seek into the prefix before that chapter. */
    step_chapter(-1, p);
    wait_chapter_position(&media, -1, 0);
    step_chapter(1, p);
    wait_chapter_position(&media, 0, 1);

    select_chapter(2, p);
    wait_chapter_position(&media, 2, 15);

    /* Invalidate real replies already queued for the old metadata state.
     * A fresh snapshot must restore both count and current chapter even
     * though the engine's observed values have not changed. */
    request_chapters(p);
    g_assert_cmpuint(p->chapter_request, >, CHAPTERS_REFRESH_BASE);
    p->timeline.loaded = FALSE;
    clear_chapters(p);
    chapter_barrier(&media);
    g_assert_cmpint(p->chapter_count, ==, 0);
    g_assert_cmpint(p->current_chapter, ==, -1);
    p->timeline.loaded = TRUE;
    guint snapshots = media.snapshots;
    request_chapters(p);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (media.snapshots == snapshots || p->current_chapter != 2) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_chapters(&media);
    }
    g_assert_cmpint(p->chapter_count, ==, 3);
    const char *near_end[] = {"seek", "17.8", "absolute+exact", NULL};
    g_assert_cmpint(mpv_command_async(p->mpv, 0, near_end), >=, 0);
    const char *resume[] = {"set", "pause", "no", NULL};
    g_assert_cmpint(mpv_command_async(p->mpv, 0, resume), >=, 0);
    deadline = g_get_monotonic_time() + 5000000;
    while (!media.ended) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_chapters(&media);
    }
    g_assert_false(p->timeline.loaded);
    g_assert_cmpint(p->chapter_count, ==, 0);
    g_assert_cmpint(p->current_chapter, ==, -1);
    g_assert_cmpuint(p->chapter_request, ==, 0);
    assert_navigation_ignored(&media);

    int pause = 1;
    g_assert_cmpint(mpv_set_property(p->mpv, "pause", MPV_FORMAT_FLAG, &pause), >=, 0);
    load_chapters(&media, plain_file, 0);
    g_assert_cmpint(p->current_chapter, ==, -1);
    assert_navigation_ignored(&media);
    g_assert_cmpuint(media.started, ==, 2);
    g_assert_cmpuint(media.loaded, ==, 2);
    g_assert_cmpuint(media.ended, ==, 1);

    mpv_terminate_destroy(p->mpv);
    g_assert_cmpint(g_remove(metadata), ==, 0);
    g_assert_cmpint(g_remove(chapter_file), ==, 0);
    g_assert_cmpint(g_remove(plain_file), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS chapters: real metadata, observed selection, Ctrl+PageUp/PageDown, native previous/next, boundaries, EOF and no-chapter reset");
    return 0;
}
