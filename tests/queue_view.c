/* Check the real GTK editor without presenting a window. Reuse the isolated
 * native-core polling helpers from the model integration check. */
#define main queue_model_check_main
#include "queue_model.c"
#undef main
#include "../src/queue_view.h"
#include "../src/i18n.h"

typedef struct { Host host; QueueView *view; } ViewHost;

static void view_changed(gpointer data)
{
    ViewHost *host = data;
    host->host.changed++;
    if (host->view) queue_view_refresh(host->view);
}

static GtkWidget *find_type(GtkWidget *widget, GType type)
{
    if (g_type_is_a(G_OBJECT_TYPE(widget), type)) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_type(child, type);
        if (found) return found;
    }
    return NULL;
}

static GtkWidget *find_button(GtkWidget *widget, const char *key)
{
    if (g_strcmp0(g_object_get_data(G_OBJECT(widget), "queue-button-label"), key) == 0) return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_button(child, key);
        if (found) return found;
    }
    return NULL;
}

static GtkListBoxRow *row_named(QueueView *view, const char *title)
{
    GtkListBox *list = GTK_LIST_BOX(queue_view_list(view));
    for (int i = 0;; ++i) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, i);
        if (!row) return NULL;
        GtkWidget *box = gtk_list_box_row_get_child(row);
        for (GtkWidget *child = gtk_widget_get_first_child(box); child; child = gtk_widget_get_next_sibling(child))
            if (GTK_IS_LABEL(child) && gtk_widget_has_css_class(child, "queue-entry-title") &&
                g_strcmp0(gtk_label_get_text(GTK_LABEL(child)), title) == 0) return row;
    }
}

static guint visible_rows(QueueView *view)
{
    guint visible = 0;
    GtkListBox *list = GTK_LIST_BOX(queue_view_list(view));
    for (int i = 0;; ++i) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, i);
        if (!row) return visible;
        visible += gtk_widget_get_child_visible(GTK_WIDGET(row));
    }
}

static guint selection_count(QueueView *view)
{
    GList *selected = gtk_list_box_get_selected_rows(GTK_LIST_BOX(queue_view_list(view)));
    guint count = g_list_length(selected); g_list_free(selected); return count;
}

static void query(GtkWidget *entry, const char *text)
{
    gtk_editable_set_text(GTK_EDITABLE(entry), text);
    g_signal_emit_by_name(entry, "search-changed");
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static GObject *controller(GtkWidget *widget, GType type)
{
    g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        GObject *item = g_list_model_get_item(controllers, i);
        if (g_type_is_a(G_OBJECT_TYPE(item), type)) return item;
        g_object_unref(item);
    }
    return NULL;
}

static gboolean press(GtkWidget *root, guint key, GdkModifierType state)
{
    g_autoptr(GObject) keys = controller(root, GTK_TYPE_EVENT_CONTROLLER_KEY);
    g_assert_nonnull(keys);
    gboolean handled = FALSE;
    g_signal_emit_by_name(keys, "key-pressed", key, 0, state, &handled);
    return handled;
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    gdk_set_allowed_backends("wayland");
    if (!gtk_init_check()) { puts("SKIP: native Wayland display is unavailable."); return 77; }
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-queue-view-XXXXXX", NULL);
    g_autofree char *playlist = g_build_filename(directory, "queue.m3u", NULL);
    const char *contents = "#EXTM3U\n#EXTINF:-1,Alpha\n/tmp/queue-test-alpha.mp4\n"
        "#EXTINF:-1,Бета\n/tmp/queue-test-beta.mp4\n"
        "#EXTINF:-1,Other\n/tmp/queue-folder-needle.mp4\n"
        "#EXTINF:-1,Alpha copy\n/tmp/queue-test-alpha.mp4\n";
    g_assert_true(g_file_set_contents(playlist, contents, -1, NULL));
    ViewHost test = {.host.mpv = mpv_create()}; Host *host = &test.host;
    const char *options[][2] = {{"config", "no"}, {"vo", "null"}, {"ao", "null"},
        {"idle", "yes"}, {"pause", "yes"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"osc", "no"}, {"ytdl", "no"}};
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(host->mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(host->mpv), >=, 0);
    host->model = queue_model_new(host->mpv, view_changed, failed, &test);
    g_assert_cmpint(mpv_observe_property(host->mpv, 1, "playlist", MPV_FORMAT_NODE), >=, 0);
    const char *load[] = {"loadlist", playlist, "append", NULL};
    g_assert_cmpint(mpv_command(host->mpv, load), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (queue_model_entries(host->model)->len != 4) {
        if (g_get_monotonic_time() > deadline) g_error("Idle playlist did not populate.");
        poll_host(host);
    }
    test.view = queue_view_new(host->model);
    GtkWidget *root = queue_view_widget(test.view), *list = queue_view_list(test.view);
    GtkWidget *window = gtk_window_new(); gtk_window_set_child(GTK_WINDOW(window), root);
    GtkWidget *search = find_type(root, GTK_TYPE_SEARCH_ENTRY);
    GtkWidget *remove = find_button(root, "Удалить"), *clear = find_button(root, "Очистить");
    g_assert_nonnull(search); g_assert_nonnull(remove); g_assert_nonnull(clear);
    const char *date_labels[] = {"Дата создания: сначала старые", "Дата создания: сначала новые",
        "Дата изменения: сначала старые", "Дата изменения: сначала новые"};
    for (guint i = 0; i < G_N_ELEMENTS(date_labels); ++i)
        g_assert_nonnull(find_button(root, date_labels[i]));
    GtkWidget *shuffle = find_button(root, "Перемешать");
    g_assert_true(GTK_IS_DRAWING_AREA(gtk_button_get_child(GTK_BUTTON(shuffle))));
    GtkWidget *repeat = gtk_widget_get_next_sibling(shuffle);
    g_assert_true(GTK_IS_TOGGLE_BUTTON(repeat));
    g_assert_true(GTK_IS_DRAWING_AREA(gtk_button_get_child(GTK_BUTTON(repeat))));
    /* A filename must not make the sidebar's natural width swallow video. */
    GtkWidget *title = gtk_widget_get_first_child(
        gtk_list_box_row_get_child(row_named(test.view, "Alpha")));
    while (title && !gtk_widget_has_css_class(title, "queue-entry-title"))
        title = gtk_widget_get_next_sibling(title);
    g_assert_true(GTK_IS_LABEL(title));
    g_autofree char *short_title = g_strdup(gtk_label_get_text(GTK_LABEL(title)));
    gtk_label_set_text(GTK_LABEL(title),
        "Replay 2026-10-26 23-59-59 — A very long recording title with multiple "
        "audio tracks, commentary, game details and another lengthy suffix.mp4");
    int minimum = 0, natural = 0;
    gtk_widget_measure(root, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, NULL, NULL);
    g_assert_cmpint(natural, <=, 400);
    g_assert_false(gtk_widget_compute_expand(root, GTK_ORIENTATION_HORIZONTAL));
    gtk_label_set_text(GTK_LABEL(title), short_title);
    g_assert_cmpint(gtk_list_box_get_selection_mode(GTK_LIST_BOX(list)), ==, GTK_SELECTION_MULTIPLE);
    g_assert_false(gtk_list_box_get_activate_on_single_click(GTK_LIST_BOX(list)));
    /* The core may already mark an idle first entry current; initial view
     * selection is independent of this explicit two-entry selection check. */
    gtk_list_box_unselect_all(GTK_LIST_BOX(list));
    gtk_list_box_select_row(GTK_LIST_BOX(list), row_named(test.view, "Бета"));
    gtk_list_box_select_row(GTK_LIST_BOX(list), row_named(test.view, "Alpha copy"));
    g_assert_cmpuint(selection_count(test.view), ==, 2);
    queue_model_sort(host->model, QUEUE_SORT_TITLE_ASC); settle(host); assert_ok(host);
    g_assert_cmpuint(selection_count(test.view), ==, 2);
    g_assert_true(gtk_list_box_row_is_selected(row_named(test.view, "Бета")));
    g_assert_true(gtk_list_box_row_is_selected(row_named(test.view, "Alpha copy")));
    /* A date menu action must reach the model while retaining the selection;
     * every missing local file is an unknown date, so this is a stable no-op. */
    g_signal_emit_by_name(find_button(root, date_labels[3]), "clicked"); settle(host); assert_ok(host);
    g_assert_cmpuint(selection_count(test.view), ==, 2);
    queue_model_set_position(host->model, 0);
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
    g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(first), "queue-current"));
    g_assert_cmpuint(selection_count(test.view), ==, 2);

    query(search, "БЕТА"); g_assert_cmpuint(visible_rows(test.view), ==, 1);
    g_assert_cmpuint(selection_count(test.view), ==, 1);
    query(search, "FOLDER-NEEDLE"); g_assert_cmpuint(visible_rows(test.view), ==, 1);
    query(search, "not present"); g_assert_cmpuint(visible_rows(test.view), ==, 0);
    query(search, ""); g_assert_cmpuint(visible_rows(test.view), ==, 4);
    g_assert_true(queue_view_wants_key(test.view, list, GDK_KEY_Delete, 0));
    g_assert_false(queue_view_wants_key(test.view, search, GDK_KEY_Delete, 0));
    gtk_window_set_focus(GTK_WINDOW(window), list);
    g_assert_true(press(root, GDK_KEY_a, GDK_CONTROL_MASK));
    g_assert_cmpuint(selection_count(test.view), ==, 4);
    g_assert_true(press(root, GDK_KEY_Escape, 0));
    g_assert_cmpuint(selection_count(test.view), ==, 0);

    /* Emit GTK's typed drag/drop signals, preserving process-local identity. */
    GtkListBoxRow *from = row_named(test.view, "Other"), *to = row_named(test.view, "Alpha");
    GtkWidget *handle = gtk_widget_get_first_child(gtk_list_box_row_get_child(from));
    g_autoptr(GObject) drag = controller(handle, GTK_TYPE_DRAG_SOURCE);
    g_autoptr(GObject) drop = controller(GTK_WIDGET(to), GTK_TYPE_DROP_TARGET);
    GdkContentProvider *provider = NULL; g_signal_emit_by_name(drag, "prepare", 0.0, 0.0, &provider);
    g_assert_nonnull(provider);
    GValue payload = G_VALUE_INIT; g_value_init(&payload, g_type_from_name("QueueDrag"));
    g_assert_true(gdk_content_provider_get_value(provider, &payload, NULL));
    QueueView *other_view = queue_view_new(host->model);
    g_autoptr(GObject) other_drop = controller(GTK_WIDGET(row_named(other_view, "Alpha")), GTK_TYPE_DROP_TARGET);
    gboolean accepted = TRUE;
    g_signal_emit_by_name(other_drop, "drop", &payload, 0.0, -1.0, &accepted);
    g_assert_false(accepted); queue_view_free(other_view);
    g_signal_emit_by_name(drop, "drop", &payload, 0.0, -1.0, &accepted); g_assert_true(accepted);
    queue_view_refresh(test.view); g_assert_false(gtk_widget_get_sensitive(clear));
    g_value_unset(&payload); g_object_unref(provider); settle(host); assert_ok(host);
    g_assert_true(row_named(test.view, "Other") == gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
    g_assert_true(gtk_widget_get_sensitive(clear));

    /* Filtered positions differ from native positions: the hidden first
     * entry must stay first when the two visible duplicates are reordered. */
    query(search, "ALPHA");
    from = row_named(test.view, "Alpha copy"); to = row_named(test.view, "Alpha");
    handle = gtk_widget_get_first_child(gtk_list_box_row_get_child(from));
    g_autoptr(GObject) filtered_drag = controller(handle, GTK_TYPE_DRAG_SOURCE);
    g_autoptr(GObject) filtered_drop = controller(GTK_WIDGET(to), GTK_TYPE_DROP_TARGET);
    provider = NULL; g_signal_emit_by_name(filtered_drag, "prepare", 0.0, 0.0, &provider);
    g_assert_nonnull(provider); g_value_init(&payload, g_type_from_name("QueueDrag"));
    g_assert_true(gdk_content_provider_get_value(provider, &payload, NULL));
    g_signal_emit_by_name(filtered_drop, "drop", &payload, 0.0, -1.0, &accepted);
    g_assert_true(accepted); g_value_unset(&payload); g_object_unref(provider);
    settle(host); assert_ok(host); query(search, "");
    g_assert_true(row_named(test.view, "Other") == gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
    g_assert_true(row_named(test.view, "Alpha copy") == gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 1));
    g_assert_true(row_named(test.view, "Alpha") == gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 2));

    gtk_list_box_select_row(GTK_LIST_BOX(list), row_named(test.view, "Бета"));
    gtk_list_box_select_row(GTK_LIST_BOX(list), row_named(test.view, "Other"));
    gtk_window_set_focus(GTK_WINDOW(window), list);
    g_assert_true(press(root, GDK_KEY_Delete, 0)); settle(host); assert_ok(host);
    g_assert_cmpint(integer(host->mpv, "playlist-count"), ==, 2);
    g_assert_null(row_named(test.view, "Бета")); g_assert_null(row_named(test.view, "Other"));
    i18n_set_language("en"); queue_view_translate(test.view);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(queue_view_count_label(test.view))), ==, "Queue · 2");
    g_assert_true(GTK_IS_DRAWING_AREA(gtk_button_get_child(GTK_BUTTON(shuffle))));
    g_assert_true(GTK_IS_DRAWING_AREA(gtk_button_get_child(GTK_BUTTON(repeat))));
    g_signal_emit_by_name(clear, "clicked"); settle(host); assert_ok(host);
    g_assert_cmpint(integer(host->mpv, "playlist-count"), ==, 0);
    g_assert_false(gtk_widget_get_sensitive(clear)); g_assert_false(gtk_widget_get_sensitive(remove));
    gtk_window_set_child(GTK_WINDOW(window), NULL); queue_view_free(test.view); test.view = NULL;
    gtk_window_destroy(GTK_WINDOW(window)); queue_model_free(host->model); host->model = NULL;
    mpv_terminate_destroy(host->mpv); g_free(host->error); remove_tree(directory);
    puts("PASS queue GTK: Unicode/path search, stable multi-selection, current marker, keyboard edits, typed owner-scoped drag/drop, busy actions, translations and clear; window never presented.");
    return 0;
}
