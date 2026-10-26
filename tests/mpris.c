/* Exercise the public MPRIS interfaces over an isolated session bus using
 * generated media and null libmpv outputs. No desktop player is contacted. */
#define _GNU_SOURCE
#include "../src/mpris.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define ROOT_IFACE "org.mpris.MediaPlayer2"
#define PLAYER_IFACE ROOT_IFACE ".Player"
#define PROPERTIES_IFACE "org.freedesktop.DBus.Properties"

typedef struct {
    mpv_handle *mpv;
    Mpris *mpris;
    GDBusConnection *client;
    gboolean loaded, restarted, live;
    guint raised, quit, fullscreen_requests, opened;
    guint changes, metadata_changes, status_changes, position_changes, seeks;
    gint64 last_seek;
    char *opened_uri;
} Host;

typedef struct { gboolean done; GVariant *result; GError *error; } PendingCall;
typedef struct { gboolean done; char *output, *diagnostic; GError *error; } PendingProcess;

static void pump(Host *host, gint64 span)
{
    gint64 until = g_get_monotonic_time() + span;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        if (host->mpv) {
            mpv_event *event = mpv_wait_event(host->mpv, .002);
            if (event->event_id == MPV_EVENT_START_FILE)
                host->loaded = host->restarted = FALSE;
            else if (event->event_id == MPV_EVENT_FILE_LOADED) host->loaded = TRUE;
            else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) host->restarted = TRUE;
            else if (event->event_id == MPV_EVENT_END_FILE) host->loaded = FALSE;
            else if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->error < 0)
                g_error("Asynchronous mpv command failed: %s", mpv_error_string(event->error));
            if (host->mpris) mpris_handle_event(host->mpris, event);
        } else g_usleep(1000);
    } while (g_get_monotonic_time() < until);
}

static void call_finished(GObject *object, GAsyncResult *result, gpointer data)
{
    PendingCall *pending = data;
    pending->result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &pending->error);
    pending->done = TRUE;
}

static void process_finished(GObject *object, GAsyncResult *result, gpointer data)
{
    PendingProcess *pending = data;
    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object), result,
        &pending->output, &pending->diagnostic, &pending->error);
    pending->done = TRUE;
}

static char *playerctl(Host *host, const char *program, const char *command, const char *argument)
{
    const char *arguments[] = {program, "--player=mpv_gtk", command, argument, NULL};
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocess) process = g_subprocess_newv(arguments,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_assert_no_error(error);
    PendingProcess pending = {0};
    g_subprocess_communicate_utf8_async(process, NULL, NULL, process_finished, &pending);
    gint64 deadline = g_get_monotonic_time() + 4000000;
    while (!pending.done) {
        if (g_get_monotonic_time() > deadline) {
            g_subprocess_force_exit(process);
            g_error("playerctl %s timed out.", command);
        }
        pump(host, 1000);
    }
    if (pending.error || !g_subprocess_get_successful(process))
        g_error("playerctl %s failed: %s%s", command, pending.error ? pending.error->message : "",
                pending.diagnostic ? pending.diagnostic : "");
    g_free(pending.diagnostic);
    return pending.output;
}

static GVariant *call_to(Host *host, const char *name, const char *path,
                         const char *interface, const char *method, GVariant *arguments,
                         const GVariantType *reply_type, GError **error)
{
    PendingCall pending = {0};
    g_dbus_connection_call(host->client, name, path, interface, method, arguments,
        reply_type, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, call_finished, &pending);
    gint64 deadline = g_get_monotonic_time() + 4000000;
    while (!pending.done) {
        if (g_get_monotonic_time() > deadline) g_error("MPRIS call timed out: %s.%s", interface, method);
        pump(host, 1000);
    }
    if (pending.error) {
        if (error) g_propagate_error(error, pending.error);
        else g_error("MPRIS %s.%s: %s", interface, method, pending.error->message);
    }
    return pending.result;
}

static GVariant *call(Host *host, const char *interface, const char *method,
                       GVariant *arguments, const GVariantType *reply_type)
{
    return call_to(host, mpris_bus_name(host->mpris), MPRIS_PATH, interface,
                   method, arguments, reply_type, NULL);
}

static void method(Host *host, const char *interface, const char *name, GVariant *arguments)
{
    g_autoptr(GVariant) result = call(host, interface, name, arguments, G_VARIANT_TYPE_UNIT);
    g_assert_nonnull(result);
}

static GVariant *property(Host *host, const char *interface, const char *name)
{
    g_autoptr(GVariant) reply = call(host, PROPERTIES_IFACE, "Get",
        g_variant_new("(ss)", interface, name), G_VARIANT_TYPE("(v)"));
    g_autoptr(GVariant) boxed = NULL;
    g_variant_get(reply, "(@v)", &boxed);
    return g_variant_get_variant(boxed);
}

static gboolean boolean(Host *host, const char *interface, const char *name)
{
    g_autoptr(GVariant) value = property(host, interface, name);
    g_assert_true(g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN));
    return g_variant_get_boolean(value);
}

static gint64 position(Host *host)
{
    g_autoptr(GVariant) value = property(host, PLAYER_IFACE, "Position");
    g_assert_true(g_variant_is_of_type(value, G_VARIANT_TYPE_INT64));
    return g_variant_get_int64(value);
}

static void assert_string(Host *host, const char *interface, const char *name, const char *expected)
{
    g_autoptr(GVariant) value = property(host, interface, name);
    g_assert_true(g_variant_is_of_type(value, G_VARIANT_TYPE_STRING));
    g_assert_cmpstr(g_variant_get_string(value, NULL), ==, expected);
}

static void wait_status(Host *host, const char *expected)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    for (;;) {
        g_autoptr(GVariant) value = property(host, PLAYER_IFACE, "PlaybackStatus");
        if (g_strcmp0(g_variant_get_string(value, NULL), expected) == 0) return;
        if (g_get_monotonic_time() > deadline)
            g_error("Expected MPRIS status %s, got %s", expected, g_variant_get_string(value, NULL));
        pump(host, 10000);
    }
}

static void set_property(Host *host, const char *interface, const char *name, GVariant *value)
{
    method(host, PROPERTIES_IFACE, "Set", g_variant_new("(ssv)", interface, name, value));
}

static void reject_property(Host *host, const char *name, GVariant *value)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = call_to(host, mpris_bus_name(host->mpris), MPRIS_PATH,
        PROPERTIES_IFACE, "Set", g_variant_new("(ssv)", PLAYER_IFACE, name, value),
        G_VARIANT_TYPE_UNIT, &error);
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
}

static void wait_mpv_number(Host *host, const char *name, double expected, double tolerance)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    for (;;) {
        double value = NAN;
        if (mpv_get_property(host->mpv, name, MPV_FORMAT_DOUBLE, &value) >= 0 &&
            fabs(value - expected) <= tolerance) return;
        if (g_get_monotonic_time() > deadline) g_error("%s did not reach %.3f (got %.3f).", name, expected, value);
        pump(host, 10000);
    }
}

static void wait_loaded(Host *host)
{
    gint64 deadline = g_get_monotonic_time() + 4000000;
    while (!host->loaded || !host->restarted) {
        if (g_get_monotonic_time() > deadline) g_error("MPRIS fixture failed to load.");
        pump(host, 1000);
    }
    pump(host, 20000);
}

static void raise_requested(gpointer data)
{
    Host *host = data;
    g_assert_true(host->live);
    host->raised++;
}

static void quit_requested(gpointer data)
{
    Host *host = data;
    g_assert_true(host->live);
    host->quit++;
}

static void fullscreen_requested(gboolean fullscreen, gpointer data)
{
    Host *host = data;
    g_assert_true(host->live);
    host->fullscreen_requests++;
    int state = fullscreen;
    g_assert_cmpint(mpv_set_property_async(host->mpv, 0, "fullscreen", MPV_FORMAT_FLAG, &state), >=, 0);
}

static gboolean open_requested(const char *uri, gpointer data)
{
    Host *host = data;
    g_assert_true(host->live);
    host->opened++;
    g_free(host->opened_uri); host->opened_uri = g_strdup(uri);
    return TRUE;
}

static void signal_received(GDBusConnection *connection, const char *sender,
                             const char *path, const char *interface, const char *name,
                             GVariant *parameters, gpointer data)
{
    (void)connection; (void)sender; (void)path;
    Host *host = data;
    if (g_str_equal(interface, PLAYER_IFACE) && g_str_equal(name, "Seeked")) {
        host->seeks++;
        g_variant_get(parameters, "(x)", &host->last_seek);
        return;
    }
    if (!g_str_equal(interface, PROPERTIES_IFACE) || !g_str_equal(name, "PropertiesChanged")) return;
    const char *changed_interface;
    g_autoptr(GVariant) changed = NULL;
    g_auto(GStrv) invalidated = NULL;
    g_variant_get(parameters, "(&s@a{sv}^as)", &changed_interface, &changed, &invalidated);
    host->changes++;
    GVariantIter iterator;
    const char *key;
    GVariant *value;
    g_variant_iter_init(&iterator, changed);
    while (g_variant_iter_next(&iterator, "{&sv}", &key, &value)) {
        if (g_str_equal(key, "Metadata")) host->metadata_changes++;
        if (g_str_equal(key, "PlaybackStatus")) host->status_changes++;
        if (g_str_equal(key, "Position")) host->position_changes++;
        g_variant_unref(value);
    }
    for (guint i = 0; invalidated[i]; ++i)
        if (g_str_equal(invalidated[i], "Position")) host->position_changes++;
    g_assert_true(g_str_equal(changed_interface, PLAYER_IFACE) || g_str_equal(changed_interface, ROOT_IFACE));
}

static void set_option(mpv_handle *mpv, const char *name, const char *value, gboolean optional)
{
    int status = mpv_set_option_string(mpv, name, value);
    g_assert_true(status >= 0 || (optional && status == MPV_ERROR_OPTION_NOT_FOUND));
}

static mpv_handle *new_core(void)
{
    mpv_handle *mpv = mpv_create();
    g_assert_nonnull(mpv);
    const char *pairs[][2] = {{"config", "no"}, {"vo", "null"}, {"ao", "null"},
        {"idle", "yes"}, {"pause", "yes"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"input-default-bindings", "no"}, {"input-terminal", "no"}, {"hwdec", "no"}};
    for (guint i = 0; i < G_N_ELEMENTS(pairs); ++i) set_option(mpv, pairs[i][0], pairs[i][1], FALSE);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console", "load-auto-profiles",
        "load-select", "load-positioning", "load-commands", "load-context-menu"};
    for (guint i = 0; i < G_N_ELEMENTS(scripts); ++i) set_option(mpv, scripts[i], "no", TRUE);
    g_assert_cmpint(mpv_initialize(mpv), >=, 0);
    return mpv;
}

static void generate_media(const char *ffmpeg, const char *filename, const char *title)
{
    g_autofree char *title_option = g_strdup_printf("title=%s", title);
    char *arguments[] = {(char *)ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
        "testsrc2=size=160x90:rate=20:duration=12", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=8000:duration=12", "-c:v", "libx264", "-preset", "ultrafast",
        "-threads", "1", "-g", "20", "-c:a", "flac", "-metadata", title_option,
        "-metadata", "artist=Alice", "-metadata", "album=Sampler", (char *)filename, NULL};
    g_autoptr(GError) error = NULL;
    g_autofree char *diagnostic = NULL;
    int status;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, 0, NULL, NULL, NULL, &diagnostic, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("Cannot generate MPRIS fixture: %s; %s", error->message, diagnostic ? diagnostic : "");
}

static char *check_metadata(Host *host, const char *filename, const char *expected_title)
{
    g_autoptr(GVariant) metadata = NULL;
    g_autofree char *uri = g_filename_to_uri(filename, NULL, NULL);
    gint64 deadline = g_get_monotonic_time() + 3000000;
    for (;;) {
        metadata = property(host, PLAYER_IFACE, "Metadata");
        const char *current = NULL, *current_url = NULL;
        gint64 current_length = 0;
        if (g_variant_lookup(metadata, "xesam:title", "&s", &current) &&
            g_strcmp0(current, expected_title) == 0 &&
            g_variant_lookup(metadata, "xesam:url", "&s", &current_url) && g_strcmp0(current_url, uri) == 0 &&
            g_variant_lookup(metadata, "mpris:length", "x", &current_length) && current_length > 0) break;
        if (g_get_monotonic_time() > deadline) g_error("MPRIS metadata did not reach %s.", expected_title);
        g_clear_pointer(&metadata, g_variant_unref);
        pump(host, 10000);
    }
    const char *title = NULL, *url = NULL, *track = NULL, *album = NULL;
    gint64 length = 0;
    g_assert_true(g_variant_lookup(metadata, "xesam:title", "&s", &title));
    g_assert_cmpstr(title, ==, expected_title);
    g_assert_true(g_variant_lookup(metadata, "xesam:url", "&s", &url));
    g_assert_cmpstr(url, ==, uri);
    g_assert_true(g_variant_lookup(metadata, "xesam:album", "&s", &album));
    g_assert_cmpstr(album, ==, "Sampler");
    g_autoptr(GVariant) artists = g_variant_lookup_value(metadata, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY);
    g_assert_nonnull(artists);
    g_auto(GStrv) names = g_variant_dup_strv(artists, NULL);
    g_assert_cmpstr(names[0], ==, "Alice");
    g_assert_true(g_variant_lookup(metadata, "mpris:length", "x", &length));
    g_assert_cmpint(llabs(length - 12000000), <, 100000);
    g_assert_true(g_variant_lookup(metadata, "mpris:trackid", "&o", &track));
    g_assert_cmpstr(track, !=, "/org/mpris/MediaPlayer2/TrackList/NoTrack");
    g_assert_true(g_variant_is_object_path(track));
    return g_strdup(track);
}

static void assert_art(Host *host, const char *expected)
{
    g_autoptr(GVariant) metadata = property(host, PLAYER_IFACE, "Metadata");
    g_autoptr(GVariant) art = g_variant_lookup_value(metadata, "mpris:artUrl", G_VARIANT_TYPE_STRING);
    if (expected) {
        g_assert_nonnull(art);
        g_assert_cmpstr(g_variant_get_string(art, NULL), ==, expected);
    } else g_assert_null(art);
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
    setvbuf(stdout, NULL, _IOLBF, 0);
    setlocale(LC_NUMERIC, "C");
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    g_autofree char *daemon = g_find_program_in_path("dbus-daemon");
    if (!ffmpeg || !daemon) { puts("SKIP: FFmpeg or dbus-daemon is unavailable."); return 77; }
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-mpris-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *first = g_build_filename(directory, "One · Ω.mkv", NULL);
    g_autofree char *second = g_build_filename(directory, "Two.mkv", NULL);
    g_autofree char *art_file = g_build_filename(directory, "Artwork Ω.svg", NULL);
    g_assert_true(g_file_set_contents(art_file,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\"><rect width=\"32\" height=\"32\" fill=\"red\"/></svg>",
        -1, &error));
    g_assert_no_error(error);
    g_autofree char *art_uri = g_filename_to_uri(art_file, NULL, &error);
    g_assert_no_error(error);
    generate_media(ffmpeg, first, "First · Ω");
    generate_media(ffmpeg, second, "Second");
    g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    g_setenv("XDG_RUNTIME_DIR", directory, TRUE);
    Host host = {.mpv = new_core(), .live = TRUE};
    host.client = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(host.client);
    g_dbus_connection_set_exit_on_close(host.client, FALSE);
    MprisHost callbacks = {.raise = raise_requested, .quit = quit_requested,
        .set_fullscreen = fullscreen_requested, .open_uri = open_requested, .data = &host};
    host.mpris = mpris_new(host.mpv, &callbacks);
    g_assert_nonnull(host.mpris);
    const char *name = mpris_bus_name(host.mpris);
    g_assert_true(g_str_has_prefix(name, "org.mpris.MediaPlayer2.mpv_gtk.instance"));
    gint64 deadline = g_get_monotonic_time() + 3000000;
    for (;;) {
        g_autoptr(GVariant) reply = call_to(&host, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"), NULL);
        gboolean owned = FALSE;
        g_variant_get(reply, "(b)", &owned);
        if (owned) break;
        if (g_get_monotonic_time() > deadline) g_error("MPRIS bus name was not acquired.");
        pump(&host, 10000);
    }
    guint properties_subscription = g_dbus_connection_signal_subscribe(host.client, name,
        PROPERTIES_IFACE, "PropertiesChanged", MPRIS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_received, &host, NULL);
    guint seek_subscription = g_dbus_connection_signal_subscribe(host.client, name,
        PLAYER_IFACE, "Seeked", MPRIS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_received, &host, NULL);
    g_autoptr(GVariant) introspection = call(&host, "org.freedesktop.DBus.Introspectable", "Introspect",
        NULL, G_VARIANT_TYPE("(s)"));
    const char *xml;
    g_variant_get(introspection, "(&s)", &xml);
    g_assert_nonnull(strstr(xml, ROOT_IFACE));
    g_assert_nonnull(strstr(xml, PLAYER_IFACE));
    assert_string(&host, ROOT_IFACE, "Identity", "mpv GTK");
    assert_string(&host, ROOT_IFACE, "DesktopEntry", "io.github.emless.mpvgtk");
    g_assert_true(boolean(&host, ROOT_IFACE, "CanRaise"));
    g_assert_true(boolean(&host, ROOT_IFACE, "CanQuit"));
    g_assert_false(boolean(&host, ROOT_IFACE, "HasTrackList"));
    wait_status(&host, "Stopped");
    g_assert_false(boolean(&host, PLAYER_IFACE, "CanSeek"));
    g_assert_false(boolean(&host, PLAYER_IFACE, "CanPlay"));
    method(&host, PLAYER_IFACE, "Seek", g_variant_new("(x)", (gint64)1000000));
    g_assert_cmpuint(host.seeks, ==, 0);
    method(&host, ROOT_IFACE, "Raise", NULL);
    method(&host, ROOT_IFACE, "Quit", NULL);
    g_assert_cmpuint(host.raised, ==, 1);
    g_assert_cmpuint(host.quit, ==, 1);
    set_property(&host, ROOT_IFACE, "Fullscreen", g_variant_new_boolean(TRUE));
    pump(&host, 20000);
    g_assert_cmpuint(host.fullscreen_requests, ==, 1);
    g_assert_true(boolean(&host, ROOT_IFACE, "Fullscreen"));
    g_autofree char *first_uri = g_filename_to_uri(first, NULL, NULL);
    method(&host, PLAYER_IFACE, "OpenUri", g_variant_new("(s)", first_uri));
    g_assert_cmpuint(host.opened, ==, 1);
    g_assert_cmpstr(host.opened_uri, ==, first_uri);
    const char *load[] = {"loadfile", first, "replace", NULL};
    const char *append[] = {"loadfile", second, "append", NULL};
    g_assert_cmpint(mpv_command_async(host.mpv, 0, load), >=, 0);
    wait_loaded(&host);
    g_assert_cmpint(mpv_command_async(host.mpv, 0, append), >=, 0);
    pump(&host, 30000);
    wait_status(&host, "Paused");
    g_assert_true(boolean(&host, PLAYER_IFACE, "CanPlay"));
    g_assert_true(boolean(&host, PLAYER_IFACE, "CanPause"));
    g_assert_true(boolean(&host, PLAYER_IFACE, "CanSeek"));
    g_assert_true(boolean(&host, PLAYER_IFACE, "CanGoNext"));
    g_assert_false(boolean(&host, PLAYER_IFACE, "CanGoPrevious"));
    g_autofree char *first_track = check_metadata(&host, first, "First · Ω");
    pump(&host, 30000);
    g_autofree char *stable_track = check_metadata(&host, first, "First · Ω");
    g_assert_cmpstr(first_track, ==, stable_track);
    pump(&host, 30000);
    guint before_art = host.metadata_changes;
    g_autofree char *borrowed_uri = g_strdup(art_uri);
    mpris_set_art_url(host.mpris, borrowed_uri);
    memset(borrowed_uri, 'x', strlen(borrowed_uri));
    assert_art(&host, art_uri);
    pump(&host, 30000);
    g_assert_cmpuint(host.metadata_changes, >, before_art);
    before_art = host.metadata_changes;
    mpris_set_art_url(host.mpris, art_uri);
    pump(&host, 30000);
    g_assert_cmpuint(host.metadata_changes, ==, before_art);
    mpris_set_art_url(host.mpris, "relative/artwork.png");
    assert_art(&host, art_uri);
    mpris_set_art_url(host.mpris, NULL);
    assert_art(&host, NULL);
    mpris_set_art_url(host.mpris, art_uri);
    assert_art(&host, art_uri);
    const char *playerctl_program = g_getenv("MPV_GTK_TEST_PLAYERCTL");
    if (playerctl_program && *playerctl_program) {
        g_autofree char *status = playerctl(&host, playerctl_program, "status", NULL);
        g_assert_cmpstr(g_strstrip(status), ==, "Paused");
        g_autofree char *title = playerctl(&host, playerctl_program, "metadata", "xesam:title");
        g_assert_cmpstr(g_strstrip(title), ==, "First · Ω");
        g_autofree char *artwork = playerctl(&host, playerctl_program, "metadata", "mpris:artUrl");
        g_assert_cmpstr(g_strstrip(artwork), ==, art_uri);
        g_autofree char *toggle = playerctl(&host, playerctl_program, "play-pause", NULL);
        wait_status(&host, "Playing");
        method(&host, PLAYER_IFACE, "Pause", NULL);
        wait_status(&host, "Paused");
        puts("PASS actual playerctl: base-name selection, status, metadata/artwork and play-pause on the private bus.");
    }
    method(&host, PLAYER_IFACE, "Play", NULL);
    wait_status(&host, "Playing");
    gint64 before_progress = position(&host);
    guint before_seeks = host.seeks;
    pump(&host, 180000);
    g_assert_cmpint(position(&host), >, before_progress + 50000);
    g_assert_cmpuint(host.position_changes, ==, 0);
    g_assert_cmpuint(host.seeks, ==, before_seeks);
    method(&host, PLAYER_IFACE, "Pause", NULL);
    wait_status(&host, "Paused");
    method(&host, PLAYER_IFACE, "PlayPause", NULL);
    wait_status(&host, "Playing");
    method(&host, PLAYER_IFACE, "PlayPause", NULL);
    wait_status(&host, "Paused");
    set_property(&host, PLAYER_IFACE, "Volume", g_variant_new_double(.35));
    wait_mpv_number(&host, "volume", 35, .001);
    set_property(&host, PLAYER_IFACE, "Volume", g_variant_new_double(-1));
    wait_mpv_number(&host, "volume", 0, .001);
    set_property(&host, PLAYER_IFACE, "Volume", g_variant_new_double(.35));
    wait_mpv_number(&host, "volume", 35, .001);
    set_property(&host, PLAYER_IFACE, "Rate", g_variant_new_double(1.25));
    wait_mpv_number(&host, "speed", 1.25, .001);
    set_property(&host, PLAYER_IFACE, "Rate", g_variant_new_double(1));
    set_property(&host, PLAYER_IFACE, "LoopStatus", g_variant_new_string("Track"));
    pump(&host, 30000);
    assert_string(&host, PLAYER_IFACE, "LoopStatus", "Track");
    set_property(&host, PLAYER_IFACE, "LoopStatus", g_variant_new_string("Playlist"));
    pump(&host, 30000);
    assert_string(&host, PLAYER_IFACE, "LoopStatus", "Playlist");
    set_property(&host, PLAYER_IFACE, "LoopStatus", g_variant_new_string("None"));
    pump(&host, 30000);
    assert_string(&host, PLAYER_IFACE, "LoopStatus", "None");
    method(&host, PLAYER_IFACE, "Play", NULL);
    wait_status(&host, "Playing");
    set_property(&host, PLAYER_IFACE, "Rate", g_variant_new_double(0));
    wait_status(&host, "Paused");
    reject_property(&host, "Rate", g_variant_new_double(NAN));
    reject_property(&host, "Rate", g_variant_new_string("invalid"));
    reject_property(&host, "LoopStatus", g_variant_new_string("invalid"));
    reject_property(&host, "Position", g_variant_new_int64(0));
    g_autofree char *before_seek_track = check_metadata(&host, first, "First · Ω");
    g_assert_cmpstr(before_seek_track, ==, first_track);
    g_assert_true(boolean(&host, PLAYER_IFACE, "CanSeek"));
    method(&host, PLAYER_IFACE, "SetPosition", g_variant_new("(ox)", first_track, (gint64)1000000));
    wait_mpv_number(&host, "time-pos", 1, .08);
    pump(&host, 30000);
    g_assert_cmpint(llabs(position(&host) - 1000000), <, 100000);
    before_seeks = host.seeks;
    method(&host, PLAYER_IFACE, "Seek", g_variant_new("(x)", (gint64)700000));
    wait_mpv_number(&host, "time-pos", 1.7, .08);
    pump(&host, 30000);
    g_assert_cmpuint(host.seeks, >, before_seeks);
    g_assert_cmpint(llabs(host.last_seek - 1700000), <, 100000);
    /* Overlapping targets must eventually announce the final actual position,
     * even when an earlier seek's asynchronous sample is still in flight. */
    before_seeks = host.seeks;
    method(&host, PLAYER_IFACE, "SetPosition", g_variant_new("(ox)", first_track, (gint64)3000000));
    method(&host, PLAYER_IFACE, "SetPosition", g_variant_new("(ox)", first_track, (gint64)1700000));
    wait_mpv_number(&host, "time-pos", 1.7, .08);
    deadline = g_get_monotonic_time() + 3000000;
    while (host.seeks <= before_seeks || llabs(host.last_seek - 1700000) >= 100000) {
        if (g_get_monotonic_time() > deadline) g_error("The final overlapping seek was not announced.");
        pump(&host, 10000);
    }
    before_seeks = host.seeks;
    const gint64 ignored[] = {-1, 14000000};
    for (guint i = 0; i < G_N_ELEMENTS(ignored); ++i)
        method(&host, PLAYER_IFACE, "SetPosition", g_variant_new("(ox)", first_track, ignored[i]));
    method(&host, PLAYER_IFACE, "SetPosition", g_variant_new("(ox)", "/stale/track", (gint64)4000000));
    pump(&host, 100000);
    g_assert_cmpint(llabs(position(&host) - 1700000), <, 100000);
    g_assert_cmpuint(host.seeks, ==, before_seeks);
    host.loaded = host.restarted = FALSE;
    method(&host, PLAYER_IFACE, "Seek", g_variant_new("(x)", (gint64)12000000));
    wait_loaded(&host);
    wait_status(&host, "Paused");
    g_autofree char *second_track = check_metadata(&host, second, "Second");
    assert_art(&host, NULL);
    g_assert_cmpstr(second_track, !=, first_track);
    g_assert_true(boolean(&host, PLAYER_IFACE, "CanGoPrevious"));
    g_assert_false(boolean(&host, PLAYER_IFACE, "CanGoNext"));
    method(&host, PLAYER_IFACE, "SetPosition", g_variant_new("(ox)", first_track, (gint64)4000000));
    pump(&host, 50000);
    g_assert_cmpint(position(&host), <, 150000);
    host.loaded = host.restarted = FALSE;
    method(&host, PLAYER_IFACE, "Previous", NULL);
    wait_loaded(&host);
    g_autofree char *returned_track = check_metadata(&host, first, "First · Ω");
    g_assert_cmpstr(returned_track, !=, second_track);
    host.loaded = host.restarted = FALSE;
    method(&host, PLAYER_IFACE, "Next", NULL);
    wait_loaded(&host);
    g_autofree char *before_stop_track = check_metadata(&host, second, "Second");
    method(&host, PLAYER_IFACE, "Stop", NULL);
    wait_status(&host, "Stopped");
    method(&host, PLAYER_IFACE, "Play", NULL);
    wait_status(&host, "Playing");
    g_assert_cmpint(position(&host), <, 500000);
    g_autofree char *after_stop_track = check_metadata(&host, second, "Second");
    g_assert_cmpstr(after_stop_track, ==, before_stop_track);
    method(&host, PLAYER_IFACE, "Pause", NULL);
    wait_status(&host, "Paused");
    g_assert_cmpuint(host.metadata_changes, >, 0);
    g_assert_cmpuint(host.status_changes, >, 0);
    g_assert_cmpuint(host.position_changes, ==, 0);
    g_dbus_connection_signal_unsubscribe(host.client, properties_subscription);
    g_dbus_connection_signal_unsubscribe(host.client, seek_subscription);
    g_autofree char *old_name = g_strdup(mpris_bus_name(host.mpris));
    mpris_free(host.mpris); host.mpris = NULL; host.live = FALSE;
    pump(&host, 100000);
    g_autoptr(GVariant) owner = call_to(&host, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", old_name), G_VARIANT_TYPE("(b)"), NULL);
    gboolean still_owned = TRUE;
    g_variant_get(owner, "(b)", &still_owned);
    g_assert_false(still_owned);
    g_assert_true(g_dbus_connection_close_sync(host.client, NULL, &error));
    g_assert_no_error(error);
    g_clear_object(&host.client);
    mpv_terminate_destroy(host.mpv); host.mpv = NULL;
    pump(&host, 20000);
    /* Free immediately while asynchronous bus setup is still pending. */
    host.mpv = new_core(); host.live = TRUE;
    host.mpris = mpris_new(host.mpv, &callbacks);
    mpris_free(host.mpris); host.mpris = NULL; host.live = FALSE;
    mpv_terminate_destroy(host.mpv); host.mpv = NULL;
    pump(&host, 100000);
    g_test_dbus_down(bus);
    g_setenv("XDG_RUNTIME_DIR", directory, TRUE);
    /* A missing session bus is optional desktop infrastructure; neither a
     * failed connection nor freeing its pending setup may touch freed hosts. */
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/mpv-gtk-mpris-test-bus", TRUE);
    host.mpv = new_core(); host.live = TRUE;
    host.mpris = mpris_new(host.mpv, &callbacks);
    pump(&host, 100000);
    mpris_free(host.mpris); host.mpris = NULL; host.live = FALSE;
    mpv_terminate_destroy(host.mpv); host.mpv = NULL;
    pump(&host, 100000);
    g_free(host.opened_uri);
    remove_tree(directory);
    puts("PASS MPRIS: private-bus methods, metadata, signals, seeking, properties, playlist controls and async teardown.");
    return 0;
}
