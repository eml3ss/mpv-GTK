#include "trim.h"
#include "i18n.h"
#include "transport.h"

#include <math.h>
#include <string.h>

#define TRIM_LOAD_ID (UINT64_C(1) << 39)
#define TRIM_OBSERVE_BASE (UINT64_C(1) << 40)
#define TRIM_FRAME_ID (UINT64_C(1) << 41)
#define TRIM_SELECTION_DURATION_US INT64_C(200000)

typedef enum { SCISSORS, KEY_PREVIOUS, KEY_NEXT, SET_START, SET_END,
               EXPORT_COPY, EXPORT_REPLACE } TrimGlyph;

struct TrimMode {
    TrimHost host;
    char *config_path;
    GtkWidget *button, *meta, *canvas, *group, *entries[2], *keys[2];
    GtkWidget *play_icon, *exports[2], *progress, *progress_label, *length;
    GArray *keyframes;
    cairo_path_t *marks;
    int marks_width, marks_height;
    gint64 marks_duration;
    gboolean active, available, ready, loading, stopped, busy, updating, dragging, small;
    int requested;
    guint load_timeout, seek_source, restore_source;
    guint64 serial, entry_serial[2];
    char *submitted[2];
    gboolean enter_pending[2];
    gboolean frame_pending;
    guint generation, frame_generation;
    gint64 next_frame_at;
    gint64 first_ms, last_ms, duration_ms, frame;
    gboolean selection_animated;
    gint64 selection_started, selection_file_serial;
    double selection_from[2], selection_to[2];
    double position, pending_seek;
    char *phase;
};

static void send_action(TrimMode *t, const char *action, const char *argument)
{
    if (t->stopped || !t->ready) return;
    const char *args[] = {"script-message-to", "trim_mode", action, argument, NULL};
    mpv_command_async(t->host.mpv, 0, args);
}

static const mpv_node *field(const mpv_node *map, const char *key)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], key) == 0) return &map->u.list->values[i];
    return NULL;
}

static const char *text_field(const mpv_node *map, const char *key)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_STRING ? value->u.string : "";
}

static gboolean flag_field(const mpv_node *map, const char *key)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_FLAG && value->u.flag;
}

static gint64 number_field(const mpv_node *map, const char *key)
{
    const mpv_node *value = field(map, key);
    return value && value->format == MPV_FORMAT_INT64 ? value->u.int64
        : value && value->format == MPV_FORMAT_DOUBLE && isfinite(value->u.double_)
            ? (gint64)value->u.double_ : 0;
}

static char *time_text(gint64 ms)
{
    ms = MAX((gint64)0, ms);
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT
        ":%02" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT,
        ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

static void set_tooltip(GtkWidget *widget, const char *key)
{
    g_object_set_data_full(G_OBJECT(widget), "trim-tooltip", g_strdup(key), g_free);
    gtk_widget_set_tooltip_text(widget, tr(key));
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  tr(key), -1);
}

static void glyph_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    TrimGlyph glyph = GPOINTER_TO_INT(data);
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_translate(cr, (width - 24) / 2.0, (height - 24) / 2.0);
    cairo_set_line_width(cr, 1.7);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (glyph == SCISSORS) {
        cairo_arc(cr, 6, 7, 3, 0, 2 * G_PI); cairo_stroke(cr);
        cairo_arc(cr, 6, 17, 3, 0, 2 * G_PI); cairo_stroke(cr);
        cairo_move_to(cr, 8.3, 9); cairo_line_to(cr, 21, 18);
        cairo_move_to(cr, 8.3, 15); cairo_line_to(cr, 21, 6); cairo_stroke(cr);
    } else if (glyph == KEY_PREVIOUS || glyph == KEY_NEXT) {
        if (glyph == KEY_NEXT) { cairo_translate(cr, 24, 0); cairo_scale(cr, -1, 1); }
        cairo_arc(cr, 15, 12, 4, 0, 2 * G_PI); cairo_stroke(cr);
        cairo_move_to(cr, 11, 12); cairo_line_to(cr, 3, 12);
        cairo_line_to(cr, 3, 16); cairo_move_to(cr, 7, 12); cairo_line_to(cr, 7, 15);
        cairo_stroke(cr);
    } else if (glyph == SET_START || glyph == SET_END) {
        if (glyph == SET_END) { cairo_translate(cr, 24, 0); cairo_scale(cr, -1, 1); }
        cairo_move_to(cr, 6, 4); cairo_line_to(cr, 6, 20);
        cairo_move_to(cr, 20, 12); cairo_line_to(cr, 10, 12);
        cairo_move_to(cr, 14, 8); cairo_line_to(cr, 10, 12); cairo_line_to(cr, 14, 16);
        cairo_stroke(cr);
    } else {
        cairo_rectangle(cr, 2.5, 4, 19, 16); cairo_stroke(cr);
        if (glyph == EXPORT_COPY) {
            cairo_arc(cr, 12, 12, 5, 0, 2 * G_PI); cairo_stroke(cr);
            cairo_move_to(cr, 9, 12); cairo_line_to(cr, 15, 12);
            cairo_move_to(cr, 12, 9); cairo_line_to(cr, 12, 15); cairo_stroke(cr);
        } else {
            cairo_move_to(cr, 6, 9); cairo_line_to(cr, 17, 9);
            cairo_line_to(cr, 14, 6); cairo_move_to(cr, 17, 9); cairo_line_to(cr, 14, 12);
            cairo_move_to(cr, 18, 15); cairo_line_to(cr, 7, 15);
            cairo_line_to(cr, 10, 12); cairo_move_to(cr, 7, 15); cairo_line_to(cr, 10, 18);
            cairo_stroke(cr);
        }
    }
}

static GtkWidget *glyph_button(TrimGlyph glyph, const char *tooltip)
{
    GtkWidget *button = gtk_button_new(), *icon = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(icon), 24);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(icon), 24);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(icon), glyph_draw, GINT_TO_POINTER(glyph), NULL);
    gtk_widget_set_can_target(icon, FALSE);
    gtk_button_set_child(GTK_BUTTON(button), icon);
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "control-button");
    set_tooltip(button, tooltip);
    return button;
}

static void commit_entry(TrimMode *t, int side, gboolean enter)
{
    if (t->stopped || !t->ready || t->updating || !t->available) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(t->entries[side]));
    t->entry_serial[side] = ++t->serial;
    t->enter_pending[side] = enter;
    g_free(t->submitted[side]);
    t->submitted[side] = g_strdup(text);
    g_autofree char *serial = g_strdup_printf("%" G_GUINT64_FORMAT, t->serial);
    const char *args[] = {"script-message-to", "trim_mode", "set-time",
        side ? "end" : "start", text, serial, NULL};
    mpv_command_async(t->host.mpv, 0, args);
}

static int entry_side(GtkWidget *entry)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "trim-side"));
}

static void entry_entered(GtkEntry *entry, gpointer data)
{
    commit_entry(data, entry_side(GTK_WIDGET(entry)), TRUE);
}

static void entry_focused(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller;
    send_action(data, "gtk-focus", "yes");
}

static void entry_left(GtkEventControllerFocus *controller, gpointer data)
{
    TrimMode *t = data;
    commit_entry(t, entry_side(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller))), FALSE);
    send_action(t, "gtk-focus", "no");
}

static gboolean entry_key(GtkEventControllerKey *controller, guint key, guint code,
                           GdkModifierType modifiers, gpointer data)
{
    (void)code; (void)modifiers;
    TrimMode *t = data;
    if (key != GDK_KEY_Escape) return FALSE;
    GtkWidget *entry = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    g_autofree char *text = time_text(entry_side(entry) ? t->last_ms : t->first_ms);
    t->updating = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(entry), text);
    gtk_widget_remove_css_class(entry, "error");
    gtk_window_set_focus(t->host.window, NULL);
    t->updating = FALSE;
    return TRUE;
}

static void entry_changed(GtkEditable *entry, gpointer data)
{
    (void)data;
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
}

static void action_clicked(GtkButton *button, gpointer data)
{
    TrimMode *t = data;
    if (t->stopped) return;
    GtkWidget *focus = gtk_window_get_focus(t->host.window);
    for (int side = 0; side < 2; ++side)
        if (focus && (focus == t->entries[side] || gtk_widget_is_ancestor(focus, t->entries[side])))
            commit_entry(t, side, FALSE);
    gtk_window_set_focus(t->host.window, NULL);
    send_action(t, g_object_get_data(G_OBJECT(button), "trim-action"),
                g_object_get_data(G_OBJECT(button), "trim-argument"));
}

static GtkWidget *action_button(TrimMode *t, TrimGlyph glyph, const char *tooltip,
                                const char *action, const char *argument)
{
    GtkWidget *button = glyph_button(glyph, tooltip);
    g_object_set_data(G_OBJECT(button), "trim-action", (gpointer)action);
    g_object_set_data(G_OBJECT(button), "trim-argument", (gpointer)argument);
    g_signal_connect(button, "clicked", G_CALLBACK(action_clicked), t);
    return button;
}

static void rounded(cairo_t *cr, double x, double y, double width, double height, double r)
{
    r = MAX(0, MIN(r, MIN(width, height) / 2));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - r, y + r, r, -G_PI_2, 0);
    cairo_arc(cr, x + width - r, y + height - r, r, 0, G_PI_2);
    cairo_arc(cr, x + r, y + height - r, r, G_PI_2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
    cairo_close_path(cr);
}

static void selection_at(TrimMode *t, gint64 now, double *first, double *last)
{
    if (t->selection_animated) {
        double progress = CLAMP((double)(now - t->selection_started) / TRIM_SELECTION_DURATION_US, 0, 1);
        double remaining = 1 - progress;
        double eased = 1 - remaining * remaining * remaining;
        *first = t->selection_from[0] + (t->selection_to[0] - t->selection_from[0]) * eased;
        *last = t->selection_from[1] + (t->selection_to[1] - t->selection_from[1]) * eased;
        if (progress >= 1) t->selection_animated = FALSE;
        return;
    }
    *first = t->duration_ms > 0 ? CLAMP((double)t->first_ms / t->duration_ms, 0, 1) : 0;
    *last = t->duration_ms > 0 ? CLAMP((double)t->last_ms / t->duration_ms, 0, 1) : 1;
}

static void update_selection(TrimMode *t, gint64 first, gint64 last, gint64 duration,
                             gint64 serial, gboolean available)
{
    if (!t->active || !t->canvas || !gtk_widget_get_mapped(t->canvas) || !available ||
        available != t->available || duration != t->duration_ms || serial != t->selection_file_serial) {
        t->selection_animated = FALSE;
    } else if (duration > 0 && (first != t->first_ms || last != t->last_ms)) {
        gboolean animations = TRUE;
        g_object_get(gtk_widget_get_settings(t->canvas), "gtk-enable-animations", &animations, NULL);
        if (animations) {
            gint64 now = g_get_monotonic_time();
            /* Match the original 200 ms ease-out, retargeting both edges from
             * their current drawing when another edit arrives mid-animation. */
            selection_at(t, now, &t->selection_from[0], &t->selection_from[1]);
            t->selection_to[0] = CLAMP((double)first / duration, 0, 1);
            t->selection_to[1] = CLAMP((double)last / duration, 0, 1);
            t->selection_started = now;
            t->selection_animated = TRUE;
        } else t->selection_animated = FALSE;
    }
    t->selection_file_serial = serial;
}

static void timeline_unmapped(GtkWidget *widget, gpointer data)
{
    (void)widget;
    TrimMode *t = data;
    t->selection_animated = FALSE;
}

static void timeline_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    TrimMode *t = data;
    GdkRGBA foreground, accent;
    gtk_widget_get_color(GTK_WIDGET(t->host.controls), &foreground);
    gtk_widget_get_color(GTK_WIDGET(area), &accent);
    double y = 3, h = MAX(1, height - 6), w = MAX(1, width - 2);
    cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, .14);
    rounded(cr, 1, y, w, h, 5); cairo_fill(cr);
    if (!t->available || t->duration_ms <= 0) return;
    double first, last;
    selection_at(t, g_get_monotonic_time(), &first, &last);
    first = 1 + w * first;
    last = 1 + w * last;
    gdk_cairo_set_source_rgba(cr, &accent);
    rounded(cr, first, y, MAX(0, last - first), h, 4); cairo_fill(cr);
    rounded(cr, 1, y, w, h, 5); cairo_clip(cr);
    cairo_set_line_width(cr, 1);
    /* Cache the pixel-deduplicated geometry, as the original OSC does.
     * Long files must not rescan every keyframe on each GTK frame. */
    if (!t->marks || t->marks_width != width || t->marks_height != height || t->marks_duration != t->duration_ms) {
        if (t->marks) cairo_path_destroy(t->marks);
        cairo_new_path(cr);
        double previous = -1;
        for (guint i = 0; i < t->keyframes->len; ++i) {
            double sec = g_array_index(t->keyframes, double, i);
            if (sec < 0 || sec * 1000 > t->duration_ms) continue;
            double x = floor(1 + w * sec * 1000 / t->duration_ms) + .5;
            if (x == previous) continue;
            previous = x;
            cairo_move_to(cr, x, y + 5); cairo_line_to(cr, x, y + h - 5);
        }
        t->marks = cairo_copy_path(cr);
        cairo_new_path(cr);
        t->marks_width = width; t->marks_height = height; t->marks_duration = t->duration_ms;
    }
    cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, .42);
    cairo_append_path(cr, t->marks); cairo_stroke(cr);
    double x = 1 + w * CLAMP(t->position * 1000 / t->duration_ms, 0, 1);
    gdk_cairo_set_source_rgba(cr, &foreground);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, x, y + 1); cairo_line_to(cr, x, y + h - 1); cairo_stroke(cr);
}

static double timeline_seconds(TrimMode *t, double x)
{
    double width = MAX(1, gtk_widget_get_width(t->canvas) - 2);
    return CLAMP((x - 1) / width, 0, 1) * t->duration_ms / 1000;
}

static gboolean flush_seek(gpointer data)
{
    TrimMode *t = data;
    t->seek_source = 0;
    if (!t->stopped && t->available) t->host.seek_to(t->pending_seek, t->host.data);
    return G_SOURCE_REMOVE;
}

static void drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
    (void)y;
    TrimMode *t = data;
    if (!t->available || t->busy || t->stopped) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    t->dragging = TRUE;
    t->pending_seek = timeline_seconds(t, x);
    t->host.seek_to(t->pending_seek, t->host.data);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
    (void)dy;
    TrimMode *t = data;
    if (!t->dragging || t->stopped) return;
    double x, y;
    gtk_gesture_drag_get_start_point(gesture, &x, &y);
    t->pending_seek = timeline_seconds(t, x + dx);
    if (!t->seek_source) t->seek_source = g_timeout_add(16, flush_seek, t);
}

static void drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
    TrimMode *t = data;
    if (!t->dragging) return;
    drag_update(gesture, dx, dy, data);
    if (t->seek_source) { g_source_remove(t->seek_source); t->seek_source = 0; }
    flush_seek(t);
    t->dragging = FALSE;
}

static void drag_cancel(GtkGesture *gesture, GdkEventSequence *sequence, gpointer data)
{
    (void)gesture; (void)sequence;
    TrimMode *t = data;
    t->dragging = FALSE;
    if (t->seek_source) { g_source_remove(t->seek_source); t->seek_source = 0; }
}

static void arrange(TrimMode *t)
{
    if (!t->group) return;
    gboolean small = gtk_widget_get_width(GTK_WIDGET(t->host.window)) < 1160;
    GtkWidget *parent = gtk_widget_get_parent(t->group);
    GtkBox *target = small ? t->host.controls : t->host.transport;
    if (parent != GTK_WIDGET(target)) {
        if (parent) gtk_box_remove(GTK_BOX(parent), t->group);
        gtk_box_insert_child_after(target, t->group, small ? t->canvas : t->host.audio_time);
    }
    t->small = small;
    /* The status label is also the toolbar's expanding spacer. When trim
     * controls occupy their own row, retain it to anchor the right buttons. */
    gtk_widget_set_visible(t->host.status, !t->active || small);
}

static const char *phase_label(const char *phase)
{
    if (g_strcmp0(phase, "verifying") == 0) return tr("Проверка результата…");
    if (g_strcmp0(phase, "preparing") == 0) return tr("Подготовка экспорта…");
    if (g_strcmp0(phase, "rollback") == 0 || g_strcmp0(phase, "restoring") == 0)
        return tr("Восстановление оригинала…");
    if (g_strcmp0(phase, "opening") == 0) return tr("Открытие результата…");
    if (g_strcmp0(phase, "exporting") == 0) return tr("Экспорт без перекодирования…");
    return tr("Завершение экспорта…");
}

static void build_controls(TrimMode *t)
{
    if (t->meta) return;
    t->meta = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
    gtk_widget_set_margin_start(t->meta, 12);
    GtkWidget *title = gtk_label_new(tr("Обрезка"));
    g_object_set_data(G_OBJECT(title), "mpv-label-key", "Обрезка");
    gtk_widget_add_css_class(title, "heading");
    gtk_box_append(GTK_BOX(t->meta), title);
    t->length = gtk_label_new("");
    gtk_widget_add_css_class(t->length, "dim-label");
    gtk_widget_set_hexpand(t->length, TRUE);
    gtk_label_set_xalign(GTK_LABEL(t->length), 0);
    gtk_box_append(GTK_BOX(t->meta), t->length);
    t->exports[0] = action_button(t, EXPORT_REPLACE, "Экспортировать с заменой оригинала · Ctrl+Shift+E", "export", "replace");
    t->exports[1] = action_button(t, EXPORT_COPY, "Экспортировать копию · Ctrl+E", "export", "keep");
    gtk_box_append(GTK_BOX(t->meta), t->exports[0]);
    gtk_box_append(GTK_BOX(t->meta), t->exports[1]);
    gtk_box_insert_child_after(t->host.controls, t->meta, NULL);
    t->canvas = g_object_ref_sink(gtk_drawing_area_new());
    gtk_widget_add_css_class(t->canvas, "trim-timeline");
    gtk_widget_set_margin_start(t->canvas, 12);
    gtk_widget_set_margin_end(t->canvas, 12);
    gtk_widget_set_hexpand(t->canvas, TRUE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(t->canvas), 32);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(t->canvas), timeline_draw, t, NULL);
    g_signal_connect(t->canvas, "unmap", G_CALLBACK(timeline_unmapped), t);
    set_tooltip(t->canvas, "Диапазон обрезки и ключевые кадры");
    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), t);
    g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), t);
    g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), t);
    g_signal_connect(drag, "cancel", G_CALLBACK(drag_cancel), t);
    gtk_widget_add_controller(t->canvas, GTK_EVENT_CONTROLLER(drag));
    gtk_box_insert_child_after(t->host.controls, t->canvas, t->meta);
    t->group = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
    gtk_widget_add_css_class(t->group, "trim-controls");
    gtk_widget_set_halign(t->group, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(t->group, TRUE);
    gtk_widget_set_margin_bottom(t->group, 2);
    for (int side = 0; side < 2; ++side) {
        t->entries[side] = gtk_entry_new();
        gtk_widget_add_css_class(t->entries[side], "trim-time");
        gtk_widget_add_css_class(t->entries[side], "timecode");
        gtk_editable_set_width_chars(GTK_EDITABLE(t->entries[side]), 12);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(t->entries[side]), 14);
        gtk_entry_set_alignment(GTK_ENTRY(t->entries[side]), .5);
        gtk_entry_set_max_length(GTK_ENTRY(t->entries[side]), 256);
        g_autofree char *initial = time_text(side ? t->last_ms : t->first_ms);
        gtk_editable_set_text(GTK_EDITABLE(t->entries[side]), initial);
        g_object_set_data(G_OBJECT(t->entries[side]), "trim-side", GINT_TO_POINTER(side));
        set_tooltip(t->entries[side], side ? "Конец: ЧЧ:ММ:СС.мс или секунды" : "Начало: ЧЧ:ММ:СС.мс или секунды");
        g_signal_connect(t->entries[side], "activate", G_CALLBACK(entry_entered), t);
        g_signal_connect(t->entries[side], "changed", G_CALLBACK(entry_changed), t);
        GtkEventController *focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "enter", G_CALLBACK(entry_focused), t);
        g_signal_connect(focus, "leave", G_CALLBACK(entry_left), t);
        gtk_widget_add_controller(t->entries[side], focus);
        GtkEventController *keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed", G_CALLBACK(entry_key), t);
        gtk_widget_add_controller(t->entries[side], keys);
        t->keys[side] = action_button(t, side ? KEY_NEXT : KEY_PREVIOUS,
            side ? "Следующий ключевой кадр · ]" : "Предыдущий ключевой кадр · [", "keyframe", side ? "next" : "previous");
    }
    gtk_box_append(GTK_BOX(t->group), action_button(t, SET_START, "Начало в текущей позиции · I", "set-boundary", "start"));
    gtk_box_append(GTK_BOX(t->group), t->entries[0]);
    gtk_box_append(GTK_BOX(t->group), t->keys[0]);
    GtkWidget *play = gtk_button_new();
    t->play_icon = transport_icon_new(TRANSPORT_PLAY);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(t->play_icon), 28);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(t->play_icon), 28);
    gtk_button_set_child(GTK_BUTTON(play), t->play_icon);
    gtk_widget_add_css_class(play, "play-button");
    gtk_widget_set_size_request(play, 40, 40);
    gtk_widget_set_focus_on_click(play, FALSE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(play), "app.play");
    set_tooltip(play, "Пауза / воспроизведение · Пробел");
    gtk_box_append(GTK_BOX(t->group), play);
    gtk_box_append(GTK_BOX(t->group), t->keys[1]);
    gtk_box_append(GTK_BOX(t->group), t->entries[1]);
    gtk_box_append(GTK_BOX(t->group), action_button(t, SET_END, "Конец в текущей позиции · O", "set-boundary", "end"));
    arrange(t);
    t->progress = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_widget_add_css_class(t->progress, "playback-info-overlay");
    gtk_widget_set_halign(t->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(t->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(t->progress, FALSE);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_widget_set_size_request(spinner, 32, 32);
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(t->progress), spinner);
    t->progress_label = gtk_label_new("");
    gtk_box_append(GTK_BOX(t->progress), t->progress_label);
    gtk_widget_set_visible(t->progress, FALSE);
    gtk_overlay_add_overlay(t->host.video, t->progress);
    gtk_overlay_set_measure_overlay(t->host.video, t->progress, FALSE);
}

static void update_sensitivity(TrimMode *t)
{
    if (!t->meta) return;
    gtk_widget_set_sensitive(t->group, t->available && !t->busy);
    gtk_widget_set_sensitive(t->canvas, t->available && !t->busy);
    for (int i = 0; i < 2; ++i) {
        gtk_widget_set_sensitive(t->exports[i], t->available && !t->busy);
        gtk_widget_set_sensitive(t->keys[i], t->keyframes->len > 0);
    }
}

static void set_active(TrimMode *t, gboolean active)
{
    if (active) build_controls(t);
    t->active = active;
    if (!active) {
        t->dragging = FALSE;
        t->selection_animated = FALSE;
        if (t->seek_source) { g_source_remove(t->seek_source); t->seek_source = 0; }
    }
    gtk_widget_set_visible(t->host.seek, !active);
    gtk_widget_set_visible(t->host.playback, !active);
    gtk_widget_set_visible(t->host.status, !active || t->small);
    if (t->meta) {
        gtk_widget_set_visible(t->meta, active);
        gtk_widget_set_visible(t->canvas, active);
        gtk_widget_set_visible(t->group, active);
    }
    if (active) gtk_widget_add_css_class(t->button, "trim-active");
    else gtk_widget_remove_css_class(t->button, "trim-active");
    gtk_accessible_update_state(GTK_ACCESSIBLE(t->button), GTK_ACCESSIBLE_STATE_PRESSED,
        active ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
    if (!active) send_action(t, "gtk-focus", "no");
}

static gboolean load_failed(gpointer data)
{
    TrimMode *t = data;
    t->load_timeout = 0;
    t->loading = FALSE;
    t->requested = -1;
    t->host.message(tr("Не удалось загрузить backend обрезки."), TRUE, t->host.data);
    return G_SOURCE_REMOVE;
}

static void load_backend(TrimMode *t)
{
    if (t->stopped || t->ready || t->loading) return;
    g_autofree char *executable = g_file_read_link("/proc/self/exe", NULL);
    if (!executable) { load_failed(t); return; }
    const char *deleted = " (deleted)";
    if (g_str_has_suffix(executable, deleted)) executable[strlen(executable) - strlen(deleted)] = '\0';
    g_autofree char *directory = g_path_get_dirname(executable);
    g_autofree char *script = g_build_filename(directory, "scripts", "trim-mode.lua", NULL);
    if (!g_file_test(script, G_FILE_TEST_IS_REGULAR)) { load_failed(t); return; }
    mpv_set_property_string(t->host.mpv, "user-data/mpv-gtk-trim-config", t->config_path);
    static const struct { const char *name; mpv_format format; } properties[] = {
        {"user-data/trim-mode", MPV_FORMAT_NODE}, {"user-data/trim-controls", MPV_FORMAT_NODE},
        {"user-data/trim-export", MPV_FORMAT_NODE}, {"user-data/trim-field-result", MPV_FORMAT_NODE}
    };
    for (guint i = 0; i < G_N_ELEMENTS(properties); ++i)
        mpv_observe_property(t->host.mpv, TRIM_OBSERVE_BASE + i, properties[i].name, properties[i].format);
    t->loading = TRUE;
    const char *args[] = {"load-script", script, NULL};
    if (mpv_command_async(t->host.mpv, TRIM_LOAD_ID, args) < 0) { load_failed(t); return; }
    t->load_timeout = g_timeout_add_seconds(5, load_failed, t);
}

static gboolean restore_mode(gpointer data)
{
    TrimMode *t = data;
    t->restore_source = 0;
    load_backend(t);
    return G_SOURCE_REMOVE;
}

TrimMode *trim_mode_new(const TrimHost *host)
{
    TrimMode *t = g_new0(TrimMode, 1);
    t->host = *host;
    t->requested = -1;
    t->frame = -1;
    t->config_path = g_strdup(host->config_path);
    t->keyframes = g_array_new(FALSE, FALSE, sizeof(double));
    t->button = g_object_ref_sink(glyph_button(SCISSORS, "Режим обрезки · Ctrl+T"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(t->button), "app.trim");
    g_autofree char *directory = g_path_get_dirname(t->config_path);
    g_autofree char *conf = g_build_filename(directory, "script-opts", "trim-mode.conf", NULL);
    g_autofree char *contents = NULL;
    if (g_file_get_contents(conf, &contents, NULL, NULL) &&
        g_regex_match_simple("(?m)^\\s*remember\\s*=\\s*(yes|true)\\s*(#.*)?$", contents, 0, 0))
        t->restore_source = g_idle_add(restore_mode, t);
    return t;
}

GtkWidget *trim_mode_button(TrimMode *t) { return t ? t->button : NULL; }
gboolean trim_mode_active(const TrimMode *t) { return t && t->active; }

void trim_mode_toggle(TrimMode *t)
{
    if (!t || t->stopped) return;
    if (t->active && t->group) {
        GtkWidget *focus = gtk_window_get_focus(t->host.window);
        for (int i = 0; i < 2; ++i)
            if (focus && (focus == t->entries[i] || gtk_widget_is_ancestor(focus, t->entries[i])))
                commit_entry(t, i, FALSE);
        gtk_window_set_focus(t->host.window, NULL);
    }
    t->requested = !(t->requested >= 0 ? t->requested : t->active);
    if (t->ready) {
        send_action(t, "set-enabled", t->requested ? "yes" : "no");
    } else load_backend(t);
}

gboolean trim_mode_holding(const TrimMode *t)
{
    if (!t || t->stopped) return FALSE;
    if (t->dragging) return TRUE;
    if (!t->active || !t->group) return FALSE;
    GtkWidget *focus = gtk_window_get_focus(t->host.window);
    return focus && (focus == t->group || gtk_widget_is_ancestor(focus, t->group));
}

void trim_mode_position(TrimMode *t, double seconds, gboolean paused)
{
    if (!t || !t->active || t->stopped || !t->canvas) return;
    t->position = seconds;
    /* estimated-frame-number does not emit dependable change notifications.
     * Read it asynchronously while this view is active, never blocking GTK. */
    gint64 now = g_get_monotonic_time();
    if (!t->frame_pending && now >= t->next_frame_at) {
        t->frame_generation = t->generation;
        t->frame_pending = mpv_get_property_async(t->host.mpv, TRIM_FRAME_ID,
            "estimated-frame-number", MPV_FORMAT_INT64) >= 0;
        t->next_frame_at = now + 30000;
    }
    arrange(t);
    transport_icon_set(t->play_icon, paused ? TRANSPORT_PLAY : TRANSPORT_PAUSE);
    gint64 ms = (gint64)llround(MAX(0, seconds) * 1000);
    g_autofree char *time = ms >= 3600000 ? time_text(ms)
        : g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT,
                          ms / 60000, ms / 1000 % 60, ms % 1000);
    g_autofree char *label = t->frame >= 0 ? g_strdup_printf("%s (%" G_GINT64_FORMAT ")", time, t->frame) : g_strdup(time);
    if (g_strcmp0(label, gtk_label_get_text(t->host.time)) != 0) gtk_label_set_text(t->host.time, label);
    gtk_widget_queue_draw(t->canvas);
}

void trim_mode_translate(TrimMode *t)
{
    if (!t) return;
    GtkWidget *roots[] = {t->button, t->meta, t->canvas, t->group};
    GQueue queue = G_QUEUE_INIT;
    for (guint i = 0; i < G_N_ELEMENTS(roots); ++i) if (roots[i]) g_queue_push_tail(&queue, roots[i]);
    while (!g_queue_is_empty(&queue)) {
        GtkWidget *widget = g_queue_pop_head(&queue);
        const char *key = g_object_get_data(G_OBJECT(widget), "trim-tooltip");
        if (key) {
            gtk_widget_set_tooltip_text(widget, tr(key));
            gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, tr(key), -1);
        }
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
            g_queue_push_tail(&queue, child);
    }
    if (t->progress_label) gtk_label_set_text(GTK_LABEL(t->progress_label), phase_label(t->phase));
}

gboolean trim_mode_handle_event(TrimMode *t, const mpv_event *event)
{
    if (!t || t->stopped) return FALSE;
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata == TRIM_FRAME_ID) {
        const mpv_event_property *property = event->data;
        t->frame_pending = FALSE;
        if (event->error >= 0 && property && property->data && property->format == MPV_FORMAT_INT64 &&
            t->generation == t->frame_generation)
            t->frame = *(int64_t *)property->data;
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE) {
        t->available = t->dragging = FALSE;
        t->selection_animated = FALSE;
        t->frame = -1;
        ++t->generation;
        if (t->seek_source) { g_source_remove(t->seek_source); t->seek_source = 0; }
        update_sensitivity(t);
        return FALSE;
    }
    if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->reply_userdata == TRIM_LOAD_ID) {
        if (event->error < 0 && !t->ready) {
            if (t->load_timeout) { g_source_remove(t->load_timeout); t->load_timeout = 0; }
            load_failed(t);
        }
        return TRUE;
    }
    if (event->event_id != MPV_EVENT_PROPERTY_CHANGE || event->reply_userdata < TRIM_OBSERVE_BASE ||
        event->reply_userdata > TRIM_OBSERVE_BASE + 3) return FALSE;
    const mpv_event_property *property = event->data;
    if (!property || !property->data) return TRUE;
    if (property->format != MPV_FORMAT_NODE) return TRUE;
    const mpv_node *node = property->data;
    if (node->format != MPV_FORMAT_NODE_MAP) return TRUE;
    if (event->reply_userdata == TRIM_OBSERVE_BASE) {
        t->ready = TRUE;
        t->loading = FALSE;
        if (t->load_timeout) { g_source_remove(t->load_timeout); t->load_timeout = 0; }
        set_active(t, flag_field(node, "enabled"));
        const mpv_node *keyframes = field(node, "keyframes");
        g_array_set_size(t->keyframes, 0);
        if (t->marks) { cairo_path_destroy(t->marks); t->marks = NULL; }
        if (keyframes && keyframes->format == MPV_FORMAT_NODE_ARRAY && keyframes->u.list) {
            for (int i = 0; i < keyframes->u.list->num; ++i) {
                const mpv_node *value = &keyframes->u.list->values[i];
                double sec = value->format == MPV_FORMAT_DOUBLE ? value->u.double_
                    : value->format == MPV_FORMAT_INT64 ? (double)value->u.int64 : NAN;
                if (isfinite(sec) && sec >= 0) g_array_append_val(t->keyframes, sec);
            }
        }
        if (t->requested >= 0) {
            if (t->active == t->requested) t->requested = -1;
            else send_action(t, "set-enabled", t->requested ? "yes" : "no");
        }
    } else if (event->reply_userdata == TRIM_OBSERVE_BASE + 1) {
        gboolean available = flag_field(node, "available");
        gint64 first = number_field(node, "start_ms"), last = number_field(node, "end_ms");
        gint64 duration = number_field(node, "duration_ms");
        update_selection(t, first, last, duration, number_field(node, "file_serial"), available);
        t->available = available;
        t->first_ms = first;
        t->last_ms = last;
        t->duration_ms = duration;
        if (t->meta) {
            GtkWidget *focus = gtk_window_get_focus(t->host.window);
            for (int i = 0; i < 2; ++i) {
                if (!focus || (focus != t->entries[i] && !gtk_widget_is_ancestor(focus, t->entries[i])))
                    gtk_editable_set_text(GTK_EDITABLE(t->entries[i]), text_field(node, i ? "end_text" : "start_text"));
            }
            g_autofree char *length = time_text(t->last_ms - t->first_ms);
            gtk_label_set_text(GTK_LABEL(t->length), length);
        }
    } else if (event->reply_userdata == TRIM_OBSERVE_BASE + 2) {
        t->busy = flag_field(node, "busy");
        g_free(t->phase); t->phase = g_strdup(text_field(node, "phase"));
        if (t->busy) build_controls(t);
        if (t->progress) {
            gtk_widget_set_visible(t->progress, t->busy);
            gtk_label_set_text(GTK_LABEL(t->progress_label), phase_label(t->phase));
        }
        if (!t->busy && field(node, "success")) {
            gboolean success = flag_field(node, "success");
            g_autofree char *name = g_path_get_basename(text_field(node, "path"));
            g_autofree char *message = success ? g_strdup_printf(tr("Сохранено: %s"), name)
                : g_strdup_printf("%s%s%s", text_field(node, "error"),
                    *text_field(node, "backup") ? "\n" : "", text_field(node, "backup"));
            t->host.message(message, !success, t->host.data);
        }
    } else {
        int side = g_strcmp0(text_field(node, "boundary"), "end") == 0;
        guint64 serial = g_ascii_strtoull(text_field(node, "serial"), NULL, 10);
        if (t->meta && serial == t->entry_serial[side]) {
            GtkWidget *entry = t->entries[side], *focus = gtk_window_get_focus(t->host.window);
            gboolean focused = focus && (focus == entry || gtk_widget_is_ancestor(focus, entry));
            gboolean unchanged = g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(entry)), t->submitted[side]) == 0;
            if (!focused || unchanged) {
                if (flag_field(node, "valid") || !focused) {
                    t->updating = TRUE;
                    gtk_editable_set_text(GTK_EDITABLE(entry), text_field(node, "text"));
                    if (t->enter_pending[side] && focused) gtk_window_set_focus(t->host.window, NULL);
                    t->updating = FALSE;
                } else gtk_widget_add_css_class(entry, "error");
            }
        }
    }
    update_sensitivity(t);
    if (t->canvas) gtk_widget_queue_draw(t->canvas);
    return TRUE;
}

void trim_mode_shutdown(TrimMode *t)
{
    if (!t || t->stopped) return;
    t->stopped = TRUE;
    t->selection_animated = FALSE;
    if (t->load_timeout) { g_source_remove(t->load_timeout); t->load_timeout = 0; }
    if (t->seek_source) { g_source_remove(t->seek_source); t->seek_source = 0; }
    if (t->restore_source) { g_source_remove(t->restore_source); t->restore_source = 0; }
}

void trim_mode_free(TrimMode *t)
{
    if (!t) return;
    trim_mode_shutdown(t);
    /* A forced application quit can leave windows alive until app disposal.
     * Detach our owned roots before freeing callbacks' shared state. */
    GtkWidget *roots[] = {t->meta, t->canvas, t->group, t->progress, t->button};
    for (guint i = 0; i < G_N_ELEMENTS(roots); ++i) {
        if (!roots[i]) continue;
        GtkWidget *parent = gtk_widget_get_parent(roots[i]);
        if (GTK_IS_BOX(parent)) gtk_box_remove(GTK_BOX(parent), roots[i]);
        else if (GTK_IS_OVERLAY(parent)) gtk_overlay_remove_overlay(GTK_OVERLAY(parent), roots[i]);
    }
    g_clear_object(&t->meta);
    g_clear_object(&t->canvas);
    g_clear_object(&t->group);
    g_clear_object(&t->progress);
    g_clear_object(&t->button);
    g_array_unref(t->keyframes);
    if (t->marks) cairo_path_destroy(t->marks);
    g_free(t->submitted[0]); g_free(t->submitted[1]);
    g_free(t->config_path); g_free(t->phase);
    g_free(t);
}
