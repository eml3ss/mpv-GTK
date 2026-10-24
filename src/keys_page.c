#include "keys_page.h"
#include "key_commands.h"
#include "input.h"
#include "i18n.h"

#include <string.h>

typedef struct {
    GtkWidget *page;
    GtkListBox *list;
    GtkSizeGroup *key_columns;
    GtkLabel *summary, *error;
    GtkWidget *add;
    GtkWindow *editor;
    GPtrArray *rows, *commands;
    KeyBindingDefault *defaults;
    gsize default_count;
    char *input_path, *search;
    KeysPageChangeFunc on_change;
    gpointer user_data;
    gboolean have_bindings;
} KeysPage;

typedef struct {
    KeysPage *owner;
    GtkWindow *window;
    GtkEntry *key, *command;
    GtkDropDown *catalog, *presets;
    GtkLabel *description, *signature, *example, *error, *conflict;
    GtkButton *capture;
    GdkToplevel *capture_toplevel;
    GPtrArray *commands;
    char *old_key;
    gboolean updating, capturing;
} KeyEditor;

typedef struct {
    KeysPage *owner;
    char *key, *command;
} RowAction;

static void open_editor(KeysPage *p, const char *key, const char *command);

static GtkWidget *text_label(const char *text, gboolean dim)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    if (dim)
        gtk_widget_add_css_class(label, "dim-label");
    return label;
}

static KeysPage *page_data(GtkWidget *page)
{
    return g_object_get_data(G_OBJECT(page), "mpv-keys-page");
}

static void free_defaults(KeysPage *p)
{
    for (gsize i = 0; i < p->default_count; ++i) {
        g_free((gpointer)p->defaults[i].key);
        g_free((gpointer)p->defaults[i].command);
        g_free((gpointer)p->defaults[i].comment);
    }
    g_clear_pointer(&p->defaults, g_free);
    p->default_count = 0;
}

static void free_page(gpointer data)
{
    KeysPage *p = data;
    if (p->editor) {
        g_object_remove_weak_pointer(G_OBJECT(p->editor), (gpointer *)&p->editor);
        gtk_window_destroy(p->editor);
    }
    g_clear_pointer(&p->rows, g_ptr_array_unref);
    g_clear_pointer(&p->commands, g_ptr_array_unref);
    g_clear_object(&p->key_columns);
    free_defaults(p);
    g_free(p->input_path);
    g_free(p->search);
    g_free(p);
}

static void free_row_action(gpointer data)
{
    RowAction *action = data;
    g_free(action->key);
    g_free(action->command);
    g_free(action);
}

static gboolean filter_row(GtkListBoxRow *row, gpointer data)
{
    KeysPage *p = data;
    const char *haystack = g_object_get_data(G_OBJECT(row), "mpv-keys-search");
    return !p->search || !*p->search || (haystack && strstr(haystack, p->search));
}

static void search_changed(GtkSearchEntry *entry, gpointer data)
{
    KeysPage *p = data;
    g_free(p->search);
    p->search = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(entry)), -1);
    gtk_list_box_invalidate_filter(p->list);
}

static void edit_row(GtkButton *button, gpointer data)
{
    (void)button;
    RowAction *action = data;
    open_editor(action->owner, action->key, action->command);
}

static void update_summary(KeysPage *p)
{
    g_autofree char *summary = p->have_bindings
        ? g_strdup_printf(tr("Привязок: %u · Команд: %u"),
                          p->rows ? p->rows->len : 0, p->commands ? p->commands->len : 0)
        : g_strdup(tr("Загрузка привязок mpv…"));
    gtk_label_set_text(p->summary, summary);
}

static char *display_key(const char *key)
{
    static const struct { const char *raw, *display; } names[] = {
        {"KP_SUBTRACT", "Num −"}, {"KP_MULTIPLY", "Num ×"},
        {"KP_DIVIDE", "Num ÷"}, {"KP_ADD", "Num +"},
        {"KP_ENTER", "Num Enter"}, {"KP_DEC", "Num ."},
        {"SPACE", "Space"}, {"ENTER", "Enter"}, {"ESC", "Esc"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        if (!g_str_has_suffix(key, names[i].raw))
            continue;
        gsize prefix_length = strlen(key) - strlen(names[i].raw);
        if (prefix_length && key[prefix_length - 1] != '+')
            continue;
        g_autofree char *prefix = g_strndup(key, prefix_length);
        return g_strconcat(prefix, names[i].display, NULL);
    }
    return g_strdup(key);
}

static void rebuild_rows(KeysPage *p)
{
    gtk_list_box_remove_all(p->list);
    g_clear_object(&p->key_columns);
    p->key_columns = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    gboolean editable = p->on_change && p->have_bindings && p->commands && p->commands->len;
    gtk_widget_set_sensitive(p->add, editable);
    for (guint i = 0; p->rows && i < p->rows->len; ++i) {
        const KeyBinding *binding = g_ptr_array_index(p->rows, i);
        GtkWidget *list_row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        gtk_widget_set_margin_top(box, 12);
        gtk_widget_set_margin_bottom(box, 12);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), box);
        GtkWidget *key_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_size_request(key_column, 100, -1);
        gtk_widget_set_valign(key_column, GTK_ALIGN_START);
        gtk_size_group_add_widget(p->key_columns, key_column);
        g_autofree char *key_text = display_key(binding->key);
        GtkWidget *key = text_label(key_text, FALSE);
        gtk_widget_add_css_class(key, "monospace");
        gtk_widget_add_css_class(key, "keybind-keycap");
        gtk_label_set_wrap(GTK_LABEL(key), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(key), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(key), 10);
        gtk_widget_set_halign(key, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(key, binding->key);
        gtk_box_append(GTK_BOX(key_column), key);
        gtk_box_append(GTK_BOX(box), key_column);
        GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(labels, TRUE);
        GtkWidget *command = text_label(binding->command, FALSE);
        gtk_label_set_wrap(GTK_LABEL(command), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(command), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(command), 40);
        gtk_widget_set_tooltip_text(command, binding->command);
        gtk_box_append(GTK_BOX(labels), command);
        g_autofree char *description = key_command_description(binding->command);
        GtkWidget *help = text_label(description, TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(help), 64);
        gtk_box_append(GTK_BOX(labels), help);
        const char *source = binding->app_default ? tr("Интерфейс")
                           : binding->weak ? tr("Стандарт mpv") : tr("Пользовательская");
        g_autofree char *origin = g_strdup_printf(tr("%s · %s"), source,
                                     binding->active ? tr("Активна") : tr("Неактивна"));
        gtk_box_append(GTK_BOX(labels), text_label(origin, TRUE));
        if (binding->comment && *binding->comment)
            gtk_box_append(GTK_BOX(labels), text_label(tr(binding->comment), TRUE));
        gtk_box_append(GTK_BOX(box), labels);
        GtkWidget *button = gtk_button_new_with_label(tr("Изменить"));
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        /* Leave room for the overlay scrollbar inside the list panel. */
        gtk_widget_set_margin_end(button, 12);
        gtk_widget_set_sensitive(button, editable);
        RowAction *action = g_new0(RowAction, 1);
        action->owner = p;
        action->key = g_strdup(binding->key);
        action->command = g_strdup(binding->command);
        g_object_set_data_full(G_OBJECT(button), "mpv-keys-row", action, free_row_action);
        g_signal_connect(button, "clicked", G_CALLBACK(edit_row), action);
        gtk_box_append(GTK_BOX(box), button);
        g_autofree char *combined = g_strdup_printf("%s %s %s %s %s %s %s",
            binding->key, key_text, binding->command, description, source,
            binding->comment ? binding->comment : "", binding->section ? binding->section : "");
        g_object_set_data_full(G_OBJECT(list_row), "mpv-keys-search",
                              g_utf8_casefold(combined, -1), g_free);
        gtk_list_box_append(p->list, list_row);
    }
    update_summary(p);
    gtk_list_box_invalidate_filter(p->list);
}

static const KeyCommandSpec *find_command(KeyEditor *e, const char *line, guint *index)
{
    g_auto(GStrv) words = g_strsplit_set(line, " \t;", -1);
    char *base = NULL;
    for (guint i = 0; words[i]; ++i) {
        if (!*words[i] || g_str_equal(words[i], "no-osd") || g_str_equal(words[i], "osd-auto") ||
            g_str_equal(words[i], "osd-msg") || g_str_equal(words[i], "osd-bar") ||
            g_str_equal(words[i], "osd-msg-bar") || g_str_equal(words[i], "async") ||
            g_str_equal(words[i], "sync") || g_str_equal(words[i], "raw") ||
            g_str_equal(words[i], "expand-properties") || g_str_equal(words[i], "repeatable") ||
            g_str_equal(words[i], "nonrepeatable") || g_str_equal(words[i], "nonscalable"))
            continue;
        base = words[i];
        break;
    }
    if (base) {
        g_strdelimit(base, "_", '-');
        for (guint i = 0; e->commands && i < e->commands->len; ++i) {
            const KeyCommandSpec *spec = g_ptr_array_index(e->commands, i);
            if (g_str_equal(spec->name, base)) {
                if (index) *index = i;
                return spec;
            }
        }
    }
    if (index) *index = GTK_INVALID_LIST_POSITION;
    return NULL;
}

static void command_changed(GtkEditable *editable, gpointer data)
{
    KeyEditor *e = data;
    const char *line = gtk_editable_get_text(editable);
    guint selected;
    const KeyCommandSpec *spec = find_command(e, line, &selected);
    e->updating = TRUE;
    gtk_drop_down_set_selected(e->catalog, selected);
    gsize preset_count;
    const KeyCommandPreset *presets = key_command_presets(&preset_count);
    guint preset = GTK_INVALID_LIST_POSITION;
    for (gsize i = 0; i < preset_count; ++i) {
        if (g_str_equal(line, presets[i].command)) {
            preset = (guint)i;
            break;
        }
    }
    gtk_drop_down_set_selected(e->presets, preset);
    e->updating = FALSE;
    g_autofree char *description = *line ? key_command_description(line)
        : g_strdup(tr("Выберите команду или введите её с аргументами."));
    gtk_label_set_text(e->description, description);
    g_autofree char *signature = spec ? g_strdup_printf(tr("Синтаксис: %s"), spec->signature) : g_strdup("");
    gtk_label_set_text(e->signature, signature);
    g_autofree char *example = spec && spec->example && *spec->example
        ? g_strdup_printf(tr("Пример: %s"), spec->example) : g_strdup("");
    gtk_label_set_text(e->example, example);
}

static void catalog_changed(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    KeyEditor *e = data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (e->updating || !e->commands || selected >= e->commands->len)
        return;
    const KeyCommandSpec *spec = g_ptr_array_index(e->commands, selected);
    gtk_editable_set_text(GTK_EDITABLE(e->command),
                         spec->example && *spec->example ? spec->example : spec->name);
}

static void preset_changed(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    KeyEditor *e = data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    gsize count;
    const KeyCommandPreset *presets = key_command_presets(&count);
    if (e->updating || selected >= count)
        return;
    gtk_editable_set_text(GTK_EDITABLE(e->command), presets[selected].command);
}

static void release_capture_shortcuts(KeyEditor *e)
{
    if (!e->capture_toplevel)
        return;
    if (!gdk_surface_is_destroyed(GDK_SURFACE(e->capture_toplevel)))
        gdk_toplevel_restore_system_shortcuts(e->capture_toplevel);
    g_clear_object(&e->capture_toplevel);
}

static void set_capturing(KeyEditor *e, gboolean capturing)
{
    if (e->capturing == capturing)
        return;
    e->capturing = capturing;
    if (capturing) {
        GtkNative *native = gtk_widget_get_native(GTK_WIDGET(e->window));
        GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
        if (GDK_IS_TOPLEVEL(surface)) {
            e->capture_toplevel = g_object_ref(GDK_TOPLEVEL(surface));
            /* Wayland otherwise reserves compositor shortcuts before GTK
             * can deliver their key events to the recording controller. */
            gdk_toplevel_inhibit_system_shortcuts(e->capture_toplevel, NULL);
        }
    } else {
        release_capture_shortcuts(e);
    }
    gtk_button_set_label(e->capture,
        capturing ? tr("Нажмите сочетание…") : tr("Записать сочетание"));
    gtk_label_set_text(e->error,
        capturing ? tr("Нажмите сочетание. Esc отменяет запись.") : "");
}

static void capture_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    KeyEditor *e = data;
    set_capturing(e, !e->capturing);
}

static void capture_active_changed(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    KeyEditor *e = data;
    if (!gtk_window_is_active(GTK_WINDOW(object)))
        set_capturing(e, FALSE);
}

static void capture_unrealized(GtkWidget *widget, gpointer data)
{
    (void)widget;
    KeyEditor *e = data;
    e->capturing = FALSE;
    release_capture_shortcuts(e);
}

static gboolean capture_key(GtkEventControllerKey *controller, guint key, guint code,
                            GdkModifierType modifiers, gpointer data)
{
    (void)controller; (void)code;
    KeyEditor *e = data;
    if (!e->capturing)
        return FALSE;
    GdkModifierType shortcut_modifiers = GDK_SHIFT_MASK | GDK_CONTROL_MASK |
        GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK;
    gboolean cancel = key == GDK_KEY_Escape && !(modifiers & shortcut_modifiers);
    if (!cancel) {
        g_autofree char *name = input_key_name(key, modifiers);
        /* Pure modifiers have no binding yet. Keep recording until a primary
         * key arrives, and consume the event instead of activating the UI. */
        if (!name)
            return TRUE;
        gtk_editable_set_text(GTK_EDITABLE(e->key), name);
    }
    set_capturing(e, FALSE);
    return TRUE;
}

static gboolean apply_edit(KeyEditor *e, const char *command, gboolean reset)
{
    const char *key = reset ? e->old_key : gtk_editable_get_text(GTK_EDITABLE(e->key));
    if (!key || !*key) {
        gtk_label_set_text(e->error, tr("Укажите клавишу или сочетание."));
        return FALSE;
    }
    g_autoptr(GError) error = NULL;
    if (!e->owner->on_change || !e->owner->on_change(e->old_key, key, command, reset,
                                                  &error, e->owner->user_data)) {
        gtk_label_set_text(e->error, error ? error->message : tr("Не удалось сохранить привязку."));
        return FALSE;
    }
    gtk_label_set_text(e->owner->error, "");
    gtk_widget_set_visible(GTK_WIDGET(e->owner->error), FALSE);
    gtk_label_set_text(e->owner->summary, tr("Сохранено. Обновление привязок…"));
    gtk_window_destroy(e->window);
    return TRUE;
}

static void save_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    KeyEditor *e = data;
    const char *command = gtk_editable_get_text(GTK_EDITABLE(e->command));
    if (!*command) {
        gtk_label_set_text(e->error, tr("Укажите команду с нужными аргументами."));
        return;
    }
    if (!find_command(e, command, NULL)) {
        gtk_label_set_text(e->error, tr("Неизвестная команда mpv. Выберите команду из списка."));
        return;
    }
    apply_edit(e, command, FALSE);
}

static void key_changed(GtkEditable *editable, gpointer data)
{
    KeyEditor *e = data;
    const char *key = gtk_editable_get_text(editable);
    gtk_label_set_text(e->conflict, "");
    if (g_strcmp0(key, e->old_key) == 0)
        return;
    for (guint i = 0; e->owner->rows && i < e->owner->rows->len; ++i) {
        const KeyBinding *binding = g_ptr_array_index(e->owner->rows, i);
        if (binding->active && g_str_equal(binding->key, key)) {
            g_autofree char *text = g_strdup_printf(
                tr("Уже назначено: %s. Сохранение заменит эту привязку."), binding->command);
            gtk_label_set_text(e->conflict, text);
            break;
        }
    }
}

static void disable_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    apply_edit(data, "ignore", FALSE);
}

static void reset_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    apply_edit(data, NULL, TRUE);
}

static void cancel_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    KeyEditor *e = data;
    gtk_window_destroy(e->window);
}

static void free_editor(gpointer data)
{
    KeyEditor *e = data;
    release_capture_shortcuts(e);
    g_clear_pointer(&e->commands, g_ptr_array_unref);
    g_free(e->old_key);
    g_free(e);
}

static void open_editor(KeysPage *p, const char *key, const char *command)
{
    if (p->editor) {
        gtk_window_present(p->editor);
        return;
    }
    KeyEditor *e = g_new0(KeyEditor, 1);
    e->owner = p;
    e->old_key = g_strdup(key);
    e->commands = p->commands ? g_ptr_array_ref(p->commands) : g_ptr_array_new();
    e->window = GTK_WINDOW(gtk_window_new());
    p->editor = e->window;
    g_object_add_weak_pointer(G_OBJECT(p->editor), (gpointer *)&p->editor);
    g_object_set_data_full(G_OBJECT(e->window), "mpv-key-editor", e, free_editor);
    GtkRoot *root = gtk_widget_get_root(p->page);
    if (GTK_IS_WINDOW(root)) {
        gtk_window_set_transient_for(e->window, GTK_WINDOW(root));
        gtk_window_set_destroy_with_parent(e->window, TRUE);
    }
    gtk_window_set_modal(e->window, TRUE);
    gtk_window_set_default_size(e->window, 640, 560);
    gtk_window_set_title(e->window, key ? tr("Изменить привязку") : tr("Добавить привязку"));
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(e->window, header);
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(e->window, layout);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(content, 20);
    gtk_widget_set_margin_end(content, 20);
    gtk_widget_set_margin_top(content, 20);
    gtk_widget_set_margin_bottom(content, 20);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_box_append(GTK_BOX(layout), scroll);
    gtk_box_append(GTK_BOX(content), text_label(tr("Клавиша или сочетание"), FALSE));
    GtkWidget *key_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    e->key = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(e->key, "Ctrl+RIGHT");
    gtk_widget_set_hexpand(GTK_WIDGET(e->key), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(e->key), key ? key : "");
    gtk_box_append(GTK_BOX(key_row), GTK_WIDGET(e->key));
    e->capture = GTK_BUTTON(gtk_button_new_with_label(tr("Записать сочетание")));
    g_signal_connect(e->capture, "clicked", G_CALLBACK(capture_clicked), e);
    gtk_box_append(GTK_BOX(key_row), GTK_WIDGET(e->capture));
    gtk_box_append(GTK_BOX(content), key_row);
    e->conflict = GTK_LABEL(text_label("", TRUE));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->conflict));
    g_signal_connect(e->key, "changed", G_CALLBACK(key_changed), e);
    gtk_box_append(GTK_BOX(content), text_label(tr("Готовое действие"), FALSE));
    GtkStringList *preset_names = gtk_string_list_new(NULL);
    gsize preset_count;
    const KeyCommandPreset *presets = key_command_presets(&preset_count);
    for (gsize i = 0; i < preset_count; ++i)
        gtk_string_list_append(preset_names, tr(presets[i].title));
    e->presets = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(preset_names), NULL));
    gtk_drop_down_set_enable_search(e->presets, TRUE);
    gtk_drop_down_set_selected(e->presets, GTK_INVALID_LIST_POSITION);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->presets));
    g_signal_connect(e->presets, "notify::selected", G_CALLBACK(preset_changed), e);
    gtk_box_append(GTK_BOX(content), text_label(tr("Команда mpv"), FALSE));
    GtkStringList *names = gtk_string_list_new(NULL);
    for (guint i = 0; i < e->commands->len; ++i) {
        const KeyCommandSpec *spec = g_ptr_array_index(e->commands, i);
        gtk_string_list_append(names, spec->name);
    }
    e->catalog = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(names), NULL));
    gtk_drop_down_set_enable_search(e->catalog, TRUE);
    gtk_drop_down_set_selected(e->catalog, GTK_INVALID_LIST_POSITION);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->catalog));
    g_signal_connect(e->catalog, "notify::selected", G_CALLBACK(catalog_changed), e);
    e->command = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(e->command, "seek 5");
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->command));
    e->description = GTK_LABEL(text_label("", TRUE));
    e->signature = GTK_LABEL(text_label("", FALSE));
    e->example = GTK_LABEL(text_label("", TRUE));
    gtk_label_set_selectable(e->signature, TRUE);
    gtk_label_set_selectable(e->example, TRUE);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->description));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->signature));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->example));
    e->error = GTK_LABEL(text_label("", FALSE));
    gtk_widget_add_css_class(GTK_WIDGET(e->error), "error");
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(e->error));
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(footer, 20);
    gtk_widget_set_margin_end(footer, 20);
    gtk_widget_set_margin_top(footer, 10);
    gtk_widget_set_margin_bottom(footer, 20);
    const char *reset_help = tr("Удаляет назначение, сделанное в этом окне, и восстанавливает исходный input.conf или клавиши проигрывателя.");
    if (key)
        gtk_box_append(GTK_BOX(footer), text_label(reset_help, TRUE));
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *reset = gtk_button_new_with_label(tr("Отменить изменение"));
    gtk_widget_set_sensitive(reset, key != NULL);
    gtk_widget_set_tooltip_text(reset, reset_help);
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_clicked), e);
    gtk_box_append(GTK_BOX(buttons), reset);
    GtkWidget *disable = gtk_button_new_with_label(tr("Отключить"));
    g_signal_connect(disable, "clicked", G_CALLBACK(disable_clicked), e);
    gtk_box_append(GTK_BOX(buttons), disable);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(buttons), spacer);
    GtkWidget *cancel = gtk_button_new_with_label(tr("Отмена"));
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), e);
    gtk_box_append(GTK_BOX(buttons), cancel);
    GtkWidget *save = gtk_button_new_with_label(tr("Сохранить"));
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), e);
    gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(footer), buttons);
    gtk_box_append(GTK_BOX(layout), footer);
    g_signal_connect(e->command, "changed", G_CALLBACK(command_changed), e);
    gtk_editable_set_text(GTK_EDITABLE(e->command), command ? command : "cycle pause");
    command_changed(GTK_EDITABLE(e->command), e);
    GtkEventController *capture = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(capture, GTK_PHASE_CAPTURE);
    g_signal_connect(capture, "key-pressed", G_CALLBACK(capture_key), e);
    gtk_widget_add_controller(GTK_WIDGET(e->window), capture);
    g_signal_connect(e->window, "notify::is-active", G_CALLBACK(capture_active_changed), e);
    g_signal_connect(e->window, "unrealize", G_CALLBACK(capture_unrealized), e);
    gtk_window_present(e->window);
    gtk_widget_grab_focus(GTK_WIDGET(e->key));
}

static void add_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    open_editor(data, NULL, NULL);
}

GtkWidget *keys_page_new(void)
{
    KeysPage *p = g_new0(KeysPage, 1);
    p->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(p->page, "preferences-page");
    gtk_widget_set_margin_start(p->page, 22);
    gtk_widget_set_margin_end(p->page, 22);
    gtk_widget_set_margin_top(p->page, 22);
    gtk_widget_set_margin_bottom(p->page, 22);
    g_object_set_data_full(G_OBJECT(p->page), "mpv-keys-page", p, free_page);
    GtkWidget *title = text_label(tr("Клавиши"), FALSE);
    gtk_widget_add_css_class(title, "title-2");
    gtk_box_append(GTK_BOX(p->page), title);
    gtk_box_append(GTK_BOX(p->page), text_label(
        tr("Все привязки mpv и интерфейса. Выберите клавишу, чтобы изменить команду и её аргументы."), TRUE));
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_set_hexpand(search, TRUE);
    g_object_set(search, "placeholder-text", tr("Найти клавишу, команду или описание"), NULL);
    g_signal_connect(search, "search-changed", G_CALLBACK(search_changed), p);
    gtk_box_append(GTK_BOX(toolbar), search);
    p->add = gtk_button_new_with_label(tr("Добавить"));
    gtk_widget_set_sensitive(p->add, FALSE);
    g_signal_connect(p->add, "clicked", G_CALLBACK(add_clicked), p);
    gtk_box_append(GTK_BOX(toolbar), p->add);
    gtk_box_append(GTK_BOX(p->page), toolbar);
    p->summary = GTK_LABEL(text_label(tr("Загрузка привязок mpv…"), TRUE));
    gtk_box_append(GTK_BOX(p->page), GTK_WIDGET(p->summary));
    p->error = GTK_LABEL(text_label("", FALSE));
    gtk_widget_add_css_class(GTK_WIDGET(p->error), "error");
    gtk_widget_set_visible(GTK_WIDGET(p->error), FALSE);
    gtk_box_append(GTK_BOX(p->page), GTK_WIDGET(p->error));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    p->list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(p->list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(p->list), "boxed-list");
    gtk_list_box_set_filter_func(p->list, filter_row, p, NULL);
    gtk_list_box_set_placeholder(p->list, text_label(tr("Ничего не найдено."), TRUE));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(p->list));
    /* Clip the stationary viewport, including its scrollbar, rather than
     * relying on the first and last rows that move out of view. */
    GtkWidget *panel = gtk_frame_new(NULL);
    gtk_widget_add_css_class(panel, "keybinds-panel");
    gtk_widget_set_vexpand(panel, TRUE);
    gtk_frame_set_child(GTK_FRAME(panel), scroll);
    gtk_box_append(GTK_BOX(p->page), panel);
    return p->page;
}

void keys_page_configure(GtkWidget *page, const char *input_path,
                          const KeyBindingDefault *defaults, gsize count,
                          KeysPageChangeFunc on_change, gpointer user_data)
{
    KeysPage *p = page_data(page);
    g_return_if_fail(p != NULL);
    free_defaults(p);
    p->defaults = g_new0(KeyBindingDefault, count);
    p->default_count = count;
    for (gsize i = 0; i < count; ++i) {
        p->defaults[i].key = g_strdup(defaults[i].key);
        p->defaults[i].command = g_strdup(defaults[i].command);
        p->defaults[i].comment = g_strdup(defaults[i].comment);
    }
    g_free(p->input_path);
    p->input_path = g_strdup(input_path);
    p->on_change = on_change;
    p->user_data = user_data;
    gtk_widget_set_sensitive(p->add, on_change != NULL);
    if (!p->rows)
        p->rows = keybindings_snapshot(NULL, p->defaults, p->default_count);
    rebuild_rows(p);
}

static gboolean same_rows(const GPtrArray *left, const GPtrArray *right)
{
    if (!left || !right || left->len != right->len)
        return FALSE;
    for (guint i = 0; i < left->len; ++i) {
        const KeyBinding *a = g_ptr_array_index(left, i);
        const KeyBinding *b = g_ptr_array_index(right, i);
        if (g_strcmp0(a->key, b->key) || g_strcmp0(a->command, b->command) ||
            g_strcmp0(a->comment, b->comment) || g_strcmp0(a->section, b->section) ||
            a->weak != b->weak || a->active != b->active ||
            a->app_default != b->app_default || a->priority != b->priority)
            return FALSE;
    }
    return TRUE;
}

void keys_page_update(GtkWidget *page, const mpv_node *bindings, const mpv_node *commands)
{
    KeysPage *p = page_data(page);
    g_return_if_fail(p != NULL);
    gboolean changed = FALSE;
    if (commands) {
        GPtrArray *snapshot = key_command_list_from_node(commands);
        g_clear_pointer(&p->commands, g_ptr_array_unref);
        p->commands = snapshot;
        changed = TRUE;
    }
    if (bindings) {
        GPtrArray *snapshot = keybindings_snapshot(bindings, p->defaults, p->default_count);
        if (same_rows(p->rows, snapshot)) {
            g_ptr_array_unref(snapshot);
        } else {
            g_clear_pointer(&p->rows, g_ptr_array_unref);
            p->rows = snapshot;
            changed = TRUE;
        }
        if (!p->have_bindings)
            changed = TRUE;
        p->have_bindings = TRUE;
    }
    if (changed)
        rebuild_rows(p);
    else if (bindings)
        update_summary(p);
}

void keys_page_show_error(GtkWidget *page, const char *message)
{
    KeysPage *p = page_data(page);
    g_return_if_fail(p != NULL);
    gtk_label_set_text(p->error, message ? message : "");
    gtk_widget_set_visible(GTK_WIDGET(p->error), message && *message);
    if (p->editor) {
        KeyEditor *e = g_object_get_data(G_OBJECT(p->editor), "mpv-key-editor");
        if (e)
            gtk_label_set_text(e->error, message ? message : "");
    }
}
