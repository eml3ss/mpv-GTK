#include "mpris.h"

#include <gio/gio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define OBJECT_PATH "/org/mpris/MediaPlayer2"
#define ROOT_INTERFACE "org.mpris.MediaPlayer2"
#define PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define NO_TRACK "/org/mpris/MediaPlayer2/TrackList/NoTrack"
#define OBSERVE_BASE (UINT64_C(1) << 52)
#define REQUEST_BASE (OBSERVE_BASE + (UINT64_C(1) << 32))
#define COMMAND_BASE (OBSERVE_BASE + (UINT64_C(1) << 33))
#define REQUEST_LIMIT (OBSERVE_BASE + (UINT64_C(1) << 34))
#define MINIMUM_RATE .01
#define MAXIMUM_RATE 100.0

typedef struct { const char *name; mpv_format format; } Observation;
typedef struct { guint index; guint64 generation; } PropertySample;
static const Observation observations[] = {
    {"idle-active", MPV_FORMAT_FLAG}, {"pause", MPV_FORMAT_FLAG},
    {"paused-for-cache", MPV_FORMAT_FLAG}, {"seekable", MPV_FORMAT_FLAG},
    {"seeking", MPV_FORMAT_FLAG}, {"fullscreen", MPV_FORMAT_FLAG},
    {"mute", MPV_FORMAT_FLAG}, {"time-pos", MPV_FORMAT_DOUBLE},
    {"duration", MPV_FORMAT_DOUBLE}, {"speed", MPV_FORMAT_DOUBLE},
    {"volume", MPV_FORMAT_DOUBLE}, {"volume-max", MPV_FORMAT_DOUBLE},
    {"media-title", MPV_FORMAT_STRING}, {"path", MPV_FORMAT_STRING},
    {"working-directory", MPV_FORMAT_STRING}, {"metadata", MPV_FORMAT_NODE},
    {"playlist", MPV_FORMAT_NODE}, {"playlist-pos", MPV_FORMAT_INT64},
    {"playlist-count", MPV_FORMAT_INT64}, {"loop-file", MPV_FORMAT_STRING},
    {"loop-playlist", MPV_FORMAT_STRING}, {"eof-reached", MPV_FORMAT_FLAG}
};

static const char *root_properties[] = {"CanQuit", "Fullscreen", "CanSetFullscreen", "CanRaise",
    "HasTrackList", "Identity", "DesktopEntry", "SupportedUriSchemes", "SupportedMimeTypes"};
static const char *player_properties[] = {"PlaybackStatus", "LoopStatus", "Rate", "Metadata",
    "Volume", "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek"};
static const char *uri_schemes[] = {"file", "http", "https", "ftp", "ftps", "rtsp", "rtmp", "rtmps", NULL};
static const char *mime_types[] = {"video/mp4", "video/x-matroska", "video/webm", "video/ogg",
    "video/mpeg", "video/quicktime", "video/x-msvideo", "video/mp2t", "audio/mpeg", "audio/mp4",
    "audio/aac", "audio/flac", "audio/ogg", "audio/opus", "audio/x-wav", "audio/x-matroska",
    "application/ogg", "application/vnd.apple.mpegurl", "application/x-mpegurl", NULL};

static const char introspection_xml[] =
    "<node>"
    "<interface name='org.mpris.MediaPlayer2'>"
    "<method name='Raise'/><method name='Quit'/>"
    "<property name='CanQuit' type='b' access='read'/>"
    "<property name='Fullscreen' type='b' access='readwrite'/>"
    "<property name='CanSetFullscreen' type='b' access='read'/>"
    "<property name='CanRaise' type='b' access='read'/>"
    "<property name='HasTrackList' type='b' access='read'/>"
    "<property name='Identity' type='s' access='read'/>"
    "<property name='DesktopEntry' type='s' access='read'/>"
    "<property name='SupportedUriSchemes' type='as' access='read'/>"
    "<property name='SupportedMimeTypes' type='as' access='read'/>"
    "</interface>"
    "<interface name='org.mpris.MediaPlayer2.Player'>"
    "<method name='Next'/><method name='Previous'/><method name='Pause'/>"
    "<method name='PlayPause'/><method name='Stop'/><method name='Play'/>"
    "<method name='Seek'><arg name='Offset' type='x' direction='in'/></method>"
    "<method name='SetPosition'><arg name='TrackId' type='o' direction='in'/>"
    "<arg name='Position' type='x' direction='in'/></method>"
    "<method name='OpenUri'><arg name='Uri' type='s' direction='in'/></method>"
    "<signal name='Seeked'><arg name='Position' type='x'/></signal>"
    "<property name='PlaybackStatus' type='s' access='read'/>"
    "<property name='LoopStatus' type='s' access='readwrite'/>"
    "<property name='Rate' type='d' access='readwrite'/>"
    "<property name='Metadata' type='a{sv}' access='read'/>"
    "<property name='Volume' type='d' access='readwrite'/>"
    "<property name='Position' type='x' access='read'>"
    "<annotation name='org.freedesktop.DBus.Property.EmitsChangedSignal' value='false'/>"
    "</property>"
    "<property name='MinimumRate' type='d' access='read'/>"
    "<property name='MaximumRate' type='d' access='read'/>"
    "<property name='CanGoNext' type='b' access='read'/>"
    "<property name='CanGoPrevious' type='b' access='read'/>"
    "<property name='CanPlay' type='b' access='read'/>"
    "<property name='CanPause' type='b' access='read'/>"
    "<property name='CanSeek' type='b' access='read'/>"
    "<property name='CanControl' type='b' access='read'>"
    "<annotation name='org.freedesktop.DBus.Property.EmitsChangedSignal' value='false'/>"
    "</property>"
    "</interface></node>";

struct Mpris {
    gint references;
    mpv_handle *mpv;
    MprisHost host;
    GCancellable *cancel;
    GDBusConnection *connection;
    GDBusNodeInfo *introspection;
    GHashTable *sent[2], *samples;
    guint registrations[2], owner, emit_source;
    gboolean stopped, owned, loaded, loading, idle, paused, buffering, seekable, seeking;
    gboolean fullscreen, muted, eof, seek_pending, restart_at_zero;
    guint64 generation, seek_generation, serial, position_request;
    gint64 entry_id, resume_id, restart_entry_id, playlist_position, playlist_count, sampled_at;
    double position, duration, rate, volume, volume_max;
    char *bus_name, *title, *path, *working_directory, *loop_file, *loop_playlist, *track_id, *art_url;
    GArray *playlist_ids;
    GVariant *tags, *metadata;
};

static void schedule_publish(Mpris *m);
static void rebuild_metadata(Mpris *m);

static Mpris *mpris_ref(Mpris *m) { g_atomic_int_inc(&m->references); return m; }

static void mpris_unref(gpointer data)
{
    Mpris *m = data;
    if (!g_atomic_int_dec_and_test(&m->references)) return;
    g_clear_object(&m->cancel);
    g_clear_object(&m->connection);
    g_clear_pointer(&m->introspection, g_dbus_node_info_unref);
    for (int i = 0; i < 2; ++i) g_hash_table_unref(m->sent[i]);
    g_hash_table_unref(m->samples);
    g_array_unref(m->playlist_ids);
    g_clear_pointer(&m->tags, g_variant_unref);
    g_clear_pointer(&m->metadata, g_variant_unref);
    g_free(m->bus_name); g_free(m->title); g_free(m->path); g_free(m->working_directory);
    g_free(m->loop_file); g_free(m->loop_playlist); g_free(m->track_id);
    g_free(m->art_url);
    g_free(m);
}

static const mpv_node *field(const mpv_node *node, const char *name)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list) return NULL;
    for (int i = 0; i < node->u.list->num; ++i)
        if (g_strcmp0(node->u.list->keys[i], name) == 0) return &node->u.list->values[i];
    return NULL;
}

static gboolean repeat_enabled(const char *text)
{
    return text && *text && !g_str_equal(text, "no") && !g_str_equal(text, "0") && !g_str_equal(text, "1");
}

static gboolean file_repeat(const Mpris *m)
{
    return m->loop_file && *m->loop_file && !g_str_equal(m->loop_file, "no") && !g_str_equal(m->loop_file, "0");
}

static const char *loop_status(const Mpris *m)
{
    return file_repeat(m) ? "Track" : repeat_enabled(m->loop_playlist) ? "Playlist" : "None";
}

static gboolean can_navigate(const Mpris *m, gboolean next)
{
    return m->loaded && !m->idle && m->playlist_count > 1 && m->playlist_position >= 0 &&
        (repeat_enabled(m->loop_playlist) || (next ? m->playlist_position < m->playlist_count - 1
                                                   : m->playlist_position > 0));
}

static gint64 position_us(const Mpris *m)
{
    double value = m->loaded ? m->position : 0;
    if (m->loaded && !m->idle && !m->paused && !m->buffering && !m->seeking && !m->eof && m->sampled_at > 0)
        value += MAX(0, g_get_monotonic_time() - m->sampled_at) / 1000000.0 * m->rate;
    if (m->duration > 0) value = MIN(value, m->duration);
    if (!isfinite(value) || value < 0) return 0;
    return (gint64)MIN(value * 1000000.0, (double)G_MAXINT64 - 1024);
}

static const char *tag(Mpris *m, const char *key)
{
    if (!m->tags) return "";
    GVariantIter iter;
    const char *name, *value;
    g_variant_iter_init(&iter, m->tags);
    while (g_variant_iter_next(&iter, "{&s&s}", &name, &value))
        if (g_ascii_strcasecmp(name, key) == 0) return value;
    return "";
}

static char *media_uri(Mpris *m)
{
    if (!m->path || !*m->path) return NULL;
    g_autofree char *scheme = g_uri_parse_scheme(m->path);
    if (scheme) {
        if (g_str_equal(scheme, "file")) {
            g_autofree char *filename = g_filename_from_uri(m->path, NULL, NULL);
            if (filename) {
                g_autofree char *canonical = g_canonicalize_filename(filename, NULL);
                return g_filename_to_uri(canonical, NULL, NULL);
            }
        }
        return g_utf8_make_valid(m->path, -1);
    }
    g_autofree char *filename = g_canonicalize_filename(m->path,
        m->working_directory && g_path_is_absolute(m->working_directory) ? m->working_directory : NULL);
    return g_filename_to_uri(filename, NULL, NULL);
}

static void add_string(GVariantBuilder *builder, const char *key, const char *value)
{
    if (value && *value) {
        g_autofree char *valid = g_utf8_make_valid(value, -1);
        g_variant_builder_add(builder, "{sv}", key, g_variant_new_string(valid));
    }
}

static void add_string_array(GVariantBuilder *builder, const char *key, const char *value)
{
    if (value && *value) {
        g_autofree char *valid = g_utf8_make_valid(value, -1);
        const char *items[] = {valid, NULL};
        g_variant_builder_add(builder, "{sv}", key, g_variant_new_strv(items, -1));
    }
}

static void rebuild_metadata(Mpris *m)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_free(m->track_id);
    m->track_id = m->entry_id >= 0
        ? g_strdup_printf("/io/github/emless/mpvgtk/track/entry_%" G_GINT64_FORMAT, m->entry_id)
        : g_strdup_printf("/io/github/emless/mpvgtk/track/file_%" G_GUINT64_FORMAT, m->generation);
    if (m->loaded && !m->idle) {
        g_variant_builder_add(&builder, "{sv}", "mpris:trackid", g_variant_new_object_path(m->track_id));
        if (m->duration > 0 && isfinite(m->duration))
            g_variant_builder_add(&builder, "{sv}", "mpris:length",
                g_variant_new_int64((gint64)MIN(m->duration * 1000000.0, (double)G_MAXINT64 - 1024)));
        const char *title = tag(m, "title");
        add_string(&builder, "xesam:title", *title ? title : m->title);
        add_string_array(&builder, "xesam:artist", tag(m, "artist"));
        const char *album_artist = tag(m, "album_artist");
        if (!*album_artist) album_artist = tag(m, "album artist");
        add_string_array(&builder, "xesam:albumArtist", album_artist);
        add_string(&builder, "xesam:album", tag(m, "album"));
        add_string_array(&builder, "xesam:genre", tag(m, "genre"));
        add_string_array(&builder, "xesam:composer", tag(m, "composer"));
        add_string(&builder, "xesam:contentCreated", tag(m, "date"));
        const char *track = tag(m, "track");
        char *end = NULL;
        gint64 number = g_ascii_strtoll(track, &end, 10);
        if (end != track && number > 0 && number <= G_MAXINT32)
            g_variant_builder_add(&builder, "{sv}", "xesam:trackNumber", g_variant_new_int32((gint32)number));
        g_autofree char *uri = media_uri(m);
        add_string(&builder, "xesam:url", uri);
        add_string(&builder, "mpris:artUrl", m->art_url);
    }
    GVariant *metadata = g_variant_ref_sink(g_variant_builder_end(&builder));
    g_clear_pointer(&m->metadata, g_variant_unref);
    m->metadata = metadata;
    schedule_publish(m);
}

static GVariant *property_value(Mpris *m, const char *interface, const char *name)
{
    if (g_str_equal(interface, ROOT_INTERFACE)) {
        if (g_str_equal(name, "CanQuit")) return g_variant_new_boolean(m->host.quit != NULL);
        if (g_str_equal(name, "Fullscreen")) return g_variant_new_boolean(m->fullscreen);
        if (g_str_equal(name, "CanSetFullscreen")) return g_variant_new_boolean(m->host.set_fullscreen != NULL);
        if (g_str_equal(name, "CanRaise")) return g_variant_new_boolean(m->host.raise != NULL);
        if (g_str_equal(name, "HasTrackList")) return g_variant_new_boolean(FALSE);
        if (g_str_equal(name, "Identity")) return g_variant_new_string("mpv GTK");
        if (g_str_equal(name, "DesktopEntry")) return g_variant_new_string("io.github.emless.mpvgtk");
        if (g_str_equal(name, "SupportedUriSchemes")) return g_variant_new_strv(uri_schemes, -1);
        if (g_str_equal(name, "SupportedMimeTypes")) return g_variant_new_strv(mime_types, -1);
    } else if (g_str_equal(interface, PLAYER_INTERFACE)) {
        if (g_str_equal(name, "PlaybackStatus")) return g_variant_new_string(!m->loaded || m->idle || m->eof
            ? "Stopped" : m->paused || m->buffering ? "Paused" : "Playing");
        if (g_str_equal(name, "LoopStatus")) return g_variant_new_string(loop_status(m));
        if (g_str_equal(name, "Rate")) return g_variant_new_double(m->rate);
        if (g_str_equal(name, "Metadata")) return g_variant_ref(m->metadata);
        if (g_str_equal(name, "Volume")) return g_variant_new_double(m->muted ? 0 : m->volume / 100.0);
        if (g_str_equal(name, "Position")) return g_variant_new_int64(position_us(m));
        if (g_str_equal(name, "MinimumRate")) return g_variant_new_double(MINIMUM_RATE);
        if (g_str_equal(name, "MaximumRate")) return g_variant_new_double(MAXIMUM_RATE);
        if (g_str_equal(name, "CanGoNext")) return g_variant_new_boolean(can_navigate(m, TRUE));
        if (g_str_equal(name, "CanGoPrevious")) return g_variant_new_boolean(can_navigate(m, FALSE));
        if (g_str_equal(name, "CanPlay")) return g_variant_new_boolean(m->loaded || m->playlist_count > 0);
        if (g_str_equal(name, "CanPause")) return g_variant_new_boolean(m->loaded || m->playlist_count > 0);
        if (g_str_equal(name, "CanSeek")) return g_variant_new_boolean(m->loaded && !m->idle && m->seekable);
        if (g_str_equal(name, "CanControl")) return g_variant_new_boolean(TRUE);
    }
    return NULL;
}

static void publish_interface(Mpris *m, int index)
{
    const char *interface = index ? PLAYER_INTERFACE : ROOT_INTERFACE;
    const char *const *names = index ? player_properties : root_properties;
    gsize count = index ? G_N_ELEMENTS(player_properties) : G_N_ELEMENTS(root_properties);
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
    guint updates = 0;
    for (gsize i = 0; i < count; ++i) {
        GVariant *value = g_variant_take_ref(property_value(m, interface, names[i]));
        GVariant *old = g_hash_table_lookup(m->sent[index], names[i]);
        if (!old || !g_variant_equal(old, value)) {
            g_variant_builder_add(&changed, "{sv}", names[i], value);
            g_hash_table_replace(m->sent[index], g_strdup(names[i]), g_variant_ref(value));
            ++updates;
        }
        g_variant_unref(value);
    }
    if (updates) g_dbus_connection_emit_signal(m->connection, NULL, OBJECT_PATH,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(s@a{sv}@as)", interface, g_variant_builder_end(&changed),
                      g_variant_new_strv(NULL, 0)), NULL);
    else g_variant_builder_clear(&changed);
}

static gboolean publish_idle(gpointer data)
{
    Mpris *m = data;
    m->emit_source = 0;
    if (!m->stopped && m->owned && m->connection) {
        publish_interface(m, 0);
        publish_interface(m, 1);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_publish(Mpris *m)
{
    if (!m->stopped && !m->emit_source)
        m->emit_source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, publish_idle, mpris_ref(m), mpris_unref);
}

static gboolean command(Mpris *m, const char *const *arguments, GError **error)
{
    int status = mpv_command_async(m->mpv, COMMAND_BASE + ++m->serial, (const char **)arguments);
    if (status >= 0) return TRUE;
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "mpv: %s", mpv_error_string(status));
    return FALSE;
}

static gboolean set_number(Mpris *m, const char *name, double value, GError **error)
{
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(text, sizeof(text), value);
    const char *args[] = {"set", name, text, NULL};
    return command(m, args, error);
}

static gboolean set_pause(Mpris *m, gboolean pause, GError **error)
{
    const char *args[] = {"set", "pause", pause ? "yes" : "no", NULL};
    return command(m, args, error);
}

static gboolean seek_absolute(Mpris *m, double seconds, GError **error)
{
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(text, sizeof(text), MAX(0, seconds));
    const char *args[] = {"seek", text, "absolute+exact", NULL};
    return command(m, args, error);
}

static gint64 entry_index(Mpris *m, gint64 id)
{
    for (guint i = 0; id >= 0 && i < m->playlist_ids->len; ++i)
        if (g_array_index(m->playlist_ids, gint64, i) == id) return i;
    return -1;
}

static gboolean play(Mpris *m, GError **error)
{
    if (m->loaded && !m->idle) {
        if (m->eof && m->seekable && !seek_absolute(m, 0, error)) return FALSE;
        return set_pause(m, FALSE, error);
    }
    if (m->playlist_count <= 0) return TRUE;
    gint64 index = entry_index(m, m->resume_id);
    if (index < 0) index = m->playlist_position >= 0 && m->playlist_position < m->playlist_count
        ? m->playlist_position : 0;
    g_autofree char *text = g_strdup_printf("%" G_GINT64_FORMAT, index);
    const char *args[] = {"playlist-play-index", text, NULL};
    if (!set_pause(m, FALSE, error) || !command(m, args, error)) return FALSE;
    m->restart_at_zero = TRUE;
    m->restart_entry_id = index < (gint64)m->playlist_ids->len
        ? g_array_index(m->playlist_ids, gint64, index) : -1;
    return TRUE;
}

static gboolean navigate(Mpris *m, gboolean next, GError **error)
{
    if (!can_navigate(m, next)) return TRUE;
    gint64 index = next ? m->playlist_position + 1 : m->playlist_position - 1;
    if (index >= m->playlist_count) index = 0;
    if (index < 0) index = m->playlist_count - 1;
    g_autofree char *text = g_strdup_printf("%" G_GINT64_FORMAT, index);
    const char *args[] = {"playlist-play-index", text, NULL};
    return command(m, args, error);
}

static void method_call(GDBusConnection *connection, const char *sender, const char *path,
                          const char *interface, const char *method, GVariant *parameters,
                          GDBusMethodInvocation *invocation, gpointer data)
{
    (void)connection; (void)sender; (void)path;
    Mpris *m = mpris_ref(data);
    g_autoptr(GError) error = NULL;
    if (m->stopped) g_set_error_literal(&error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "The player is closing.");
    else if (g_str_equal(interface, ROOT_INTERFACE)) {
        if (g_str_equal(method, "Raise")) { if (m->host.raise) m->host.raise(m->host.data); }
        else if (g_str_equal(method, "Quit")) { if (m->host.quit) m->host.quit(m->host.data); }
    } else if (g_str_equal(method, "Play")) play(m, &error);
    else if (g_str_equal(method, "Pause")) { if (m->loaded && !m->idle) set_pause(m, TRUE, &error); }
    else if (g_str_equal(method, "PlayPause")) {
        if (!m->loaded && m->playlist_count <= 0)
            g_set_error_literal(&error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "There is no track to play.");
        else if (!m->loaded || m->idle || m->eof) play(m, &error);
        else set_pause(m, !m->paused, &error);
    } else if (g_str_equal(method, "Stop")) {
        if (m->loaded && !m->idle) {
            m->resume_id = m->entry_id;
            const char *args[] = {"stop", "keep-playlist", NULL};
            command(m, args, &error);
        }
    } else if (g_str_equal(method, "Next")) navigate(m, TRUE, &error);
    else if (g_str_equal(method, "Previous")) navigate(m, FALSE, &error);
    else if (g_str_equal(method, "Seek")) {
        gint64 offset;
        g_variant_get(parameters, "(x)", &offset);
        if (m->loaded && !m->idle && m->seekable) {
            double target = position_us(m) / 1000000.0 + offset / 1000000.0;
            if (m->duration > 0 && target > m->duration) navigate(m, TRUE, &error);
            else seek_absolute(m, target, &error);
        }
    } else if (g_str_equal(method, "SetPosition")) {
        const char *track;
        gint64 position;
        g_variant_get(parameters, "(&ox)", &track, &position);
        if (m->loaded && !m->idle && m->seekable && position >= 0 &&
            g_str_equal(track, m->track_id) && !g_str_equal(track, NO_TRACK) &&
            (m->duration <= 0 || position / 1000000.0 <= m->duration))
            seek_absolute(m, position / 1000000.0, &error);
    } else if (g_str_equal(method, "OpenUri")) {
        const char *uri;
        g_variant_get(parameters, "(&s)", &uri);
        g_autofree char *scheme = g_uri_parse_scheme(uri);
        gboolean supported = FALSE;
        for (guint i = 0; scheme && uri_schemes[i]; ++i)
            if (g_ascii_strcasecmp(scheme, uri_schemes[i]) == 0) supported = TRUE;
        if (!supported || !g_uri_is_valid(uri, G_URI_FLAGS_NONE, NULL))
            g_set_error_literal(&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "The URI scheme or syntax is not supported.");
        else if (!m->host.open_uri || !m->host.open_uri(uri, m->host.data))
            g_set_error_literal(&error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "The player could not open this URI.");
    }
    if (error) g_dbus_method_invocation_return_gerror(invocation, error);
    else g_dbus_method_invocation_return_value(invocation, NULL);
    mpris_unref(m);
}

static GVariant *get_property(GDBusConnection *connection, const char *sender, const char *path,
                                const char *interface, const char *name, GError **error, gpointer data)
{
    (void)connection; (void)sender; (void)path;
    Mpris *m = data;
    GVariant *value = m->stopped ? NULL : property_value(m, interface, name);
    if (!value) g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s.", name);
    return value;
}

static gboolean set_property(GDBusConnection *connection, const char *sender, const char *path,
                               const char *interface, const char *name, GVariant *value,
                               GError **error, gpointer data)
{
    (void)connection; (void)sender; (void)path;
    Mpris *m = mpris_ref(data);
    gboolean result = FALSE;
    if (m->stopped) g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "The player is closing.");
    else if (g_str_equal(interface, ROOT_INTERFACE) && g_str_equal(name, "Fullscreen")) {
        if (m->host.set_fullscreen) {
            m->host.set_fullscreen(g_variant_get_boolean(value), m->host.data);
            result = TRUE;
        } else g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Fullscreen is unavailable.");
    } else if (g_str_equal(interface, PLAYER_INTERFACE) && g_str_equal(name, "LoopStatus")) {
        const char *loop = g_variant_get_string(value, NULL);
        if (!g_str_equal(loop, "None") && !g_str_equal(loop, "Track") && !g_str_equal(loop, "Playlist"))
            g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Unknown loop status.");
        else {
            const char *file[] = {"set", "loop-file", g_str_equal(loop, "Track") ? "inf" : "no", NULL};
            const char *playlist[] = {"set", "loop-playlist", g_str_equal(loop, "Playlist") ? "inf" : "no", NULL};
            result = command(m, file, error) && command(m, playlist, error);
        }
    } else if (g_str_equal(interface, PLAYER_INTERFACE) && g_str_equal(name, "Rate")) {
        double rate = g_variant_get_double(value);
        if (!isfinite(rate) || (rate != 0 && (rate < MINIMUM_RATE || rate > MAXIMUM_RATE)))
            g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Unsupported playback rate.");
        else result = rate == 0 ? set_pause(m, TRUE, error) : set_number(m, "speed", rate, error);
    } else if (g_str_equal(interface, PLAYER_INTERFACE) && g_str_equal(name, "Volume")) {
        double volume = g_variant_get_double(value);
        if (!isfinite(volume)) g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Volume must be finite.");
        else {
            volume = CLAMP(volume, 0, MAX(0, m->volume_max) / 100.0);
            result = set_number(m, "volume", volume * 100.0, error);
            if (result && volume > 0 && m->muted) {
                const char *args[] = {"set", "mute", "no", NULL};
                result = command(m, args, error);
            }
        }
    } else g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY, "Property %s is not writable.", name);
    mpris_unref(m);
    return result;
}

static const GDBusInterfaceVTable vtable = {method_call, get_property, set_property, {0}};

static void name_acquired(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)connection; (void)name;
    Mpris *m = data;
    if (!m->stopped) { m->owned = TRUE; schedule_publish(m); }
}

static void name_lost(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)connection; (void)name;
    ((Mpris *)data)->owned = FALSE;
}

static void bus_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    Mpris *m = data;
    g_autoptr(GError) error = NULL;
    GDBusConnection *connection = g_bus_get_finish(result, &error);
    if (connection && !m->stopped) {
        m->connection = connection;
        for (int i = 0; i < 2; ++i) {
            m->registrations[i] = g_dbus_connection_register_object(connection, OBJECT_PATH,
                m->introspection->interfaces[i], &vtable, mpris_ref(m), mpris_unref, &error);
            if (!m->registrations[i]) { mpris_unref(m); break; }
        }
        if (m->registrations[0] && m->registrations[1])
            m->owner = g_bus_own_name_on_connection(connection, m->bus_name,
                G_BUS_NAME_OWNER_FLAGS_NONE, name_acquired, name_lost, mpris_ref(m), mpris_unref);
        else {
            for (int i = 0; i < 2; ++i) if (m->registrations[i]) {
                g_dbus_connection_unregister_object(connection, m->registrations[i]);
                m->registrations[i] = 0;
            }
            g_clear_object(&m->connection);
        }
    } else if (connection) g_object_unref(connection);
    mpris_unref(m);
}

static void replace_string(char **target, const mpv_event_property *property)
{
    g_free(*target);
    const char *value = property->data && property->format == MPV_FORMAT_STRING
        ? *(char **)property->data : NULL;
    *target = g_strdup(value ? value : "");
}

static void update_playlist(Mpris *m, const mpv_node *node)
{
    g_array_set_size(m->playlist_ids, 0);
    if (node && node->format == MPV_FORMAT_NODE_ARRAY && node->u.list) {
        for (int i = 0; i < node->u.list->num; ++i) {
            const mpv_node *item = &node->u.list->values[i];
            const mpv_node *id = field(item, "id");
            gint64 entry = id && id->format == MPV_FORMAT_INT64 ? id->u.int64 : -1;
            g_array_append_val(m->playlist_ids, entry);
            const mpv_node *playing = field(item, "playing");
            if (m->entry_id < 0 && m->loaded && playing && playing->format == MPV_FORMAT_FLAG && playing->u.flag)
                m->entry_id = entry;
        }
    }
    if (m->entry_id < 0 && m->loaded && m->playlist_position >= 0 && m->playlist_position < (gint64)m->playlist_ids->len)
        m->entry_id = g_array_index(m->playlist_ids, gint64, m->playlist_position);
    if (m->resume_id >= 0 && entry_index(m, m->resume_id) < 0) m->resume_id = -1;
    rebuild_metadata(m);
}

static void update_tags(Mpris *m, const mpv_node *node)
{
    GVariantBuilder tags;
    g_variant_builder_init(&tags, G_VARIANT_TYPE("a{ss}"));
    if (node && node->format == MPV_FORMAT_NODE_MAP && node->u.list) {
        for (int i = 0; i < node->u.list->num; ++i) {
            const mpv_node *value = &node->u.list->values[i];
            if (value->format == MPV_FORMAT_STRING && value->u.string) {
                g_autofree char *key = g_utf8_make_valid(node->u.list->keys[i], -1);
                g_autofree char *text = g_utf8_make_valid(value->u.string, -1);
                g_variant_builder_add(&tags, "{ss}", key, text);
            }
        }
    }
    g_clear_pointer(&m->tags, g_variant_unref);
    m->tags = g_variant_ref_sink(g_variant_builder_end(&tags));
    rebuild_metadata(m);
}

static void update_property(Mpris *m, guint index, const mpv_event_property *property)
{
    const char *name = observations[index].name;
    if (index <= 6 || g_str_equal(name, "eof-reached")) {
        gboolean value = property->data && property->format == MPV_FORMAT_FLAG && *(int *)property->data;
        if (g_str_equal(name, "idle-active")) {
            m->idle = value;
            if (!m->loading) m->loaded = !value;
            rebuild_metadata(m);
        } else if (g_str_equal(name, "pause")) m->paused = value;
        else if (g_str_equal(name, "paused-for-cache")) m->buffering = value;
        else if (g_str_equal(name, "seekable")) m->seekable = value;
        else if (g_str_equal(name, "seeking")) m->seeking = value;
        else if (g_str_equal(name, "fullscreen")) m->fullscreen = value;
        else if (g_str_equal(name, "mute")) m->muted = value;
        else if (g_str_equal(name, "eof-reached")) m->eof = value;
    } else if (observations[index].format == MPV_FORMAT_DOUBLE) {
        double value = property->data && property->format == MPV_FORMAT_DOUBLE ? *(double *)property->data : NAN;
        if (g_str_equal(name, "time-pos")) {
            if (isfinite(value)) { m->position = MAX(0, value); m->sampled_at = g_get_monotonic_time(); }
            return;
        }
        if (g_str_equal(name, "duration")) { m->duration = isfinite(value) ? MAX(0, value) : 0; rebuild_metadata(m); }
        else if (g_str_equal(name, "speed") && isfinite(value) && value > 0) m->rate = value;
        else if (g_str_equal(name, "volume") && isfinite(value)) m->volume = MAX(0, value);
        else if (g_str_equal(name, "volume-max") && isfinite(value)) m->volume_max = MAX(0, value);
    } else if (g_str_equal(name, "metadata")) update_tags(m, property->data);
    else if (g_str_equal(name, "playlist")) update_playlist(m, property->data);
    else if (g_str_equal(name, "playlist-pos")) {
        m->playlist_position = property->data ? *(int64_t *)property->data : -1;
        if (m->entry_id < 0 && m->loaded && m->playlist_position >= 0 && m->playlist_position < (gint64)m->playlist_ids->len) {
            m->entry_id = g_array_index(m->playlist_ids, gint64, m->playlist_position);
            rebuild_metadata(m);
        }
    } else if (g_str_equal(name, "playlist-count")) m->playlist_count = property->data ? *(int64_t *)property->data : 0;
    else if (g_str_equal(name, "media-title")) { replace_string(&m->title, property); rebuild_metadata(m); }
    else if (g_str_equal(name, "path")) { replace_string(&m->path, property); rebuild_metadata(m); }
    else if (g_str_equal(name, "working-directory")) { replace_string(&m->working_directory, property); rebuild_metadata(m); }
    else if (g_str_equal(name, "loop-file")) replace_string(&m->loop_file, property);
    else if (g_str_equal(name, "loop-playlist")) replace_string(&m->loop_playlist, property);
    schedule_publish(m);
}

static void refresh_loaded_properties(Mpris *m)
{
    /* Observers coalesce unchanged values across fast same-file reloads.
     * Explicit asynchronous samples repopulate a new file's cleared cache;
     * the generation fence rejects replies after another source starts. */
    static const guint indices[] = {1, 3, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18};
    for (guint i = 0; i < G_N_ELEMENTS(indices); ++i) {
        guint64 *id = g_new(guint64, 1);
        *id = REQUEST_BASE + ++m->serial;
        PropertySample *sample = g_new(PropertySample, 1);
        *sample = (PropertySample){.index = indices[i], .generation = m->generation};
        if (mpv_get_property_async(m->mpv, *id, observations[indices[i]].name, observations[indices[i]].format) < 0) {
            g_free(id); g_free(sample);
        } else g_hash_table_insert(m->samples, id, sample);
    }
}

Mpris *mpris_new(mpv_handle *mpv, const MprisHost *host)
{
    g_return_val_if_fail(mpv != NULL, NULL);
    Mpris *m = g_new0(Mpris, 1);
    m->references = 1; m->mpv = mpv;
    if (host) m->host = *host;
    m->cancel = g_cancellable_new();
    m->idle = TRUE; m->rate = 1; m->volume = 100; m->volume_max = 130;
    m->entry_id = m->resume_id = m->playlist_position = -1;
    m->playlist_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    m->bus_name = g_strdup_printf("org.mpris.MediaPlayer2.mpv_gtk.instance%ld", (long)getpid());
    for (int i = 0; i < 2; ++i)
        m->sent[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    m->samples = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    m->introspection = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    rebuild_metadata(m);
    for (guint i = 0; i < G_N_ELEMENTS(observations); ++i)
        mpv_observe_property(mpv, OBSERVE_BASE + i, observations[i].name, observations[i].format);
    g_bus_get(G_BUS_TYPE_SESSION, m->cancel, bus_ready, mpris_ref(m));
    return m;
}

const char *mpris_bus_name(const Mpris *m) { return m ? m->bus_name : NULL; }

void mpris_set_art_url(Mpris *m, const char *uri)
{
    if (!m || m->stopped) return;
    if (uri && *uri) {
        g_autofree char *scheme = g_uri_parse_scheme(uri);
        if (!scheme || !g_utf8_validate(uri, -1, NULL) || !g_uri_is_valid(uri, G_URI_FLAGS_NONE, NULL)) return;
    } else uri = NULL;
    if (g_strcmp0(uri, m->art_url) == 0) return;
    g_free(m->art_url);
    m->art_url = g_strdup(uri);
    rebuild_metadata(m);
}

gboolean mpris_handle_event(Mpris *m, const mpv_event *event)
{
    if (!m || m->stopped || !event) return FALSE;
    if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && event->reply_userdata >= OBSERVE_BASE &&
        event->reply_userdata < OBSERVE_BASE + G_N_ELEMENTS(observations)) {
        const mpv_event_property *property = event->data;
        if (property) update_property(m, (guint)(event->reply_userdata - OBSERVE_BASE), property);
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->reply_userdata >= COMMAND_BASE &&
        event->reply_userdata < REQUEST_LIMIT) return TRUE;
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata >= REQUEST_BASE &&
        event->reply_userdata < COMMAND_BASE && event->reply_userdata != m->position_request) {
        PropertySample *sample = g_hash_table_lookup(m->samples, &event->reply_userdata);
        if (sample && sample->generation == m->generation && m->loaded) {
            const mpv_event_property *property = event->data;
            mpv_event_property unavailable = {.name = observations[sample->index].name, .format = MPV_FORMAT_NONE};
            update_property(m, sample->index, event->error >= 0 && property ? property : &unavailable);
        }
        g_hash_table_remove(m->samples, &event->reply_userdata);
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata == m->position_request &&
        event->reply_userdata >= REQUEST_BASE && event->reply_userdata < COMMAND_BASE) {
        m->position_request = 0;
        const mpv_event_property *property = event->data;
        if (event->error >= 0 && m->seek_pending && m->seek_generation == m->generation && m->loaded &&
            property && property->format == MPV_FORMAT_DOUBLE && property->data) {
            double value = *(double *)property->data;
            if (isfinite(value)) {
                m->position = MAX(0, value); m->sampled_at = g_get_monotonic_time();
                if (m->owned && m->connection)
                    g_dbus_connection_emit_signal(m->connection, NULL, OBJECT_PATH, PLAYER_INTERFACE,
                        "Seeked", g_variant_new("(x)", position_us(m)), NULL);
            }
        }
        m->seek_pending = FALSE;
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_START_FILE) {
        const mpv_event_start_file *start = event->data;
        ++m->generation;
        g_hash_table_remove_all(m->samples);
        m->position_request = 0;
        m->loading = TRUE; m->loaded = FALSE; m->seek_pending = FALSE; m->eof = FALSE;
        m->position = m->duration = 0;
        m->entry_id = start ? start->playlist_entry_id : -1;
        if (m->restart_at_zero && m->restart_entry_id >= 0 && m->entry_id != m->restart_entry_id)
            m->restart_at_zero = FALSE;
        g_clear_pointer(&m->tags, g_variant_unref);
        g_clear_pointer(&m->art_url, g_free);
        g_clear_pointer(&m->title, g_free); g_clear_pointer(&m->path, g_free);
        rebuild_metadata(m);
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
        m->loading = FALSE; m->loaded = TRUE; m->idle = FALSE;
        if (m->restart_at_zero) { seek_absolute(m, 0, NULL); m->restart_at_zero = FALSE; }
        refresh_loaded_properties(m);
        rebuild_metadata(m);
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        g_clear_pointer(&m->art_url, g_free);
        m->position_request = 0;
        m->loaded = m->loading = m->seek_pending = FALSE;
        m->position = 0;
        if (m->entry_id >= 0) m->resume_id = m->entry_id;
        m->entry_id = -1;
        rebuild_metadata(m);
    } else if (event->event_id == MPV_EVENT_SEEK && m->loaded) {
        m->position_request = 0;
        m->seek_pending = TRUE; m->seek_generation = m->generation;
    } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && m->seek_pending && m->loaded) {
        m->position_request = REQUEST_BASE + ++m->serial;
        if (mpv_get_property_async(m->mpv, m->position_request, "time-pos", MPV_FORMAT_DOUBLE) < 0)
            m->position_request = 0;
    }
    return FALSE;
}

void mpris_free(Mpris *m)
{
    if (!m || m->stopped) return;
    m->stopped = TRUE;
    g_cancellable_cancel(m->cancel);
    if (m->emit_source) { g_source_remove(m->emit_source); m->emit_source = 0; }
    for (guint i = 0; i < G_N_ELEMENTS(observations); ++i) mpv_unobserve_property(m->mpv, OBSERVE_BASE + i);
    if (m->connection) for (int i = 0; i < 2; ++i) if (m->registrations[i]) {
        g_dbus_connection_unregister_object(m->connection, m->registrations[i]);
        m->registrations[i] = 0;
    }
    if (m->owner) { g_bus_unown_name(m->owner); m->owner = 0; }
    mpris_unref(m);
}
