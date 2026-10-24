/* Drive the actual scrubbing helpers against paused libmpv playback without
 * GTK widgets, so a frame must change while the drag is still in progress. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <string.h>

typedef struct {
    Player player;
    double position;
    gboolean loaded, paused;
    unsigned seek_replies;
} ScrubMedia;

static char *scrub_clip(void)
{
    char *filename = NULL;
    int fd = g_file_open_tmp("mpv-gtk-scrub-XXXXXX.mp4", &filename, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    char *args[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=1:duration=12",
        "-c:v", "mpeg4", "-q:v", "4", "-pix_fmt", "yuv420p", "-an",
        filename, NULL
    };
    int status = -1;
    g_autofree char *errors = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, &errors, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("ffmpeg: %s; %s", error->message, errors ? errors : "");
    return filename;
}

static void poll_scrub(ScrubMedia *media, double seconds)
{
    mpv_event *event = mpv_wait_event(media->player.mpv, seconds);
    g_assert_cmpint(event->error, >=, 0);
    if (event->event_id == MPV_EVENT_FILE_LOADED) {
        media->loaded = TRUE;
    } else if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
        if (event->reply_userdata == 0)
            media->seek_replies++;
    } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
        mpv_event_property *property = event->data;
        if (!property->data || property->format == MPV_FORMAT_NONE)
            return;
        if (g_str_equal(property->name, "time-pos"))
            media->position = *(double *)property->data;
        else if (g_str_equal(property->name, "pause"))
            media->paused = !!*(int *)property->data;
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->error, >=, 0);
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
        mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
    }
}

static void await_scrub(ScrubMedia *media, double position, unsigned replies)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!media->loaded || !media->paused || !isfinite(media->position) ||
           fabs(media->position - position) > 0.05 || media->seek_replies < replies) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_scrub(media, 0.05);
    }
    g_assert_cmpuint(media->seek_replies, ==, replies);
    g_assert_cmpuint(media->player.error_count, ==, 0);
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    g_autofree char *filename = scrub_clip();
    ScrubMedia media = {.position = NAN};
    Player *p = &media.player;
    p->mpv = mpv_create();
    g_assert_nonnull(p->mpv);
    const char *options[][2] = {
        {"config", "no"}, {"vo", "null"}, {"ao", "null"}, {"hwdec", "no"},
        {"pause", "yes"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"video-timing-offset", "0"}
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(p->mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(p->mpv), >=, 0);
    g_assert_cmpint(mpv_request_log_messages(p->mpv, "error"), >=, 0);
    g_assert_cmpint(mpv_observe_property(p->mpv, 1, "time-pos", MPV_FORMAT_DOUBLE), >=, 0);
    g_assert_cmpint(mpv_observe_property(p->mpv, 2, "pause", MPV_FORMAT_FLAG), >=, 0);
    const char *load[] = {"loadfile", filename, "replace", NULL};
    g_assert_cmpint(mpv_command_async(p->mpv, 1, load), >=, 0);
    await_scrub(&media, 0, 0);
    p->timeline.loaded = TRUE;
    p->timeline.duration = 12;
    p->timeline.paused = TRUE;
    p->dragging = TRUE;

    const gint64 first = 1000000;
    scrub_queue(p, 3, first);
    g_assert_true(p->dragging);
    g_assert_false(p->scrub_pending);
    g_assert_cmpint(p->last_scrub_us, ==, first);
    await_scrub(&media, 3, 1); /* Playback seeks before the pointer is released. */

    scrub_queue(p, 4, first + 1000);
    scrub_queue(p, 5, first + 1000);
    g_assert_true(p->scrub_pending);
    g_assert_cmpfloat(p->preview, ==, 5);
    g_assert_cmpint(p->last_scrub_us, ==, first);
    scrub_flush(p, first + 33332);
    g_assert_true(p->scrub_pending);
    gint64 stable_until = g_get_monotonic_time() + 100000;
    while (g_get_monotonic_time() < stable_until)
        poll_scrub(&media, 0.01);
    g_assert_cmpfloat_with_epsilon(media.position, 3, 0.05);
    g_assert_cmpuint(media.seek_replies, ==, 1);

    scrub_flush(p, first + 33333);
    g_assert_false(p->scrub_pending);
    g_assert_true(p->dragging);
    g_assert_cmpint(p->last_scrub_us, ==, first + 33333);
    await_scrub(&media, 5, 2); /* Coalesced updates apply the latest drag target. */

    p->dragging = FALSE;
    p->scrub_pending = FALSE;
    seek_absolute(p, 6);
    await_scrub(&media, 6, 3);
    scrub_flush(p, first + 1000000);
    g_assert_cmpfloat(p->preview, ==, 6);
    g_assert_true(media.paused);

    mpv_terminate_destroy(p->mpv);
    g_assert_cmpint(unlink(filename), ==, 0);
    puts("PASS scrubbing: seek during drag, 33 ms latest-target coalescing, exact final seek while paused");
    return 0;
}
