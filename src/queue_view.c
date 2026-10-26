#include "queue_view.h"
#include "i18n.h"

#include <string.h>

/* A process-local object payload cannot be confused with dropped files or
 * text, and a retained token cannot alias a later reopened queue view. */
typedef struct {
    GObject parent;
    GObject *token;
    int64_t id;
} QueueDrag;
typedef struct { GObjectClass parent; } QueueDragClass;
G_DEFINE_TYPE(QueueDrag, queue_drag, G_TYPE_OBJECT)

static void queue_drag_finalize(GObject *object)
{
    QueueDrag *drag = (QueueDrag *)object;
    g_clear_object(&drag->token);
    G_OBJECT_CLASS(queue_drag_parent_class)->finalize(object);
}

static void queue_drag_class_init(QueueDragClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = queue_drag_finalize;
}

static void queue_drag_init(QueueDrag *drag) { (void)drag; }

typedef struct {
    QueueView *view;
    int64_t id;
    char *title, *filename, *search;
    GtkWidget *row, *body, *index, *label, *remove, *handle;
} QueueRow;

struct QueueView {
    QueueModel *model;
    GtkWidget *root, *list, *count, *search, *scroller, *placeholder;
    GtkWidget *clear, *sort, *shuffle, *repeat, *remove, *selected, *spinner;
    GObject *drag_token;
    GPtrArray *rows;
    char *query;
    guint restore_source, click_reset_source, drag_leave_source;
    GtkDragSource *drag_source;
    int64_t drag_id, drag_before;
    int64_t current_id;
    double scroll_position;
    gboolean updating, rebuilding, selection_click, manual_selection;
    gboolean drag_active, drag_committed;
};

static void clear_drag_preview(QueueView *view);
static void cancel_view_drag(QueueView *view);

static char *search_text(const char *text)
{
    g_autofree char *valid = g_utf8_make_valid(text ? text : "", -1);
    g_autofree char *folded = g_utf8_casefold(valid, -1);
    return g_utf8_normalize(folded, -1, G_NORMALIZE_ALL_COMPOSE);
}

static char *display_title(const QueueEntry *entry)
{
    if (entry->title && *entry->title)
        return g_utf8_make_valid(entry->title, -1);
    g_autoptr(GFile) file = g_file_new_for_commandline_arg(entry->filename ? entry->filename : "");
    g_autofree char *base = g_file_get_basename(file);
    return g_utf8_make_valid(base && *base ? base : tr("Медиафайл"), -1);
}

static void row_free(gpointer data)
{
    QueueRow *row = data;
    g_free(row->title);
    g_free(row->filename);
    g_free(row->search);
    g_free(row);
}

static void tooltip(GtkWidget *widget, const char *key)
{
    g_object_set_data(G_OBJECT(widget), "queue-tooltip", (gpointer)key);
    gtk_widget_set_tooltip_text(widget, tr(key));
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
        GTK_ACCESSIBLE_PROPERTY_LABEL, tr(key), -1);
}

static GtkWidget *text_button(const char *key)
{
    GtkWidget *button = gtk_button_new_with_label(tr(key));
    g_object_set_data(G_OBJECT(button), "queue-button-label", (gpointer)key);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "queue-action");
    return button;
}

static gboolean row_matches(QueueView *view, QueueRow *row)
{
    return !view->query || !*view->query || strstr(row->search, view->query) != NULL;
}

static gboolean filter_row(GtkListBoxRow *widget, gpointer data)
{
    QueueRow *row = g_object_get_data(G_OBJECT(widget), "mpv-queue-row");
    return row && row_matches(data, row);
}

static GArray *selected_ids(QueueView *view)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(int64_t));
    GList *rows = gtk_list_box_get_selected_rows(GTK_LIST_BOX(view->list));
    for (GList *p = rows; p; p = p->next) {
        QueueRow *row = g_object_get_data(G_OBJECT(p->data), "mpv-queue-row");
        if (row && row_matches(view, row)) g_array_append_val(ids, row->id);
    }
    g_list_free(rows);
    return ids;
}

static void update_actions(QueueView *view)
{
    const GPtrArray *entries = queue_model_entries(view->model);
    guint count = entries ? entries->len : 0;
    gboolean ready = !queue_model_busy(view->model) && !view->drag_active && !view->drag_committed;
    g_autoptr(GArray) ids = selected_ids(view);
    g_autofree char *selected = g_strdup_printf(tr("Выбрано: %u"), ids->len);
    gtk_label_set_text(GTK_LABEL(view->selected), selected);
    gtk_widget_set_sensitive(view->remove, ready && ids->len > 0);
    gtk_widget_set_sensitive(view->clear, ready && count > 0);
    gtk_widget_set_sensitive(view->sort, ready && count > 1);
    gtk_widget_set_sensitive(view->shuffle, ready && count > 1);
    gtk_widget_set_sensitive(view->repeat, ready);
    gtk_widget_set_visible(view->spinner, queue_model_busy(view->model));
    gtk_spinner_set_spinning(GTK_SPINNER(view->spinner), queue_model_busy(view->model));
    for (guint i = 0; i < view->rows->len; ++i) {
        QueueRow *row = g_ptr_array_index(view->rows, i);
        gtk_widget_set_sensitive(row->remove, ready);
        gtk_widget_set_sensitive(row->handle, (view->drag_active && row->id == view->drag_id) ||
                                 (ready && count > 1));
    }
}

static void selection_changed(GtkListBox *list, gpointer data)
{
    (void)list;
    QueueView *view = data;
    if (!view->rebuilding && !view->updating) {
        view->manual_selection = TRUE;
        update_actions(view);
    }
}

static void update_placeholder(QueueView *view)
{
    const GPtrArray *entries = queue_model_entries(view->model);
    gtk_label_set_text(GTK_LABEL(view->placeholder),
        tr(entries && entries->len ? "Ничего не найдено" : "Очередь пуста"));
}

static void search_changed(GtkSearchEntry *entry, gpointer data)
{
    QueueView *view = data;
    g_free(view->query);
    view->query = search_text(gtk_editable_get_text(GTK_EDITABLE(entry)));
    for (guint i = 0; i < view->rows->len; ++i) {
        QueueRow *row = g_ptr_array_index(view->rows, i);
        if (!row_matches(view, row))
            gtk_list_box_unselect_row(GTK_LIST_BOX(view->list), GTK_LIST_BOX_ROW(row->row));
    }
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(view->list));
    update_placeholder(view);
    update_actions(view);
}

static void remove_selected(QueueView *view)
{
    if (queue_model_busy(view->model)) return;
    g_autoptr(GArray) ids = selected_ids(view);
    if (ids->len) queue_model_remove(view->model, (const int64_t *)ids->data, ids->len);
}

static void selected_remove_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    remove_selected(data);
}

static void row_remove_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    QueueRow *row = data;
    if (!queue_model_busy(row->view->model))
        queue_model_remove(row->view->model, &row->id, 1);
}

static void clear_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    QueueView *view = data;
    if (!queue_model_busy(view->model)) queue_model_clear(view->model);
}

static void shuffle_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    QueueView *view = data;
    if (!queue_model_busy(view->model)) queue_model_shuffle(view->model);
}

static void repeat_toggled(GtkToggleButton *button, gpointer data)
{
    QueueView *view = data;
    if (!view->updating && !queue_model_busy(view->model))
        queue_model_set_repeat(view->model, gtk_toggle_button_get_active(button));
}

static void sort_clicked(GtkButton *button, gpointer data)
{
    QueueView *view = data;
    QueueSort sort = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "queue-sort"));
    gtk_menu_button_popdown(GTK_MENU_BUTTON(view->sort));
    if (!queue_model_busy(view->model)) queue_model_sort(view->model, sort);
}

static void cancel_click_reset(QueueView *view)
{
    if (view->click_reset_source) {
        g_source_remove(view->click_reset_source);
        view->click_reset_source = 0;
    }
}

static void reset_click_policy(QueueView *view)
{
    cancel_click_reset(view);
    view->selection_click = FALSE;
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(view->list), TRUE);
}

static gboolean reset_click_idle(gpointer data)
{
    QueueView *view = data;
    view->click_reset_source = 0;
    reset_click_policy(view);
    return G_SOURCE_REMOVE;
}

static void list_mapped(GtkWidget *widget, gpointer data)
{
    (void)widget;
    queue_view_refresh(data);
}

static void list_unmapped(GtkWidget *widget, gpointer data)
{
    (void)widget;
    cancel_view_drag(data);
    reset_click_policy(data);
}

static gboolean list_click_event(GtkEventControllerLegacy *controller, GdkEvent *event,
                                  gpointer data)
{
    (void)controller;
    QueueView *view = data;
    GdkEventType type = gdk_event_get_event_type(event);
    if (type == GDK_KEY_PRESS || type == GDK_GRAB_BROKEN || type == GDK_TOUCH_CANCEL) {
        reset_click_policy(view);
    } else if ((type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE) &&
               gdk_button_event_get_button(event) == GDK_BUTTON_PRIMARY) {
        if (type == GDK_BUTTON_PRESS) {
            cancel_click_reset(view);
            view->selection_click = (gdk_event_get_modifier_state(event) &
                                     (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != 0;
            /* GTK's single-click path bypasses its modifier-selection logic.
             * Choose the native selection path before GTK handles this press. */
            gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(view->list),
                                                      !view->selection_click);
        } else if (!view->click_reset_source) {
            /* Keep the press policy through GTK's release handler. */
            view->click_reset_source = g_idle_add_full(G_PRIORITY_HIGH_IDLE,
                reset_click_idle, view, NULL);
        }
    }
    return FALSE;
}

static void row_activated(GtkListBox *list, GtkListBoxRow *widget, gpointer data)
{
    QueueView *view = data;
    QueueRow *row = g_object_get_data(G_OBJECT(widget), "mpv-queue-row");
    if (!row || view->selection_click)
        return;
    /* GTK's single-click activation adds to a multiple selection. Ordinary
     * playback activation should leave only its target selected. */
    view->manual_selection = FALSE;
    view->updating = TRUE;
    gtk_list_box_unselect_all(list);
    gtk_list_box_select_row(list, widget);
    view->updating = FALSE;
    update_actions(view);
    if (!queue_model_busy(view->model))
        queue_model_play(view->model, row->id);
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint key,
                            guint code, GdkModifierType modifiers, gpointer data)
{
    (void)controller; (void)code;
    QueueView *view = data;
    GtkRoot *root = gtk_widget_get_root(view->root);
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (!queue_view_wants_key(view, focus, key, modifiers)) return FALSE;
    if (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete) {
        remove_selected(view);
        return TRUE;
    }
    if ((key == GDK_KEY_a || key == GDK_KEY_A) && (modifiers & GDK_CONTROL_MASK)) {
        for (guint i = 0; i < view->rows->len; ++i) {
            QueueRow *row = g_ptr_array_index(view->rows, i);
            if (row_matches(view, row))
                gtk_list_box_select_row(GTK_LIST_BOX(view->list), GTK_LIST_BOX_ROW(row->row));
        }
        return TRUE;
    }
    if (key == GDK_KEY_Escape) {
        gtk_list_box_unselect_all(GTK_LIST_BOX(view->list));
        return TRUE;
    }
    return FALSE; /* GtkListBox handles arrows, Enter and Shift selection. */
}

static void handle_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    (void)data;
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    for (int y = -1; y <= 1; ++y)
        for (int x = 0; x < 2; ++x) {
            cairo_arc(cr, width / 2.0 + (x ? 2 : -2), height / 2.0 + y * 5,
                      1, 0, 2 * G_PI);
            cairo_fill(cr);
        }
}

static void action_icon_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_translate(cr, (width - 16) / 2.0, (height - 16) / 2.0);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (GPOINTER_TO_INT(data)) {
        cairo_move_to(cr, 2, 4.5); cairo_line_to(cr, 4, 4.5);
        cairo_curve_to(cr, 7, 4.5, 8.5, 11.5, 11.5, 11.5); cairo_line_to(cr, 14, 11.5);
        cairo_move_to(cr, 11.5, 9); cairo_line_to(cr, 14, 11.5); cairo_line_to(cr, 11.5, 14);
        cairo_move_to(cr, 2, 11.5); cairo_line_to(cr, 4, 11.5);
        cairo_curve_to(cr, 7, 11.5, 8.5, 4.5, 11.5, 4.5); cairo_line_to(cr, 14, 4.5);
        cairo_move_to(cr, 11.5, 2); cairo_line_to(cr, 14, 4.5); cairo_line_to(cr, 11.5, 7);
    } else {
        cairo_move_to(cr, 2.5, 7); cairo_line_to(cr, 2.5, 5.5);
        cairo_curve_to(cr, 2.5, 4.4, 3.4, 3.5, 4.5, 3.5); cairo_line_to(cr, 13.5, 3.5);
        cairo_move_to(cr, 11, 1); cairo_line_to(cr, 13.5, 3.5); cairo_line_to(cr, 11, 6);
        cairo_move_to(cr, 13.5, 9); cairo_line_to(cr, 13.5, 10.5);
        cairo_curve_to(cr, 13.5, 11.6, 12.6, 12.5, 11.5, 12.5); cairo_line_to(cr, 2.5, 12.5);
        cairo_move_to(cr, 5, 10); cairo_line_to(cr, 2.5, 12.5); cairo_line_to(cr, 5, 15);
    }
    cairo_stroke(cr);
}

static GtkWidget *action_icon(gboolean shuffle)
{
    GtkWidget *icon = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(icon), 16);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(icon), 16);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(icon), action_icon_draw,
                                  GINT_TO_POINTER(shuffle), NULL);
    gtk_widget_set_can_target(icon, FALSE);
    return icon;
}

static void cancel_drag_leave(QueueView *view)
{
    if (view->drag_leave_source) g_source_remove(view->drag_leave_source);
    view->drag_leave_source = 0;
}

static void clear_drag_preview(QueueView *view)
{
    cancel_drag_leave(view);
    for (guint i = 0; i < view->rows->len; ++i) {
        QueueRow *row = view->rows->pdata[i];
        gtk_widget_remove_css_class(row->body, "queue-shift-up");
        gtk_widget_remove_css_class(row->body, "queue-shift-down");
        gtk_widget_remove_css_class(row->body, "queue-drag-source");
        gtk_widget_remove_css_class(row->row, "drop-before");
        gtk_widget_remove_css_class(row->row, "drop-after");
    }
    gtk_widget_remove_css_class(view->root, "queue-dragging");
}

static void cancel_view_drag(QueueView *view)
{
    if (view->drag_source) {
        g_signal_handlers_disconnect_by_data(view->drag_source, view);
        gtk_drag_source_drag_cancel(view->drag_source);
        g_clear_object(&view->drag_source);
    }
    view->drag_active = view->drag_committed = FALSE;
    clear_drag_preview(view);
}

static gboolean leave_drag_idle(gpointer data)
{
    QueueView *view = data;
    view->drag_leave_source = 0;
    if (!view->drag_committed) clear_drag_preview(view);
    return G_SOURCE_REMOVE;
}

static void schedule_drag_leave(QueueView *view)
{
    if (!view->drag_leave_source && !view->drag_committed)
        view->drag_leave_source = g_idle_add_full(G_PRIORITY_HIGH_IDLE, leave_drag_idle, view, NULL);
}

static void drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer data)
{
    (void)drag;
    QueueRow *row = data;
    QueueView *view = row->view;
    g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(row->row);
    g_autoptr(GdkPaintable) image = gdk_paintable_get_current_image(paintable);
    gtk_drag_source_set_icon(source, image, 16, gtk_widget_get_height(row->row) / 2);
    view->drag_source = g_object_ref(source);
    view->drag_id = row->id;
    view->drag_before = row->id;
    view->drag_active = TRUE;
    view->drag_committed = FALSE;
    gtk_widget_add_css_class(view->root, "queue-dragging");
    gtk_widget_add_css_class(row->body, "queue-drag-source");
    update_actions(view);
}

static void drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete_data, gpointer data)
{
    (void)source; (void)drag; (void)delete_data;
    QueueView *view = data;
    view->drag_active = FALSE;
    g_clear_object(&view->drag_source);
    if (!view->drag_committed || !queue_model_busy(view->model)) {
        view->drag_committed = FALSE;
        clear_drag_preview(view);
    }
    queue_view_refresh(view);
}

static gboolean drag_cancel(GtkDragSource *source, GdkDrag *drag,
                            GdkDragCancelReason reason, gpointer data)
{
    (void)source; (void)drag; (void)reason;
    QueueView *view = data;
    view->drag_active = view->drag_committed = FALSE;
    clear_drag_preview(view);
    return FALSE;
}

static void handle_released(GtkGestureClick *gesture, int count,
                             double x, double y, gpointer data)
{
    (void)count; (void)x; (void)y; (void)data;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static GdkContentProvider *drag_prepare(GtkDragSource *source, double x, double y, gpointer data)
{
    (void)source; (void)x; (void)y;
    QueueRow *row = data;
    if (queue_model_busy(row->view->model) || row->view->drag_active ||
        row->view->drag_committed) return NULL;
    QueueDrag *drag = g_object_new(queue_drag_get_type(), NULL);
    drag->token = g_object_ref(row->view->drag_token);
    drag->id = row->id;
    GValue value = G_VALUE_INIT;
    g_value_init(&value, queue_drag_get_type());
    g_value_take_object(&value, drag);
    GdkContentProvider *provider = gdk_content_provider_new_for_value(&value);
    g_value_unset(&value);
    return provider;
}

static gboolean accepts_drag(QueueView *view, const GValue *value)
{
    if (!value || !G_VALUE_HOLDS(value, queue_drag_get_type()) || queue_model_busy(view->model))
        return FALSE;
    QueueDrag *drag = g_value_get_object(value);
    return drag && drag->token == view->drag_token;
}

/* All hover decisions use unchanged GtkListBoxRow allocations. Only each
 * row's inner content moves, so crossing an animated item cannot oscillate
 * the insertion slot or change the native destination index. */
static int64_t insertion_at(QueueView *view, double y, QueueRow **marked, gboolean *after)
{
    *marked = NULL;
    *after = FALSE;
    GtkListBoxRow *widget = gtk_list_box_get_row_at_y(GTK_LIST_BOX(view->list), (int)y);
    if (!widget) {
        for (guint i = 0; i < view->rows->len; ++i) {
            QueueRow *candidate = view->rows->pdata[i];
            graphene_rect_t bounds;
            if (row_matches(view, candidate) &&
                gtk_widget_compute_bounds(candidate->row, view->list, &bounds) &&
                bounds.origin.y > y) {
                widget = GTK_LIST_BOX_ROW(candidate->row);
                break;
            }
        }
    }
    if (!widget) return -1;
    QueueRow *row = g_object_get_data(G_OBJECT(widget), "mpv-queue-row");
    if (!row) return -1;
    graphene_point_t from = GRAPHENE_POINT_INIT(0, y), to;
    if (!gtk_widget_compute_point(view->list, GTK_WIDGET(widget), &from, &to)) return -1;
    *marked = row;
    *after = to.y >= gtk_widget_get_height(GTK_WIDGET(widget)) / 2.0;
    if (!*after) return row->id;
    const GPtrArray *entries = queue_model_entries(view->model);
    for (guint i = 0; entries && i < entries->len; ++i) {
        const QueueEntry *entry = entries->pdata[i];
        if (entry->id == row->id)
            return i + 1 < entries->len ? ((QueueEntry *)entries->pdata[i + 1])->id : -1;
    }
    return -2; /* The hovered entry was removed by an external queue edit. */
}

static void preview_insertion(QueueView *view, int64_t before, QueueRow *marked, gboolean after)
{
    cancel_drag_leave(view);
    if (view->drag_before == before && gtk_widget_has_css_class(view->root, "queue-dragging")) return;
    gtk_widget_add_css_class(view->root, "queue-dragging");
    gboolean below = FALSE;
    for (guint i = 0; i < view->rows->len; ++i) {
        QueueRow *row = view->rows->pdata[i];
        if (row->id == before) below = TRUE;
        gtk_widget_remove_css_class(row->row, "drop-before");
        gtk_widget_remove_css_class(row->row, "drop-after");
        gboolean shifted = row->id != view->drag_id && row_matches(view, row);
        if (shifted && !below) gtk_widget_add_css_class(row->body, "queue-shift-up");
        else gtk_widget_remove_css_class(row->body, "queue-shift-up");
        if (shifted && below) gtk_widget_add_css_class(row->body, "queue-shift-down");
        else gtk_widget_remove_css_class(row->body, "queue-shift-down");
        if (row->id == view->drag_id) gtk_widget_add_css_class(row->body, "queue-drag-source");
    }
    if (marked) gtk_widget_add_css_class(marked->row, after ? "drop-after" : "drop-before");
    view->drag_before = before;
}

static GdkDragAction preview_at(GtkDropTarget *target, double y, QueueView *view)
{
    if (!accepts_drag(view, gtk_drop_target_get_value(target))) return 0;
    QueueRow *marked;
    gboolean after;
    int64_t before = insertion_at(view, y, &marked, &after);
    if (before < -1) return 0;
    if (view->drag_active) preview_insertion(view, before, marked, after);
    return GDK_ACTION_MOVE;
}

static GdkDragAction drop_motion(GtkDropTarget *target, double x, double y, gpointer data)
{
    QueueRow *row = data;
    graphene_point_t from = GRAPHENE_POINT_INIT(x, y), to;
    if (!gtk_widget_compute_point(row->row, row->view->list, &from, &to)) return 0;
    return preview_at(target, to.y, row->view);
}

static GdkDragAction list_drop_motion(GtkDropTarget *target, double x, double y, gpointer data)
{
    (void)x;
    return preview_at(target, y, data);
}

static void drop_leave(GtkDropTarget *target, gpointer data)
{
    (void)target;
    QueueRow *row = data;
    gtk_widget_remove_css_class(row->row, "drop-before");
    gtk_widget_remove_css_class(row->row, "drop-after");
    schedule_drag_leave(row->view);
}

static void list_drop_leave(GtkDropTarget *target, gpointer data)
{
    (void)target;
    schedule_drag_leave(data);
}

static gboolean drop_at_end(GtkDropTarget *target, const GValue *value,
                             double x, double y, gpointer data)
{
    (void)target; (void)x;
    QueueView *view = data;
    if (!accepts_drag(view, value)) return FALSE;
    QueueDrag *drag = g_value_get_object(value);
    QueueRow *marked;
    gboolean after;
    int64_t before = insertion_at(view, y, &marked, &after);
    if (before < -1) return FALSE;
    if (before == drag->id) return TRUE;
    if (view->drag_active) {
        preview_insertion(view, before, marked, after);
        view->drag_committed = TRUE;
    }
    queue_model_move(view->model, drag->id, before);
    return TRUE;
}

static gboolean drop_on_row(GtkDropTarget *target, const GValue *value,
                             double x, double y, gpointer data)
{
    QueueRow *row = data;
    graphene_point_t from = GRAPHENE_POINT_INIT(x, y), to;
    if (!gtk_widget_compute_point(row->row, row->view->list, &from, &to)) return FALSE;
    return drop_at_end(target, value, to.x, to.y, row->view);
}

static QueueRow *new_row(QueueView *view, const QueueEntry *entry)
{
    QueueRow *row = g_new0(QueueRow, 1);
    row->view = view;
    row->id = entry->id;
    row->title = display_title(entry);
    row->filename = g_strdup(entry->filename ? entry->filename : "");
    g_autofree char *search = g_strconcat(row->title, "\n", row->filename, NULL);
    row->search = search_text(search);
    row->row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row->row, "queue-entry");
    g_object_set_data_full(G_OBJECT(row->row), "mpv-queue-row", row, row_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    row->body = box;
    gtk_widget_add_css_class(box, "queue-row-body");
    gtk_widget_set_overflow(row->row, GTK_OVERFLOW_VISIBLE);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);
    row->handle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(row->handle, 28, -1);
    gtk_widget_set_valign(row->handle, GTK_ALIGN_FILL);
    GtkWidget *grip = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(grip), 12);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(grip), 22);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(grip), handle_draw, NULL, NULL);
    gtk_widget_set_halign(grip, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grip, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(grip, TRUE);
    gtk_widget_set_can_target(grip, FALSE);
    gtk_box_append(GTK_BOX(row->handle), grip);
    gtk_widget_add_css_class(row->handle, "queue-drag-handle");
    gtk_widget_add_css_class(row->handle, "dim-label");
    tooltip(row->handle, "Перетащить запись");
    gtk_widget_set_cursor_from_name(row->handle, "grab");
    GtkDragSource *source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    g_signal_connect(source, "prepare", G_CALLBACK(drag_prepare), row);
    g_signal_connect(source, "drag-begin", G_CALLBACK(drag_begin), row);
    g_signal_connect(source, "drag-end", G_CALLBACK(drag_end), view);
    g_signal_connect(source, "drag-cancel", G_CALLBACK(drag_cancel), view);
    gtk_widget_add_controller(row->handle, GTK_EVENT_CONTROLLER(source));
    GtkGesture *handle_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(handle_click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(handle_click), GTK_PHASE_CAPTURE);
    g_signal_connect(handle_click, "released", G_CALLBACK(handle_released), NULL);
    gtk_widget_add_controller(row->handle, GTK_EVENT_CONTROLLER(handle_click));
    gtk_box_append(GTK_BOX(box), row->handle);
    row->index = gtk_label_new(NULL);
    gtk_widget_add_css_class(row->index, "queue-index");
    gtk_widget_set_size_request(row->index, 22, -1);
    gtk_box_append(GTK_BOX(box), row->index);
    row->label = gtk_label_new(row->title);
    gtk_widget_add_css_class(row->label, "queue-entry-title");
    gtk_label_set_xalign(GTK_LABEL(row->label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(row->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(row->label), 24);
    gtk_widget_set_hexpand(row->label, TRUE);
    gtk_widget_set_tooltip_text(row->label, *row->filename ? row->filename : row->title);
    gtk_box_append(GTK_BOX(box), row->label);
    row->remove = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(row->remove, "flat");
    gtk_widget_add_css_class(row->remove, "queue-action");
    gtk_widget_set_focus_on_click(row->remove, FALSE);
    tooltip(row->remove, "Удалить из очереди");
    g_signal_connect(row->remove, "clicked", G_CALLBACK(row_remove_clicked), row);
    gtk_box_append(GTK_BOX(box), row->remove);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row->row), box);
    GtkDropTarget *target = gtk_drop_target_new(queue_drag_get_type(), GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(target, TRUE);
    g_signal_connect(target, "enter", G_CALLBACK(drop_motion), row);
    g_signal_connect(target, "motion", G_CALLBACK(drop_motion), row);
    g_signal_connect(target, "leave", G_CALLBACK(drop_leave), row);
    g_signal_connect(target, "drop", G_CALLBACK(drop_on_row), row);
    gtk_widget_add_controller(row->row, GTK_EVENT_CONTROLLER(target));
    return row;
}

static gboolean restore_scroll(gpointer data)
{
    QueueView *view = data;
    view->restore_source = 0;
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->scroller));
    gtk_adjustment_set_value(adjustment, view->scroll_position);
    return G_SOURCE_REMOVE;
}

static gboolean ids_contain(const GArray *ids, int64_t id)
{
    for (guint i = 0; i < ids->len; ++i)
        if (g_array_index(ids, int64_t, i) == id) return TRUE;
    return FALSE;
}

void queue_view_refresh(QueueView *view)
{
    if (!view) return;
    const GPtrArray *entries = queue_model_entries(view->model);
    guint count = entries ? entries->len : 0;
    gboolean rebuild = count != view->rows->len;
    for (guint i = 0; !rebuild && i < count; ++i) {
        const QueueEntry *entry = g_ptr_array_index(entries, i);
        QueueRow *row = g_ptr_array_index(view->rows, i);
        g_autofree char *title = display_title(entry);
        rebuild = row->id != entry->id || g_strcmp0(row->title, title) ||
                  g_strcmp0(row->filename, entry->filename ? entry->filename : "");
    }
    /* A sort can produce many intermediate native snapshots. The stable-ID
     * selection stays on the old rows until the serialized edit completes. */
    if (rebuild && (queue_model_busy(view->model) || view->drag_active)) {
        g_autofree char *count_text = g_strdup_printf(tr("Очередь · %d"), (int)count);
        gtk_label_set_text(GTK_LABEL(view->count), count_text);
        update_actions(view);
        return;
    }
    if (view->drag_committed && !view->drag_active && !queue_model_busy(view->model)) {
        view->drag_committed = FALSE;
        clear_drag_preview(view);
    }
    if (rebuild) {
        g_autoptr(GArray) selected = selected_ids(view);
        gboolean first = view->rows->len == 0;
        if (first) view->manual_selection = FALSE;
        GtkRoot *root = gtk_widget_get_root(view->root);
        GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
        gboolean had_list_focus = focus &&
            (focus == view->list || gtk_widget_is_ancestor(focus, view->list));
        int64_t focused_id = -1;
        for (GtkWidget *widget = focus; had_list_focus && widget && widget != view->list;
             widget = gtk_widget_get_parent(widget)) {
            QueueRow *row = g_object_get_data(G_OBJECT(widget), "mpv-queue-row");
            if (row) { focused_id = row->id; break; }
        }
        GtkWidget *focus_target = NULL, *first_visible = NULL;
        GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(view->scroller));
        view->scroll_position = gtk_adjustment_get_value(adjustment);
        view->rebuilding = TRUE;
        for (guint i = 0; i < view->rows->len; ++i) {
            QueueRow *row = g_ptr_array_index(view->rows, i);
            gtk_list_box_remove(GTK_LIST_BOX(view->list), row->row);
        }
        g_ptr_array_set_size(view->rows, 0);
        for (guint i = 0; i < count; ++i) {
            const QueueEntry *entry = g_ptr_array_index(entries, i);
            QueueRow *row = new_row(view, entry);
            g_ptr_array_add(view->rows, row);
            gtk_list_box_append(GTK_LIST_BOX(view->list), row->row);
            if (row_matches(view, row) && !first_visible) first_visible = row->row;
            if (entry->id == focused_id && row_matches(view, row)) focus_target = row->row;
            if (row_matches(view, row) &&
                (ids_contain(selected, entry->id) || (first && entry->current)))
                gtk_list_box_select_row(GTK_LIST_BOX(view->list), GTK_LIST_BOX_ROW(row->row));
        }
        if (had_list_focus)
            gtk_widget_grab_focus(focus_target ? focus_target : first_visible ? first_visible : view->list);
        view->rebuilding = FALSE;
        if (view->restore_source) g_source_remove(view->restore_source);
        view->restore_source = g_idle_add_full(G_PRIORITY_LOW, restore_scroll, view, NULL);
    }
    int64_t current_id = -1;
    GtkListBoxRow *current_row = NULL;
    for (guint i = 0; i < count; ++i) {
        const QueueEntry *entry = g_ptr_array_index(entries, i);
        QueueRow *row = g_ptr_array_index(view->rows, i);
        g_autofree char *index = entry->current ? g_strdup("▶") : g_strdup_printf("%02u", i + 1);
        gtk_label_set_text(GTK_LABEL(row->index), index);
        if (entry->current) gtk_widget_add_css_class(row->row, "queue-current");
        else gtk_widget_remove_css_class(row->row, "queue-current");
        if (entry->current) {
            current_id = entry->id;
            current_row = row_matches(view, row) ? GTK_LIST_BOX_ROW(row->row) : NULL;
        }
    }
    if (!queue_model_busy(view->model) && view->current_id != current_id) {
        view->current_id = current_id;
        if (!view->manual_selection) {
            view->updating = TRUE;
            gtk_list_box_unselect_all(GTK_LIST_BOX(view->list));
            if (current_row) gtk_list_box_select_row(GTK_LIST_BOX(view->list), current_row);
            view->updating = FALSE;
        }
    }
    g_autofree char *count_text = g_strdup_printf(tr("Очередь · %d"), (int)count);
    gtk_label_set_text(GTK_LABEL(view->count), count_text);
    view->updating = TRUE;
    const char *repeat = queue_model_repeat(view->model);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->repeat),
        repeat && *repeat && g_strcmp0(repeat, "no") && g_strcmp0(repeat, "0"));
    view->updating = FALSE;
    update_placeholder(view);
    update_actions(view);
}

static void translate_widgets(GtkWidget *widget)
{
    const char *key = g_object_get_data(G_OBJECT(widget), "queue-tooltip");
    if (key) tooltip(widget, key);
    key = g_object_get_data(G_OBJECT(widget), "queue-button-label");
    if (key && GTK_IS_BUTTON(widget) && !g_object_get_data(G_OBJECT(widget), "queue-icon-button"))
        gtk_button_set_label(GTK_BUTTON(widget), tr(key));
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) translate_widgets(child);
}

void queue_view_translate(QueueView *view)
{
    if (!view) return;
    translate_widgets(view->root);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(view->search), tr("Поиск в очереди"));
    gtk_menu_button_set_label(GTK_MENU_BUTTON(view->sort), tr("Сортировка"));
    queue_view_refresh(view);
}

static void horizontal_margins(GtkWidget *widget)
{
    gtk_widget_set_margin_start(widget, 12);
    gtk_widget_set_margin_end(widget, 12);
}

QueueView *queue_view_new(QueueModel *model)
{
    g_return_val_if_fail(model != NULL, NULL);
    QueueView *view = g_new0(QueueView, 1);
    view->model = model;
    view->current_id = -1;
    view->rows = g_ptr_array_new();
    view->drag_token = g_object_new(G_TYPE_OBJECT, NULL);
    view->root = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_set_hexpand(view->root, FALSE);
    gtk_widget_add_css_class(view->root, "queue");
    gtk_widget_add_css_class(view->root, "queue-editor");
    gtk_widget_set_size_request(view->root, 320, -1);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    horizontal_margins(header);
    gtk_widget_set_margin_top(header, 10);
    gtk_widget_set_margin_bottom(header, 8);
    view->count = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(view->count), 0);
    gtk_widget_add_css_class(view->count, "heading");
    gtk_widget_set_hexpand(view->count, TRUE);
    gtk_box_append(GTK_BOX(header), view->count);
    view->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(view->spinner, 16, 16);
    gtk_box_append(GTK_BOX(header), view->spinner);
    view->clear = text_button("Очистить");
    tooltip(view->clear, "Остановить воспроизведение и очистить всю очередь");
    g_signal_connect(view->clear, "clicked", G_CALLBACK(clear_clicked), view);
    gtk_box_append(GTK_BOX(header), view->clear);
    gtk_box_append(GTK_BOX(view->root), header);
    view->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(view->search), tr("Поиск в очереди"));
    horizontal_margins(view->search);
    gtk_widget_set_margin_bottom(view->search, 8);
    g_signal_connect(view->search, "search-changed", G_CALLBACK(search_changed), view);
    gtk_box_append(GTK_BOX(view->root), view->search);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    horizontal_margins(actions);
    gtk_widget_set_margin_bottom(actions, 8);
    view->sort = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(view->sort), tr("Сортировка"));
    gtk_widget_add_css_class(view->sort, "queue-sort");
    tooltip(view->sort, "Сортировать всю очередь");
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *sorts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    const char *labels[] = {"Название: А–Я", "Название: Я–А", "Путь: А–Я", "Путь: Я–А",
        "Дата создания: сначала старые", "Дата создания: сначала новые",
        "Дата изменения: сначала старые", "Дата изменения: сначала новые"};
    for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
        if (i == 4 || i == 6)
            gtk_box_append(GTK_BOX(sorts), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        GtkWidget *button = text_button(labels[i]);
        g_object_set_data(G_OBJECT(button), "queue-sort", GINT_TO_POINTER(i));
        g_signal_connect(button, "clicked", G_CALLBACK(sort_clicked), view);
        gtk_box_append(GTK_BOX(sorts), button);
    }
    gtk_popover_set_child(GTK_POPOVER(popover), sorts);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(view->sort), popover);
    gtk_box_append(GTK_BOX(actions), view->sort);
    view->shuffle = text_button("Перемешать");
    g_object_set_data(G_OBJECT(view->shuffle), "queue-icon-button", GINT_TO_POINTER(TRUE));
    gtk_button_set_child(GTK_BUTTON(view->shuffle), action_icon(TRUE));
    tooltip(view->shuffle, "Перемешать всю очередь");
    g_signal_connect(view->shuffle, "clicked", G_CALLBACK(shuffle_clicked), view);
    gtk_box_append(GTK_BOX(actions), view->shuffle);
    view->repeat = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(view->repeat), action_icon(FALSE));
    gtk_widget_add_css_class(view->repeat, "flat");
    gtk_widget_add_css_class(view->repeat, "queue-action");
    tooltip(view->repeat, "Повторять всю очередь");
    g_signal_connect(view->repeat, "toggled", G_CALLBACK(repeat_toggled), view);
    gtk_box_append(GTK_BOX(actions), view->repeat);
    gtk_box_append(GTK_BOX(view->root), actions);
    view->scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(view->scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    view->list = gtk_list_box_new();
    gtk_widget_add_css_class(view->list, "queue-rows");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(view->list), GTK_SELECTION_MULTIPLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(view->list), TRUE);
    GtkEventController *clicks = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(clicks, GTK_PHASE_CAPTURE);
    g_signal_connect(clicks, "event", G_CALLBACK(list_click_event), view);
    gtk_widget_add_controller(view->list, clicks);
    g_signal_connect(view->list, "unmap", G_CALLBACK(list_unmapped), view);
    g_signal_connect(view->list, "map", G_CALLBACK(list_mapped), view);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(view->list), filter_row, view, NULL);
    tooltip(view->list, "Один щелчок — воспроизвести\nCtrl/Shift — выбрать несколько\nDelete — удалить выбранное");
    g_signal_connect(view->list, "row-activated", G_CALLBACK(row_activated), view);
    g_signal_connect(view->list, "selected-rows-changed", G_CALLBACK(selection_changed), view);
    view->placeholder = gtk_label_new(NULL);
    gtk_widget_add_css_class(view->placeholder, "dim-label");
    gtk_widget_set_margin_top(view->placeholder, 24);
    gtk_widget_set_margin_bottom(view->placeholder, 24);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(view->list), view->placeholder);
    GtkDropTarget *end = gtk_drop_target_new(queue_drag_get_type(), GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(end, TRUE);
    g_signal_connect(end, "enter", G_CALLBACK(list_drop_motion), view);
    g_signal_connect(end, "motion", G_CALLBACK(list_drop_motion), view);
    g_signal_connect(end, "leave", G_CALLBACK(list_drop_leave), view);
    g_signal_connect(end, "drop", G_CALLBACK(drop_at_end), view);
    gtk_widget_add_controller(view->list, GTK_EVENT_CONTROLLER(end));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->scroller), view->list);
    gtk_box_append(GTK_BOX(view->root), view->scroller);
    gtk_box_append(GTK_BOX(view->root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(footer, "queue-selection-bar");
    horizontal_margins(footer);
    gtk_widget_set_margin_top(footer, 4);
    gtk_widget_set_margin_bottom(footer, 4);
    view->selected = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(view->selected), 0);
    gtk_widget_add_css_class(view->selected, "dim-label");
    gtk_widget_set_hexpand(view->selected, TRUE);
    gtk_box_append(GTK_BOX(footer), view->selected);
    view->remove = text_button("Удалить");
    tooltip(view->remove, "Удалить выбранные записи из очереди");
    g_signal_connect(view->remove, "clicked", G_CALLBACK(selected_remove_clicked), view);
    gtk_box_append(GTK_BOX(footer), view->remove);
    gtk_box_append(GTK_BOX(view->root), footer);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), view);
    gtk_widget_add_controller(view->root, keys);
    queue_view_refresh(view);
    return view;
}

GtkWidget *queue_view_widget(QueueView *view) { return view ? view->root : NULL; }
GtkWidget *queue_view_list(QueueView *view) { return view ? view->list : NULL; }
GtkWidget *queue_view_count_label(QueueView *view) { return view ? view->count : NULL; }

gboolean queue_view_wants_key(QueueView *view, GtkWidget *focus, guint key,
                              GdkModifierType modifiers)
{
    if (!view || !focus || GTK_IS_EDITABLE(focus) ||
        !(focus == view->root || gtk_widget_is_ancestor(focus, view->root))) return FALSE;
    if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_a || key == GDK_KEY_A)) return TRUE;
    switch (key) {
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_Escape:
    case GDK_KEY_Up: case GDK_KEY_Down: case GDK_KEY_Home: case GDK_KEY_End:
    case GDK_KEY_Page_Up: case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Up: case GDK_KEY_KP_Down: case GDK_KEY_KP_Home: case GDK_KEY_KP_End:
    case GDK_KEY_KP_Page_Up: case GDK_KEY_KP_Page_Down:
        return !(modifiers & (GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK));
    default: return FALSE;
    }
}

void queue_view_free(QueueView *view)
{
    if (!view) return;
    if (view->restore_source) g_source_remove(view->restore_source);
    cancel_click_reset(view);
    cancel_view_drag(view);
    view->rebuilding = TRUE;
    /* The host removes the root from its revealer before dropping this ref. */
    g_object_unref(view->root);
    g_object_unref(view->drag_token);
    g_ptr_array_unref(view->rows);
    g_free(view->query);
    g_free(view);
}
