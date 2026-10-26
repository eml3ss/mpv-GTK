/* Exercise live silent scrubbing against real audio/video playback without
 * GTK widgets or a desktop player. Audio settings remain user-owned. */
#include <mpv/client.h>

static int observed_command_async(mpv_handle *mpv, uint64_t id, const char *const *args);
#define mpv_command_async observed_command_async
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main
#undef mpv_command_async

#include <glib/gstdio.h>
#include <string.h>

#define TEST_SILENT_GAIN (-150.0)

static mpv_handle *observed_core;
static guint interactive_seeks;

static int observed_command_async(mpv_handle *mpv, uint64_t id, const char *const *args)
{
    if (mpv == observed_core && args && args[0] && g_str_equal(args[0], "seek")) ++interactive_seeks;
    return mpv_command_async(mpv, id, (const char **)args);
}

typedef struct {
    Player player;
    double position, gain;
    gboolean loaded, paused, seeking, release_pending, early_restore;
    double release_target;
    guint restarts;
} ScrubMedia;

static char *scrub_clip(void)
{
    char *filename = NULL;
    int fd = g_file_open_tmp("mpv-gtk-scrub-XXXXXX.mp4", &filename, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    char *args[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=20:duration=12",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=12",
        "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=12",
        "-map", "0:v", "-map", "1:a", "-map", "2:a", "-c:v", "mpeg4",
        "-q:v", "4", "-pix_fmt", "yuv420p", "-c:a", "aac", "-threads", "1",
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

static double native_number(ScrubMedia *media, const char *name)
{
    double number = NAN;
    int status = mpv_get_property(media->player.mpv, name, MPV_FORMAT_DOUBLE, &number);
    if (status < 0) g_error("Native property %s: %s", name, mpv_error_string(status));
    return number;
}

static gboolean native_flag(ScrubMedia *media, const char *name)
{
    int value = FALSE;
    g_assert_cmpint(mpv_get_property(media->player.mpv, name, MPV_FORMAT_FLAG, &value), >=, 0);
    return value;
}

static char *native_text(ScrubMedia *media, const char *name)
{
    char *value = mpv_get_property_string(media->player.mpv, name);
    g_assert_nonnull(value);
    char *text = g_strdup(value);
    mpv_free(value);
    return text;
}

static char *video_frame_hash(ScrubMedia *media)
{
    const char *args[] = {"screenshot-raw", "video", "bgra", NULL};
    mpv_node result = {0};
    g_assert_cmpint(mpv_command_ret(media->player.mpv, args, &result), >=, 0);
    g_assert_cmpint(result.format, ==, MPV_FORMAT_NODE_MAP);
    const mpv_node *data = NULL;
    for (int i = 0; i < result.u.list->num; ++i)
        if (g_str_equal(result.u.list->keys[i], "data")) data = &result.u.list->values[i];
    g_assert_nonnull(data);
    g_assert_cmpint(data->format, ==, MPV_FORMAT_BYTE_ARRAY);
    g_assert_cmpuint(data->u.ba->size, >, 0);
    char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data->u.ba->data, data->u.ba->size);
    mpv_free_node_contents(&result);
    return hash;
}

static void poll_scrub(ScrubMedia *media, double seconds)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    Player *p = &media->player;
    mpv_event *event = mpv_wait_event(p->mpv, seconds);
    audio_scrub_handle_event(p->audio_scrub, event);
    if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE) {
        media->loaded = p->timeline.loaded = FALSE;
        media->release_pending = FALSE;
        p->dragging = p->scrub_pending = FALSE;
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
        media->loaded = p->timeline.loaded = TRUE;
        p->timeline.duration = 12;
    } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
        ++media->restarts;
    } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
        const mpv_event_property *property = event->data;
        if (!property || !property->data || property->format == MPV_FORMAT_NONE) return;
        if (g_str_equal(property->name, "time-pos") && property->format == MPV_FORMAT_DOUBLE) {
            media->position = p->timeline.position = *(double *)property->data;
        } else if (g_str_equal(property->name, "pause") && property->format == MPV_FORMAT_FLAG) {
            media->paused = p->timeline.paused = !!*(int *)property->data;
        } else if (g_str_equal(property->name, "seeking") && property->format == MPV_FORMAT_FLAG) {
            media->seeking = !!*(int *)property->data;
        } else if (g_str_equal(property->name, "mute") && property->format == MPV_FORMAT_FLAG) {
            p->muted = !!*(int *)property->data;
        } else if (g_str_equal(property->name, "volume-gain") && property->format == MPV_FORMAT_DOUBLE) {
            media->gain = *(double *)property->data;
            if (media->release_pending && fabs(media->gain - TEST_SILENT_GAIN) > .000001) {
                double current = native_number(media, "time-pos");
                double tolerance = media->paused ? .15 : .5;
                if (fabs(current - media->release_target) > tolerance) media->early_restore = TRUE;
            }
        }
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
        const mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
    }
}

static void pump(ScrubMedia *media, gint64 span)
{
    gint64 deadline = g_get_monotonic_time() + span;
    while (g_get_monotonic_time() < deadline) poll_scrub(media, .002);
}

static void await_position(ScrubMedia *media, double target, gboolean paused)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    double tolerance = paused ? .06 : .4;
    while (!media->loaded || media->paused != paused || media->seeking || !isfinite(media->position) ||
           fabs(media->position - target) > tolerance) {
        if (g_get_monotonic_time() > deadline)
            g_error("Scrub position did not settle at %.2f: got %.3f (pause=%d, seeking=%d)",
                    target, media->position, media->paused, media->seeking);
        poll_scrub(media, .002);
    }
}

static void await_gain(ScrubMedia *media, double gain)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (!isfinite(media->gain) || fabs(media->gain - gain) > .000001) {
        if (g_get_monotonic_time() > deadline)
            g_error("Gain did not become %.2f: got %.3f", gain, media->gain);
        poll_scrub(media, .002);
    }
    g_assert_cmpfloat_with_epsilon(native_number(media, "volume-gain"), gain, .000001);
}

static void await_restart(ScrubMedia *media, guint previous)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (media->restarts <= previous) {
        if (g_get_monotonic_time() > deadline) g_error("Scrub frame did not restart.");
        poll_scrub(media, .002);
    }
}

static void await_restored(ScrubMedia *media, double gain)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (audio_scrub_active(media->player.audio_scrub)) {
        if (g_get_monotonic_time() > deadline) g_error("Scrub audio suppression did not finish.");
        poll_scrub(media, .002);
    }
    await_gain(media, gain);
    g_assert_false(media->early_restore);
    media->release_pending = FALSE;
}

static void release_at(ScrubMedia *media, double target, double restored_gain, gboolean paused)
{
    media->release_pending = TRUE;
    media->release_target = target;
    media->early_restore = FALSE;
    guint previous_restart = media->restarts;
    scrub_end(&media->player, target);
    g_assert_false(media->player.dragging);
    await_position(media, target, paused);
    await_restart(media, previous_restart);
    await_restored(media, restored_gain);
}

static void set_number_native(ScrubMedia *media, const char *name, double value)
{
    g_assert_cmpint(mpv_set_property(media->player.mpv, name, MPV_FORMAT_DOUBLE, &value), >=, 0);
    pump(media, 20000);
}

static void set_flag_native(ScrubMedia *media, const char *name, gboolean value)
{
    int flag = value;
    g_assert_cmpint(mpv_set_property(media->player.mpv, name, MPV_FORMAT_FLAG, &flag), >=, 0);
    pump(media, 20000);
}

static void set_track(ScrubMedia *media, const char *track)
{
    g_assert_cmpint(mpv_set_property(media->player.mpv, "aid", MPV_FORMAT_STRING, &track), >=, 0);
    pump(media, 30000);
}

static void assert_track(ScrubMedia *media, const char *track)
{
    char *actual = mpv_get_property_string(media->player.mpv, "aid");
    g_assert_cmpstr(actual, ==, track);
    mpv_free(actual);
}

static void assert_watch_later_clean(ScrubMedia *media, const char *directory)
{
    const char *save[] = {"write-watch-later-config", NULL};
    g_assert_cmpint(mpv_command(media->player.mpv, save), >=, 0);
    g_autoptr(GDir) entries = g_dir_open(directory, 0, NULL);
    g_assert_nonnull(entries);
    gboolean found = FALSE;
    const char *entry;
    while ((entry = g_dir_read_name(entries))) {
        g_autofree char *path = g_build_filename(directory, entry, NULL);
        g_autofree char *contents = NULL;
        g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
        g_assert_null(strstr(contents, "volume-gain"));
        found |= strstr(contents, "start=") != NULL; /* Parent redirects contain no position. */
    }
    g_assert_true(found);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setlocale(LC_NUMERIC, "C");
    g_autofree char *filename = scrub_clip();
    g_autofree char *watch_later = g_strdup_printf("%s.watch-later", filename);
    g_assert_cmpint(g_mkdir(watch_later, 0700), ==, 0);
    ScrubMedia media = {.position = NAN, .gain = NAN};
    Player *p = &media.player;
    p->mpv = observed_core = mpv_create();
    g_assert_nonnull(p->mpv);
    const char *options[][2] = {
        {"config", "no"}, {"vo", "null"}, {"ao", "null"}, {"hwdec", "no"},
        {"pause", "yes"}, {"idle", "yes"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"resume-playback", "no"},
        {"video-timing-offset", "0"}, {"volume-gain", "2.5"}, {"volume", "37"},
        {"af", "@user-gain:lavfi=[volume=0.8]"}, {"watch-later-dir", watch_later}
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(p->mpv, options[i][0], options[i][1]), >=, 0);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console", "load-auto-profiles",
        "load-select", "load-positioning", "load-commands", "load-context-menu"};
    for (guint i = 0; i < G_N_ELEMENTS(scripts); ++i) {
        int status = mpv_set_option_string(p->mpv, scripts[i], "no");
        g_assert_true(status >= 0 || status == MPV_ERROR_OPTION_NOT_FOUND);
    }
    g_assert_cmpint(mpv_initialize(p->mpv), >=, 0);
    g_assert_cmpint(mpv_request_log_messages(p->mpv, "error"), >=, 0);
    const struct { const char *name; mpv_format format; } properties[] = {
        {"time-pos", MPV_FORMAT_DOUBLE}, {"pause", MPV_FORMAT_FLAG}, {"volume-gain", MPV_FORMAT_DOUBLE},
        {"mute", MPV_FORMAT_FLAG}, {"seeking", MPV_FORMAT_FLAG}
    };
    for (guint i = 0; i < G_N_ELEMENTS(properties); ++i)
        g_assert_cmpint(mpv_observe_property(p->mpv, i + 1, properties[i].name, properties[i].format), >=, 0);
    const char *load[] = {"loadfile", filename, "replace", NULL};
    guint previous_restart = media.restarts;
    g_assert_cmpint(mpv_command_async(p->mpv, 1, load), >=, 0);
    await_position(&media, 0, TRUE);
    await_restart(&media, previous_restart);
    await_gain(&media, 2.5);
    p->timeline.loaded = TRUE; p->timeline.duration = 12; p->timeline.paused = TRUE;
    assert_track(&media, "1");
    g_autofree char *filters = native_text(&media, "af");
    g_autofree char *initial_frame = video_frame_hash(&media);

    scrub_begin(p);
    await_gain(&media, TEST_SILENT_GAIN);
    g_assert_true(p->dragging);
    const gint64 first = 1000000;
    previous_restart = media.restarts;
    scrub_queue(p, 3, first);
    g_assert_false(p->scrub_pending);
    g_assert_cmpint(p->last_scrub_us, ==, first);
    await_position(&media, 3, TRUE);
    await_restart(&media, previous_restart);
    g_autofree char *first_frame = video_frame_hash(&media);
    g_assert_cmpstr(first_frame, !=, initial_frame);
    g_assert_cmpuint(interactive_seeks, ==, 1);
    g_assert_true(audio_scrub_active(p->audio_scrub));

    scrub_queue(p, 4, first + 1000);
    scrub_queue(p, 5, first + 1000);
    g_assert_true(p->scrub_pending);
    g_assert_cmpfloat(p->preview, ==, 5);
    scrub_flush(p, first + 33332);
    g_assert_true(p->scrub_pending);
    pump(&media, 80000);
    g_assert_cmpfloat_with_epsilon(media.position, 3, .06);
    g_assert_cmpuint(interactive_seeks, ==, 1);
    previous_restart = media.restarts;
    scrub_flush(p, first + 33333);
    await_position(&media, 5, TRUE);
    await_restart(&media, previous_restart);
    g_autofree char *second_frame = video_frame_hash(&media);
    g_assert_cmpstr(second_frame, !=, first_frame);
    g_assert_cmpuint(interactive_seeks, ==, 2);
    await_gain(&media, TEST_SILENT_GAIN);
    g_assert_false(native_flag(&media, "mute"));
    g_assert_cmpfloat(native_number(&media, "volume"), ==, 37);
    release_at(&media, 6, 2.5, TRUE);
    g_assert_cmpuint(interactive_seeks, ==, 2); /* Final seek belongs to the audio controller. */
    scrub_flush(p, first + 1000000);
    g_assert_cmpfloat(p->preview, ==, 6);
    g_assert_true(media.paused);
    g_autofree char *filters_after = native_text(&media, "af");
    g_assert_cmpstr(filters_after, ==, filters);
    puts("PASS silent live scrubbing: video seeks while held, coalesced motion, final seek then gain restoration.");

    /* All ordinary audio settings stay user-owned during suppression. */
    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    set_flag_native(&media, "mute", TRUE);
    set_number_native(&media, "volume", 43);
    set_track(&media, "2");
    await_gain(&media, TEST_SILENT_GAIN);
    release_at(&media, 4, 2.5, TRUE);
    g_assert_true(native_flag(&media, "mute"));
    g_assert_cmpfloat(native_number(&media, "volume"), ==, 43);
    assert_track(&media, "2");
    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    set_flag_native(&media, "mute", FALSE);
    release_at(&media, 5, 2.5, TRUE);
    g_assert_false(native_flag(&media, "mute"));
    set_number_native(&media, "volume-gain", 4.25);
    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    set_number_native(&media, "volume-gain", 6.5);
    await_gain(&media, TEST_SILENT_GAIN);
    release_at(&media, 6, 6.5, TRUE);
    puts("PASS audio ownership: original/latest gain, mute, volume, track, and user filters preserved.");

    set_flag_native(&media, "pause", FALSE);
    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    const gint64 now = g_get_monotonic_time();
    for (int i = 0; i < 6; ++i) scrub_queue(p, 2 + i * .5, now + i * 1000);
    release_at(&media, 8, 6.5, FALSE);
    g_assert_false(media.paused);
    set_flag_native(&media, "pause", TRUE);
    puts("PASS playing rapid release: final target wins before sound resumes, play state unchanged.");

    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    scrub_end(p, 3);
    scrub_begin(p); /* Re-grab before the earlier release reply is processed. */
    scrub_queue(p, 5, g_get_monotonic_time());
    release_at(&media, 7, 6.5, TRUE);

    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    double late_user_gain = 9.5;
    g_assert_cmpint(mpv_set_property(p->mpv, "volume-gain", MPV_FORMAT_DOUBLE, &late_user_gain), >=, 0);
    scrub_cancel(p); /* The gain observation has deliberately not been pumped. */
    await_restored(&media, 9.5);
    set_number_native(&media, "volume-gain", 6.5);
    puts("PASS re-grab and late user edits: stale release replies cannot restore the wrong gain.");

    scrub_begin(p); scrub_cancel(p); /* No event loop between capture and cancellation. */
    await_restored(&media, 6.5);
    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    assert_watch_later_clean(&media, watch_later);
    scrub_cancel(p); await_restored(&media, 6.5);
    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    media.loaded = FALSE;
    g_assert_cmpint(mpv_command_async(p->mpv, 1, load), >=, 0);
    await_position(&media, 0, TRUE);
    await_restored(&media, 6.5);
    g_assert_false(p->dragging);
    puts("PASS cancellation and source change: no stuck silence or temporary gain in watch-later state.");

    scrub_begin(p); await_gain(&media, TEST_SILENT_GAIN);
    audio_scrub_end(p->audio_scrub, 7);
    audio_scrub_free(p->audio_scrub); p->audio_scrub = NULL;
    await_gain(&media, 6.5);
    pump(&media, 350000); /* Outlive the controller's recovery timer after free. */
    g_assert_cmpuint(p->error_count, ==, 0);
    mpv_terminate_destroy(p->mpv);
    observed_core = NULL;
    g_assert_cmpint(g_unlink(filename), ==, 0);
    g_autoptr(GDir) entries = g_dir_open(watch_later, 0, NULL);
    g_assert_nonnull(entries);
    const char *entry;
    while ((entry = g_dir_read_name(entries))) {
        g_autofree char *path = g_build_filename(watch_later, entry, NULL);
        g_assert_cmpint(g_unlink(path), ==, 0);
    }
    g_assert_cmpint(g_rmdir(watch_later), ==, 0);
    puts("PASS silent scrub teardown with pending completion; no GUI or desktop audio device used.");
    return 0;
}
