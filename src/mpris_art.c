#define _GNU_SOURCE
#include "mpris_art.h"
#include "preview_decoder.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QUERY_BASE (UINT64_C(1) << 53)
#define QUERY_LIMIT (QUERY_BASE + (UINT64_C(1) << 32))
#define ICON_RESOURCE "/io/github/emless/mpvgtk/icons/scalable/apps/io.github.emless.mpvgtk.svg"
#define ART_MAX_SIDE 1024
#define ART_PIXEL_LIMIT (16 * 1024 * 1024)

typedef struct { const char *name; mpv_format format; } ArtProperty;
static const ArtProperty properties[] = {
    {"path", MPV_FORMAT_STRING}, {"working-directory", MPV_FORMAT_STRING},
    {"current-tracks/video", MPV_FORMAT_NODE}, {"duration", MPV_FORMAT_DOUBLE},
    {"demuxer-start-time", MPV_FORMAT_DOUBLE}, {"rebase-start-time", MPV_FORMAT_FLAG},
    {"demuxer-via-network", MPV_FORMAT_FLAG}, {"idle-active", MPV_FORMAT_FLAG}
};
typedef struct { guint index; guint64 generation; } ArtSample;
typedef enum { WORK_ICON, WORK_INSPECT, WORK_PNG, WORK_CLEANUP } WorkKind;
typedef struct {
    MprisArt *owner;
    WorkKind kind;
    guint64 generation;
    char *filename;
    GBytes *pixels;
    int width, height, stride;
    double seconds;
    gint64 ff_index;
} ArtWork;

struct MprisArt {
    gint references, stopped;
    mpv_handle *mpv;
    PreviewDecoder *decoder;
    MprisArtCallback callback;
    gpointer data;
    GCancellable *cancel;
    GHashTable *samples;
    guint64 generation, serial;
    guint pending;
    gboolean loaded, queried, decoding, writing, video, image, rebase, network;
    char *path, *working_directory, *external;
    double duration, offset;
    gint64 ff_index;
    GBytes *fast_pixels;
    int fast_width, fast_height, fast_stride;
    GMutex files_mutex;
    /* Directory is set once by the icon worker under files_mutex. Subsequent
     * reads by workers share that mutex; the main context waits for its task. */
    char *directory, *fallback_uri;
    GQueue thumbnails;
};

static void maybe_decode(MprisArt *art);
static void run_work(MprisArt *art, ArtWork *work);

static MprisArt *art_ref(MprisArt *art)
{
    g_atomic_int_inc(&art->references);
    return art;
}

static void art_unref(MprisArt *art)
{
    if (!g_atomic_int_dec_and_test(&art->references)) return;
    g_clear_object(&art->cancel);
    g_hash_table_unref(art->samples);
    g_clear_pointer(&art->fast_pixels, g_bytes_unref);
    g_free(art->path); g_free(art->working_directory); g_free(art->external);
    g_free(art->directory); g_free(art->fallback_uri);
    g_queue_clear_full(&art->thumbnails, g_free);
    g_mutex_clear(&art->files_mutex);
    g_free(art);
}

static void work_free(gpointer data)
{
    ArtWork *work = data;
    g_free(work->filename);
    g_clear_pointer(&work->pixels, g_bytes_unref);
    art_unref(work->owner);
    g_free(work);
}

static void emit_uri(MprisArt *art, const char *uri)
{
    if (!g_atomic_int_get(&art->stopped) && art->callback) art->callback(uri, art->data);
}

static const mpv_node *field(const mpv_node *map, const char *key)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], key) == 0) return &map->u.list->values[i];
    return NULL;
}

static const char *node_text(const mpv_node *map, const char *key)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_STRING && value->u.string ? value->u.string : "";
}

static gboolean node_flag(const mpv_node *map, const char *key)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_FLAG && value->u.flag;
}

static void clear_workspace(MprisArt *art)
{
    if (!art->directory) return;
    GDir *directory = g_dir_open(art->directory, 0, NULL);
    if (directory) {
        const char *name;
        while ((name = g_dir_read_name(directory))) {
            g_autofree char *path = g_build_filename(art->directory, name, NULL);
            g_unlink(path);
        }
        g_dir_close(directory);
    }
    g_rmdir(art->directory);
}

static gboolean cancelled(MprisArt *art, GCancellable *cancel)
{
    return g_atomic_int_get(&art->stopped) || (cancel && g_cancellable_is_cancelled(cancel));
}

static void file_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source;
    ArtWork *work = data;
    MprisArt *art = work->owner;
    char *uri = NULL;
    g_mutex_lock(&art->files_mutex);
    if (work->kind == WORK_CLEANUP) clear_workspace(art);
    else if (!cancelled(art, cancel)) {
        if (work->kind == WORK_ICON) {
            g_autoptr(GBytes) bytes = g_resources_lookup_data(ICON_RESOURCE, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
            if (bytes) {
                g_autofree char *base = g_build_filename(g_get_user_cache_dir(), "mpv-gtk", "mpris", NULL);
                if (g_mkdir_with_parents(base, 0700) == 0) {
                    char *template = g_strdup_printf("%s/instance-%ld-XXXXXX", base, (long)getpid());
                    if (g_mkdtemp_full(template, 0700)) art->directory = template;
                    else g_free(template);
                }
                if (art->directory && !cancelled(art, cancel)) {
                    g_autofree char *filename = g_build_filename(art->directory, "icon.svg", NULL);
                    gsize size;
                    const char *contents = g_bytes_get_data(bytes, &size);
                    if (g_file_set_contents_full(filename, contents, (gssize)size,
                        G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL))
                        uri = g_filename_to_uri(filename, NULL, NULL);
                }
            }
        } else if (work->kind == WORK_INSPECT) {
            GStatBuf info;
            if (g_stat(work->filename, &info) == 0 && S_ISREG(info.st_mode)) uri = g_strdup("regular");
        } else if (work->kind == WORK_PNG && art->directory) {
            gsize size;
            const guchar *input = g_bytes_get_data(work->pixels, &size);
            if (work->width > 0 && work->width <= ART_MAX_SIDE &&
                work->height > 0 && work->height <= ART_MAX_SIDE &&
                work->stride >= work->width * 4 && work->stride <= ART_MAX_SIDE * 4 &&
                size <= ART_PIXEL_LIMIT && size >= (gsize)work->stride * work->height) {
                gsize stride = (gsize)work->width * 4;
                guchar *rgba = g_malloc(stride * work->height);
                for (int y = 0; y < work->height; ++y) for (int x = 0; x < work->width; ++x) {
                    const guchar *src = input + (gsize)y * work->stride + x * 4;
                    guchar *dst = rgba + y * stride + x * 4;
                    dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
                }
                g_autoptr(GBytes) pixels = g_bytes_new_take(rgba, stride * work->height);
                g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_bytes(pixels,
                    GDK_COLORSPACE_RGB, TRUE, 8, work->width, work->height, (int)stride);
                char *encoded = NULL;
                gsize length = 0;
                if (gdk_pixbuf_save_to_buffer(pixbuf, &encoded, &length, "png", NULL, NULL)) {
                    g_autofree char *name = g_strdup_printf("thumbnail-%" G_GUINT64_FORMAT ".png", work->generation);
                    g_autofree char *filename = g_build_filename(art->directory, name, NULL);
                    if (!cancelled(art, cancel) && g_file_set_contents_full(filename, encoded, (gssize)length,
                        G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL)) {
                        if (cancelled(art, cancel)) g_unlink(filename);
                        else {
                            uri = g_filename_to_uri(filename, NULL, NULL);
                            g_queue_push_tail(&art->thumbnails, g_strdup(filename));
                            while (g_queue_get_length(&art->thumbnails) > 2) {
                                char *old = g_queue_pop_head(&art->thumbnails);
                                g_unlink(old); g_free(old);
                            }
                        }
                    }
                }
                g_free(encoded);
            }
        }
    }
    if (g_atomic_int_get(&art->stopped)) { clear_workspace(art); g_clear_pointer(&uri, g_free); }
    g_mutex_unlock(&art->files_mutex);
    g_task_return_pointer(task, uri, g_free);
}

static void file_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)data;
    GTask *task = G_TASK(result);
    ArtWork *work = g_task_get_task_data(task);
    MprisArt *art = work->owner;
    g_autofree char *uri = g_task_propagate_pointer(task, NULL);
    if (g_atomic_int_get(&art->stopped)) return;
    if (work->kind == WORK_ICON) {
        g_free(art->fallback_uri); art->fallback_uri = g_steal_pointer(&uri);
        if (art->loaded && art->fallback_uri) emit_uri(art, art->fallback_uri);
        maybe_decode(art);
    } else if (work->generation == art->generation && art->loaded && uri) {
        if (work->kind == WORK_INSPECT) {
            preview_decoder_set_enabled(art->decoder, TRUE);
            g_autofree char *key = g_strdup_printf("mpris:%" G_GUINT64_FORMAT ":%s", art->generation, work->filename);
            preview_decoder_request(art->decoder, key, work->filename, work->ff_index,
                                    work->seconds, art->generation);
        } else if (work->kind == WORK_PNG) emit_uri(art, uri);
    }
}

static void run_work(MprisArt *art, ArtWork *work)
{
    work->owner = art_ref(art);
    GTask *task = g_task_new(NULL, work->kind == WORK_ICON || work->kind == WORK_CLEANUP ? NULL : art->cancel,
                            file_finished, NULL);
    g_task_set_task_data(task, work, work_free);
    g_task_set_check_cancellable(task, FALSE);
    g_task_run_in_thread(task, file_worker);
    g_object_unref(task);
}

static void decoded_frame(const PreviewDecoderFrame *frame, gpointer data)
{
    MprisArt *art = data;
    if (g_atomic_int_get(&art->stopped) || !art->loaded || art->writing || frame->token != art->generation) return;
    if (frame->pixels) {
        g_clear_pointer(&art->fast_pixels, g_bytes_unref);
        art->fast_pixels = g_bytes_ref(frame->pixels);
        art->fast_width = frame->width; art->fast_height = frame->height; art->fast_stride = frame->stride;
    }
    if (!frame->refined) return;
    art->writing = TRUE;
    preview_decoder_set_enabled(art->decoder, FALSE);
    if (!art->fast_pixels) return;
    ArtWork *work = g_new0(ArtWork, 1);
    *work = (ArtWork){.kind = WORK_PNG, .generation = art->generation,
        .pixels = g_bytes_ref(art->fast_pixels), .width = art->fast_width,
        .height = art->fast_height, .stride = art->fast_stride};
    run_work(art, work);
    g_clear_pointer(&art->fast_pixels, g_bytes_unref);
}

static char *local_filename(MprisArt *art)
{
    const char *path = art->external && *art->external ? art->external : art->path;
    if (!path || !*path || art->network) return NULL;
    g_autofree char *scheme = g_uri_parse_scheme(path);
    g_autofree char *filename = NULL;
    if (scheme) {
        if (g_ascii_strcasecmp(scheme, "file") != 0) return NULL;
        filename = g_filename_from_uri(path, NULL, NULL);
        if (!filename) return NULL;
    }
    return g_canonicalize_filename(filename ? filename : path,
        art->working_directory && g_path_is_absolute(art->working_directory) ? art->working_directory : NULL);
}

static void maybe_decode(MprisArt *art)
{
    if (g_atomic_int_get(&art->stopped) || !art->loaded || !art->queried ||
        !art->fallback_uri || art->decoding || !art->video) return;
    g_autofree char *filename = local_filename(art);
    if (!filename) return;
    art->decoding = TRUE;
    double second = art->image ? 0 : MIN(30, MAX(0, art->duration) * .1);
    if (art->rebase) second += art->offset;
    ArtWork *work = g_new0(ArtWork, 1);
    *work = (ArtWork){.kind = WORK_INSPECT, .generation = art->generation,
        .filename = g_steal_pointer(&filename), .ff_index = art->ff_index, .seconds = second};
    run_work(art, work);
}

static void reset_source(MprisArt *art, gboolean loaded)
{
    ++art->generation;
    art->loaded = loaded;
    art->queried = art->decoding = art->writing = art->video = art->image = art->network = FALSE;
    art->duration = art->offset = 0; art->rebase = TRUE; art->ff_index = -1;
    art->pending = 0;
    g_hash_table_remove_all(art->samples);
    g_cancellable_cancel(art->cancel);
    g_clear_object(&art->cancel); art->cancel = g_cancellable_new();
    preview_decoder_set_enabled(art->decoder, FALSE);
    g_clear_pointer(&art->fast_pixels, g_bytes_unref);
    g_clear_pointer(&art->path, g_free); g_clear_pointer(&art->working_directory, g_free);
    g_clear_pointer(&art->external, g_free);
    emit_uri(art, loaded ? art->fallback_uri : NULL);
}

static void query_source(MprisArt *art)
{
    for (guint i = 0; i < G_N_ELEMENTS(properties); ++i) {
        guint64 *id = g_new(guint64, 1);
        *id = QUERY_BASE + ++art->serial;
        ArtSample *sample = g_new(ArtSample, 1);
        *sample = (ArtSample){.index = i, .generation = art->generation};
        if (mpv_get_property_async(art->mpv, *id, properties[i].name, properties[i].format) < 0) {
            g_free(id); g_free(sample);
        } else { g_hash_table_insert(art->samples, id, sample); ++art->pending; }
    }
    if (!art->pending) { art->queried = TRUE; maybe_decode(art); }
}

static void sample_property(MprisArt *art, guint index, const mpv_event_property *property)
{
    if (!property || !property->data) return;
    if (property->format == MPV_FORMAT_STRING) {
        char **target = index == 0 ? &art->path : &art->working_directory;
        g_free(*target); *target = g_strdup(*(char **)property->data);
    } else if (index == 2 && property->format == MPV_FORMAT_NODE) {
        const mpv_node *track = property->data;
        art->video = track->format == MPV_FORMAT_NODE_MAP;
        art->image = node_flag(track, "image") || node_flag(track, "albumart");
        const mpv_node *id = field(track, "ff-index");
        art->ff_index = id && id->format == MPV_FORMAT_INT64 ? id->u.int64 : -1;
        g_free(art->external); art->external = g_strdup(node_text(track, "external-filename"));
    } else if (property->format == MPV_FORMAT_DOUBLE) {
        double value = *(double *)property->data;
        if (isfinite(value)) { if (index == 3) art->duration = value; else if (index == 4) art->offset = value; }
    } else if (property->format == MPV_FORMAT_FLAG) {
        gboolean value = *(int *)property->data != 0;
        if (index == 5) art->rebase = value;
        else if (index == 6) art->network = value;
        else if (index == 7) art->loaded = !value;
    }
}

MprisArt *mpris_art_new(mpv_handle *mpv, MprisArtCallback callback, gpointer data)
{
    g_return_val_if_fail(mpv != NULL && callback != NULL, NULL);
    MprisArt *art = g_new0(MprisArt, 1);
    art->references = 1; art->mpv = mpv; art->callback = callback; art->data = data;
    art->cancel = g_cancellable_new();
    art->samples = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    g_mutex_init(&art->files_mutex);
    art->decoder = preview_decoder_new_sized(decoded_frame, art, ART_MAX_SIDE, ART_MAX_SIDE);
    reset_source(art, FALSE);
    ArtWork *work = g_new0(ArtWork, 1); work->kind = WORK_ICON;
    run_work(art, work);
    query_source(art);
    return art;
}

gboolean mpris_art_handle_event(MprisArt *art, const mpv_event *event)
{
    if (!art || g_atomic_int_get(&art->stopped) || !event) return FALSE;
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY &&
        event->reply_userdata >= QUERY_BASE && event->reply_userdata < QUERY_LIMIT) {
        ArtSample *sample = g_hash_table_lookup(art->samples, &event->reply_userdata);
        if (sample && sample->generation == art->generation) {
            if (event->error >= 0) sample_property(art, sample->index, event->data);
            g_hash_table_remove(art->samples, &event->reply_userdata);
            if (art->pending) --art->pending;
            if (!art->pending) {
                art->queried = TRUE;
                if (art->loaded && art->fallback_uri) emit_uri(art, art->fallback_uri);
                maybe_decode(art);
            }
        }
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE) reset_source(art, FALSE);
    else if (event->event_id == MPV_EVENT_FILE_LOADED) {
        reset_source(art, TRUE);
        query_source(art);
    }
    return FALSE;
}

void mpris_art_free(MprisArt *art)
{
    if (!art || g_atomic_int_get(&art->stopped)) return;
    g_atomic_int_set(&art->stopped, TRUE);
    art->callback = NULL; art->data = NULL;
    g_cancellable_cancel(art->cancel);
    preview_decoder_free(art->decoder);
    art->decoder = NULL;
    ArtWork *work = g_new0(ArtWork, 1); work->kind = WORK_CLEANUP;
    run_work(art, work);
    art_unref(art);
}
