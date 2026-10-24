/* Exercise the real asynchronous metadata requests and cache handlers with
 * libmpv. No display, GTK widgets, renderer, or property observations are used. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <glib/gstdio.h>

#define AUDIO_RECOVERY_BARRIER_ID UINT64_C(50000)

typedef struct {
    Player player;
    gboolean loaded, restarted, decoder_seen, video_track_seen;
    gboolean expected_pcm_failure, audio_barrier;
    const char *expected_requested_aid;
    guint started_files, loaded_files, pcm_failure_logs;
    guint64 playback_seen;
} PlaybackMedia;

static char *playback_clip(void)
{
    char *filename = NULL;
    int fd = g_file_open_tmp("mpv-gtk-playback-XXXXXX.mp4", &filename, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    char *args[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=3",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3",
        "-c:v", "mpeg4", "-q:v", "4", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "64k", "-ac", "2", "-shortest",
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

static void poll_playback(PlaybackMedia *media)
{
    Player *p = &media->player;
    mpv_event *event = mpv_wait_event(p->mpv, 0.05);
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY) {
        mpv_event_property *property = event->data;
        uint64_t id = event->reply_userdata;
        if (id >= PLAYBACK_REFRESH_BASE &&
            id < PLAYBACK_REFRESH_BASE + G_N_ELEMENTS(playback_properties)) {
            guint index = (guint)(id - PLAYBACK_REFRESH_BASE);
            media->playback_seen |= UINT64_C(1) << index;
            if (event->error < 0) {
                /* A/V sync and cache duration can be unavailable for a
                 * paused local file. Unknown property names still fail. */
                if (index < 10 || event->error != MPV_ERROR_PROPERTY_UNAVAILABLE)
                    g_error("GET %s: %s", playback_properties[index].name,
                             mpv_error_string(event->error));
                return;
            }
            g_assert_nonnull(property);
            g_assert_cmpstr(property->name, ==, playback_properties[index].name);
            g_assert_cmpint(property->format, ==, playback_properties[index].format);
            g_assert_true(playback_property_changed(p, property));
        } else if (id == HWDEC_REFRESH_ID || id == VIDEO_TRACK_REFRESH_ID) {
            g_assert_cmpint(event->error, >=, 0);
            g_assert_nonnull(property);
            g_assert_cmpint(property->format, ==, MPV_FORMAT_STRING);
            g_assert_cmpstr(property->name, ==,
                            id == HWDEC_REFRESH_ID ? "hwdec-current" : "vid");
            property_changed(p, property);
            if (id == HWDEC_REFRESH_ID)
                media->decoder_seen = TRUE;
            else
                media->video_track_seen = TRUE;
        } else if (id == AUDIO_RECOVERY_BARRIER_ID) {
            g_assert_cmpint(event->error, >=, 0);
            g_assert_nonnull(media->expected_requested_aid);
            g_assert_nonnull(property);
            g_assert_cmpstr(property->name, ==, "options/aid");
            g_assert_cmpint(property->format, ==, MPV_FORMAT_STRING);
            g_assert_nonnull(property->data);
            g_assert_cmpstr(*(char **)property->data, ==, media->expected_requested_aid);
            media->audio_barrier = TRUE;
        } else {
            g_error("Unexpected asynchronous property reply: %" G_GUINT64_FORMAT, id);
        }
    } else if (event->event_id == MPV_EVENT_START_FILE) {
        media->started_files++;
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
        media->loaded_files++;
        media->loaded = TRUE;
        media->restarted = FALSE;
        p->timeline.loaded = TRUE;
        p->preference_values.media_loaded = TRUE;
    } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
        if (media->loaded)
            media->restarted = TRUE;
    } else if (event->event_id == MPV_EVENT_COMMAND_REPLY ||
               event->event_id == MPV_EVENT_SET_PROPERTY_REPLY) {
        g_assert_cmpint(event->error, >=, 0);
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->error, >=, 0);
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
        mpv_event_log_message *log = event->data;
        if (media->expected_pcm_failure) {
            if (g_str_equal(log->prefix, "ao/pcm") && g_str_equal(log->text,
                "Failed to open /proc/mpv-gtk-test-nonexistent/out.wav for writing!\n")) {
                media->pcm_failure_logs |= 1;
                return;
            }
            if (g_str_equal(log->prefix, "ao") && g_str_equal(log->text,
                "Failed to initialize audio driver 'pcm'\n")) {
                media->pcm_failure_logs |= 2;
                return;
            }
            if (g_str_equal(log->prefix, "cplayer") && g_str_equal(log->text,
                "Could not open/initialize audio device -> no sound.\n")) {
                media->pcm_failure_logs |= 4;
                return;
            }
        }
        g_error("libmpv %s: %s", log->prefix, log->text);
    }
}

static void load_playback(PlaybackMedia *media, const char *filename)
{
    media->loaded = media->restarted = FALSE;
    const char *load[] = {"loadfile", filename, "replace", NULL};
    g_assert_cmpint(mpv_command_async(media->player.mpv, 1, load), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!media->loaded || !media->restarted) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_playback(media);
    }
}

static void refresh_playback(PlaybackMedia *media)
{
    media->playback_seen = 0;
    media->decoder_seen = media->video_track_seen = FALSE;
    request_playback_values(&media->player);
    request_hwdec_state(&media->player);
    guint64 expected = (UINT64_C(1) << G_N_ELEMENTS(playback_properties)) - 1;
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (media->playback_seen != expected || !media->decoder_seen || !media->video_track_seen) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_playback(media);
    }
}

static void check_playback(const Player *p)
{
    const PlaybackValues *v = &p->playback;
    g_assert_cmpint(v->width, ==, 320);
    g_assert_cmpint(v->height, ==, 180);
    g_assert_cmpfloat_with_epsilon(v->fps, 24, 0.000001);
    g_assert_nonnull(v->video_codec);
    g_assert_cmpuint(strlen(v->video_codec), >, 0);
    g_assert_cmpstr(v->video_format, ==, "mpeg4");
    g_assert_cmpstr(v->audio_codec, ==, "aac");
    g_assert_cmpint(v->sample_rate, ==, 48000);
    g_assert_cmpstr(v->channels, ==, "stereo");
    g_assert_true(v->drops_known);
    g_assert_true(v->decoder_drops_known);
    g_assert_cmpint(v->dropped, >=, 0);
    g_assert_cmpint(v->decoder_dropped, >=, 0);
    g_assert_true(p->preference_values.has_video);
    g_assert_true(p->preference_values.hwdec_known);
    g_assert_cmpstr(p->hwdec_current, ==, "no");
    g_assert_cmpstr(p->preference_values.hwdec_current, ==, "no");
}

static void assert_audio_option(Player *p, const char *name, const char *expected)
{
    char *actual = mpv_get_property_string(p->mpv, name);
    g_assert_cmpstr(actual, ==, expected);
    mpv_free(actual);
}

static void wait_audio_state(PlaybackMedia *media, const char *output, const char *track)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    for (;;) {
        char *actual_output = mpv_get_property_string(media->player.mpv, "current-ao");
        char *actual_track = mpv_get_property_string(media->player.mpv, "aid");
        gboolean matches = g_strcmp0(actual_output, output) == 0 &&
            g_strcmp0(actual_track, track) == 0 &&
            (!media->expected_pcm_failure || media->pcm_failure_logs == 7);
        mpv_free(actual_output);
        mpv_free(actual_track);
        if (matches)
            return;
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_playback(media);
    }
}

/* A queued read follows every asynchronous setter from the host callback,
 * so an accidental late aid=auto cannot escape the explicit-No-audio check. */
static void audio_settings_barrier(PlaybackMedia *media, const char *requested_aid)
{
    media->audio_barrier = FALSE;
    media->expected_requested_aid = requested_aid;
    g_assert_cmpint(mpv_get_property_async(media->player.mpv, AUDIO_RECOVERY_BARRIER_ID,
                                         "options/aid", MPV_FORMAT_STRING), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!media->audio_barrier) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_playback(media);
    }
    media->expected_requested_aid = NULL;
}

static void check_audio_recovery(PlaybackMedia *media)
{
    Player *p = &media->player;
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-audio-recovery-XXXXXX", &error);
    g_assert_no_error(error);
    p->config.path = g_build_filename(directory, "mpv.conf", NULL);
    g_assert_true(g_file_set_contents(p->config.path,
        "# Keep this file during live recovery\nao=pcm\naudio-device=auto\n", -1, &error));
    g_assert_no_error(error);
    char *selected_track = mpv_get_property_string(p->mpv, "aid");
    g_assert_nonnull(selected_track);
    g_assert_cmpstr(selected_track, !=, "no");
    assert_audio_option(p, "options/aid", "auto");
    wait_audio_state(media, "null", selected_track);
    guint started = media->started_files, loaded = media->loaded_files;

    /* /proc cannot create this directory or output file. The only outputs
     * touched here are the intentional PCM failure and the silent null AO. */
    g_assert_cmpint(mpv_set_option_string(p->mpv, "ao-pcm-file",
        "/proc/mpv-gtk-test-nonexistent/out.wav"), >=, 0);
    const char *output = "pcm";
    media->expected_pcm_failure = TRUE;
    g_assert_cmpint(mpv_set_property_async(p->mpv, 0, "ao", MPV_FORMAT_STRING, &output), >=, 0);
    wait_audio_state(media, NULL, "no");
    assert_audio_option(p, "options/aid", "auto");
    media->expected_pcm_failure = FALSE;

    output = "null";
    preference_changed("ao", MPV_FORMAT_STRING, &output, p);
    audio_settings_barrier(media, "auto");
    wait_audio_state(media, "null", selected_track);
    g_assert_cmpstr(p->preference_values.audio_output, ==, "null");
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(p->config.path, &saved, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(saved, "ao=null\n"));

    const char *disabled = "no";
    g_assert_cmpint(mpv_set_property_async(p->mpv, 0, "aid", MPV_FORMAT_STRING, &disabled), >=, 0);
    audio_settings_barrier(media, "no");
    wait_audio_state(media, NULL, "no");
    preference_changed("ao", MPV_FORMAT_STRING, &output, p);
    audio_settings_barrier(media, "no");
    wait_audio_state(media, NULL, "no");
    const char *device = "auto";
    preference_changed("audio-device", MPV_FORMAT_STRING, &device, p);
    audio_settings_barrier(media, "no");
    wait_audio_state(media, NULL, "no");
    g_assert_cmpuint(media->started_files, ==, started);
    g_assert_cmpuint(media->loaded_files, ==, loaded);
    g_assert_true(media->loaded);
    g_assert_true(p->timeline.loaded);
    g_assert_cmpuint(p->error_count, ==, 0);

    mpv_free(selected_track);
    g_assert_cmpint(g_unlink(p->config.path), ==, 0);
    config_clear(&p->config);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS audio recovery: real PCM init failure, live null AO restores requested track "
         "without reload; explicit No audio survives output and device changes");
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    g_autofree char *filename = playback_clip();
    PlaybackMedia media = {0};
    Player *p = &media.player;
    p->mpv = mpv_create();
    g_assert_nonnull(p->mpv);
    const char *options[][2] = {
        {"config", "no"}, {"vo", "null"}, {"ao", "null"}, {"hwdec", "no"},
        {"pause", "yes"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"video-timing-offset", "0"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(p->mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(p->mpv), >=, 0);
    g_assert_cmpint(mpv_request_log_messages(p->mpv, "error"), >=, 0);

    load_playback(&media, filename);
    refresh_playback(&media);
    check_playback(p);

    /* Clear exactly the metadata mirror and decoder availability reset on
     * START_FILE, then reload identical media. Forced requests must restore
     * every field even when an observed value would not have changed. */
    clear_playback_values(&p->playback);
    p->preference_values.has_video = FALSE;
    p->preference_values.hwdec_known = FALSE;
    g_assert_cmpint(p->playback.width, ==, 0);
    g_assert_null(p->playback.audio_codec);
    load_playback(&media, filename);
    refresh_playback(&media);
    check_playback(p);

    check_audio_recovery(&media);

    mpv_terminate_destroy(p->mpv);
    clear_playback_values(&p->playback);
    g_free(p->hwdec_current);
    g_free(p->audio_output);
    g_free(p->audio_device);
    g_assert_cmpint(unlink(filename), ==, 0);
    puts("PASS playback metadata: real async schema, MPEG-4/AAC parameters, active decoder, identical-file reload after cache reset");
    return 0;
}
