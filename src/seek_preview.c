#define _GNU_SOURCE
#include "seek_preview.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PREVIEW_COMMAND_BASE (UINT64_C(1) << 48)
#define PREVIEW_COMMAND_LIMIT (PREVIEW_COMMAND_BASE + (UINT64_C(1) << 32))
#define PREVIEW_CAPTURE_LIMIT (1024 * 1024)
#define PREVIEW_CACHE_LIMIT 48
#define PREVIEW_DEBOUNCE_MS 120

typedef struct {
    struct SeekPreview *owner;
    GtkWidget *widget;
    GtkEventController *motion;
    gboolean range;
    gboolean had_tooltip;
} PreviewAnchor;

typedef struct {
    gint64 bucket;
    GdkTexture *texture;
} PreviewFrame;

struct SeekPreview {
    mpv_handle *mpv;
    GPtrArray *anchors;
    PreviewAnchor *hover;
    GtkWidget *popover, *picture, *image_area, *spinner, *label;
    GtkCssProvider *css;
    GdkDisplay *display;
    GQueue cache;
    gboolean enabled, stopped, checked, available, local, inside;
    gboolean target_ready, cancelling, timed_out;
    char *path, *stream, *source_key, *ffmpeg, *request_key;
    double duration, offset, end_guard, seconds, pointer_x;
    gint64 bucket, request_bucket;
    guint generation, request_generation;
    guint debounce_source, timeout_source;
    guint64 serial, request;
};

static void request_frame(SeekPreview *p);

static const mpv_node *field(const mpv_node *map, const char *key)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], key) == 0) return &map->u.list->values[i];
    return NULL;
}

static const char *field_text(const mpv_node *map, const char *key)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_STRING && value->u.string ? value->u.string : "";
}

static gboolean field_flag(const mpv_node *map, const char *key)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_FLAG && value->u.flag;
}

static gint64 field_integer(const mpv_node *map, const char *key, gint64 fallback)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_INT64 ? value->u.int64 : fallback;
}

static double property_number(SeekPreview *p, const char *name, double fallback)
{
    double value;
    return mpv_get_property(p->mpv, name, MPV_FORMAT_DOUBLE, &value) >= 0 && isfinite(value) ? value : fallback;
}

static gboolean property_flag(SeekPreview *p, const char *name, gboolean fallback)
{
    int value;
    return mpv_get_property(p->mpv, name, MPV_FORMAT_FLAG, &value) >= 0 ? value != 0 : fallback;
}

static void cache_frame_free(gpointer data)
{
    PreviewFrame *frame = data;
    g_clear_object(&frame->texture);
    g_free(frame);
}

static void clear_cache(SeekPreview *p)
{
    if (p->picture) gtk_picture_set_paintable(GTK_PICTURE(p->picture), NULL);
    g_queue_clear_full(&p->cache, cache_frame_free);
}

static PreviewFrame *cached_frame(SeekPreview *p, gint64 bucket)
{
    for (GList *item = p->cache.head; item; item = item->next) {
        PreviewFrame *frame = item->data;
        if (frame->bucket == bucket) {
            g_queue_unlink(&p->cache, item);
            g_queue_push_head_link(&p->cache, item);
            return frame;
        }
    }
    return NULL;
}

static void cache_frame(SeekPreview *p, gint64 bucket, GdkTexture *texture)
{
    PreviewFrame *existing = cached_frame(p, bucket);
    if (existing) return;
    PreviewFrame *frame = g_new0(PreviewFrame, 1);
    frame->bucket = bucket;
    frame->texture = texture ? g_object_ref(texture) : NULL;
    g_queue_push_head(&p->cache, frame);
    while (g_queue_get_length(&p->cache) > PREVIEW_CACHE_LIMIT)
        cache_frame_free(g_queue_pop_tail(&p->cache));
}

static void cancel_debounce(SeekPreview *p)
{
    if (p->debounce_source) g_source_remove(p->debounce_source);
    p->debounce_source = 0;
}

static void cancel_timeout(SeekPreview *p)
{
    if (p->timeout_source) g_source_remove(p->timeout_source);
    p->timeout_source = 0;
}

static void abort_request(SeekPreview *p)
{
    if (p->request && !p->cancelling) {
        p->cancelling = TRUE;
        mpv_abort_async_command(p->mpv, p->request);
    }
    /* Cancellation is asynchronous. Keep the ID until its reply so a second
     * decoder process cannot start while the old one is still terminating. */
}

void seek_preview_hide(SeekPreview *p)
{
    if (!p) return;
    if (p->hover && p->hover->had_tooltip) {
        gtk_widget_set_has_tooltip(p->hover->widget, TRUE);
        p->hover->had_tooltip = FALSE;
    }
    p->inside = p->target_ready = FALSE;
    p->hover = NULL;
    p->checked = FALSE;
    cancel_debounce(p);
    cancel_timeout(p);
    abort_request(p);
    if (p->popover) gtk_popover_popdown(GTK_POPOVER(p->popover));
}

static void invalidate_source(SeekPreview *p)
{
    seek_preview_hide(p);
    ++p->generation;
    p->available = p->local = FALSE;
    p->duration = p->offset = 0;
    g_clear_pointer(&p->path, g_free);
    g_clear_pointer(&p->stream, g_free);
    g_clear_pointer(&p->source_key, g_free);
    clear_cache(p);
}

static char *local_filename(SeekPreview *p, const char *name)
{
    if (!name || !*name) return NULL;
    g_autofree char *decoded = g_str_has_prefix(name, "file://") ? g_filename_from_uri(name, NULL, NULL) : NULL;
    if (strstr(name, "://") && !decoded) return NULL;
    char *working = mpv_get_property_string(p->mpv, "working-directory");
    g_autofree char *absolute = g_canonicalize_filename(decoded ? decoded : name,
        working && g_path_is_absolute(working) ? working : NULL);
    mpv_free(working);
    return realpath(absolute, NULL);
}

static void forget_local_source(SeekPreview *p)
{
    if (p->source_key) {
        ++p->generation;
        clear_cache(p);
        abort_request(p);
    }
    g_clear_pointer(&p->source_key, g_free);
    g_clear_pointer(&p->path, g_free);
    g_clear_pointer(&p->stream, g_free);
}

static void check_source(SeekPreview *p)
{
    if (p->checked || p->stopped) return;
    p->checked = TRUE;
    p->available = p->local = FALSE;
    double duration = property_number(p, "duration", 0);
    if (duration <= 0 || duration >= (double)G_MAXINT64 / 2) { forget_local_source(p); return; }
    mpv_node video = {0};
    if (mpv_get_property(p->mpv, "current-tracks/video", MPV_FORMAT_NODE, &video) < 0 ||
        video.format != MPV_FORMAT_NODE_MAP || field_flag(&video, "image") || field_flag(&video, "albumart")) {
        mpv_free_node_contents(&video);
        forget_local_source(p);
        return;
    }
    p->available = TRUE;
    p->duration = duration;
    char *main_path = mpv_get_property_string(p->mpv, "path");
    const char *external = field_text(&video, "external-filename");
    g_autofree char *filename = local_filename(p, *external ? external : main_path);
    mpv_free(main_path);
    gint64 index = field_integer(&video, "ff-index", -1);
    mpv_free_node_contents(&video);
    GStatBuf info;
    if (!filename || g_stat(filename, &info) < 0 || !S_ISREG(info.st_mode)) { forget_local_source(p); return; }
    double offset = property_flag(p, "rebase-start-time", TRUE) ? property_number(p, "demuxer-start-time", 0) : 0;
    double fps = property_number(p, "container-fps", 0);
    p->end_guard = fps > 0 ? MAX(.1, 1 / fps) : .1;
    g_autofree char *stream = index >= 0 ? g_strdup_printf("0:%" G_GINT64_FORMAT, index) : g_strdup("0:v:0");
    char offset_text[G_ASCII_DTOSTR_BUF_SIZE], duration_text[G_ASCII_DTOSTR_BUF_SIZE];
    char guard_text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(offset_text, sizeof(offset_text), offset);
    g_ascii_dtostr(duration_text, sizeof(duration_text), duration);
    g_ascii_dtostr(guard_text, sizeof(guard_text), p->end_guard);
    g_autofree char *key = g_strdup_printf("%s\n%s\n%s\n%s\n%s\n%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT
        ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%ld", filename, stream, offset_text, duration_text, guard_text,
        (guint64)info.st_dev, (guint64)info.st_ino, (gint64)info.st_size,
        (gint64)info.st_mtim.tv_sec, info.st_mtim.tv_nsec);
    if (g_strcmp0(key, p->source_key) != 0) {
        ++p->generation;
        clear_cache(p);
        abort_request(p);
        g_free(p->source_key); p->source_key = g_strdup(key);
    }
    g_free(p->path); p->path = g_steal_pointer(&filename);
    g_free(p->stream); p->stream = g_steal_pointer(&stream);
    p->offset = offset;
    p->local = TRUE;
    if (!p->ffmpeg) p->ffmpeg = g_find_program_in_path("ffmpeg");
}

static GtkWidget *css_child(GtkWidget *parent, const char *name)
{
    for (GtkWidget *child = gtk_widget_get_first_child(parent); child; child = gtk_widget_get_next_sibling(child))
        if (g_str_equal(gtk_widget_get_css_name(child), name)) return child;
    return NULL;
}

static double fraction_at(PreviewAnchor *anchor, double x)
{
    GtkWidget *widget = anchor->widget;
    if (!anchor->range) return CLAMP((x - 1) / MAX(1, gtk_widget_get_width(widget) - 2), 0, 1);
    GtkRange *range = GTK_RANGE(widget);
    GtkAdjustment *adjustment = gtk_range_get_adjustment(range);
    double lower = gtk_adjustment_get_lower(adjustment), upper = gtk_adjustment_get_upper(adjustment);
    double page = gtk_adjustment_get_page_size(adjustment), span = upper - lower - page;
    GtkWidget *trough = css_child(widget, "trough"), *slider = trough ? css_child(trough, "slider") : NULL;
    GdkRectangle rect;
    gtk_range_get_range_rect(range, &rect);
    double origin = rect.x, travel = rect.width;
    gboolean inverted = gtk_range_get_inverted(range) ^
        (gtk_range_get_flippable(range) && gtk_widget_get_direction(widget) == GTK_TEXT_DIR_RTL);
    if (trough && slider && span > 0) {
        int start, end, minimum;
        gtk_range_get_slider_range(range, &start, &end);
        gtk_widget_measure(slider, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
        double center = (start + end) / 2.0, length = gtk_widget_get_width(trough);
        graphene_rect_t thumb;
        if (gtk_widget_compute_bounds(slider, widget, &thumb)) center = thumb.origin.x + thumb.size.width / 2;
        double slider_length = minimum;
        if (!gtk_range_get_slider_size_fixed(range) && upper > lower)
            slider_length = MAX(slider_length, floor(length * page / (upper - lower)));
        travel = MAX(0, length - CLAMP(slider_length, 0, length));
        double value_fraction = CLAMP((gtk_adjustment_get_value(adjustment) - lower) / span, 0, 1);
        double offset = floor(travel * value_fraction);
        origin = center - (inverted ? travel - offset : offset);
    }
    double fraction = travel > 0 ? CLAMP((x - origin) / travel, 0, 1) : 0;
    return inverted ? 1 - fraction : fraction;
}

static char *timestamp(double seconds)
{
    gint64 value = (gint64)floor(MAX(0, seconds));
    if (value >= 3600)
        return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
            value / 3600, value / 60 % 60, value % 60);
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, value / 60, value % 60);
}

static void create_popover(SeekPreview *p)
{
    if (p->popover) return;
    p->popover = g_object_ref_sink(gtk_popover_new());
    gtk_popover_set_autohide(GTK_POPOVER(p->popover), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(p->popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(p->popover), GTK_POS_TOP);
    gtk_popover_set_offset(GTK_POPOVER(p->popover), 0, -6);
    gtk_widget_set_can_target(p->popover, FALSE);
    gtk_widget_set_focusable(p->popover, FALSE);
    gtk_widget_add_css_class(p->popover, "seek-preview");
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_can_target(content, FALSE);
    p->image_area = gtk_overlay_new();
    gtk_widget_set_size_request(p->image_area, 256, 144);
    gtk_widget_set_can_target(p->image_area, FALSE);
    gtk_widget_set_overflow(p->image_area, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(p->image_area, "seek-preview-image");
    p->picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(p->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(p->picture), TRUE);
    gtk_widget_set_can_target(p->picture, FALSE);
    gtk_widget_set_hexpand(p->picture, TRUE);
    gtk_widget_set_vexpand(p->picture, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(p->image_area), p->picture);
    p->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(p->spinner, 24, 24);
    gtk_widget_set_halign(p->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(p->spinner, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(p->image_area), p->spinner);
    gtk_box_append(GTK_BOX(content), p->image_area);
    p->label = gtk_label_new("");
    gtk_widget_set_can_target(p->label, FALSE);
    gtk_widget_add_css_class(p->label, "seek-preview-time");
    gtk_box_append(GTK_BOX(content), p->label);
    gtk_popover_set_child(GTK_POPOVER(p->popover), content);
    p->css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p->css,
        "popover.seek-preview > contents { padding: 6px; border-radius: 11px;"
        "background: @headerbar_bg_color; color: @headerbar_fg_color;"
        "border: 1px solid alpha(@window_fg_color,.13); box-shadow: 0 4px 12px alpha(black,.25); }"
        ".seek-preview-image { background: alpha(@window_fg_color,.06); border-radius: 6px; }"
        ".seek-preview-time { font-family: 'Adwaita Sans', sans-serif; font-variant-numeric: tabular-nums;"
        "font-weight: 600; padding: 2px 6px; }");
    p->display = g_object_ref(gtk_widget_get_display(p->hover->widget));
    gtk_style_context_add_provider_for_display(p->display, GTK_STYLE_PROVIDER(p->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void show_card(SeekPreview *p, PreviewFrame *frame, gboolean loading)
{
    if (!p->enabled || p->stopped || !p->inside || !p->hover || !p->available ||
        !gtk_widget_get_mapped(p->hover->widget)) return;
    create_popover(p);
    GtkWidget *anchor = p->hover->widget;
    if (gtk_widget_get_parent(p->popover) != anchor) {
        gtk_popover_popdown(GTK_POPOVER(p->popover));
        if (gtk_widget_get_parent(p->popover)) gtk_widget_unparent(p->popover);
        gtk_widget_set_parent(p->popover, anchor);
    }
    GdkRectangle point = {.x = (int)CLAMP(p->pointer_x, 0, gtk_widget_get_width(anchor)), .y = 0,
        .width = 1, .height = MAX(1, gtk_widget_get_height(anchor))};
    gtk_popover_set_pointing_to(GTK_POPOVER(p->popover), &point);
    g_autofree char *time = timestamp(p->seconds);
    gtk_label_set_text(GTK_LABEL(p->label), time);
    gtk_picture_set_paintable(GTK_PICTURE(p->picture), frame && frame->texture ? GDK_PAINTABLE(frame->texture) : NULL);
    gtk_widget_set_visible(p->image_area, loading || (frame && frame->texture));
    gtk_widget_set_visible(p->spinner, loading);
    if (loading) gtk_spinner_start(GTK_SPINNER(p->spinner));
    else gtk_spinner_stop(GTK_SPINNER(p->spinner));
    gtk_popover_popup(GTK_POPOVER(p->popover));
    gtk_popover_present(GTK_POPOVER(p->popover));
}

static gboolean request_timeout(gpointer data)
{
    SeekPreview *p = data;
    p->timeout_source = 0;
    if (p->request && !p->stopped) {
        p->timed_out = TRUE;
        abort_request(p);
    }
    return G_SOURCE_REMOVE;
}

static void request_frame(SeekPreview *p)
{
    if (p->stopped || !p->enabled || !p->inside || !p->target_ready || !p->hover || !p->available) return;
    PreviewFrame *cached = cached_frame(p, p->bucket);
    if (cached) { show_card(p, cached, FALSE); return; }
    if (!p->local || !p->ffmpeg) { show_card(p, NULL, FALSE); return; }
    show_card(p, NULL, TRUE);
    if (p->request) {
        if (p->request_bucket != p->bucket || p->request_generation != p->generation) abort_request(p);
        return;
    }
    double second = MIN(p->bucket / 2.0, MAX(0, p->duration - p->end_guard)) + p->offset;
    char time[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(time, sizeof(time), "%.3f", second);
    const char *arguments[] = {p->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin",
        "-threads", "1", "-filter_threads", "1", "-protocol_whitelist", "file,pipe",
        "-ss", time, "-seek_timestamp", "1", "-i", p->path, "-map", p->stream,
        "-frames:v", "1", "-an", "-sn", "-dn", "-vf",
        "scale=w='max(2,trunc(min(256,144*dar)/2)*2)':h='max(2,trunc(min(144,256/dar)/2)*2)',setsar=1",
        "-threads", "1", "-pix_fmt", "rgb24", "-c:v", "png", "-f", "image2pipe", "pipe:1", NULL};
    int count = 0;
    while (arguments[count]) ++count;
    g_autofree mpv_node *args = g_new0(mpv_node, count);
    for (int i = 0; i < count; ++i) args[i] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = (char *)arguments[i]};
    mpv_node_list arg_list = {.num = count, .values = args};
    char *keys[] = {"name", "args", "playback_only", "capture_stdout", "capture_stderr", "capture_size"};
    mpv_node values[] = {
        {.format = MPV_FORMAT_STRING, .u.string = "subprocess"},
        {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &arg_list},
        {.format = MPV_FORMAT_FLAG, .u.flag = 1},
        {.format = MPV_FORMAT_FLAG, .u.flag = 1},
        {.format = MPV_FORMAT_FLAG, .u.flag = 1},
        {.format = MPV_FORMAT_INT64, .u.int64 = PREVIEW_CAPTURE_LIMIT}
    };
    mpv_node_list map = {.num = G_N_ELEMENTS(keys), .keys = keys, .values = values};
    mpv_node command = {.format = MPV_FORMAT_NODE_MAP, .u.list = &map};
    p->request = PREVIEW_COMMAND_BASE + ++p->serial;
    p->request_bucket = p->bucket;
    p->request_generation = p->generation;
    g_free(p->request_key); p->request_key = g_strdup(p->source_key);
    p->cancelling = p->timed_out = FALSE;
    if (mpv_command_node_async(p->mpv, p->request, &command) < 0) {
        p->request = 0;
        cache_frame(p, p->bucket, NULL);
        show_card(p, cached_frame(p, p->bucket), FALSE);
    } else p->timeout_source = g_timeout_add_seconds(5, request_timeout, p);
}

static gboolean debounce_finished(gpointer data)
{
    SeekPreview *p = data;
    p->debounce_source = 0;
    p->target_ready = TRUE;
    request_frame(p);
    return G_SOURCE_REMOVE;
}

static void hover_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data)
{
    (void)controller; (void)y;
    PreviewAnchor *anchor = data;
    SeekPreview *p = anchor->owner;
    if (p->stopped || !p->enabled || !gtk_widget_get_mapped(anchor->widget)) return;
    if (p->hover != anchor) {
        seek_preview_hide(p);
        p->hover = anchor;
    }
    check_source(p);
    if (!p->available) {
        if (p->popover) gtk_popover_popdown(GTK_POPOVER(p->popover));
        return;
    }
    if (gtk_widget_get_has_tooltip(anchor->widget)) {
        anchor->had_tooltip = TRUE;
        gtk_widget_set_has_tooltip(anchor->widget, FALSE);
    }
    p->inside = TRUE;
    p->pointer_x = x;
    p->seconds = fraction_at(anchor, x) * p->duration;
    gint64 bucket = (gint64)floor(p->seconds * 2);
    gboolean changed = bucket != p->bucket;
    p->bucket = bucket;
    PreviewFrame *frame = cached_frame(p, bucket);
    if (frame) {
        cancel_debounce(p);
        if (p->request && p->request_bucket != bucket) abort_request(p);
        p->target_ready = TRUE;
        show_card(p, frame, FALSE);
        return;
    }
    if (changed || (!p->target_ready && !p->debounce_source)) {
        p->target_ready = FALSE;
        cancel_debounce(p);
        p->debounce_source = g_timeout_add(PREVIEW_DEBOUNCE_MS, debounce_finished, p);
    }
    if (p->popover && gtk_widget_get_visible(p->popover))
        show_card(p, NULL, p->local && p->ffmpeg != NULL);
    if (p->target_ready) request_frame(p);
}

static void hover_leave(GtkEventControllerMotion *controller, gpointer data)
{
    (void)controller;
    PreviewAnchor *anchor = data;
    if (anchor->owner->hover == anchor) seek_preview_hide(anchor->owner);
}

static void anchor_unmapped(GtkWidget *widget, gpointer data)
{
    (void)widget;
    PreviewAnchor *anchor = data;
    if (anchor->owner->hover == anchor) seek_preview_hide(anchor->owner);
}

SeekPreview *seek_preview_new(mpv_handle *mpv)
{
    g_return_val_if_fail(mpv != NULL, NULL);
    SeekPreview *p = g_new0(SeekPreview, 1);
    p->mpv = mpv;
    p->enabled = TRUE;
    p->bucket = -1;
    p->anchors = g_ptr_array_new();
    return p;
}

void seek_preview_attach(SeekPreview *p, GtkWidget *timeline, gboolean is_range)
{
    if (!p || p->stopped || !GTK_IS_WIDGET(timeline) || (is_range && !GTK_IS_RANGE(timeline))) return;
    for (guint i = 0; i < p->anchors->len; ++i)
        if (((PreviewAnchor *)g_ptr_array_index(p->anchors, i))->widget == timeline) return;
    PreviewAnchor *anchor = g_new0(PreviewAnchor, 1);
    anchor->owner = p;
    anchor->widget = g_object_ref(timeline);
    anchor->range = is_range;
    GtkEventController *motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_name(motion, "timeline-preview-motion");
    g_signal_connect(motion, "enter", G_CALLBACK(hover_motion), anchor);
    g_signal_connect(motion, "motion", G_CALLBACK(hover_motion), anchor);
    g_signal_connect(motion, "leave", G_CALLBACK(hover_leave), anchor);
    gtk_widget_add_controller(timeline, motion);
    anchor->motion = g_object_ref(motion);
    g_signal_connect(timeline, "unmap", G_CALLBACK(anchor_unmapped), anchor);
    g_ptr_array_add(p->anchors, anchor);
}

void seek_preview_set_enabled(SeekPreview *p, gboolean enabled)
{
    if (!p || p->stopped || p->enabled == !!enabled) return;
    p->enabled = !!enabled;
    if (!p->enabled) {
        seek_preview_hide(p);
        clear_cache(p);
    }
}

gboolean seek_preview_handle_event(SeekPreview *p, const mpv_event *event)
{
    if (!p || p->stopped || !event) return FALSE;
    if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE ||
        event->event_id == MPV_EVENT_FILE_LOADED || event->event_id == MPV_EVENT_VIDEO_RECONFIG) {
        invalidate_source(p);
        return FALSE;
    }
    if (event->event_id != MPV_EVENT_COMMAND_REPLY || event->reply_userdata < PREVIEW_COMMAND_BASE ||
        event->reply_userdata >= PREVIEW_COMMAND_LIMIT) return FALSE;
    if (event->reply_userdata != p->request) return TRUE;
    cancel_timeout(p);
    guint generation = p->request_generation;
    gint64 bucket = p->request_bucket;
    gboolean cancelled = p->cancelling, timed_out = p->timed_out;
    p->request = 0;
    p->cancelling = p->timed_out = FALSE;
    gboolean same_source = generation == p->generation && g_strcmp0(p->request_key, p->source_key) == 0;
    if (p->enabled && same_source && (!cancelled || timed_out)) {
        const mpv_event_command *reply = event->data;
        const mpv_node *result = reply ? &reply->result : NULL;
        const mpv_node *output = field(result, "stdout");
        const void *data = NULL;
        gsize size = 0;
        if (output && output->format == MPV_FORMAT_BYTE_ARRAY && output->u.ba) {
            data = output->u.ba->data; size = output->u.ba->size;
        }
        GdkTexture *texture = NULL;
        guint32 png_width = 0, png_height = 0;
        if (data && size >= 24 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0 &&
            memcmp((const guint8 *)data + 12, "IHDR", 4) == 0) {
            memcpy(&png_width, (const guint8 *)data + 16, sizeof(png_width));
            memcpy(&png_height, (const guint8 *)data + 20, sizeof(png_height));
            png_width = GUINT32_FROM_BE(png_width); png_height = GUINT32_FROM_BE(png_height);
        }
        if (event->error >= 0 && field_integer(result, "status", -1) == 0 && data &&
            size >= 24 && size < PREVIEW_CAPTURE_LIMIT && png_width > 0 && png_width <= 256 &&
            png_height > 0 && png_height <= 144) {
            g_autoptr(GBytes) bytes = g_bytes_new(data, size);
            texture = gdk_texture_new_from_bytes(bytes, NULL);
            if (texture && (gdk_texture_get_width(texture) > 256 || gdk_texture_get_height(texture) > 144))
                g_clear_object(&texture);
        }
        cache_frame(p, bucket, texture);
        g_clear_object(&texture);
        if (p->inside && p->hover && p->bucket == bucket) show_card(p, cached_frame(p, bucket), FALSE);
    }
    g_clear_pointer(&p->request_key, g_free);
    if (p->inside && p->target_ready) request_frame(p);
    return TRUE;
}

void seek_preview_shutdown(SeekPreview *p)
{
    if (!p || p->stopped) return;
    seek_preview_hide(p);
    p->stopped = TRUE;
    for (guint i = 0; i < p->anchors->len; ++i) {
        PreviewAnchor *anchor = g_ptr_array_index(p->anchors, i);
        g_signal_handlers_disconnect_by_data(anchor->widget, anchor);
        g_signal_handlers_disconnect_by_data(anchor->motion, anchor);
        gtk_widget_remove_controller(anchor->widget, anchor->motion);
    }
    clear_cache(p);
}

void seek_preview_free(SeekPreview *p)
{
    if (!p) return;
    seek_preview_shutdown(p);
    if (p->popover) {
        if (gtk_widget_get_parent(p->popover)) gtk_widget_unparent(p->popover);
        g_clear_object(&p->popover);
    }
    if (p->css && p->display)
        gtk_style_context_remove_provider_for_display(p->display, GTK_STYLE_PROVIDER(p->css));
    g_clear_object(&p->css); g_clear_object(&p->display);
    for (guint i = 0; i < p->anchors->len; ++i) {
        PreviewAnchor *anchor = g_ptr_array_index(p->anchors, i);
        g_clear_object(&anchor->motion); g_clear_object(&anchor->widget); g_free(anchor);
    }
    g_ptr_array_unref(p->anchors);
    g_free(p->path); g_free(p->stream); g_free(p->source_key); g_free(p->ffmpeg); g_free(p->request_key);
    g_free(p);
}
