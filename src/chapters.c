#include "chapters.h"
#include "i18n.h"

#include <math.h>
#include <string.h>

typedef struct {
    int64_t index;
    double time;
    char *title;
} ChapterItem;

typedef struct {
    GtkMenuButton *button;
    GtkPopover *popover;
    GtkListBox *list;
    GtkWidget *previous, *next;
    GPtrArray *items;
    int64_t current;
    ChaptersSelectFunc on_select;
    ChaptersStepFunc on_step;
    gpointer user_data;
} Chapters;

static const mpv_node *chapter_field(const mpv_node *node, const char *name)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
        return NULL;
    for (int i = 0; i < node->u.list->num; i++)
        if (g_strcmp0(node->u.list->keys[i], name) == 0)
            return &node->u.list->values[i];
    return NULL;
}

static void chapter_item_free(gpointer data)
{
    ChapterItem *item = data;
    g_free(item->title);
    g_free(item);
}

static GPtrArray *chapter_items_from_node(const mpv_node *node)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(chapter_item_free);
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return items;
    for (int i = 0; i < node->u.list->num; i++) {
        const mpv_node *entry = &node->u.list->values[i];
        const mpv_node *time = chapter_field(entry, "time");
        double seconds = time && time->format == MPV_FORMAT_DOUBLE ? time->u.double_
            : time && time->format == MPV_FORMAT_INT64 ? (double)time->u.int64 : NAN;
        if (!isfinite(seconds) || seconds < 0)
            continue;
        ChapterItem *item = g_new0(ChapterItem, 1);
        item->index = i; /* Keep libmpv's indices when malformed entries are skipped. */
        item->time = seconds;
        const mpv_node *title = chapter_field(entry, "title");
        if (title && title->format == MPV_FORMAT_STRING && title->u.string) {
            item->title = g_utf8_make_valid(title->u.string, -1);
            g_strstrip(item->title);
            if (!*item->title)
                g_clear_pointer(&item->title, g_free);
        }
        g_ptr_array_add(items, item);
    }
    return items;
}

static char *chapter_title(const ChapterItem *item)
{
    return item->title ? g_strdup(item->title)
        : g_strdup_printf(tr("Глава %u"), (unsigned)item->index + 1);
}

static char *chapter_time(double seconds)
{
    guint64 whole = (guint64)floor(fmin(seconds, (double)G_MAXINT64));
    return g_strdup_printf("%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
                           whole / 3600, whole / 60 % 60, whole % 60);
}

static void chapters_free(gpointer data)
{
    Chapters *p = data;
    g_ptr_array_unref(p->items);
    g_free(p);
}

static void clear_content(Chapters *p)
{
    p->list = NULL;
    p->previous = p->next = NULL;
    gtk_popover_set_child(p->popover, NULL);
}

static void refresh_current(Chapters *p)
{
    if (!p->list)
        return;
    GtkListBoxRow *selected = NULL;
    for (guint i = 0; i < p->items->len; i++) {
        const ChapterItem *item = g_ptr_array_index(p->items, i);
        if (item->index == p->current) {
            selected = gtk_list_box_get_row_at_index(p->list, (int)i);
            break;
        }
    }
    gtk_list_box_select_row(p->list, selected);
    const ChapterItem *last = p->items->len
        ? g_ptr_array_index(p->items, p->items->len - 1) : NULL;
    /* mpv's decrement can restart the current chapter according to its
     * chapter-seek-threshold, including at the first chapter. */
    gtk_widget_set_sensitive(p->previous, last && p->current >= 0);
    gtk_widget_set_sensitive(p->next, last && p->current < last->index);
}

static void chapter_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list;
    Chapters *p = data;
    int position = gtk_list_box_row_get_index(row);
    if (position < 0 || (guint)position >= p->items->len)
        return;
    const ChapterItem *item = g_ptr_array_index(p->items, (guint)position);
    int64_t index = item->index;
    ChaptersSelectFunc select = p->on_select;
    gpointer user_data = p->user_data;
    gtk_menu_button_popdown(p->button);
    if (select)
        select(index, user_data);
}

static void step_clicked(GtkButton *button, gpointer data)
{
    Chapters *p = data;
    int direction = GTK_WIDGET(button) == p->previous ? -1 : 1;
    if (p->on_step)
        p->on_step(direction, p->user_data);
}

static GtkWidget *step_button(Chapters *p, const char *icon, const char *tooltip)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_tooltip_text(button, tr(tooltip));
    g_object_set_data(G_OBJECT(button), "mpv-tooltip-key", (gpointer)tooltip);
    g_signal_connect(button, "clicked", G_CALLBACK(step_clicked), p);
    return button;
}

static gboolean title_tooltip(GtkWidget *widget, int x, int y,
                              gboolean keyboard_mode, GtkTooltip *tooltip,
                              gpointer data)
{
    (void)x; (void)y; (void)keyboard_mode; (void)data;
    GtkLabel *label = GTK_LABEL(widget);
    if (!pango_layout_is_ellipsized(gtk_label_get_layout(label)))
        return FALSE;
    gtk_tooltip_set_text(tooltip, gtk_label_get_text(label));
    return TRUE;
}

static void build_content(Chapters *p)
{
    clear_content(p);
    if (!p->items->len)
        return;
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(content, 4);
    gtk_widget_set_margin_bottom(content, 4);
    gtk_widget_set_margin_start(content, 4);
    gtk_widget_set_margin_end(content, 4);
    gtk_widget_set_valign(content, GTK_ALIGN_START);
    gtk_widget_set_vexpand(content, FALSE);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 6);
    GtkWidget *title = gtk_label_new(tr("Главы"));
    g_object_set_data(G_OBJECT(title), "mpv-label-key", "Главы");
    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(header), title);
    p->previous = step_button(p, "go-previous-symbolic", "Предыдущая глава");
    p->next = step_button(p, "go-next-symbolic", "Следующая глава");
    gtk_box_append(GTK_BOX(header), p->previous);
    gtk_box_append(GTK_BOX(header), p->next);
    gtk_box_append(GTK_BOX(content), header);
    p->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(p->list, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(p->list, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(p->list), "navigation-sidebar");
    gtk_widget_add_css_class(GTK_WIDGET(p->list), "chapters-list");
    gtk_widget_set_valign(GTK_WIDGET(p->list), GTK_ALIGN_START);
    gtk_widget_set_vexpand(GTK_WIDGET(p->list), FALSE);
    for (guint i = 0; i < p->items->len; i++) {
        const ChapterItem *item = g_ptr_array_index(p->items, i);
        g_autofree char *name = chapter_title(item);
        g_autofree char *time = chapter_time(item->time);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(row, 6);
        gtk_widget_set_margin_bottom(row, 6);
        GtkWidget *label = gtk_label_new(name);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_set_has_tooltip(label, TRUE);
        g_signal_connect(label, "query-tooltip", G_CALLBACK(title_tooltip), NULL);
        gtk_box_append(GTK_BOX(row), label);
        GtkWidget *stamp = gtk_label_new(time);
        gtk_widget_add_css_class(stamp, "timecode");
        gtk_widget_add_css_class(stamp, "dim-label");
        gtk_box_append(GTK_BOX(row), stamp);
        gtk_list_box_append(p->list, row);
    }
    g_signal_connect(p->list, "row-activated", G_CALLBACK(chapter_activated), p);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_valign(scroll, GTK_ALIGN_START);
    gtk_widget_set_vexpand(scroll, FALSE);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 0);
    gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(scroll), 340);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 0);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(p->list));
    gtk_box_append(GTK_BOX(content), scroll);
    gtk_popover_set_child(p->popover, content);
    refresh_current(p);
}

static void active_changed(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Chapters *p = data;
    if (gtk_menu_button_get_active(GTK_MENU_BUTTON(object)) && !p->list)
        build_content(p);
    else if (!gtk_menu_button_get_active(GTK_MENU_BUTTON(object)))
        clear_content(p);
}

static void create_popup(GtkMenuButton *button, gpointer data)
{
    (void)button;
    /* GTK invokes this before popup placement, so the first measurement sees
     * the real chapter list. Closed menus keep only their empty container. */
    build_content(data);
}

static void popup_mapped(GtkWidget *popover, gpointer data)
{
    (void)popover;
    Chapters *p = data;
    if (!p->list)
        return;
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(p->list);
    if (!selected)
        return;
    gtk_widget_grab_focus(GTK_WIDGET(selected));
    GtkWidget *viewport = gtk_widget_get_parent(GTK_WIDGET(p->list));
    if (GTK_IS_VIEWPORT(viewport))
        gtk_viewport_scroll_to(GTK_VIEWPORT(viewport), GTK_WIDGET(selected), NULL);
}

GtkWidget *chapters_button_new(ChaptersSelectFunc on_select,
                                ChaptersStepFunc on_step, gpointer user_data)
{
    GtkWidget *button = gtk_menu_button_new();
    Chapters *p = g_new0(Chapters, 1);
    p->button = GTK_MENU_BUTTON(button);
    p->items = g_ptr_array_new_with_free_func(chapter_item_free);
    p->current = -1;
    p->on_select = on_select;
    p->on_step = on_step;
    p->user_data = user_data;
    p->popover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_add_css_class(GTK_WIDGET(p->popover), "chapters-popover");
    gtk_menu_button_set_has_frame(p->button, FALSE);
    gtk_widget_set_focus_on_click(button, FALSE);
    /* GtkMenuButton wraps a GtkToggleButton; the wrapper's click-focus
     * property does not propagate to that actual input target. */
    GtkWidget *toggle = gtk_widget_get_first_child(button);
    if (GTK_IS_BUTTON(toggle))
        gtk_widget_set_focus_on_click(toggle, FALSE);
    gtk_menu_button_set_direction(p->button, GTK_ARROW_UP);
    gtk_popover_set_position(p->popover, GTK_POS_TOP);
    gtk_menu_button_set_popover(p->button, GTK_WIDGET(p->popover));
    gtk_menu_button_set_create_popup_func(p->button, create_popup, p, NULL);
    gtk_menu_button_set_icon_name(p->button, "view-list-ordered-symbolic");
    const char *tooltip = "Главы · Ctrl+Page Up / Ctrl+Page Down\nПКМ — следующая глава";
    gtk_widget_set_tooltip_text(button, tr(tooltip));
    g_object_set_data(G_OBJECT(button), "mpv-tooltip-key", (gpointer)tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   tr("Главы"), -1);
    g_object_set_data_full(G_OBJECT(button), "mpv-gtk-chapters", p, chapters_free);
    g_signal_connect(button, "notify::active", G_CALLBACK(active_changed), p);
    g_signal_connect(p->popover, "map", G_CALLBACK(popup_mapped), p);
    gtk_widget_set_visible(button, FALSE);
    return button;
}

void chapters_button_update(GtkWidget *button, const mpv_node *chapter_list, int64_t current)
{
    Chapters *p = g_object_get_data(G_OBJECT(button), "mpv-gtk-chapters");
    g_return_if_fail(p != NULL);
    GPtrArray *items = chapter_items_from_node(chapter_list);
    g_ptr_array_unref(p->items);
    p->items = items;
    p->current = current;
    if (!items->len)
        gtk_menu_button_popdown(p->button);
    gtk_widget_set_visible(button, items->len > 0);
    if (gtk_menu_button_get_active(p->button))
        build_content(p);
    else
        clear_content(p);
}

void chapters_button_set_current(GtkWidget *button, int64_t current)
{
    Chapters *p = g_object_get_data(G_OBJECT(button), "mpv-gtk-chapters");
    g_return_if_fail(p != NULL);
    p->current = current;
    refresh_current(p);
}

void chapters_button_reset(GtkWidget *button)
{
    chapters_button_update(button, NULL, -1);
}
