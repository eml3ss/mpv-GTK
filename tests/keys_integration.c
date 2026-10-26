/* Drive the host's real initialization and settings callback without GTK.
 * Only command acknowledgements/input snapshots enter handle_event(); widget
 * property updates and frontend client messages are inspected independently. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <glib/gstdio.h>

#define QUICK_CONTROL_READ_ID UINT64_C(49000)

typedef struct {
    Player player;
    GPtrArray *bindings;
    char *message;
    double volume;
    uint64_t next_id, replied;
    guint reloads, snapshots, edit_defines, edit_enables, snapshot_enable_generation;
    gboolean fullscreen_known, fullscreen;
    gboolean pause_known, paused;
    guint loaded_files;
    const char *requested_property;
    mpv_format requested_format;
    gboolean property_read;
    double property_number;
    char *property_text;
    guint notice_replies, accepted_notices[2];
    char *notice_ids[2];
    uint64_t defer_notice_id, deferred_notice_id;
    char *deferred_notice_sid;
} KeysHost;

static void dispatch_track_notice(KeysHost *host, mpv_event *event)
{
    Player *p = &host->player;
    int index = -1;
    for (guint i = 0; i < G_N_ELEMENTS(p->track_notices); ++i)
        if (p->track_notices[i].request == event->reply_userdata)
            index = (int)i;
    gboolean silent = index >= 0 && p->track_notices[index].priming;
    g_autofree char *previous = index >= 0 ? g_strdup(p->track_notices[index].actual) : NULL;
    accept_track_notice(p, event); /* No video_overlay: the production show guard is safe. */
    if (index >= 0 && !silent && previous && p->timeline.loaded && !p->closing &&
        g_strcmp0(previous, p->track_notices[index].actual) != 0) {
        host->accepted_notices[index]++;
        g_free(host->notice_ids[index]);
        host->notice_ids[index] = g_strdup(p->track_notices[index].actual);
    }
}

static void poll_host(KeysHost *host)
{
    Player *p = &host->player;
    mpv_event *event = mpv_wait_event(p->mpv, .02);
    g_assert_cmpint(event->error, >=, 0);
    if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
        if (event->reply_userdata == INPUT_RELOAD_CLEAR_ID ||
            event->reply_userdata == INPUT_RELOAD_FILE_ID ||
            event->reply_userdata == INPUT_EDIT_DEFINE_ID ||
            event->reply_userdata == INPUT_EDIT_ENABLE_ID) {
            if (event->reply_userdata == INPUT_RELOAD_FILE_ID)
                host->reloads++;
            else if (event->reply_userdata == INPUT_EDIT_DEFINE_ID)
                host->edit_defines++;
            else if (event->reply_userdata == INPUT_EDIT_ENABLE_ID)
                host->edit_enables++;
            handle_event(p, event);
        } else if (event->reply_userdata >= 50000) {
            host->replied = event->reply_userdata;
        }
    } else if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY &&
               event->reply_userdata == QUICK_CONTROL_READ_ID) {
        mpv_event_property *property = event->data;
        g_assert_cmpstr(property->name, ==, host->requested_property);
        g_assert_cmpint(property->format, ==, host->requested_format);
        g_assert_nonnull(property->data);
        if (property->format == MPV_FORMAT_DOUBLE)
            host->property_number = *(double *)property->data;
        else if (property->format == MPV_FORMAT_STRING) {
            g_free(host->property_text);
            host->property_text = g_strdup(*(char **)property->data);
        }
        host->property_read = TRUE;
    } else if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY &&
               event->reply_userdata >= TRACK_NOTICE_REFRESH_BASE) {
        mpv_event_property *property = event->data;
        g_assert_true(g_str_equal(property->name, "sid") || g_str_equal(property->name, "aid"));
        g_assert_cmpint(property->format, ==, MPV_FORMAT_STRING);
        g_assert_nonnull(property->data);
        host->notice_replies++;
        if (host->defer_notice_id == event->reply_userdata) {
            g_assert_cmpstr(property->name, ==, "sid");
            host->defer_notice_id = 0;
            host->deferred_notice_id = event->reply_userdata;
            g_free(host->deferred_notice_sid);
            host->deferred_notice_sid = g_strdup(*(char **)property->data);
        } else {
            dispatch_track_notice(host, event);
        }
    } else if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY &&
               event->reply_userdata >= INITIAL_SUBTITLE_REFRESH_BASE) {
        handle_event(p, event);
    } else if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY &&
               event->reply_userdata == INPUT_BINDINGS_REFRESH_ID) {
        mpv_event_property *property = event->data;
        g_assert_cmpint(property->format, ==, MPV_FORMAT_NODE);
        g_clear_pointer(&host->bindings, g_ptr_array_unref);
        host->bindings = keybindings_snapshot(property->data,
            app_key_defaults, G_N_ELEMENTS(app_key_defaults));
        handle_event(p, event); /* Unopened Keybinds needs no frontend copy. */
        host->snapshots++;
        host->snapshot_enable_generation = host->edit_enables;
    } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
        mpv_event_property *property = event->data;
        if (property->format == MPV_FORMAT_DOUBLE && g_str_equal(property->name, "volume"))
            host->volume = *(double *)property->data;
        else if (property->format == MPV_FORMAT_FLAG && g_str_equal(property->name, "fullscreen")) {
            host->fullscreen_known = TRUE;
            host->fullscreen = !!*(int *)property->data;
        } else if (property->format == MPV_FORMAT_FLAG && g_str_equal(property->name, "pause")) {
            host->pause_known = TRUE;
            host->paused = !!*(int *)property->data;
        } else if (property->format == MPV_FORMAT_STRING &&
                   (g_str_equal(property->name, "sid") || g_str_equal(property->name, "aid"))) {
            observe_track_notice(p, g_str_equal(property->name, "aid"), *(char **)property->data);
        }
    } else if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
        mpv_event_client_message *message = event->data;
        g_assert_cmpint(message->num_args, >=, 2);
        g_assert_cmpstr(message->args[0], ==, "mpv-gtk");
        g_free(host->message);
        host->message = g_strdup(message->args[1]);
    } else if (event->event_id == MPV_EVENT_HOOK) {
        /* Release only the render barrier: this fixture uses stock null VO
         * and AO and has no GTK surface to initialize. */
        mpv_event_hook *hook = event->data;
        g_assert_cmpint(mpv_hook_continue(p->mpv, hook->id), >=, 0);
    } else if (event->event_id == MPV_EVENT_START_FILE) {
        p->timeline.loaded = FALSE;
        clear_initial_subtitle(p);
        clear_track_notices(p);
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
        host->loaded_files++;
        p->timeline.loaded = TRUE;
        request_initial_subtitle(p);
        request_track_notice(p, FALSE, TRUE);
        request_track_notice(p, TRUE, TRUE);
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->error, >=, 0);
        p->timeline.loaded = FALSE;
        clear_initial_subtitle(p);
        clear_track_notices(p);
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
        mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
    }
}

static void await_volume(KeysHost *host, double expected, uint64_t reply)
{
    gint64 until = g_get_monotonic_time() + 3000000;
    while (!isfinite(host->volume) || fabs(host->volume - expected) > .000001 ||
           host->replied < reply) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
}

static uint64_t send_key(KeysHost *host, const char *key)
{
    const char *args[] = {"keypress", key, NULL};
    uint64_t id = ++host->next_id;
    g_assert_cmpint(mpv_command_async(host->player.mpv, id, args), >=, 0);
    return id;
}

static void press_volume(KeysHost *host, const char *key, double expected)
{
    uint64_t id = send_key(host, key);
    await_volume(host, expected, id);
}

static const KeyBinding *binding(KeysHost *host, const char *key)
{
    for (guint i = 0; host->bindings && i < host->bindings->len; ++i) {
        const KeyBinding *row = g_ptr_array_index(host->bindings, i);
        if (g_str_equal(row->key, key))
            return row;
    }
    g_error("Missing host binding: %s", key);
    return NULL;
}

static void edit(KeysHost *host, const char *old_key, const char *key,
                   const char *command, gboolean reset)
{
    guint reloads = host->reloads, defines = host->edit_defines, enables = host->edit_enables;
    g_autoptr(GError) error = NULL;
    g_assert_true(change_key_binding(old_key, key, command, reset, &error, &host->player));
    g_assert_no_error(error);
    g_assert_true(host->player.keys_reload_pending);
    gint64 until = g_get_monotonic_time() + 3000000;
    while (host->reloads == reloads || host->edit_defines == defines ||
           host->edit_enables == enables || host->player.keys_reload_pending) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
    /* The Keybinds page is unopened in this display-free host. Ask explicitly
     * for the snapshot used by assertions, after the real edit was enabled. */
    guint snapshots = host->snapshots;
    g_assert_cmpint(mpv_get_property_async(host->player.mpv, INPUT_BINDINGS_REFRESH_ID,
        "input-bindings", MPV_FORMAT_NODE), >=, 0);
    while (host->snapshots == snapshots) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
    g_assert_cmpuint(host->snapshot_enable_generation, >, enables);
    g_assert_cmpuint(host->player.error_count, ==, 0);
}

static void press_fullscreen_key(KeysHost *host, const char *key, gboolean expected)
{
    uint64_t id = send_key(host, key);
    gint64 until = g_get_monotonic_time() + 3000000;
    while (!host->fullscreen_known || host->fullscreen != expected || host->replied < id) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
}

static void assert_fullscreen(KeysHost *host, gboolean expected)
{
    int actual = -1;
    g_assert_cmpint(mpv_get_property(host->player.mpv, "fullscreen", MPV_FORMAT_FLAG, &actual), >=, 0);
    g_assert_cmpint(!!actual, ==, expected);
}

static void assert_pause(KeysHost *host, gboolean expected)
{
    int actual = -1;
    g_assert_cmpint(mpv_get_property(host->player.mpv, "pause", MPV_FORMAT_FLAG, &actual), >=, 0);
    g_assert_cmpint(!!actual, ==, expected);
}

static void press_pause_key(KeysHost *host, const char *key, gboolean expected)
{
    uint64_t id = send_key(host, key);
    gint64 until = g_get_monotonic_time() + 3000000;
    while (!host->pause_known || host->paused != expected || host->replied < id) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
    assert_pause(host, expected);
}

static void read_quick_property(KeysHost *host, const char *name, mpv_format format)
{
    host->requested_property = name;
    host->requested_format = format;
    host->property_read = FALSE;
    g_assert_cmpint(mpv_get_property_async(host->player.mpv, QUICK_CONTROL_READ_ID,
                                         name, format), >=, 0);
    gint64 until = g_get_monotonic_time() + 3000000;
    while (!host->property_read) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
}

static void check_quick_speed(KeysHost *host)
{
    read_quick_property(host, "speed", MPV_FORMAT_DOUBLE);
    g_assert_cmpfloat_with_epsilon(host->property_number, 1, .000001);
    /* The production action queues native relative commands. A burst must
     * accumulate even while the host's observed speed is still unchanged. */
    for (int i = 0; i < 4; ++i)
        quick_control_action(&host->player, TRUE, GDK_BUTTON_SECONDARY);
    read_quick_property(host, "speed", MPV_FORMAT_DOUBLE);
    g_assert_cmpfloat_with_epsilon(host->property_number, 2, .000001);
    quick_control_action(&host->player, TRUE, GDK_BUTTON_MIDDLE);
    read_quick_property(host, "speed", MPV_FORMAT_DOUBLE);
    g_assert_cmpfloat_with_epsilon(host->property_number, 1, .000001);
}

static void load_subtitle_fixture(KeysHost *host, const char *filename)
{
    guint loaded = host->loaded_files;
    const char *args[] = {"loadfile", filename, "replace", NULL};
    g_assert_cmpint(mpv_command_async(host->player.mpv, ++host->next_id, args), >=, 0);
    gint64 until = g_get_monotonic_time() + 3000000;
    while (host->loaded_files == loaded || !host->player.initial_subtitle ||
           host->player.initial_subtitle_request ||
           !host->player.track_notices[0].actual || !host->player.track_notices[1].actual ||
           host->player.track_notices[0].request || host->player.track_notices[1].request) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
}

static void assert_sid(KeysHost *host, const char *expected)
{
    read_quick_property(host, "sid", MPV_FORMAT_STRING);
    g_assert_cmpstr(host->property_text, ==, expected);
}

static void await_initial_subtitle(KeysHost *host)
{
    gint64 until = g_get_monotonic_time() + 3000000;
    while (!host->player.initial_subtitle || host->player.initial_subtitle_request) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
}

static void settle_track(KeysHost *host, gboolean audio, const char *id)
{
    TrackNoticeWatch *watch = &host->player.track_notices[!!audio];
    gint64 until = g_get_monotonic_time() + 3000000;
    while (watch->request || watch->priming || g_strcmp0(watch->actual, id)) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
}

static void await_track_notice(KeysHost *host, gboolean audio, guint previous, const char *id)
{
    guint index = !!audio;
    gint64 until = g_get_monotonic_time() + 3000000;
    while (host->accepted_notices[index] == previous) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
    g_assert_cmpuint(host->accepted_notices[index], ==, previous + 1);
    g_assert_cmpstr(host->notice_ids[index], ==, id);
    settle_track(host, audio, id);
}

static void check_subtitle_notice_text(KeysHost *host)
{
    Player *p = &host->player;
    p->audio_menu = g_menu_new();
    p->subtitle_menu = g_menu_new();
    mpv_node tracks = {0};
    g_assert_cmpint(mpv_get_property(p->mpv, "track-list", MPV_FORMAT_NODE, &tracks), >=, 0);
    update_track_menu(p, &tracks);
    mpv_free_node_contents(&tracks);
    for (guint audio = 0; audio < 2; ++audio) {
        GMenuModel *menu = G_MENU_MODEL(audio ? p->audio_menu : p->subtitle_menu);
        g_autofree char *label = NULL;
        for (int i = 0; i < g_menu_model_get_n_items(menu); ++i) {
            g_autoptr(GVariant) target = g_menu_model_get_item_attribute_value(
                menu, i, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE_STRING);
            if (target && g_str_equal(g_variant_get_string(target, NULL), "2"))
                g_assert_true(g_menu_model_get_item_attribute(menu, i,
                    G_MENU_ATTRIBUTE_LABEL, "s", &label));
        }
        g_assert_nonnull(label);
        g_assert_nonnull(strstr(label, audio ? "Dubbed audio · Ω" : "Translated subtitles · Ω"));
        g_assert_nonnull(strstr(label, "rus"));
        g_autofree char *text = track_notice_text(p, audio, "2");
        g_assert_cmpstr(text, ==, label);
        g_autofree char *off = track_notice_text(p, audio, "no");
        g_assert_cmpstr(off, ==, tr(audio ? "Без звука" : "Выключены"));
        g_autofree char *automatic = track_notice_text(p, audio, "auto");
        g_assert_cmpstr(automatic, ==, tr("Автоматически"));
        g_autofree char *unknown = track_notice_text(p, audio, "99");
        g_autofree char *expected = g_strdup_printf(tr("Дорожка %s"), "99");
        g_assert_cmpstr(unknown, ==, expected);
    }
}

static void check_nonmouse_subtitle_changes(KeysHost *host)
{
    Player *p = &host->player;
    settle_track(host, FALSE, "1");
    guint accepted = host->accepted_notices[0];
    g_autoptr(GSimpleAction) subtitle = g_simple_action_new("subtitle", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) two = g_variant_ref_sink(g_variant_new_string("2"));
    action_activated(subtitle, two, p);
    assert_sid(host, "2");
    settle_track(host, FALSE, "2");
    g_assert_cmpuint(host->accepted_notices[0], ==, accepted);
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    assert_sid(host, "1");
    settle_track(host, FALSE, "1");
    g_assert_cmpuint(host->accepted_notices[0], ==, accepted);

    edit(host, NULL, "j", "cycle sid", FALSE);
    send_key(host, "j");
    /* Native keypress acknowledges injection before its binding executes.
     * The following UP action is an input FIFO barrier for the sid cycle. */
    press_volume(host, "UP", host->volume + 5);
    assert_sid(host, "2");
    await_track_notice(host, FALSE, accepted, "2");
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    assert_sid(host, "1");
    settle_track(host, FALSE, "1");
    g_assert_cmpuint(host->accepted_notices[0], ==, accepted + 1);

    for (const char **id = (const char *[]){"2", "no", "1", NULL}; *id; ++id) {
        guint audio_count = host->accepted_notices[1];
        quick_audio_action(p, GDK_BUTTON_SECONDARY);
        read_quick_property(host, "aid", MPV_FORMAT_STRING);
        g_assert_cmpstr(host->property_text, ==, *id);
        await_track_notice(host, TRUE, audio_count, *id);
    }
    edit(host, NULL, "a", "cycle aid", FALSE);
    guint audio_count = host->accepted_notices[1];
    send_key(host, "a");
    press_volume(host, "UP", host->volume + 5);
    read_quick_property(host, "aid", MPV_FORMAT_STRING);
    g_assert_cmpstr(host->property_text, ==, "2");
    await_track_notice(host, TRUE, audio_count, "2");
    g_autoptr(GSimpleAction) audio = g_simple_action_new("audio", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) one = g_variant_ref_sink(g_variant_new_string("1"));
    action_activated(audio, one, p);
    settle_track(host, TRUE, "1");
    g_assert_cmpuint(host->accepted_notices[1], ==, audio_count + 1);
    quick_audio_action(p, GDK_BUTTON_MIDDLE); /* A null menu is intentionally safe in this host. */
    guint subtitles_before = host->accepted_notices[0];
    guint audio_before = host->accepted_notices[1];
    check_quick_speed(host);
    g_assert_cmpuint(host->accepted_notices[0], ==, subtitles_before);
    g_assert_cmpuint(host->accepted_notices[1], ==, audio_before);
}

static void check_notice_after_reload(KeysHost *host, const char *filename)
{
    Player *p = &host->player;
    /* Retain an actual libmpv reply in owned storage until a different file
     * load has established its own current notice request. Finish the prior
     * reset's silent baseline before issuing the cycle we intend to retain. */
    settle_track(host, FALSE, "no");
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    host->defer_notice_id = p->track_notices[0].request;
    g_assert_cmpuint(host->defer_notice_id, >, TRACK_NOTICE_REFRESH_BASE);
    g_assert_false(p->track_notices[0].priming);
    uint64_t intended = host->defer_notice_id;
    gint64 until = g_get_monotonic_time() + 3000000;
    while (host->defer_notice_id) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(host);
    }
    g_assert_cmpuint(host->deferred_notice_id, ==, intended);
    g_assert_cmpstr(host->deferred_notice_sid, ==, "1");
    uint64_t old = host->deferred_notice_id;
    load_subtitle_fixture(host, filename);
    g_assert_cmpuint(p->track_notices[0].request, ==, 0);
    const char *two = "2";
    g_assert_cmpint(mpv_set_property_async(p->mpv, 0, "sid", MPV_FORMAT_STRING, &two), >=, 0);
    observe_track_notice(p, FALSE, "2");
    uint64_t current = p->track_notices[0].request;
    g_assert_cmpuint(current, >, old);
    guint accepted = host->accepted_notices[0];
    mpv_event_property property = {"sid", MPV_FORMAT_STRING, &host->deferred_notice_sid};
    mpv_event deferred = {.event_id = MPV_EVENT_GET_PROPERTY_REPLY,
        .reply_userdata = old, .data = &property};
    dispatch_track_notice(host, &deferred);
    g_assert_cmpuint(host->accepted_notices[0], ==, accepted);
    g_assert_cmpuint(p->track_notices[0].request, ==, current);
    await_track_notice(host, FALSE, accepted, "2");
}

static void check_quick_subtitles(KeysHost *host, const char *directory)
{
    g_autofree char *first = g_build_filename(directory, "first.srt", NULL);
    g_autofree char *second = g_build_filename(directory, "second.srt", NULL);
    g_autofree char *filename = g_build_filename(directory, "subtitles.mkv", NULL);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(first,
        "1\n00:00:00,000 --> 00:00:29,000\nFirst subtitle\n", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(second,
        "1\n00:00:00,000 --> 00:00:29,000\nSecond subtitle\n", -1, &error));
    g_assert_no_error(error);
    char *args[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "color=c=black:size=32x32:rate=1:duration=30", "-i", first, "-i", second,
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=30",
        "-map", "0:v", "-map", "3:a", "-map", "3:a", "-map", "1:s", "-map", "2:s",
        "-c:v", "mpeg4", "-c:a", "flac", "-c:s", "srt",
        "-disposition:a:0", "default", "-disposition:a:1", "0",
        "-metadata:s:a:0", "title=Original audio", "-metadata:s:a:0", "language=eng",
        "-metadata:s:a:1", "title=Dubbed audio · Ω", "-metadata:s:a:1", "language=rus",
        "-disposition:s:0", "default", "-disposition:s:1", "0",
        "-metadata:s:s:0", "title=Original subtitles", "-metadata:s:s:0", "language=eng",
        "-metadata:s:s:1", "title=Translated subtitles · Ω", "-metadata:s:s:1", "language=rus",
        filename, NULL
    };
    g_autofree char *messages = NULL;
    int status = -1;
    g_assert_true(g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, &messages, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("ffmpeg: %s; %s", error->message, messages ? messages : "");

    Player *p = &host->player;
    g_assert_cmpint(mpv_set_option_string(p->mpv, "vo", "null"), >=, 0);
    g_assert_cmpint(mpv_set_option_string(p->mpv, "pause", "yes"), >=, 0);
    load_subtitle_fixture(host, filename);
    g_assert_cmpstr(p->initial_subtitle, ==, "1");
    assert_sid(host, "1");
    check_subtitle_notice_text(host);

    /* A middle click before the queued initial SID reply must wait for that
     * snapshot. The intervening cycle must not become the reset baseline. */
    request_initial_subtitle(p);
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    g_assert_true(p->initial_subtitle_reset_pending);
    await_initial_subtitle(host);
    g_assert_false(p->initial_subtitle_reset_pending);
    g_assert_cmpstr(p->initial_subtitle, ==, "1");
    assert_sid(host, "1");

    /* Click order also matters in the opposite direction: a reset followed
     * by a cycle must finish on the next track, even before the initial
     * snapshot has been delivered to the frontend. */
    request_initial_subtitle(p);
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    await_initial_subtitle(host);
    assert_sid(host, "2");
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    assert_sid(host, "1");

    request_initial_subtitle(p);
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    await_initial_subtitle(host);
    assert_sid(host, "1"); /* The last reset supersedes that queued cycle. */

    /* Supersede a real queued SID=1 read with a SID=2 read. Accepting the
     * first request would clear the newer request and retain the wrong SID. */
    request_initial_subtitle(p);
    uint64_t stale = p->initial_subtitle_request;
    const char *two = "2";
    g_assert_cmpint(mpv_set_property_async(p->mpv, 0, "sid", MPV_FORMAT_STRING, &two), >=, 0);
    request_initial_subtitle(p);
    g_assert_cmpuint(p->initial_subtitle_request, >, stale);
    await_initial_subtitle(host);
    g_assert_cmpstr(p->initial_subtitle, ==, "2");
    const char *one = "1";
    g_assert_cmpint(mpv_set_property_async(p->mpv, 0, "sid", MPV_FORMAT_STRING, &one), >=, 0);
    request_initial_subtitle(p);
    await_initial_subtitle(host);
    g_assert_cmpstr(p->initial_subtitle, ==, "1");

    settle_track(host, FALSE, "1");
    guint notices = host->accepted_notices[0];
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    assert_sid(host, "2");
    await_track_notice(host, FALSE, notices, "2");
    notices = host->accepted_notices[0];
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    assert_sid(host, "1"); /* Restore the file's initial track, rather than off. */
    settle_track(host, FALSE, "1");
    g_assert_cmpuint(host->accepted_notices[0], ==, notices);
    check_nonmouse_subtitle_changes(host);

    /* A menu pick must also invalidate an already queued RMB notice. */
    notices = host->accepted_notices[0];
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    uint64_t before_menu = p->track_notices[0].request;
    g_assert_cmpuint(before_menu, >, TRACK_NOTICE_REFRESH_BASE);
    g_autoptr(GSimpleAction) menu = g_simple_action_new("subtitle", G_VARIANT_TYPE_STRING);
    g_autoptr(GVariant) selected = g_variant_ref_sink(g_variant_new_string("1"));
    action_activated(menu, selected, p);
    g_assert_cmpuint(p->track_notices[0].request, >, before_menu);
    g_assert_true(p->track_notices[0].priming);
    assert_sid(host, "1");
    settle_track(host, FALSE, "1");
    g_assert_cmpuint(host->accepted_notices[0], ==, notices);

    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    assert_sid(host, "2");
    await_track_notice(host, FALSE, notices, "2");
    notices = host->accepted_notices[0];
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    assert_sid(host, "no");
    await_track_notice(host, FALSE, notices, "no");
    notices = host->accepted_notices[0];
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    assert_sid(host, "1");
    settle_track(host, FALSE, "1");
    g_assert_cmpuint(host->accepted_notices[0], ==, notices);

    /* Initial state belongs to each file load, including an explicit No
     * subtitles setting. Reopening must replace the old initial-track cache. */
    const char *off = "no";
    g_assert_cmpint(mpv_set_property_async(p->mpv, 0, "sid", MPV_FORMAT_STRING, &off), >=, 0);
    assert_sid(host, "no");
    load_subtitle_fixture(host, filename);
    g_assert_cmpstr(p->initial_subtitle, ==, "no");
    quick_control_action(p, FALSE, GDK_BUTTON_SECONDARY);
    assert_sid(host, "1");
    quick_control_action(p, FALSE, GDK_BUTTON_MIDDLE);
    assert_sid(host, "no");
    check_notice_after_reload(host, filename);
    g_clear_object(&p->audio_menu);
    g_clear_object(&p->subtitle_menu);
    g_assert_cmpint(g_remove(first), ==, 0);
    g_assert_cmpint(g_remove(second), ==, 0);
    g_assert_cmpint(g_remove(filename), ==, 0);
}

int main(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-keys-host-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *config = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input = g_build_filename(directory, "input.conf", NULL);
    const char *original =
        "# Existing host override\nSPACE set volume 17 # preserve this note\n"
        "t {advanced} set volume 79\nu {advanced} set volume 81\n";
    g_assert_true(g_file_set_contents(config, "hwdec=no\nvolume=50\n", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(input, original, -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_setenv("MPV_GTK_CONFIG_FILE", config, TRUE));
    KeysHost host = {.player = {.wake_fd = -1, .test_seconds = 1},
                     .volume = NAN, .next_id = 50000};
    Player *p = &host.player;
    g_assert_true(init_mpv(p));
    await_volume(&host, 50, 0);
    /* Defaults and user bindings must work before any frontend snapshot. */
    press_volume(&host, "UP", 55);
    press_volume(&host, "SPACE", 17);
    uint64_t info_reply = send_key(&host, "i");
    gint64 until = g_get_monotonic_time() + 3000000;
    while (g_strcmp0(host.message, "info") || host.replied < info_reply) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(&host);
    }
    press_fullscreen_key(&host, "F11", TRUE);
    press_fullscreen_key(&host, "F11", FALSE);
    assert_fullscreen(&host, FALSE);
    press_pause_key(&host, "MBTN_RIGHT", TRUE);
    /* GTK identifies the second nearby click as a double-click. With no
     * explicit double binding, it must still use the current right action. */
    press_pause_key(&host, "MBTN_RIGHT_DBL", FALSE);
    press_pause_key(&host, "MBTN_RIGHT", TRUE);
    press_pause_key(&host, "MBTN_RIGHT_DBL", FALSE);

    /* Focused media keys work before the optional key catalog is loaded. */
    press_pause_key(&host, "PLAY", TRUE);
    press_pause_key(&host, "PAUSE", TRUE);
    press_pause_key(&host, "PLAY", FALSE);
    check_quick_speed(&host);
    g_assert_cmpuint(host.snapshots, ==, 0);
    g_assert_false(p->keys_loaded);

    g_assert_cmpint(mpv_get_property_async(p->mpv, INPUT_BINDINGS_REFRESH_ID,
                                         "input-bindings", MPV_FORMAT_NODE), >=, 0);
    until = g_get_monotonic_time() + 3000000;
    while (!host.snapshots) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_host(&host);
    }
    g_assert_true(binding(&host, "UP")->app_default);
    g_assert_true(binding(&host, "UP")->active);
    g_assert_false(binding(&host, "UP")->weak);
    g_assert_false(binding(&host, "SPACE")->app_default);
    g_assert_cmpstr(binding(&host, "SPACE")->command, ==, "set volume 17");
    g_assert_true(binding(&host, "q")->weak);
    g_assert_false(binding(&host, "q")->active);
    g_assert_true(binding(&host, "MBTN_RIGHT")->app_default);
    g_assert_cmpstr(binding(&host, "MBTN_RIGHT")->command, ==, "cycle pause");

    const char *media_keys[] = {"PLAY", "PAUSE", "STOP", "PREV", "NEXT"};
    for (guint i = 0; i < G_N_ELEMENTS(media_keys); ++i) {
        g_assert_true(binding(&host, media_keys[i])->app_default);
        g_assert_true(binding(&host, media_keys[i])->active);
    }

    /* Right mouse follows the same input.conf precedence and live editing
     * path as keyboard bindings; no GTK mouse event is synthesized here. */
    edit(&host, "MBTN_RIGHT", "MBTN_RIGHT", "set volume 29", FALSE);
    press_volume(&host, "MBTN_RIGHT_DBL", 29);
    assert_pause(&host, FALSE);
    edit(&host, "MBTN_RIGHT", "MBTN_RIGHT", "ignore", FALSE);
    press_volume(&host, "MBTN_RIGHT_DBL", 29);
    press_volume(&host, "UP", 34); /* Bound commands run after keypress acknowledgements. */
    assert_pause(&host, FALSE);
    edit(&host, "MBTN_RIGHT", "MBTN_RIGHT", NULL, TRUE);
    g_assert_true(binding(&host, "MBTN_RIGHT")->app_default);
    press_pause_key(&host, "MBTN_RIGHT", TRUE);
    press_pause_key(&host, "MBTN_RIGHT_DBL", FALSE);

    /* An explicit double-click binding replaces the fallback independently
     * of the ordinary right-click action. */
    edit(&host, "MBTN_RIGHT_DBL", "MBTN_RIGHT_DBL", "set volume 41", FALSE);
    press_volume(&host, "MBTN_RIGHT_DBL", 41);
    assert_pause(&host, FALSE);
    edit(&host, "MBTN_RIGHT_DBL", "MBTN_RIGHT_DBL", NULL, TRUE);
    press_pause_key(&host, "MBTN_RIGHT_DBL", TRUE);
    press_pause_key(&host, "MBTN_RIGHT", FALSE);

    edit(&host, "SPACE", "SPACE", "set volume 23", FALSE);
    g_assert_cmpstr(binding(&host, "SPACE")->command, ==, "set volume 23");
    press_volume(&host, "SPACE", 23);
    edit(&host, "SPACE", "SPACE", NULL, TRUE);
    g_assert_cmpstr(binding(&host, "SPACE")->command, ==, "set volume 17");
    press_volume(&host, "SPACE", 17);
    edit(&host, NULL, "x", "set volume 31", FALSE);
    press_volume(&host, "x", 31);
    edit(&host, "x", "Ctrl+j", "set volume 37", FALSE);
    g_assert_cmpstr(binding(&host, "x")->command, ==, "ignore");
    press_volume(&host, "Ctrl+j", 37);
    press_volume(&host, "x", 37);
    edit(&host, "UP", "UP", "ignore", FALSE);
    press_volume(&host, "UP", 37);
    edit(&host, "UP", "UP", NULL, TRUE);
    g_assert_true(binding(&host, "UP")->app_default);
    press_volume(&host, "UP", 42);

    /* Activate a named user section after the edit overlay already exists.
     * Applying a UI edit must then raise only that edited key above it. */
    const char *advanced[] = {"enable-section", "advanced", NULL};
    uint64_t advanced_reply = ++host.next_id;
    g_assert_cmpint(mpv_command_async(p->mpv, advanced_reply, advanced), >=, 0);
    await_volume(&host, 42, advanced_reply);
    press_volume(&host, "t", 79);
    press_volume(&host, "u", 81);
    edit(&host, "t", "t", "set volume 88", FALSE);
    g_assert_cmpstr(binding(&host, "t")->section, ==, KEYBINDINGS_EDIT_SECTION);
    g_assert_cmpstr(binding(&host, "t")->command, ==, "set volume 88");
    press_volume(&host, "t", 88);
    press_volume(&host, "u", 81);
    edit(&host, "t", "t", NULL, TRUE);
    g_assert_cmpstr(binding(&host, "t")->section, ==, "advanced");
    g_assert_cmpstr(binding(&host, "t")->command, ==, "set volume 79");
    press_volume(&host, "t", 79);
    press_volume(&host, "u", 81);

    /* Use the exact names produced by recording GDK keys, not hand-written
     * aliases. The host must persist and activate every modifier combination. */
    const struct { guint keyval; GdkModifierType modifiers; const char *name; double volume; } captured[] = {
        {GDK_KEY_Left, GDK_SHIFT_MASK, "Shift+LEFT", 43},
        {GDK_KEY_F, GDK_SHIFT_MASK, "F", 47},
        {GDK_KEY_Left, GDK_ALT_MASK, "Alt+LEFT", 53},
        {GDK_KEY_f, GDK_ALT_MASK, "Alt+f", 59},
        {GDK_KEY_KP_Add, GDK_SHIFT_MASK, "Shift+KP_ADD", 61},
        {GDK_KEY_KP_Add, GDK_ALT_MASK, "Alt+KP_ADD", 67},
        {GDK_KEY_F, GDK_SHIFT_MASK | GDK_ALT_MASK, "Alt+F", 71}
    };
    for (guint i = 0; i < G_N_ELEMENTS(captured); i++) {
        g_autofree char *key = input_key_name(captured[i].keyval, captured[i].modifiers);
        g_assert_cmpstr(key, ==, captured[i].name);
        g_autofree char *command_text = g_strdup_printf("set volume %.0f", captured[i].volume);
        edit(&host, NULL, key, command_text, FALSE);
        g_assert_cmpstr(binding(&host, key)->command, ==, command_text);
        g_assert_cmpstr(binding(&host, key)->section, ==, KEYBINDINGS_EDIT_SECTION);
        press_volume(&host, key, captured[i].volume);
    }
    edit(&host, "MBTN_RIGHT", "MBTN_RIGHT", "ignore", FALSE);
    check_quick_subtitles(&host, directory);

    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(input, &saved, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(saved, original));
    p->closing = TRUE;
    mpv_set_wakeup_callback(p->mpv, NULL, NULL);
    mpv_terminate_destroy(p->mpv);
    close(p->wake_fd);
    if (p->gl_library)
        dlclose(p->gl_library);
    config_clear(&p->config);
    g_clear_pointer(&host.bindings, g_ptr_array_unref);
    g_free(host.message);
    g_free(host.property_text);
    g_free(host.notice_ids[0]);
    g_free(host.notice_ids[1]);
    g_free(host.deferred_notice_sid);
    clear_track_notices(p);
    g_free(p->initial_subtitle);
    g_free(p->hwdec);
    g_free(p->hwdec_current);
    clear_playback_values(&p->playback);

    KeysHost reopened = {.player = {.wake_fd = -1, .test_seconds = 1},
                         .volume = NAN, .next_id = 50000};
    Player *again = &reopened.player;
    g_assert_true(init_mpv(again));
    await_volume(&reopened, 50, 0);
    press_volume(&reopened, "MBTN_RIGHT", 50);
    press_volume(&reopened, "UP", 55);
    assert_pause(&reopened, FALSE); /* Persisted input.conf ignore wins. */
    for (guint i = 0; i < G_N_ELEMENTS(captured); i++) {
        g_autofree char *line = g_strdup_printf("%s {default} set volume %.0f\n",
                                                captured[i].name, captured[i].volume);
        g_assert_nonnull(strstr(saved, line));
        press_volume(&reopened, captured[i].name, captured[i].volume);
    }
    again->closing = TRUE;
    mpv_set_wakeup_callback(again->mpv, NULL, NULL);
    mpv_terminate_destroy(again->mpv);
    close(again->wake_fd);
    if (again->gl_library)
        dlclose(again->gl_library);
    config_clear(&again->config);
    g_clear_pointer(&reopened.bindings, g_ptr_array_unref);
    g_free(reopened.message);
    g_free(reopened.property_text);
    g_free(again->initial_subtitle);
    g_free(again->hwdec);
    g_free(again->hwdec_current);
    clear_playback_values(&again->playback);
    g_unsetenv("MPV_GTK_CONFIG_FILE");
    g_assert_cmpint(g_remove(config), ==, 0);
    g_assert_cmpint(g_remove(input), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS key host integration: single/double mouse pause defaults/overrides, F11 fullscreen, rapid speed/reset, "
         "two-track subtitle cycle/initial reset, async editing, section priority, "
         "recorded Shift/Alt keys live and after reopening");
    return 0;
}
