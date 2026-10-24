#include "preview_decoder.h"

#include <mpv/client.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define REFINE_DELAY_US INT64_C(150000)
#define WARM_IDLE_US INT64_C(30000000)
#define DECODE_TIMEOUT_US INT64_C(5000000)
#define PIXEL_LIMIT (4 * 1024 * 1024)

typedef struct {
    char *key, *filename;
    gint64 ff_index;
    double seconds;
    guint64 token, revision;
    gint64 changed_at;
} DecoderRequest;

struct PreviewDecoder {
    gint references;
    GMutex mutex;
    GCond condition;
    GMainContext *context;
    PreviewDecoderCallback callback;
    gpointer data;
    gboolean enabled, active, stopped, worker_started;
    guint64 revision;
    gint64 last_activity;
    DecoderRequest wanted;
    /* Only mpv_wakeup() is used through this pointer outside the worker.
     * Publication and removal share the request mutex. */
    mpv_handle *wake_core;
};

typedef enum { PHASE_IDLE, PHASE_LOADING, PHASE_FAST, PHASE_REFINE_WAIT,
               PHASE_EXACT, PHASE_DONE } DecoderPhase;

typedef struct {
    PreviewDecoder *owner;
    mpv_handle *core;
    DecoderRequest current;
    char *source_key;
    gint64 source_index;
    guint64 seen_revision, command_serial, load_id, seek_id;
    DecoderPhase phase;
    gboolean loaded, load_acknowledged, saw_start, file_ready;
    gboolean seek_acknowledged, saw_seek, fast_delivered;
    gint64 deadline, refine_at;
} DecoderWorker;

typedef struct {
    PreviewDecoder *owner;
    PreviewDecoderFrame frame;
    guint64 revision;
    char *error;
} FrameDelivery;

static void request_clear(DecoderRequest *request)
{
    g_free(request->key);
    g_free(request->filename);
    *request = (DecoderRequest){0};
}

static void request_copy(DecoderRequest *target, const DecoderRequest *source)
{
    request_clear(target);
    *target = *source;
    target->key = g_strdup(source->key);
    target->filename = g_strdup(source->filename);
}

static PreviewDecoder *decoder_ref(PreviewDecoder *decoder)
{
    g_atomic_int_inc(&decoder->references);
    return decoder;
}

static void decoder_unref(PreviewDecoder *decoder)
{
    if (!g_atomic_int_dec_and_test(&decoder->references)) return;
    request_clear(&decoder->wanted);
    g_main_context_unref(decoder->context);
    g_cond_clear(&decoder->condition);
    g_mutex_clear(&decoder->mutex);
    g_free(decoder);
}

static gboolean deliver_frame(gpointer data)
{
    FrameDelivery *delivery = data;
    PreviewDecoder *decoder = delivery->owner;
    PreviewDecoderCallback callback = NULL;
    gpointer callback_data = NULL;
    g_mutex_lock(&decoder->mutex);
    if (!decoder->stopped && decoder->enabled && decoder->active &&
        decoder->revision == delivery->revision) {
        callback = decoder->callback;
        callback_data = decoder->data;
    }
    g_mutex_unlock(&decoder->mutex);
    if (callback) callback(&delivery->frame, callback_data);
    return G_SOURCE_REMOVE;
}

static void delivery_free(gpointer data)
{
    FrameDelivery *delivery = data;
    g_clear_pointer(&delivery->frame.pixels, g_bytes_unref);
    g_free(delivery->error);
    decoder_unref(delivery->owner);
    g_free(delivery);
}

static gboolean current_request(DecoderWorker *worker)
{
    PreviewDecoder *decoder = worker->owner;
    g_mutex_lock(&decoder->mutex);
    gboolean current = !decoder->stopped && decoder->enabled && decoder->active &&
        decoder->revision == worker->current.revision;
    g_mutex_unlock(&decoder->mutex);
    return current;
}

static void post_frame(DecoderWorker *worker, GBytes *pixels, int width, int height,
                        double seconds, gboolean refined, const char *error)
{
    if (!current_request(worker)) return;
    FrameDelivery *delivery = g_new0(FrameDelivery, 1);
    delivery->owner = decoder_ref(worker->owner);
    delivery->revision = worker->current.revision;
    delivery->error = g_strdup(error);
    delivery->frame = (PreviewDecoderFrame){.token = worker->current.token,
        .pixels = pixels ? g_bytes_ref(pixels) : NULL,
        .width = width, .height = height, .stride = width * 4,
        .seconds = seconds, .refined = refined, .error = delivery->error};
    /* invoke_full() may execute immediately on an unowned main context.
     * Attaching a source guarantees GTK consumers run in their context. */
    GSource *source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, deliver_frame, delivery, delivery_free);
    g_source_attach(source, worker->owner->context);
    g_source_unref(source);
}

static const mpv_node *field(const mpv_node *map, const char *name)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], name) == 0) return &map->u.list->values[i];
    return NULL;
}

static gint64 integer_field(const mpv_node *map, const char *name, gint64 fallback)
{
    const mpv_node *value = field(map, name);
    return value && value->format == MPV_FORMAT_INT64 ? value->u.int64 : fallback;
}

static const char *text_field(const mpv_node *map, const char *name)
{
    const mpv_node *value = field(map, name);
    return value && value->format == MPV_FORMAT_STRING && value->u.string ? value->u.string : "";
}

static double number(mpv_handle *core, const char *name, double fallback)
{
    double value;
    return mpv_get_property(core, name, MPV_FORMAT_DOUBLE, &value) >= 0 && isfinite(value)
        ? value : fallback;
}

static gboolean flag(mpv_handle *core, const char *name, gboolean fallback)
{
    int value;
    return mpv_get_property(core, name, MPV_FORMAT_FLAG, &value) >= 0 ? value != 0 : fallback;
}

static mpv_handle *create_core(char **error)
{
    mpv_handle *core = mpv_create();
    if (!core) { *error = g_strdup("Could not create the preview decoder."); return NULL; }
    static const struct { const char *name, *value; gboolean optional; } options[] = {
        {"config", "no", FALSE}, {"terminal", "no", FALSE},
        {"input-terminal", "no", FALSE}, {"input-default-bindings", "no", FALSE},
        {"vo", "null", FALSE}, {"ao", "null", FALSE},
        {"audio", "no", FALSE}, {"sub", "no", FALSE},
        {"pause", "yes", FALSE}, {"idle", "yes", FALSE},
        {"keep-open", "always", FALSE}, {"force-window", "no", FALSE},
        {"hwdec", "no", FALSE}, {"rebase-start-time", "no", FALSE},
        {"hr-seek", "no", FALSE}, {"cache", "no", FALSE},
        {"demuxer-readahead-secs", "0", FALSE},
        {"demuxer-max-bytes", "128KiB", FALSE},
        {"demuxer-max-back-bytes", "128KiB", FALSE},
        {"vd-lavc-threads", "2", FALSE}, {"vd-lavc-fast", "yes", FALSE},
        {"vd-lavc-skiploopfilter", "all", FALSE},
        {"sws-scaler", "fast-bilinear", FALSE},
        {"sws-allow-zimg", "no", TRUE},
        {"load-scripts", "no", TRUE}, {"osc", "no", TRUE},
        {"ytdl", "no", TRUE}, {"load-stats-overlay", "no", TRUE},
        {"load-console", "no", TRUE}, {"load-auto-profiles", "no", TRUE},
        {"load-select", "no", TRUE}, {"load-positioning", "no", TRUE},
        {"load-commands", "no", TRUE}, {"load-context-menu", "no", TRUE},
        {"resume-playback", "no", FALSE}, {"save-position-on-quit", "no", FALSE},
        {"vf", "lavfi=[scale=w='max(2,trunc(min(256,144*dar)/2)*2)':h='max(2,trunc(min(144,256/dar)/2)*2)':flags=fast_bilinear,setsar=1,format=bgra]", FALSE}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i) {
        int status = mpv_set_option_string(core, options[i].name, options[i].value);
        if (status < 0 && !(options[i].optional && status == MPV_ERROR_OPTION_NOT_FOUND)) {
            *error = g_strdup_printf("Preview option %s: %s", options[i].name, mpv_error_string(status));
            mpv_destroy(core);
            return NULL;
        }
    }
    int status = mpv_initialize(core);
    if (status < 0) {
        *error = g_strdup_printf("Could not initialize the preview decoder: %s", mpv_error_string(status));
        mpv_destroy(core);
        return NULL;
    }
    return core;
}

static void close_core(DecoderWorker *worker)
{
    if (!worker->core) return;
    g_mutex_lock(&worker->owner->mutex);
    worker->owner->wake_core = NULL;
    g_mutex_unlock(&worker->owner->mutex);
    mpv_terminate_destroy(worker->core);
    worker->core = NULL;
    worker->loaded = worker->file_ready = FALSE;
    g_clear_pointer(&worker->source_key, g_free);
    worker->phase = PHASE_IDLE;
}

static void report_failure(DecoderWorker *worker, const char *message)
{
    if (!worker->fast_delivered)
        post_frame(worker, NULL, 0, 0, worker->current.seconds, TRUE, message);
    worker->phase = PHASE_DONE;
}

static gboolean begin_seek(DecoderWorker *worker, gboolean exact)
{
    if (!worker->loaded || !current_request(worker)) return FALSE;
    int status = mpv_set_property_string(worker->core, "hr-seek", exact ? "yes" : "no");
    double seconds = worker->current.seconds;
    worker->seek_id = ++worker->command_serial;
    if (status >= 0) status = mpv_set_property_async(worker->core, worker->seek_id,
                                                    "time-pos", MPV_FORMAT_DOUBLE, &seconds);
    if (status < 0) {
        report_failure(worker, mpv_error_string(status));
        return FALSE;
    }
    worker->phase = exact ? PHASE_EXACT : PHASE_FAST;
    worker->seek_acknowledged = FALSE;
    worker->saw_seek = FALSE;
    worker->deadline = g_get_monotonic_time() + DECODE_TIMEOUT_US;
    return TRUE;
}

static gboolean select_stream(DecoderWorker *worker, gboolean *changed)
{
    *changed = FALSE;
    if (worker->current.ff_index < 0) return TRUE;
    mpv_node tracks = {0};
    if (mpv_get_property(worker->core, "track-list", MPV_FORMAT_NODE, &tracks) < 0 ||
        tracks.format != MPV_FORMAT_NODE_ARRAY || !tracks.u.list) {
        mpv_free_node_contents(&tracks);
        return FALSE;
    }
    gint64 wanted = -1, selected = -1;
    for (int i = 0; i < tracks.u.list->num; ++i) {
        const mpv_node *track = &tracks.u.list->values[i];
        if (g_strcmp0(text_field(track, "type"), "video") != 0) continue;
        gint64 id = integer_field(track, "id", -1);
        const mpv_node *is_selected = field(track, "selected");
        if (is_selected && is_selected->format == MPV_FORMAT_FLAG && is_selected->u.flag) selected = id;
        if (integer_field(track, "ff-index", -1) == worker->current.ff_index) wanted = id;
    }
    mpv_free_node_contents(&tracks);
    if (wanted < 0) return FALSE;
    if (wanted == selected) return TRUE;
    *changed = TRUE;
    return mpv_set_property(worker->core, "vid", MPV_FORMAT_INT64, &wanted) >= 0;
}

static void load_movie(DecoderWorker *worker)
{
    worker->loaded = worker->file_ready = worker->saw_start = worker->saw_seek = FALSE;
    worker->load_acknowledged = FALSE;
    g_free(worker->source_key);
    worker->source_key = g_strdup(worker->current.key);
    worker->source_index = worker->current.ff_index;
    mpv_set_property_string(worker->core, "hr-seek", "no");
    mpv_set_property_string(worker->core, "vid", "auto");
    char start[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(start, sizeof(start), "%.6f", MAX(0, worker->current.seconds));
    mpv_set_option_string(worker->core, "start", start);
    const char *args[] = {"loadfile", worker->current.filename, "replace", NULL};
    worker->load_id = ++worker->command_serial;
    worker->phase = PHASE_LOADING;
    worker->deadline = g_get_monotonic_time() + DECODE_TIMEOUT_US;
    int status = mpv_command_async(worker->core, worker->load_id, args);
    if (status < 0) report_failure(worker, mpv_error_string(status));
}

static GBytes *pack_pixels(const guchar *input, int width, int height, int stride,
                            int *output_width, int *output_height)
{
    /* mpv applies container rotation after the user filter. A bounded
     * 256x144 landscape frame can therefore arrive as 144x256 here. */
    double factor = MIN(1.0, MIN(256.0 / width, 144.0 / height));
    int w = MAX(1, (int)floor(width * factor + .5));
    int h = MAX(1, (int)floor(height * factor + .5));
    gsize packed_stride = (gsize)w * 4;
    guchar *packed = g_malloc(packed_stride * h);
    if (w == width && h == height) {
        for (int row = 0; row < h; ++row)
            memcpy(packed + packed_stride * row, input + stride * row, packed_stride);
    } else {
        for (int y = 0; y < h; ++y) {
            double source_y = (y + .5) * height / h - .5;
            int y0 = CLAMP((int)floor(source_y), 0, height - 1);
            int y1 = MIN(y0 + 1, height - 1);
            double fy = CLAMP(source_y - y0, 0, 1);
            for (int x = 0; x < w; ++x) {
                double source_x = (x + .5) * width / w - .5;
                int x0 = CLAMP((int)floor(source_x), 0, width - 1);
                int x1 = MIN(x0 + 1, width - 1);
                double fx = CLAMP(source_x - x0, 0, 1);
                const guchar *samples[] = {input + y0 * stride + x0 * 4,
                    input + y0 * stride + x1 * 4, input + y1 * stride + x0 * 4,
                    input + y1 * stride + x1 * 4};
                double weights[] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                double alpha = 0, colors[3] = {0};
                for (int i = 0; i < 4; ++i) {
                    double coverage = weights[i] * samples[i][3];
                    alpha += coverage;
                    for (int channel = 0; channel < 3; ++channel)
                        colors[channel] += coverage * samples[i][channel];
                }
                guchar *pixel = packed + y * packed_stride + x * 4;
                pixel[3] = (guchar)CLAMP(floor(alpha + .5), 0, 255);
                for (int channel = 0; channel < 3; ++channel)
                    pixel[channel] = alpha > 0
                        ? (guchar)CLAMP(floor(colors[channel] / alpha + .5), 0, 255) : 0;
            }
        }
    }
    *output_width = w;
    *output_height = h;
    return g_bytes_new_take(packed, packed_stride * h);
}

static void capture_frame(DecoderWorker *worker, gboolean refined)
{
    if (!current_request(worker) || flag(worker->core, "seeking", TRUE)) return;
    const char *args[] = {"screenshot-raw", "video", "bgra", NULL};
    mpv_node result = {0};
    int status = mpv_command_ret(worker->core, args, &result);
    gint64 width = integer_field(&result, "w", 0), height = integer_field(&result, "h", 0);
    gint64 stride = integer_field(&result, "stride", 0);
    const mpv_node *data = field(&result, "data");
    gboolean valid = status >= 0 && width > 0 && width <= 256 && height > 0 && height <= 256 &&
        stride >= width * 4 && stride <= PIXEL_LIMIT &&
        g_strcmp0(text_field(&result, "format"), "bgra") == 0 &&
        data && data->format == MPV_FORMAT_BYTE_ARRAY && data->u.ba && data->u.ba->data &&
        data->u.ba->size <= PIXEL_LIMIT &&
        (guint64)data->u.ba->size >= (guint64)stride * (height - 1) + width * 4;
    GBytes *pixels = NULL;
    int output_width = 0, output_height = 0;
    if (valid) {
        pixels = pack_pixels(data->u.ba->data, (int)width, (int)height, (int)stride,
                             &output_width, &output_height);
    }
    mpv_free_node_contents(&result);
    if (!pixels) { report_failure(worker, "The preview decoder produced no usable frame."); return; }
    double actual = number(worker->core, "video-pts", number(worker->core, "time-pos", NAN));
    gboolean already_exact = !refined && isfinite(actual) && fabs(actual - worker->current.seconds) < .0005;
    post_frame(worker, pixels, output_width, output_height,
        isfinite(actual) ? actual : worker->current.seconds, refined || already_exact, NULL);
    g_bytes_unref(pixels);
    worker->fast_delivered = TRUE;
    if (refined || already_exact) worker->phase = PHASE_DONE;
    else {
        worker->phase = PHASE_REFINE_WAIT;
        worker->refine_at = worker->current.changed_at + REFINE_DELAY_US;
    }
}

static void process_event(DecoderWorker *worker, const mpv_event *event)
{
    /* Command completion precedes execution of the queued load/seek. Ignore
     * earlier queued notifications so they cannot label an old frame with a
     * newer request token after rapid pointer movement or a source change. */
    if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->reply_userdata == worker->load_id &&
        worker->phase == PHASE_LOADING) {
        if (event->error < 0) report_failure(worker, mpv_error_string(event->error));
        else worker->load_acknowledged = TRUE;
    } else if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY && event->reply_userdata == worker->seek_id &&
               (worker->phase == PHASE_FAST || worker->phase == PHASE_EXACT)) {
        if (event->error < 0) report_failure(worker, mpv_error_string(event->error));
        else worker->seek_acknowledged = TRUE;
    } else if (event->event_id == MPV_EVENT_START_FILE && worker->phase == PHASE_LOADING &&
               worker->load_acknowledged) {
        worker->saw_start = TRUE;
    } else if (event->event_id == MPV_EVENT_FILE_LOADED && worker->phase == PHASE_LOADING && worker->saw_start) {
        worker->loaded = worker->file_ready = TRUE;
        gboolean changed;
        if (!select_stream(worker, &changed)) report_failure(worker, "The selected video stream is unavailable.");
        else if (changed || worker->current.seconds < 0) begin_seek(worker, FALSE);
    } else if (event->event_id == MPV_EVENT_SEEK &&
        (worker->phase == PHASE_FAST || worker->phase == PHASE_EXACT) && worker->seek_acknowledged) {
        worker->saw_seek = TRUE;
    } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
        if (worker->phase == PHASE_LOADING && worker->file_ready) capture_frame(worker, FALSE);
        else if (worker->saw_seek && (worker->phase == PHASE_FAST || worker->phase == PHASE_EXACT))
            capture_frame(worker, worker->phase == PHASE_EXACT);
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        const mpv_event_end_file *end = event->data;
        if (end && end->reason == MPV_END_FILE_REASON_ERROR &&
            ((worker->phase == PHASE_LOADING && worker->saw_start) ||
             worker->phase == PHASE_FAST || worker->phase == PHASE_EXACT)) {
            worker->loaded = FALSE;
            report_failure(worker, mpv_error_string(end->error));
        }
    } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
        report_failure(worker, "The preview decoder stopped.");
        close_core(worker);
    }
}

static gboolean update_request(DecoderWorker *worker, gboolean *enabled, gboolean *active,
                                 gint64 *last_activity)
{
    PreviewDecoder *decoder = worker->owner;
    g_mutex_lock(&decoder->mutex);
    if (decoder->stopped) { g_mutex_unlock(&decoder->mutex); return FALSE; }
    *enabled = decoder->enabled;
    *active = decoder->active;
    *last_activity = decoder->last_activity;
    gboolean changed = decoder->revision != worker->seen_revision;
    if (changed) {
        worker->seen_revision = decoder->revision;
        request_copy(&worker->current, &decoder->wanted);
    }
    g_mutex_unlock(&decoder->mutex);
    if (!changed) return TRUE;
    worker->fast_delivered = FALSE;
    if (!*enabled) close_core(worker);
    if (!*active || !*enabled) {
        /* A fast seek can preempt an expensive accurate seek without closing
         * the already-open file and decoder used by the next hover. */
        if (worker->core && worker->loaded && worker->phase == PHASE_EXACT) {
            mpv_set_property_string(worker->core, "hr-seek", "no");
            double seconds = worker->current.seconds;
            mpv_set_property(worker->core, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
        }
        worker->phase = PHASE_IDLE;
        return TRUE;
    }
    if (!worker->core) {
        g_autofree char *error = NULL;
        worker->core = create_core(&error);
        if (!worker->core) {
            report_failure(worker, error ? error : "Could not initialize the preview decoder.");
            return TRUE;
        }
        g_mutex_lock(&decoder->mutex);
        decoder->wake_core = worker->core;
        g_mutex_unlock(&decoder->mutex);
        if (!current_request(worker)) return TRUE;
    }
    if (worker->loaded && worker->source_index == worker->current.ff_index &&
        g_strcmp0(worker->source_key, worker->current.key) == 0)
        begin_seek(worker, FALSE);
    else load_movie(worker);
    return TRUE;
}

static gpointer decoder_thread(gpointer data)
{
    PreviewDecoder *decoder = data;
    DecoderWorker worker = {.owner = decoder};
    for (;;) {
        gboolean enabled, active;
        gint64 last_activity;
        if (!update_request(&worker, &enabled, &active, &last_activity)) break;
        /* A request can change while mpv_initialize() runs before wake_core
         * exists. Resnapshot it rather than sleeping with a lost wakeup. */
        if (active && !current_request(&worker)) continue;
        gint64 now = g_get_monotonic_time();
        if (worker.core && !active && now - last_activity >= WARM_IDLE_US) close_core(&worker);
        if (!worker.core) {
            g_mutex_lock(&decoder->mutex);
            while (!decoder->stopped && decoder->revision == worker.seen_revision)
                g_cond_wait(&decoder->condition, &decoder->mutex);
            g_mutex_unlock(&decoder->mutex);
            continue;
        }
        if (active && current_request(&worker)) {
            if (worker.phase == PHASE_REFINE_WAIT && now >= worker.refine_at) begin_seek(&worker, TRUE);
            else if ((worker.phase == PHASE_LOADING || worker.phase == PHASE_FAST || worker.phase == PHASE_EXACT) &&
                     now >= worker.deadline) report_failure(&worker, "Preview decoding timed out.");
        }
        double timeout = -1;
        if (!active) timeout = MAX(0.001, (last_activity + WARM_IDLE_US - now) / 1000000.0);
        else if (worker.phase == PHASE_REFINE_WAIT) timeout = MAX(0.001, (worker.refine_at - now) / 1000000.0);
        else if (worker.phase == PHASE_LOADING || worker.phase == PHASE_FAST || worker.phase == PHASE_EXACT)
            timeout = MAX(0.001, (worker.deadline - now) / 1000000.0);
        mpv_event *event = mpv_wait_event(worker.core, timeout);
        process_event(&worker, event);
    }
    close_core(&worker);
    request_clear(&worker.current);
    g_free(worker.source_key);
    decoder_unref(decoder);
    return NULL;
}

PreviewDecoder *preview_decoder_new(PreviewDecoderCallback callback, gpointer data)
{
    PreviewDecoder *decoder = g_new0(PreviewDecoder, 1);
    decoder->references = 1;
    decoder->enabled = TRUE;
    decoder->callback = callback;
    decoder->data = data;
    decoder->context = g_main_context_ref_thread_default();
    decoder->last_activity = g_get_monotonic_time();
    g_mutex_init(&decoder->mutex);
    g_cond_init(&decoder->condition);
    return decoder;
}

void preview_decoder_request(PreviewDecoder *decoder, const char *source_key,
                              const char *filename, gint64 ff_index,
                              double seconds, guint64 token)
{
    if (!decoder || !source_key || !filename || !*filename || !isfinite(seconds)) return;
    g_mutex_lock(&decoder->mutex);
    if (decoder->stopped || !decoder->enabled) { g_mutex_unlock(&decoder->mutex); return; }
    request_clear(&decoder->wanted);
    decoder->wanted = (DecoderRequest){.key = g_strdup(source_key), .filename = g_strdup(filename),
        .ff_index = ff_index, .seconds = seconds, .token = token,
        .revision = ++decoder->revision, .changed_at = g_get_monotonic_time()};
    decoder->last_activity = decoder->wanted.changed_at;
    decoder->active = TRUE;
    if (!decoder->worker_started) {
        decoder->worker_started = TRUE;
        GThread *thread = g_thread_new("preview-decoder", decoder_thread, decoder_ref(decoder));
        g_thread_unref(thread);
    }
    if (decoder->wake_core) mpv_wakeup(decoder->wake_core);
    g_cond_signal(&decoder->condition);
    g_mutex_unlock(&decoder->mutex);
}

void preview_decoder_cancel(PreviewDecoder *decoder)
{
    if (!decoder) return;
    g_mutex_lock(&decoder->mutex);
    if (!decoder->stopped) {
        decoder->active = FALSE;
        decoder->wanted.revision = ++decoder->revision;
        decoder->last_activity = g_get_monotonic_time();
        if (decoder->wake_core) mpv_wakeup(decoder->wake_core);
        g_cond_signal(&decoder->condition);
    }
    g_mutex_unlock(&decoder->mutex);
}

void preview_decoder_set_enabled(PreviewDecoder *decoder, gboolean enabled)
{
    if (!decoder) return;
    g_mutex_lock(&decoder->mutex);
    if (!decoder->stopped && decoder->enabled != !!enabled) {
        decoder->enabled = !!enabled;
        decoder->active = FALSE;
        decoder->wanted.revision = ++decoder->revision;
        decoder->last_activity = g_get_monotonic_time();
        if (decoder->wake_core) mpv_wakeup(decoder->wake_core);
        g_cond_signal(&decoder->condition);
    }
    g_mutex_unlock(&decoder->mutex);
}

void preview_decoder_free(PreviewDecoder *decoder)
{
    if (!decoder) return;
    g_mutex_lock(&decoder->mutex);
    decoder->stopped = TRUE;
    decoder->active = FALSE;
    decoder->callback = NULL;
    decoder->data = NULL;
    ++decoder->revision;
    if (decoder->wake_core) mpv_wakeup(decoder->wake_core);
    g_cond_signal(&decoder->condition);
    g_mutex_unlock(&decoder->mutex);
    decoder_unref(decoder);
}
