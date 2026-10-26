#define _GNU_SOURCE
#include "seek_preview.h"
#include "preview_decoder.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PREVIEW_CACHE_LIMIT 48
#define PREVIEW_DEBOUNCE_MS 16
#define PREVIEW_HOVER_MARGIN 16
#define DURATION_OBSERVE_ID (UINT64_C(1) << 56)
#define DURATION_QUERY_BASE (DURATION_OBSERVE_ID + (UINT64_C(1) << 32))
#define DURATION_QUERY_LIMIT (DURATION_OBSERVE_ID + (UINT64_C(1) << 33))

typedef struct { GtkPopover parent; } PreviewPopover;
typedef struct { GtkPopoverClass parent; } PreviewPopoverClass;

G_DEFINE_TYPE(PreviewPopover, preview_popover, GTK_TYPE_POPOVER)

static void preview_popover_pass_through(GtkWidget *widget)
{
    /* can-target only affects GTK picking. A popup still intercepts the
     * Wayland pointer unless its native input region is empty. GtkPopover
     * restores its input shape during every allocation, so clear it after
     * the parent has allocated, including later moves and frame changes. */
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (surface) {
        cairo_region_t *empty = cairo_region_create();
        gdk_surface_set_input_region(surface, empty);
        cairo_region_destroy(empty);
    }
}

static void preview_popover_realize(GtkWidget *widget)
{
    GTK_WIDGET_CLASS(preview_popover_parent_class)->realize(widget);
    preview_popover_pass_through(widget);
}

static void preview_popover_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
    GTK_WIDGET_CLASS(preview_popover_parent_class)->size_allocate(widget, width, height, baseline);
    preview_popover_pass_through(widget);
}

static void preview_popover_class_init(PreviewPopoverClass *klass)
{
    GTK_WIDGET_CLASS(klass)->realize = preview_popover_realize;
    GTK_WIDGET_CLASS(klass)->size_allocate = preview_popover_size_allocate;
}

static void preview_popover_init(PreviewPopover *popover)
{
    (void)popover;
}

typedef struct {
    struct SeekPreview *owner;
    GtkWidget *widget;
    GtkWidget *area;
    GtkEventController *motion;
    gboolean range;
    gboolean had_tooltip;
} PreviewAnchor;

typedef struct {
    gint64 bucket;
    GdkTexture *texture;
    gboolean refined;
} PreviewFrame;

struct SeekPreview {
    mpv_handle *mpv;
    PreviewDecoder *decoder;
    GPtrArray *anchors;
    PreviewAnchor *hover;
    GtkWidget *popover, *picture, *image_area, *spinner, *label;
    GtkCssProvider *css;
    GdkDisplay *display;
    GQueue cache;
    gboolean enabled, stopped, checked, available, local, inside, nvdec;
    gboolean target_ready;
    gboolean duration_known, duration_loading;
    char *path, *stream, *source_key, *request_key;
    gint64 ff_index;
    double duration, offset, end_guard, seconds, pointer_x;
    double duration_cache;
    gint64 bucket, request_bucket;
    guint generation, request_generation;
    guint debounce_source;
    guint64 serial, request;
    guint64 duration_request, duration_serial;
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

static void cache_frame(SeekPreview *p, gint64 bucket, GdkTexture *texture, gboolean refined)
{
    PreviewFrame *existing = cached_frame(p, bucket);
    if (existing) {
        if (refined || !existing->refined) {
            if (texture || !existing->texture) g_set_object(&existing->texture, texture);
            existing->refined = refined;
        }
        return;
    }
    PreviewFrame *frame = g_new0(PreviewFrame, 1);
    frame->bucket = bucket;
    frame->refined = refined;
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

static void abort_request(SeekPreview *p)
{
    preview_decoder_cancel(p->decoder);
    p->request = 0;
    g_clear_pointer(&p->request_key, g_free);
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
    double duration = p->duration_known ? p->duration_cache
        : p->enabled ? property_number(p, "duration", 0) : 0;
    if (duration <= 0 || duration >= (double)G_MAXINT64 / 2) { forget_local_source(p); return; }
    p->duration_known = TRUE;
    p->duration_cache = duration;
    p->available = TRUE;
    p->duration = duration;
    /* Timestamp hovers need only duration. Keep source/track inspection and
     * decoder resources out of the time-only path, including audio files. */
    if (!p->enabled) { forget_local_source(p); return; }
    mpv_node video = {0};
    if (mpv_get_property(p->mpv, "current-tracks/video", MPV_FORMAT_NODE, &video) < 0 ||
        video.format != MPV_FORMAT_NODE_MAP || field_flag(&video, "image") || field_flag(&video, "albumart")) {
        mpv_free_node_contents(&video);
        forget_local_source(p);
        return;
    }
    if (property_flag(p, "demuxer-via-network", FALSE)) {
        mpv_free_node_contents(&video);
        forget_local_source(p);
        return;
    }
    char *main_path = mpv_get_property_string(p->mpv, "path");
    const char *external = field_text(&video, "external-filename");
    g_autofree char *filename = local_filename(p, *external ? external : main_path);
    mpv_free(main_path);
    gint64 index = field_integer(&video, "ff-index", -1);
    p->ff_index = index;
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
    char *hwdec = mpv_get_property_string(p->mpv, "hwdec-current");
    p->nvdec = hwdec && g_str_has_prefix(hwdec, "nvdec");
    preview_decoder_set_hardware(p->decoder, p->nvdec);
    mpv_free(hwdec);
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
    p->popover = g_object_ref_sink(g_object_new(preview_popover_get_type(), NULL));
    gtk_popover_set_autohide(GTK_POPOVER(p->popover), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(p->popover), TRUE);
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
        "popover.seek-preview > arrow { background: @headerbar_bg_color;"
        "border: 1px solid alpha(@window_fg_color,.13); }"
        "popover.seek-preview.time-only > contents { padding: 4px; border-radius: 9px; }"
        "popover.seek-preview.time-only .seek-preview-time { padding: 0 3px; font-weight: 500; }"
        ".seek-preview-image { background: alpha(@window_fg_color,.06); border-radius: 6px; }"
        ".seek-preview-time { font-family: 'Adwaita Sans', sans-serif; font-variant-numeric: tabular-nums;"
        "font-weight: 600; padding: 2px 6px; }");
    p->display = g_object_ref(gtk_widget_get_display(p->hover->widget));
    gtk_style_context_add_provider_for_display(p->display, GTK_STYLE_PROVIDER(p->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void show_card(SeekPreview *p, PreviewFrame *frame, gboolean loading)
{
    if (p->stopped || !p->inside || !p->hover || !p->available ||
        !gtk_widget_get_mapped(p->hover->widget)) return;
    if (!p->enabled) { frame = NULL; loading = FALSE; }
    create_popover(p);
    if (loading || (frame && frame->texture)) gtk_widget_remove_css_class(p->popover, "time-only");
    else gtk_widget_add_css_class(p->popover, "time-only");
    GtkWidget *anchor = p->hover->widget;
    if (gtk_widget_get_parent(p->popover) != anchor) {
        gtk_popover_popdown(GTK_POPOVER(p->popover));
        if (gtk_widget_get_parent(p->popover)) gtk_widget_unparent(p->popover);
        gtk_widget_set_parent(p->popover, anchor);
    }
    GdkRectangle point = {.x = (int)CLAMP(p->pointer_x, 0, gtk_widget_get_width(anchor)), .y = 0,
        .width = 1, .height = MAX(1, gtk_widget_get_height(anchor))};
    GdkRectangle previous;
    if (!gtk_popover_get_pointing_to(GTK_POPOVER(p->popover), &previous) ||
        !gdk_rectangle_equal(&point, &previous))
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

static void show_pending_frame(SeekPreview *p)
{
    /* Keep the last thumbnail visible while the persistent decoder catches
     * up, instead of flashing an empty card between nearby hover targets. */
    GdkPaintable *paintable = p->picture ? gtk_picture_get_paintable(GTK_PICTURE(p->picture)) : NULL;
    PreviewFrame previous = {.texture = GDK_IS_TEXTURE(paintable) ? GDK_TEXTURE(paintable) : NULL};
    show_card(p, previous.texture ? &previous : NULL, previous.texture == NULL);
}

static void decoder_frame_ready(const PreviewDecoderFrame *frame, gpointer data)
{
    SeekPreview *p = data;
    if (p->stopped || !p->enabled || !p->inside || frame->token != p->request ||
        p->request_generation != p->generation || g_strcmp0(p->request_key, p->source_key) != 0) return;
    GdkTexture *texture = NULL;
    if (frame->pixels && frame->width > 0 && frame->width <= 256 && frame->height > 0 &&
        frame->height <= 144 && frame->stride >= frame->width * 4 && frame->stride <= 4096 &&
        g_bytes_get_size(frame->pixels) >= (gsize)frame->stride * frame->height)
        texture = gdk_memory_texture_new(frame->width, frame->height,
            GDK_MEMORY_B8G8R8A8, frame->pixels, (gsize)frame->stride);
    if (texture || frame->refined) {
        cache_frame(p, p->request_bucket, texture, frame->refined);
        g_clear_object(&texture);
        if (p->hover && p->bucket == p->request_bucket)
            show_card(p, cached_frame(p, p->bucket), FALSE);
    }
    if (frame->refined) {
        p->request = 0;
        g_clear_pointer(&p->request_key, g_free);
    }
}

static void request_frame(SeekPreview *p)
{
    if (p->stopped || !p->inside || !p->target_ready || !p->hover || !p->available) return;
    if (!p->enabled) { show_card(p, NULL, FALSE); return; }
    PreviewFrame *cached = cached_frame(p, p->bucket);
    if (cached && cached->refined) { show_card(p, cached, FALSE); return; }
    if (!p->local) { show_card(p, NULL, FALSE); return; }
    if (cached) show_card(p, cached, FALSE);
    else show_pending_frame(p);
    if (p->request && p->request_bucket == p->bucket && p->request_generation == p->generation) return;
    p->request = ++p->serial;
    p->request_bucket = p->bucket;
    p->request_generation = p->generation;
    g_free(p->request_key); p->request_key = g_strdup(p->source_key);
    double second = MIN(p->bucket / 2.0, MAX(0, p->duration - p->end_guard)) + p->offset;
    if (!p->decoder) {
        p->decoder = preview_decoder_new(decoder_frame_ready, p);
        preview_decoder_set_hardware(p->decoder, p->nvdec);
    }
    preview_decoder_request(p->decoder, p->source_key, p->path, p->ff_index, second, p->request);
}

static gboolean debounce_finished(gpointer data)
{
    SeekPreview *p = data;
    p->debounce_source = 0;
    p->target_ready = TRUE;
    request_frame(p);
    return G_SOURCE_REMOVE;
}

static gboolean anchor_coordinates(PreviewAnchor *anchor, double area_x, double area_y,
                                    double *x, double *y)
{
    if (!gtk_widget_get_mapped(anchor->widget)) return FALSE;
    graphene_point_t point = GRAPHENE_POINT_INIT(area_x, area_y), local;
    if (!gtk_widget_compute_point(anchor->area, anchor->widget, &point, &local)) return FALSE;
    *x = local.x;
    *y = local.y;
    return TRUE;
}

static gboolean over_another_control(PreviewAnchor *anchor, double x, double y)
{
    GtkWidget *picked = gtk_widget_pick(anchor->area, x, y, GTK_PICK_DEFAULT);
    for (GtkWidget *widget = picked; widget && widget != anchor->area;
         widget = gtk_widget_get_parent(widget)) {
        if (widget == anchor->widget) return FALSE;
        if (GTK_IS_BUTTON(widget) || GTK_IS_MENU_BUTTON(widget) || GTK_IS_EDITABLE(widget) ||
            GTK_IS_RANGE(widget) || GTK_IS_POPOVER(widget)) return TRUE;
    }
    return FALSE;
}

static PreviewAnchor *nearest_anchor(SeekPreview *p, GtkWidget *area, double x, double y)
{
    PreviewAnchor *nearest = NULL;
    double distance = G_MAXDOUBLE;
    for (guint i = 0; i < p->anchors->len; ++i) {
        PreviewAnchor *candidate = g_ptr_array_index(p->anchors, i);
        double local_x, local_y;
        if (candidate->area != area || !anchor_coordinates(candidate, x, y, &local_x, &local_y)) continue;
        int width = gtk_widget_get_width(candidate->widget), height = gtk_widget_get_height(candidate->widget);
        int margin = area == candidate->widget ? 0 : PREVIEW_HOVER_MARGIN;
        if (local_x < 0 || local_x >= width || local_y < -margin || local_y >= height + margin ||
            over_another_control(candidate, x, y)) continue;
        double current_distance = fabs(local_y - height / 2.0);
        if (current_distance < distance) { distance = current_distance; nearest = candidate; }
    }
    return nearest;
}

static void hover_motion(GtkEventControllerMotion *controller, double x, double y, gpointer data)
{
    (void)controller;
    PreviewAnchor *anchor = data;
    SeekPreview *p = anchor->owner;
    if (p->stopped) return;
    PreviewAnchor *nearest = nearest_anchor(p, anchor->area, x, y);
    if (!nearest) {
        if (p->hover && p->hover->area == anchor->area) seek_preview_hide(p);
        return;
    }
    if (nearest != anchor || !anchor_coordinates(anchor, x, y, &x, &y)) return;
    if (p->hover != anchor) {
        seek_preview_hide(p);
        p->hover = anchor;
    }
    p->inside = TRUE;
    p->pointer_x = x;
    check_source(p);
    if (!p->available) {
        if (p->popover) gtk_popover_popdown(GTK_POPOVER(p->popover));
        return;
    }
    if (gtk_widget_get_has_tooltip(anchor->widget)) {
        anchor->had_tooltip = TRUE;
        gtk_widget_set_has_tooltip(anchor->widget, FALSE);
    }
    p->seconds = fraction_at(anchor, x) * p->duration;
    gint64 bucket = (gint64)floor(p->seconds * 2);
    gboolean changed = bucket != p->bucket;
    p->bucket = bucket;
    if (!p->enabled) {
        p->target_ready = TRUE;
        show_card(p, NULL, FALSE);
        return;
    }
    PreviewFrame *frame = cached_frame(p, bucket);
    if (frame && frame->refined) {
        cancel_debounce(p);
        if (p->request && p->request_bucket != bucket) abort_request(p);
        p->target_ready = TRUE;
        show_card(p, frame, FALSE);
        return;
    }
    if (changed || (!p->target_ready && !p->debounce_source)) p->target_ready = FALSE;
    /* Throttle to the latest target without waiting for motion to stop. */
    if (!p->target_ready && !p->debounce_source)
        p->debounce_source = g_timeout_add(PREVIEW_DEBOUNCE_MS, debounce_finished, p);
    if (frame) show_card(p, frame, FALSE);
    else if (p->popover && gtk_widget_get_visible(p->popover)) {
        if (p->local) show_pending_frame(p);
        else show_card(p, NULL, FALSE);
    }
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
    mpv_observe_property(mpv, DURATION_OBSERVE_ID, "duration", MPV_FORMAT_DOUBLE);
    return p;
}

void seek_preview_attach(SeekPreview *p, GtkWidget *timeline, gboolean is_range)
{
    seek_preview_attach_area(p, timeline, is_range, timeline);
}

void seek_preview_attach_area(SeekPreview *p, GtkWidget *timeline, gboolean is_range, GtkWidget *area)
{
    if (!p || p->stopped || !GTK_IS_WIDGET(timeline) || !GTK_IS_WIDGET(area) ||
        (is_range && !GTK_IS_RANGE(timeline))) return;
    for (guint i = 0; i < p->anchors->len; ++i)
        if (((PreviewAnchor *)g_ptr_array_index(p->anchors, i))->widget == timeline) return;
    PreviewAnchor *anchor = g_new0(PreviewAnchor, 1);
    anchor->owner = p;
    anchor->widget = g_object_ref(timeline);
    anchor->area = g_object_ref(area);
    anchor->range = is_range;
    GtkEventController *motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_name(motion, "timeline-preview-motion");
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "enter", G_CALLBACK(hover_motion), anchor);
    g_signal_connect(motion, "motion", G_CALLBACK(hover_motion), anchor);
    g_signal_connect(motion, "leave", G_CALLBACK(hover_leave), anchor);
    gtk_widget_add_controller(area, motion);
    anchor->motion = g_object_ref(motion);
    g_signal_connect(timeline, "unmap", G_CALLBACK(anchor_unmapped), anchor);
    if (area != timeline) g_signal_connect(area, "unmap", G_CALLBACK(anchor_unmapped), anchor);
    g_ptr_array_add(p->anchors, anchor);
}

void seek_preview_set_enabled(SeekPreview *p, gboolean enabled)
{
    if (!p || p->stopped || p->enabled == !!enabled) return;
    p->enabled = !!enabled;
    preview_decoder_set_enabled(p->decoder, p->enabled);
    cancel_debounce(p);
    abort_request(p);
    p->checked = FALSE;
    if (!p->enabled) {
        clear_cache(p);
        forget_local_source(p);
    }
    if (p->inside && p->hover) {
        check_source(p);
        if (!p->available) { seek_preview_hide(p); return; }
        p->seconds = fraction_at(p->hover, p->pointer_x) * p->duration;
        p->bucket = (gint64)floor(p->seconds * 2);
        p->target_ready = TRUE;
        request_frame(p);
    }
}

static void cache_duration(SeekPreview *p, const mpv_event_property *property)
{
    double value = property && property->format == MPV_FORMAT_DOUBLE && property->data
        ? *(double *)property->data : 0;
    p->duration_known = isfinite(value) && value > 0;
    p->duration_cache = p->duration_known ? value : 0;
    p->checked = FALSE;
    if (!p->enabled && p->inside && p->hover) {
        check_source(p);
        if (!p->available) {
            if (p->popover) gtk_popover_popdown(GTK_POPOVER(p->popover));
            return;
        }
        p->seconds = fraction_at(p->hover, p->pointer_x) * p->duration;
        p->target_ready = TRUE;
        show_card(p, NULL, FALSE);
    }
}

gboolean seek_preview_handle_event(SeekPreview *p, const mpv_event *event)
{
    if (!p || p->stopped || !event) return FALSE;
    if (event->event_id == MPV_EVENT_VIDEO_RECONFIG && !p->enabled) return FALSE;
    if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && event->reply_userdata == DURATION_OBSERVE_ID) {
        if (!p->duration_loading) cache_duration(p, event->data);
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata >= DURATION_QUERY_BASE &&
        event->reply_userdata < DURATION_QUERY_LIMIT) {
        if (event->reply_userdata == p->duration_request) {
            p->duration_request = 0;
            if (event->error >= 0 && !p->duration_loading) cache_duration(p, event->data);
        }
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE ||
        event->event_id == MPV_EVENT_FILE_LOADED || event->event_id == MPV_EVENT_VIDEO_RECONFIG) {
        if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE) {
            p->duration_known = FALSE; p->duration_cache = 0;
            p->duration_loading = TRUE; p->duration_request = 0;
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            p->duration_loading = FALSE;
            p->duration_request = DURATION_QUERY_BASE + ++p->duration_serial;
            if (mpv_get_property_async(p->mpv, p->duration_request, "duration", MPV_FORMAT_DOUBLE) < 0)
                p->duration_request = 0;
        }
        invalidate_source(p);
        return FALSE;
    }
    return FALSE;
}

void seek_preview_shutdown(SeekPreview *p)
{
    if (!p || p->stopped) return;
    seek_preview_hide(p);
    p->stopped = TRUE;
    mpv_unobserve_property(p->mpv, DURATION_OBSERVE_ID);
    preview_decoder_set_enabled(p->decoder, FALSE);
    for (guint i = 0; i < p->anchors->len; ++i) {
        PreviewAnchor *anchor = g_ptr_array_index(p->anchors, i);
        g_signal_handlers_disconnect_by_data(anchor->widget, anchor);
        if (anchor->area != anchor->widget) g_signal_handlers_disconnect_by_data(anchor->area, anchor);
        g_signal_handlers_disconnect_by_data(anchor->motion, anchor);
        gtk_widget_remove_controller(anchor->area, anchor->motion);
    }
    clear_cache(p);
}

void seek_preview_free(SeekPreview *p)
{
    if (!p) return;
    seek_preview_shutdown(p);
    preview_decoder_free(p->decoder);
    if (p->popover) {
        if (gtk_widget_get_parent(p->popover)) gtk_widget_unparent(p->popover);
        g_clear_object(&p->popover);
    }
    if (p->css && p->display)
        gtk_style_context_remove_provider_for_display(p->display, GTK_STYLE_PROVIDER(p->css));
    g_clear_object(&p->css); g_clear_object(&p->display);
    for (guint i = 0; i < p->anchors->len; ++i) {
        PreviewAnchor *anchor = g_ptr_array_index(p->anchors, i);
        g_clear_object(&anchor->motion); g_clear_object(&anchor->area);
        g_clear_object(&anchor->widget); g_free(anchor);
    }
    g_ptr_array_unref(p->anchors);
    g_free(p->path); g_free(p->stream); g_free(p->source_key); g_free(p->request_key);
    g_free(p);
}
