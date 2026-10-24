#include "preferences.h"
#include "i18n.h"

#include <math.h>
#include <string.h>

typedef struct {
    GtkWindow *window;
    GtkStack *stack;
    GtkListBox *sidebar;
    GtkWidget *sections[6];
    gboolean section_built[6];
    GtkWidget *keys_page;
    GtkDropDown *hwdec, *language, *gpu_api;
    GtkDropDown *audio_output, *audio_device;
    GPtrArray *audio_outputs, *audio_devices;
    GPtrArray *audio_output_values, *audio_device_values;
    char *audio_model_output, *audio_model_device;
    guint64 audio_catalog_generation, audio_model_generation;
    GtkLabel *hwdec_description, *hwdec_status;
    PreferenceValues values;
    GtkSwitch *keepaspect, *deinterlace, *deband, *loop;
    GtkSwitch *allow_replace, *add_to_queue, *timeline_previews;
    GtkDropDown *autocreate_playlist, *directory_mode;
    GPtrArray *autocreate_values, *directory_values;
    GtkLabel *autocreate_description, *directory_description;
    GtkEntry *directory_filter_types;
    GtkEntry *extension_entries[5];
    GtkExpander *extensions;
    GtkWidget *extension_content;
    gboolean extensions_built;
    GtkSpinButton *audio_delay, *sub_delay, *sub_scale, *cursor_hide_delay;
    PreferencesChangeFunc on_change;
    PreferencesOpenConfigFunc on_open_config;
    gpointer user_data;
    PreferencesKeysLoadFunc on_keys_load;
    gpointer keys_load_data;
    gboolean keys_load_requested;
    PreferencesAudioLoadFunc on_audio_load;
    gpointer audio_load_data;
    gboolean audio_loading;
    PreferencesPlaylistLoadFunc on_playlist_load;
    gpointer playlist_load_data;
    guint playlist_groups;
    char *keys_input_path, *keys_error;
    KeyBindingDefault *keys_defaults;
    gsize keys_default_count;
    KeysPageChangeFunc on_keys_change;
    gpointer keys_change_data;
    gboolean updating;
} Preferences;

typedef struct {
    Preferences *preferences;
    double value;
    gboolean dirty, committing;
} NumberEdit;

typedef struct {
    char *name, *description;
} AudioChoice;

typedef struct {
    Preferences *preferences;
    const char *name;
    GtkLabel *error;
    char *value;
    gboolean known, dirty, committing, filter_types;
} StringEdit;

static const char *const hwdec_values[] = {
    "auto-safe", "no", "auto", "nvdec", "nvdec-copy", "auto-copy", "vaapi", "vdpau"
};

static const char *const gpu_api_values[] = {"auto", "opengl", "opengl-es", "vulkan"};
static const char *const autocreate_options[] = {"no", "filter", "same"};
static const char *const directory_options[] = {"auto", "lazy", "recursive", "ignore"};
static const char *const extension_options[] = {
    "video-exts", "audio-exts", "image-exts", "archive-exts", "playlist-exts"
};

static const char *const section_names[] = {
    "video", "audio", "subtitles", "playback", "keys", "interface"
};

static void ensure_section(Preferences *p, const char *name);
static void update_audio_loader(Preferences *p);
static void update_playlist_loader(Preferences *p);
static void refresh_controls(Preferences *p, const char *section);

static const char *audio_value(const char *value)
{
    return value && *value ? value : "auto";
}

static void audio_choice_free(gpointer data)
{
    AudioChoice *choice = data;
    g_free(choice->name);
    g_free(choice->description);
    g_free(choice);
}

static const char *audio_map_text(const mpv_node *node, const char *key)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
        return NULL;
    const mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; ++i) {
        if (list->keys && list->keys[i] && g_str_equal(list->keys[i], key) &&
            list->values[i].format == MPV_FORMAT_STRING)
            return list->values[i].u.string;
    }
    return NULL;
}

static const char *audio_output_label(const char *name)
{
    static const struct { const char *name, *label; } labels[] = {
        {"pipewire", "PipeWire"}, {"pulse", "PulseAudio"}, {"alsa", "ALSA"},
        {"jack", "JACK"}, {"openal", "OpenAL"}, {"sdl", "SDL"}
    };
    if (g_str_equal(name, "null"))
        return tr("Без звука");
    for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
        if (g_str_equal(name, labels[i].name))
            return labels[i].label;
    }
    return name;
}

/* These catalogues are independent of widgets and own their snapshots. The
 * stock AO choices are strings; device names/descriptions are maps. */
static GPtrArray *audio_read_choices(const mpv_node *node, gboolean outputs)
{
    GPtrArray *choices = g_ptr_array_new_with_free_func(audio_choice_free);
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return choices;
    for (int i = 0; i < node->u.list->num; ++i) {
        const mpv_node *entry = &node->u.list->values[i];
        const char *name = outputs && entry->format == MPV_FORMAT_STRING
            ? entry->u.string : audio_map_text(entry, "name");
        if (!name || !*name || g_str_equal(name, "auto") ||
            (outputs && (g_str_equal(name, "pcm") || g_str_equal(name, "lavc"))) ||
            g_hash_table_contains(seen, name))
            continue;
        const char *description = audio_map_text(entry, "description");
        if (!description || !*description)
            description = outputs ? audio_output_label(name) : name;
        AudioChoice *choice = g_new0(AudioChoice, 1);
        choice->name = g_strdup(name);
        choice->description = g_utf8_make_valid(description, -1);
        g_ptr_array_add(choices, choice);
        g_hash_table_add(seen, choice->name);
    }
    return choices;
}

static gboolean audio_device_matches(const char *device, const char *output)
{
    output = audio_value(output);
    if (g_str_equal(output, "auto"))
        return TRUE;
    gsize length = strlen(output);
    return g_str_equal(device, output) ||
        (g_str_has_prefix(device, output) && device[length] == '/');
}

static GtkStringList *audio_make_model(GPtrArray *choices, const char *current,
                                      const char *output_filter,
                                      GPtrArray **names, guint *selected)
{
    GtkStringList *model = gtk_string_list_new(NULL);
    *names = g_ptr_array_new_with_free_func(g_free);
    gtk_string_list_append(model, tr("Авто"));
    g_ptr_array_add(*names, g_strdup("auto"));
    current = audio_value(current);
    *selected = 0;
    gboolean found = g_str_equal(current, "auto");
    for (guint i = 0; choices && i < choices->len; ++i) {
        AudioChoice *choice = g_ptr_array_index(choices, i);
        if (output_filter && !audio_device_matches(choice->name, output_filter))
            continue;
        if (g_str_equal(current, choice->name)) {
            *selected = (*names)->len;
            found = TRUE;
        }
        g_autofree char *label = NULL;
        if (output_filter && g_str_equal(audio_value(output_filter), "auto")) {
            const char *slash = strchr(choice->name, '/');
            g_autofree char *backend = slash ? g_strndup(choice->name, slash - choice->name)
                : g_strdup(choice->name);
            label = g_strdup_printf("%s · %s", choice->description, audio_output_label(backend));
        }
        gtk_string_list_append(model, label ? label : choice->description);
        g_ptr_array_add(*names, g_strdup(choice->name));
    }
    /* A hot-unplug or a custom mpv.conf value must not turn a catalogue
     * refresh into an implicit save/reset. Keep its exact value selected. */
    if (!found) {
        *selected = (*names)->len;
        g_autofree char *label = g_utf8_make_valid(current, -1);
        gtk_string_list_append(model, label);
        g_ptr_array_add(*names, g_strdup(current));
    }
    return model;
}

static void refresh_audio_controls(Preferences *p)
{
    if (!p->audio_output || !p->audio_device)
        return;
    const char *output = audio_value(p->values.audio_output);
    const char *device = audio_value(p->values.audio_device);
    if (p->audio_model_generation == p->audio_catalog_generation &&
        g_strcmp0(p->audio_model_output, output) == 0 &&
        g_strcmp0(p->audio_model_device, device) == 0) {
        /* A failed save has the same snapshot but a changed widget selection.
         * Restore it without rebuilding models or emitting another write. */
        gboolean updating = p->updating;
        p->updating = TRUE;
        for (guint i = 0; i < p->audio_output_values->len; ++i) {
            if (g_str_equal(g_ptr_array_index(p->audio_output_values, i), output))
                gtk_drop_down_set_selected(p->audio_output, i);
        }
        for (guint i = 0; i < p->audio_device_values->len; ++i) {
            if (g_str_equal(g_ptr_array_index(p->audio_device_values, i), device))
                gtk_drop_down_set_selected(p->audio_device, i);
        }
        p->updating = updating;
        return;
    }
    GPtrArray *output_values, *device_values;
    guint output_selected, device_selected;
    g_autoptr(GtkStringList) outputs = audio_make_model(p->audio_outputs, output,
        NULL, &output_values, &output_selected);
    g_autoptr(GtkStringList) devices = audio_make_model(p->audio_devices, device,
        output, &device_values, &device_selected);
    g_clear_pointer(&p->audio_output_values, g_ptr_array_unref);
    g_clear_pointer(&p->audio_device_values, g_ptr_array_unref);
    p->audio_output_values = output_values;
    p->audio_device_values = device_values;
    gboolean updating = p->updating;
    p->updating = TRUE;
    gtk_drop_down_set_model(p->audio_output, G_LIST_MODEL(outputs));
    gtk_drop_down_set_selected(p->audio_output, output_selected);
    gtk_drop_down_set_model(p->audio_device, G_LIST_MODEL(devices));
    gtk_drop_down_set_selected(p->audio_device, device_selected);
    p->updating = updating;
    g_free(p->audio_model_output);
    g_free(p->audio_model_device);
    p->audio_model_output = g_strdup(output);
    p->audio_model_device = g_strdup(device);
    p->audio_model_generation = p->audio_catalog_generation;
}

static void stop_audio_loader(Preferences *p)
{
    if (!p->audio_loading)
        return;
    p->audio_loading = FALSE;
    if (p->on_audio_load)
        p->on_audio_load(p->window, FALSE, p->audio_load_data);
}

static void update_audio_loader(Preferences *p)
{
    gboolean active = p->on_audio_load && gtk_widget_get_visible(GTK_WIDGET(p->window)) &&
        g_strcmp0(gtk_stack_get_visible_child_name(p->stack), "audio") == 0;
    if (p->audio_loading == active)
        return;
    p->audio_loading = active;
    p->on_audio_load(p->window, active, p->audio_load_data);
}

static void stop_playlist_loader(Preferences *p)
{
    if (!p->playlist_groups)
        return;
    p->playlist_groups = 0;
    if (p->on_playlist_load)
        p->on_playlist_load(p->window, 0, p->playlist_load_data);
}

static void update_playlist_loader(Preferences *p)
{
    guint groups = 0;
    if (p->on_playlist_load && gtk_widget_get_visible(GTK_WIDGET(p->window)) &&
        g_strcmp0(gtk_stack_get_visible_child_name(p->stack), "playback") == 0) {
        groups = PREFERENCES_PLAYLIST_BASIC;
        if (p->extensions && gtk_expander_get_expanded(p->extensions))
            groups |= PREFERENCES_PLAYLIST_EXTENSIONS;
    }
    if (p->playlist_groups == groups)
        return;
    p->playlist_groups = groups;
    p->on_playlist_load(p->window, groups, p->playlist_load_data);
}

static void audio_visibility_changed(GObject *object, GParamSpec *property, gpointer data)
{
    (void)object;
    (void)property;
    update_audio_loader(data);
    update_playlist_loader(data);
}

static void audio_window_unrealized(GtkWidget *widget, gpointer data)
{
    (void)widget;
    stop_audio_loader(data);
    stop_playlist_loader(data);
}

static const char *hwdec_description(guint selected)
{
    switch (selected) {
    case 0: return tr("Выбирает безопасный аппаратный декодер и при необходимости переходит на программный.");
    case 1: return tr("Декодирует видео на процессоре без аппаратного ускорения.");
    case 2: return tr("Пробует доступные аппаратные декодеры для текущего видео.");
    case 3: return tr("Декодер NVIDIA с прямой передачей кадров на видеокарту.");
    case 4: return tr("Декодер NVIDIA с копированием кадров для совместимости OpenGL и OpenGL ES.");
    case 5: return tr("Автоматически выбирает аппаратный декодер с копированием кадров для совместимости.");
    case 6: return tr("Аппаратный декодер Intel и AMD через VA-API.");
    case 7: return tr("Декодер VDPAU для совместимых видеокарт NVIDIA.");
    default: return tr("Текущий режим задан в mpv.conf.");
    }
}

char *preferences_decoder_status(const PreferenceValues *values)
{
    g_return_val_if_fail(values != NULL, NULL);
    if (!values->media_loaded)
        return g_strdup(tr("Откройте видео, чтобы увидеть активный декодер."));
    if (!values->has_video)
        return g_strdup(tr("В текущем файле нет видеодорожки."));
    if (values->hwdec_pending && !values->hwdec_restart_required)
        return g_strdup(tr("Применяется выбранный режим декодирования…"));
    if (!values->hwdec_known)
        return g_strdup(tr("Активный декодер ещё не определён."));
    const char *active = values->hwdec_current;
    if (active && *active && !g_str_equal(active, "no"))
        return g_strdup(active);
    if (values->hwdec_restart_required)
        return g_strdup(tr("Программное декодирование"));
    if (g_strcmp0(values->hwdec, "no") == 0)
        return g_strdup(tr("Аппаратное ускорение отключено; используется программный декодер."));
    return g_strdup(tr("Аппаратный режим недоступен для этого видео; используется программный декодер."));
}

static void update_hwdec_status(Preferences *p)
{
    guint selected = gtk_drop_down_get_selected(p->hwdec);
    const char *description = hwdec_description(selected);
    g_autofree char *deferred = p->values.hwdec_restart_required
        ? g_strdup_printf("%s\n%s", description,
            tr("Выбранный режим будет применён после перезапуска плеера.")) : NULL;
    gtk_label_set_text(p->hwdec_description, deferred ? deferred : description);
    PreferenceValues values = {
        .hwdec = selected < G_N_ELEMENTS(hwdec_values) ? hwdec_values[selected] : NULL,
        .hwdec_current = p->values.hwdec_current,
        .media_loaded = p->values.media_loaded,
        .has_video = p->values.has_video,
        .hwdec_known = p->values.hwdec_known,
        .hwdec_pending = p->values.hwdec_pending,
        .hwdec_restart_required = p->values.hwdec_restart_required
    };
    g_autofree char *message = preferences_decoder_status(&values);
    gtk_label_set_text(p->hwdec_status, message);
}

static void clear_keys_defaults(Preferences *p)
{
    for (gsize i = 0; i < p->keys_default_count; ++i) {
        g_free((gpointer)p->keys_defaults[i].key);
        g_free((gpointer)p->keys_defaults[i].command);
        g_free((gpointer)p->keys_defaults[i].comment);
    }
    g_clear_pointer(&p->keys_defaults, g_free);
    p->keys_default_count = 0;
}

static void preferences_free(gpointer data)
{
    Preferences *p = data;
    g_free((gpointer)p->values.hwdec);
    g_free((gpointer)p->values.hwdec_current);
    g_free((gpointer)p->values.language);
    g_free((gpointer)p->values.gpu_api);
    g_free((gpointer)p->values.audio_output);
    g_free((gpointer)p->values.audio_device);
    g_free((gpointer)p->values.autocreate_playlist);
    g_free((gpointer)p->values.directory_mode);
    g_free((gpointer)p->values.directory_filter_types);
    g_free((gpointer)p->values.video_exts);
    g_free((gpointer)p->values.audio_exts);
    g_free((gpointer)p->values.image_exts);
    g_free((gpointer)p->values.archive_exts);
    g_free((gpointer)p->values.playlist_exts);
    g_clear_pointer(&p->autocreate_values, g_ptr_array_unref);
    g_clear_pointer(&p->directory_values, g_ptr_array_unref);
    g_clear_pointer(&p->audio_outputs, g_ptr_array_unref);
    g_clear_pointer(&p->audio_devices, g_ptr_array_unref);
    g_clear_pointer(&p->audio_output_values, g_ptr_array_unref);
    g_clear_pointer(&p->audio_device_values, g_ptr_array_unref);
    g_free(p->audio_model_output);
    g_free(p->audio_model_device);
    clear_keys_defaults(p);
    g_free(p->keys_input_path);
    g_free(p->keys_error);
    g_free(p);
}

static void section_selected(GtkListBox *sidebar, GtkListBoxRow *selected,
                             gpointer data)
{
    (void)sidebar;
    if (selected) {
        Preferences *p = data;
        const char *name = g_object_get_data(G_OBJECT(selected), "mpv-section-name");
        ensure_section(p, name);
        gtk_stack_set_visible_child_name(p->stack, name);
    }
}

static void section_visible_changed(GObject *object, GParamSpec *property,
                                    gpointer data)
{
    (void)property;
    Preferences *p = data;
    GtkListBox *sidebar = p->sidebar;
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(object));
    ensure_section(p, name);
    for (int i = 0;; ++i) {
        GtkListBoxRow *item = gtk_list_box_get_row_at_index(sidebar, i);
        if (!item)
            break;
        if (g_strcmp0(name, g_object_get_data(G_OBJECT(item), "mpv-section-name")) == 0) {
            gtk_list_box_select_row(sidebar, item);
            break;
        }
    }
    update_audio_loader(p);
    update_playlist_loader(p);
}

static void add_section(Preferences *p, GtkWidget *child, const char *name,
                        const char *title, const char *icon_name)
{
    GtkStackPage *stack_page = gtk_stack_add_titled(p->stack, child, name, title);
    gtk_stack_page_set_icon_name(stack_page, icon_name);

    /* GtkStackSidebar only renders titles. Native list rows provide both the
     * section icon and title without depending on GTK's private children. */
    GtkWidget *item = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(item), "mpv-section-name", g_strdup(name), g_free);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    gtk_box_append(GTK_BOX(content), icon);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_tooltip_text(item, title);
    gtk_box_append(GTK_BOX(content), label);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(item), content);
    gtk_list_box_append(p->sidebar, item);
    if (g_strcmp0(gtk_stack_get_visible_child_name(p->stack), name) == 0)
        gtk_list_box_select_row(p->sidebar, GTK_LIST_BOX_ROW(item));
}

static GtkWidget *page(GtkWidget *section, const char *title)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(scroll, "preferences-page");
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(content, 22);
    gtk_widget_set_margin_end(content, 22);
    gtk_widget_set_margin_top(content, 22);
    gtk_widget_set_margin_bottom(content, 22);
    gtk_widget_set_valign(content, GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "title-2");
    gtk_box_append(GTK_BOX(content), label);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "preferences-card");
    gtk_box_append(GTK_BOX(content), card);
    gtk_box_append(GTK_BOX(section), scroll);
    return card;
}

static GtkWidget *row(GtkWidget *card, const char *title,
                      const char *description, GtkWidget *control)
{
    if (gtk_widget_get_first_child(card))
        gtk_box_append(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(box, "preferences-row");
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(labels, TRUE);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_box_append(GTK_BOX(labels), label);
    if (description) {
        GtkWidget *detail = gtk_label_new(description);
        gtk_label_set_xalign(GTK_LABEL(detail), 0);
        gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(detail), 40);
        gtk_widget_add_css_class(detail, "dim-label");
        gtk_box_append(GTK_BOX(labels), detail);
    }
    gtk_box_append(GTK_BOX(box), labels);
    if (control) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(control, GTK_ALIGN_END);
        gtk_accessible_update_property(GTK_ACCESSIBLE(control),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
        gtk_box_append(GTK_BOX(box), control);
    }
    gtk_box_append(GTK_BOX(card), box);
    return labels;
}

static void changed_switch(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Preferences *p = data;
    if (p->updating || !p->on_change)
        return;
    const char *name = g_object_get_data(object, "mpv-preference-name");
    int enabled = gtk_switch_get_active(GTK_SWITCH(object));
    if (g_str_equal(name, "loop-file")) {
        const char *value = enabled ? "inf" : "no";
        p->on_change(name, MPV_FORMAT_STRING, &value, p->user_data);
    } else {
        p->on_change(name, MPV_FORMAT_FLAG, &enabled, p->user_data);
    }
}

static GtkSwitch *add_switch(Preferences *p, GtkWidget *card, const char *name,
                              const char *title, const char *description)
{
    GtkSwitch *control = GTK_SWITCH(gtk_switch_new());
    g_object_set_data(G_OBJECT(control), "mpv-preference-name", (gpointer)name);
    g_signal_connect(control, "notify::active", G_CALLBACK(changed_switch), p);
    row(card, title, description, GTK_WIDGET(control));
    return control;
}

static void changed_number(GtkSpinButton *control, gpointer data)
{
    Preferences *p = data;
    NumberEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-number-edit");
    double value = gtk_spin_button_get_value(control);
    gboolean changed = fabs(value - edit->value) > 1e-9;
    edit->value = value;
    edit->dirty = FALSE;
    if (p->updating || !p->on_change || !changed)
        return;
    const char *name = g_object_get_data(G_OBJECT(control), "mpv-preference-name");
    p->on_change(name, MPV_FORMAT_DOUBLE, &value, p->user_data);
}

static void number_text_changed(GtkEditable *control, gpointer data)
{
    Preferences *p = data;
    NumberEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-number-edit");
    if (!p->updating && !edit->committing)
        edit->dirty = TRUE;
}

static void commit_number(GtkSpinButton *control)
{
    if (!control)
        return;
    NumberEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-number-edit");
    if (edit->preferences->updating || edit->committing)
        return;
    /* GtkEditable text and the adjustment are separate until an update.
     * value-changed remains the only path that applies/saves a new number. */
    edit->committing = TRUE;
    gtk_spin_button_update(control);
    edit->dirty = FALSE;
    edit->committing = FALSE;
}

static gboolean number_key_pressed(GtkEventControllerKey *controller,
                                  guint keyval, guint keycode,
                                  GdkModifierType state, gpointer data)
{
    (void)controller;
    (void)keycode;
    (void)state;
    if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter &&
        keyval != GDK_KEY_ISO_Enter)
        return FALSE;
    /* Capture Enter before the internal GtkText handles it. SpinButton's
     * activate signal may require a second Enter after text is committed. */
    commit_number(GTK_SPIN_BUTTON(data));
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(data));
    if (root)
        gtk_root_set_focus(root, NULL);
    return TRUE;
}

static void number_focus_left(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller;
    commit_number(GTK_SPIN_BUTTON(data));
}

static gboolean parse_seconds(const char *text, double *value)
{
    while (g_ascii_isspace(*text))
        text++;
    char *end;
    double parsed = g_ascii_strtod(text, &end);
    if (end == text || !isfinite(parsed))
        return FALSE;
    while (g_ascii_isspace(*end))
        end++;
    if (*end == 's') {
        end++;
        while (g_ascii_isspace(*end))
            end++;
    }
    if (*end)
        return FALSE;
    *value = parsed;
    return TRUE;
}

static int seconds_input(GtkSpinButton *control, double *value, gpointer data)
{
    (void)data;
    return parse_seconds(gtk_editable_get_text(GTK_EDITABLE(control)), value)
        ? TRUE : GTK_INPUT_ERROR;
}

static gboolean seconds_output(GtkSpinButton *control, gpointer data)
{
    (void)data;
    NumberEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-number-edit");
    /* Native +/- emits output before value-changed. Formatting must not be
     * mistaken for user typing, or an unrelated observation can freeze it. */
    gboolean committing = edit->committing;
    edit->committing = TRUE;
    char number[G_ASCII_DTOSTR_BUF_SIZE];
    char format[16];
    g_snprintf(format, sizeof(format), "%%.%uf", gtk_spin_button_get_digits(control));
    g_ascii_formatd(number, sizeof(number), format, gtk_spin_button_get_value(control));
    g_autofree char *text = g_strconcat(number, " s", NULL);
    gtk_editable_set_text(GTK_EDITABLE(control), text);
    edit->committing = committing;
    return TRUE;
}

static void use_seconds_unit(GtkSpinButton *control)
{
    /* The suffix belongs to the editable value, which accepts plain numbers
     * and pasted values with "s" through the same parser. */
    gtk_spin_button_set_numeric(control, FALSE);
    g_signal_connect(control, "input", G_CALLBACK(seconds_input), NULL);
    g_signal_connect(control, "output", G_CALLBACK(seconds_output), NULL);
    seconds_output(control, NULL);
}

static GtkSpinButton *add_number(Preferences *p, GtkWidget *card, const char *name,
                                  const char *title, const char *description,
                                  double minimum, double maximum, double step)
{
    GtkSpinButton *control = GTK_SPIN_BUTTON(
        gtk_spin_button_new_with_range(minimum, maximum, step));
    gtk_spin_button_set_digits(control, 2);
    gtk_spin_button_set_numeric(control, TRUE);
    gtk_spin_button_set_update_policy(control, GTK_UPDATE_IF_VALID);
    gtk_widget_set_size_request(GTK_WIDGET(control), 144, -1);
    NumberEdit *edit = g_new0(NumberEdit, 1);
    edit->preferences = p;
    edit->value = gtk_spin_button_get_value(control);
    g_object_set_data_full(G_OBJECT(control), "mpv-number-edit", edit, g_free);
    g_object_set_data(G_OBJECT(control), "mpv-preference-name", (gpointer)name);
    g_signal_connect(control, "changed", G_CALLBACK(number_text_changed), p);
    g_signal_connect(control, "value-changed", G_CALLBACK(changed_number), p);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(number_key_pressed), control);
    gtk_widget_add_controller(GTK_WIDGET(control), keys);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(number_focus_left), control);
    gtk_widget_add_controller(GTK_WIDGET(control), focus);
    row(card, title, description, GTK_WIDGET(control));
    return control;
}

static gboolean normalize_filter_types(const char *text, char **normalized)
{
    if (strpbrk(text, "\r\n"))
        return FALSE;
    g_autofree char *trimmed = g_strdup(text);
    g_strstrip(trimmed);
    if (!*trimmed) {
        *normalized = g_strdup("");
        return TRUE;
    }
    static const char *const types[] = {"video", "audio", "image", "archive", "playlist"};
    g_auto(GStrv) tokens = g_strsplit(trimmed, ",", -1);
    GString *result = g_string_new(NULL);
    for (guint i = 0; tokens[i]; ++i) {
        const char *token = g_strstrip(tokens[i]);
        gboolean valid = FALSE;
        for (guint j = 0; j < G_N_ELEMENTS(types); ++j)
            valid |= g_str_equal(token, types[j]);
        if (!valid) {
            g_string_free(result, TRUE);
            return FALSE;
        }
        if (i)
            g_string_append_c(result, ',');
        g_string_append(result, token);
    }
    *normalized = g_string_free(result, FALSE);
    return TRUE;
}

static void string_edit_free(gpointer data)
{
    StringEdit *edit = data;
    g_free(edit->value);
    g_free(edit);
}

static void string_changed(GtkEditable *control, gpointer data)
{
    (void)data;
    StringEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-string-edit");
    if (!edit->preferences->updating && !edit->committing) {
        edit->dirty = TRUE;
        gtk_widget_set_visible(GTK_WIDGET(edit->error), FALSE);
    }
}

static gboolean commit_string(GtkEntry *control)
{
    if (!control)
        return TRUE;
    StringEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-string-edit");
    Preferences *p = edit->preferences;
    if (p->updating || edit->committing || !edit->dirty)
        return TRUE;
    if (!edit->known || !p->on_change)
        return FALSE;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(control));
    g_autofree char *value = NULL;
    const char *error = NULL;
    if (strpbrk(text, "\r\n"))
        error = tr("Укажите список в одной строке.");
    else if (edit->filter_types && !normalize_filter_types(text, &value))
        error = tr("Допустимые типы: video, audio, image, archive, playlist. Для всех файлов очистите поле.");
    if (error) {
        gtk_label_set_text(edit->error, error);
        gtk_widget_set_visible(GTK_WIDGET(edit->error), TRUE);
        return FALSE;
    }
    if (!value)
        value = g_strdup(text);
    gtk_widget_set_visible(GTK_WIDGET(edit->error), FALSE);
    edit->dirty = FALSE;
    edit->committing = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(control), value);
    if (g_strcmp0(value, edit->value) == 0) {
        edit->committing = FALSE;
        return TRUE;
    }
    g_free(edit->value);
    edit->value = g_strdup(value);
    const char *saved = value;
    p->on_change(edit->name, MPV_FORMAT_STRING, &saved, p->user_data);
    edit->committing = FALSE;
    return TRUE;
}

static void string_activated(GtkEntry *control, gpointer data)
{
    (void)data;
    if (commit_string(control)) {
        GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(control));
        if (root)
            gtk_root_set_focus(root, NULL);
    }
}

static void string_focus_left(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller;
    commit_string(GTK_ENTRY(data));
}

static GtkEntry *add_string(Preferences *p, GtkWidget *card, const char *name,
                            const char *title, const char *description,
                            gboolean filter_types)
{
    GtkEntry *control = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(control), 22);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(control), 26);
    gtk_widget_set_sensitive(GTK_WIDGET(control), FALSE);
    GtkWidget *labels = row(card, title, description, GTK_WIDGET(control));
    GtkLabel *error = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(error, 0);
    gtk_label_set_wrap(error, TRUE);
    gtk_label_set_max_width_chars(error, 40);
    gtk_widget_add_css_class(GTK_WIDGET(error), "error");
    gtk_widget_set_visible(GTK_WIDGET(error), FALSE);
    gtk_box_append(GTK_BOX(labels), GTK_WIDGET(error));
    StringEdit *edit = g_new0(StringEdit, 1);
    edit->preferences = p;
    edit->name = name;
    edit->filter_types = filter_types;
    edit->error = error;
    g_object_set_data_full(G_OBJECT(control), "mpv-string-edit", edit, string_edit_free);
    g_signal_connect(control, "changed", G_CALLBACK(string_changed), NULL);
    g_signal_connect(control, "activate", G_CALLBACK(string_activated), NULL);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(string_focus_left), control);
    gtk_widget_add_controller(GTK_WIDGET(control), focus);
    return control;
}

static void update_string(GtkEntry *control, const char *value)
{
    if (!control)
        return;
    StringEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-string-edit");
    if (edit->dirty)
        return;
    gboolean committing = edit->committing;
    edit->committing = TRUE;
    edit->known = value != NULL;
    g_free(edit->value);
    edit->value = g_strdup(value);
    gtk_widget_set_sensitive(GTK_WIDGET(control), edit->known && edit->preferences->on_change);
    gtk_entry_set_placeholder_text(control, !value ? tr("Загрузка…")
        : edit->filter_types ? tr("Пусто — все файлы") : tr("Список расширений через запятую"));
    if (g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(control)), value ? value : "") != 0)
        gtk_editable_set_text(GTK_EDITABLE(control), value ? value : "");
    gtk_widget_set_visible(GTK_WIDGET(edit->error), FALSE);
    edit->committing = committing;
}

static void preferences_pressed(GtkGestureClick *gesture, int presses,
                                 double x, double y, gpointer data)
{
    (void)presses;
    (void)data;
    GtkWidget *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window));
    GtkWidget *control = focus;
    while (control && !g_object_get_data(G_OBJECT(control), "mpv-number-edit") &&
           !g_object_get_data(G_OBJECT(control), "mpv-string-edit"))
        control = gtk_widget_get_parent(control);
    if (!control)
        return;
    GtkWidget *target = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
    /* Keep the native +/- gestures, text selection and repeat behaviour. */
    if (target && (target == control || gtk_widget_is_ancestor(target, control)))
        return;
    if (g_object_get_data(G_OBJECT(control), "mpv-string-edit")) {
        if (!commit_string(GTK_ENTRY(control)))
            return;
    } else {
        commit_number(GTK_SPIN_BUTTON(control));
    }
    /* Labels and empty space do not take focus by themselves. Clearing it
     * here also permits later engine observations to refresh this field. */
    gtk_root_set_focus(GTK_ROOT(window), NULL);
}

void preferences_window_commit(GtkWindow *window)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    GtkSpinButton *numbers[] = {
        p->audio_delay, p->sub_delay, p->sub_scale, p->cursor_hide_delay
    };
    for (guint i = 0; i < G_N_ELEMENTS(numbers); ++i)
        commit_number(numbers[i]);
    commit_string(p->directory_filter_types);
    for (guint i = 0; i < G_N_ELEMENTS(p->extension_entries); ++i)
        commit_string(p->extension_entries[i]);
}

static gboolean preferences_close_requested(GtkWindow *window, gpointer data)
{
    (void)data;
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    preferences_window_commit(window);
    stop_audio_loader(p);
    stop_playlist_loader(p);
    return FALSE;
}

static void changed_hwdec(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Preferences *p = data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (p->updating || !p->on_change || selected >= G_N_ELEMENTS(hwdec_values))
        return;
    p->values.hwdec_pending = TRUE;
    update_hwdec_status(p);
    const char *value = hwdec_values[selected];
    p->on_change("hwdec", MPV_FORMAT_STRING, &value, p->user_data);
}

static void changed_language(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Preferences *p = data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (p->updating || !p->on_change || selected > 1)
        return;
    const char *code = selected == 0 ? "en" : "ru";
    p->on_change("ui-language", MPV_FORMAT_STRING, &code, p->user_data);
}

static void changed_gpu_api(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Preferences *p = data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (p->updating || !p->on_change ||
        selected >= G_N_ELEMENTS(gpu_api_values))
        return;
    if (selected == 3) {
        guint previous = 0;
        for (guint i = 0; i < 3; ++i) {
            if (g_strcmp0(p->values.gpu_api, gpu_api_values[i]) == 0)
                previous = i;
        }
        p->updating = TRUE;
        gtk_drop_down_set_selected(p->gpu_api, previous);
        p->updating = FALSE;
        return;
    }
    const char *api = selected == 2 ? "opengl" : gpu_api_values[selected];
    const char *es = selected == 2 ? "yes" : selected == 1 ? "no" : "auto";
    p->on_change("gpu-api", MPV_FORMAT_STRING, &api, p->user_data);
    p->on_change("opengl-es", MPV_FORMAT_STRING, &es, p->user_data);
}

static void gpu_api_item_setup(GtkSignalListItemFactory *factory,
                                GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_list_item_set_child(item, label);
}

static void gpu_api_item_bind(GtkSignalListItemFactory *factory,
                               GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;
    GtkWidget *label = gtk_list_item_get_child(item);
    GtkStringObject *value = gtk_list_item_get_item(item);
    gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(value));
    gboolean allowed = gtk_list_item_get_position(item) != 3;
    gtk_widget_set_sensitive(label, allowed);
    gtk_list_item_set_selectable(item, allowed);
    gtk_list_item_set_activatable(item, allowed);
}

static void audio_item_setup(GtkSignalListItemFactory *factory,
                              GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 26);
    gtk_list_item_set_child(item, label);
}

static void audio_item_bind(GtkSignalListItemFactory *factory,
                             GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;
    GtkWidget *label = gtk_list_item_get_child(item);
    const char *text = gtk_string_object_get_string(gtk_list_item_get_item(item));
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_widget_set_tooltip_text(label, text);
}

static GtkDropDown *string_drop_down(void)
{
    GtkExpression *expression = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
    GtkDropDown *control = GTK_DROP_DOWN(gtk_drop_down_new(NULL, expression));
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(audio_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(audio_item_bind), NULL);
    gtk_drop_down_set_factory(control, factory);
    g_object_unref(factory);
    gtk_drop_down_set_enable_search(control, TRUE);
    return control;
}

static void changed_audio(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Preferences *p = data;
    gboolean output = object == G_OBJECT(p->audio_output);
    GPtrArray *values = output ? p->audio_output_values : p->audio_device_values;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (p->updating || !p->on_change || !values || selected >= values->len)
        return;
    const char *value = g_ptr_array_index(values, selected);
    if (g_str_equal(value, audio_value(output ? p->values.audio_output : p->values.audio_device)))
        return;
    p->on_change(output ? "ao" : "audio-device", MPV_FORMAT_STRING, &value, p->user_data);
}

static const char *autocreate_hint(const char *mode)
{
    if (!mode)
        return tr("Загрузка настроек…");
    if (g_str_equal(mode, "no"))
        return tr("Открывает только выбранный файл, не добавляя соседние файлы.");
    if (g_str_equal(mode, "filter"))
        return tr("При открытии файла добавляет соседние файлы категорий, перечисленных ниже.");
    if (g_str_equal(mode, "same"))
        return tr("При открытии файла добавляет соседние файлы того же типа.");
    return tr("Текущий режим задан в mpv.conf.");
}

static const char *directory_hint(const char *mode)
{
    if (!mode)
        return tr("Загрузка настроек…");
    if (g_str_equal(mode, "auto"))
        return tr("Раскрывает подпапки по мере перехода; при случайном порядке заранее включает всё дерево.");
    if (g_str_equal(mode, "lazy"))
        return tr("Подпапки раскрываются только при переходе к ним.");
    if (g_str_equal(mode, "recursive"))
        return tr("Сразу включает файлы из всех вложенных папок.");
    if (g_str_equal(mode, "ignore"))
        return tr("Не включает вложенные папки.");
    return tr("Текущий режим задан в mpv.conf.");
}

static void update_playlist_choice(Preferences *p, GtkDropDown *control,
                                    const char *value, const char *const *options,
                                    const char *const *labels, gsize count,
                                    GPtrArray **values)
{
    if (!control)
        return;
    if (!*values) {
        g_autoptr(GtkStringList) model = gtk_string_list_new(NULL);
        *values = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < count; ++i) {
            gtk_string_list_append(model, labels[i]);
            g_ptr_array_add(*values, g_strdup(options[i]));
        }
        gtk_drop_down_set_model(control, G_LIST_MODEL(model));
    }
    guint selected = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; value && i < (*values)->len; ++i) {
        if (g_str_equal(value, g_ptr_array_index(*values, i)))
            selected = i;
    }
    if (value && selected == GTK_INVALID_LIST_POSITION) {
        /* A synchronous save refresh runs inside GtkListView activation.
         * Replacing the dropdown model there frees GTK's active selection.
         * Keep the model, including custom entries, for the widget lifetime. */
        selected = (*values)->len;
        g_autofree char *label = g_utf8_make_valid(*value ? value : "—", -1);
        g_ptr_array_add(*values, g_strdup(value));
        gtk_string_list_append(GTK_STRING_LIST(gtk_drop_down_get_model(control)), label);
    }
    gtk_drop_down_set_selected(control, selected);
    gtk_widget_set_sensitive(GTK_WIDGET(control), value != NULL && p->on_change);
}

static void changed_playlist_choice(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Preferences *p = data;
    gboolean autocreate = object == G_OBJECT(p->autocreate_playlist);
    GPtrArray *values = autocreate ? p->autocreate_values : p->directory_values;
    const char *current = autocreate ? p->values.autocreate_playlist : p->values.directory_mode;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (p->updating || !p->on_change || !current || !values || selected >= values->len)
        return;
    g_autofree char *value = g_strdup(g_ptr_array_index(values, selected));
    if (g_str_equal(value, current))
        return;
    guint known_count = autocreate ? G_N_ELEMENTS(autocreate_options)
                                  : G_N_ELEMENTS(directory_options);
    if (selected >= known_count) {
        /* Preserved custom entries describe a loaded value; they are not
         * additional supported modes that can be written back later. */
        gboolean updating = p->updating;
        p->updating = TRUE;
        for (guint i = 0; i < values->len; ++i) {
            if (g_str_equal(current, g_ptr_array_index(values, i))) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(object), i);
                break;
            }
        }
        p->updating = updating;
        return;
    }
    gtk_label_set_text(autocreate ? p->autocreate_description : p->directory_description,
        autocreate ? autocreate_hint(value) : directory_hint(value));
    p->on_change(autocreate ? "autocreate-playlist" : "directory-mode",
                  MPV_FORMAT_STRING, &value, p->user_data);
}

static void open_config(GtkButton *button, gpointer data)
{
    Preferences *p = data;
    if (p->on_open_config)
        p->on_open_config(g_object_get_data(G_OBJECT(button), "mpv-config-filename"), p->user_data);
}

static void update_number(GtkSpinButton *control, double value)
{
    if (!control)
        return;
    /* A different engine property may change while this field contains an
     * unfinished edit. Identical observations must not reformat that text. */
    if (fabs(gtk_spin_button_get_value(control) - value) < 1e-9)
        return;
    NumberEdit *edit = g_object_get_data(G_OBJECT(control), "mpv-number-edit");
    if (edit->dirty)
        return;
    gtk_spin_button_set_value(control, value);
}

static void refresh_controls(Preferences *p, const char *section)
{
    const PreferenceValues *values = &p->values;
    guint selected = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; i < G_N_ELEMENTS(hwdec_values); ++i) {
        if (g_strcmp0(values->hwdec, hwdec_values[i]) == 0) {
            selected = i;
            break;
        }
    }
    if (p->hwdec && (!section || g_str_equal(section, "video"))) {
        gtk_drop_down_set_selected(p->hwdec, selected);
        update_hwdec_status(p);
    }
    if (p->gpu_api && (!section || g_str_equal(section, "video"))) {
        guint api_selected = 0;
        for (guint i = 0; i < 3; ++i) {
            if (g_strcmp0(values->gpu_api, gpu_api_values[i]) == 0) {
                api_selected = i;
                break;
            }
        }
        gtk_drop_down_set_selected(p->gpu_api, api_selected);
    }
    const char *language = values->language ? values->language : i18n_language();
    if (p->language && (!section || g_str_equal(section, "interface")))
        gtk_drop_down_set_selected(p->language, g_strcmp0(language, "en") == 0 ? 0 : 1);
    if (p->keepaspect && (!section || g_str_equal(section, "video")))
        gtk_switch_set_active(p->keepaspect, values->keepaspect);
    if (p->deinterlace && (!section || g_str_equal(section, "video")))
        gtk_switch_set_active(p->deinterlace, values->deinterlace);
    if (p->deband && (!section || g_str_equal(section, "video")))
        gtk_switch_set_active(p->deband, values->deband);
    if (p->loop && (!section || g_str_equal(section, "playback")))
        gtk_switch_set_active(p->loop, values->loop);
    if (!section || g_str_equal(section, "playback")) {
        const char *autocreate_labels[] = {tr("Отключено"), tr("Выбранные типы"), tr("Тот же тип файла")};
        const char *directory_labels[] = {tr("Авто"), tr("Отложенно"), tr("Рекурсивно"), tr("Игнорировать подпапки")};
        update_playlist_choice(p, p->autocreate_playlist, values->autocreate_playlist,
            autocreate_options, autocreate_labels, G_N_ELEMENTS(autocreate_options),
            &p->autocreate_values);
        update_playlist_choice(p, p->directory_mode, values->directory_mode,
            directory_options, directory_labels, G_N_ELEMENTS(directory_options),
            &p->directory_values);
        if (p->autocreate_description)
            gtk_label_set_text(p->autocreate_description, autocreate_hint(values->autocreate_playlist));
        if (p->directory_description)
            gtk_label_set_text(p->directory_description, directory_hint(values->directory_mode));
        update_string(p->directory_filter_types, values->directory_filter_types);
        const char *extensions[] = {values->video_exts, values->audio_exts,
            values->image_exts, values->archive_exts, values->playlist_exts};
        for (guint i = 0; i < G_N_ELEMENTS(extensions); ++i)
            update_string(p->extension_entries[i], extensions[i]);
    }
    if (p->allow_replace && (!section || g_str_equal(section, "interface")))
        gtk_switch_set_active(p->allow_replace, values->allow_replace);
    if (p->add_to_queue && (!section || g_str_equal(section, "interface")))
        gtk_switch_set_active(p->add_to_queue, values->add_to_queue);
    if (p->timeline_previews && (!section || g_str_equal(section, "interface")))
        gtk_switch_set_active(p->timeline_previews, values->timeline_previews);
    if (!section || g_str_equal(section, "audio")) {
        refresh_audio_controls(p);
        update_number(p->audio_delay, isfinite(values->audio_delay) ? values->audio_delay : 0);
    }
    if (!section || g_str_equal(section, "subtitles")) {
        update_number(p->sub_delay, isfinite(values->sub_delay) ? values->sub_delay : 0);
        update_number(p->sub_scale, isfinite(values->sub_scale) ? values->sub_scale : 1);
    }
    if (!section || g_str_equal(section, "interface"))
        update_number(p->cursor_hide_delay,
            isfinite(values->cursor_hide_delay) && values->cursor_hide_delay >= 0.1 &&
            values->cursor_hide_delay <= 10.0 ? values->cursor_hide_delay : i18n_cursor_hide_delay());
}

void preferences_window_update(GtkWindow *window, const PreferenceValues *values)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    g_return_if_fail(values != NULL);
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    PreferenceValues snapshot = *values;
    snapshot.hwdec = g_strdup(values->hwdec);
    snapshot.hwdec_current = g_strdup(values->hwdec_current);
    snapshot.language = g_strdup(values->language);
    snapshot.gpu_api = g_strdup(values->gpu_api);
    snapshot.audio_output = g_strdup(values->audio_output);
    snapshot.audio_device = g_strdup(values->audio_device);
    snapshot.autocreate_playlist = g_strdup(values->autocreate_playlist);
    snapshot.directory_mode = g_strdup(values->directory_mode);
    snapshot.directory_filter_types = g_strdup(values->directory_filter_types);
    snapshot.video_exts = g_strdup(values->video_exts);
    snapshot.audio_exts = g_strdup(values->audio_exts);
    snapshot.image_exts = g_strdup(values->image_exts);
    snapshot.archive_exts = g_strdup(values->archive_exts);
    snapshot.playlist_exts = g_strdup(values->playlist_exts);
    g_free((gpointer)p->values.hwdec);
    g_free((gpointer)p->values.hwdec_current);
    g_free((gpointer)p->values.language);
    g_free((gpointer)p->values.gpu_api);
    g_free((gpointer)p->values.audio_output);
    g_free((gpointer)p->values.audio_device);
    g_free((gpointer)p->values.autocreate_playlist);
    g_free((gpointer)p->values.directory_mode);
    g_free((gpointer)p->values.directory_filter_types);
    g_free((gpointer)p->values.video_exts);
    g_free((gpointer)p->values.audio_exts);
    g_free((gpointer)p->values.image_exts);
    g_free((gpointer)p->values.archive_exts);
    g_free((gpointer)p->values.playlist_exts);
    p->values = snapshot;
    gboolean updating = p->updating;
    p->updating = TRUE;
    refresh_controls(p, NULL);
    p->updating = updating;
}

void preferences_window_show_interface(GtkWindow *window)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    gtk_stack_set_visible_child_name(p->stack, "interface");
}

static void load_keys(Preferences *p)
{
    if (p->keys_page && p->on_keys_load && !p->keys_load_requested) {
        p->keys_load_requested = TRUE;
        p->on_keys_load(p->window, p->keys_load_data);
    }
}

void preferences_window_set_keys_loader(GtkWindow *window,
                                        PreferencesKeysLoadFunc on_load,
                                        gpointer user_data)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    p->on_keys_load = on_load;
    p->keys_load_data = user_data;
    load_keys(p);
}

void preferences_window_set_audio_loader(GtkWindow *window,
                                         PreferencesAudioLoadFunc on_load,
                                         gpointer user_data)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    if (p->on_audio_load != on_load || p->audio_load_data != user_data)
        stop_audio_loader(p);
    p->on_audio_load = on_load;
    p->audio_load_data = user_data;
    update_audio_loader(p);
}

void preferences_window_update_audio_outputs(GtkWindow *window,
                                              const mpv_node *outputs,
                                              const mpv_node *devices)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    if (outputs && outputs->format == MPV_FORMAT_NODE_ARRAY) {
        g_clear_pointer(&p->audio_outputs, g_ptr_array_unref);
        p->audio_outputs = audio_read_choices(outputs, TRUE);
        p->audio_catalog_generation++;
    }
    if (devices && devices->format == MPV_FORMAT_NODE_ARRAY) {
        g_clear_pointer(&p->audio_devices, g_ptr_array_unref);
        p->audio_devices = audio_read_choices(devices, FALSE);
        p->audio_catalog_generation++;
    }
    refresh_audio_controls(p);
}

void preferences_window_set_playlist_loader(GtkWindow *window,
                                            PreferencesPlaylistLoadFunc on_load,
                                            gpointer user_data)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    if (p->on_playlist_load != on_load || p->playlist_load_data != user_data)
        stop_playlist_loader(p);
    p->on_playlist_load = on_load;
    p->playlist_load_data = user_data;
    update_playlist_loader(p);
}

void preferences_window_configure_keys(GtkWindow *window, const char *input_path,
                                       const KeyBindingDefault *defaults, gsize count,
                                       KeysPageChangeFunc on_change, gpointer user_data)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    if (p->keys_page) {
        keys_page_configure(p->keys_page, input_path, defaults, count, on_change, user_data);
        return;
    }
    /* A host may supply its small fallback list before Keybinds is opened;
     * keep its data without constructing widgets or the command catalog. */
    clear_keys_defaults(p);
    p->keys_defaults = g_new0(KeyBindingDefault, count);
    p->keys_default_count = count;
    for (gsize i = 0; i < count; ++i) {
        p->keys_defaults[i].key = g_strdup(defaults[i].key);
        p->keys_defaults[i].command = g_strdup(defaults[i].command);
        p->keys_defaults[i].comment = g_strdup(defaults[i].comment);
    }
    g_free(p->keys_input_path);
    p->keys_input_path = g_strdup(input_path);
    p->on_keys_change = on_change;
    p->keys_change_data = user_data;
}

void preferences_window_update_keys(GtkWindow *window, const mpv_node *bindings,
                                    const mpv_node *commands)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    if (p->keys_page)
        keys_page_update(p->keys_page, bindings, commands);
}

void preferences_window_keys_error(GtkWindow *window, const char *message)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_return_if_fail(p != NULL);
    if (p->keys_page) {
        keys_page_show_error(p->keys_page, message);
    } else {
        g_free(p->keys_error);
        p->keys_error = g_strdup(message);
    }
}

static void build_video(Preferences *p)
{
    GtkWidget *video = page(p->sections[0], tr("Видео"));
    const char *labels[] = {
        tr("Авто (безопасно)"), tr("Программное декодирование"),
        tr("Авто"), "NVDEC · NVIDIA", tr("NVDEC (копирование)"),
        tr("Авто (копирование)"), "VA-API · Intel / AMD", "VDPAU · NVIDIA", NULL
    };
    p->hwdec = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(labels));
    GtkWidget *hardware_labels = row(video, tr("Аппаратное декодирование"), NULL, GTK_WIDGET(p->hwdec));
    p->hwdec_description = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(p->hwdec_description, 0);
    gtk_label_set_wrap(p->hwdec_description, TRUE);
    gtk_label_set_max_width_chars(p->hwdec_description, 40);
    gtk_widget_add_css_class(GTK_WIDGET(p->hwdec_description), "dim-label");
    gtk_box_append(GTK_BOX(hardware_labels), GTK_WIDGET(p->hwdec_description));
    p->hwdec_status = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(p->hwdec_status, 0);
    gtk_label_set_wrap(p->hwdec_status, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(p->hwdec_status), "dim-label");
    gtk_box_append(GTK_BOX(hardware_labels), GTK_WIDGET(p->hwdec_status));
    g_signal_connect(p->hwdec, "notify::selected", G_CALLBACK(changed_hwdec), p);
    const char *api_labels[] = {tr("Авто"), "OpenGL", "OpenGL ES", "Vulkan", NULL};
    p->gpu_api = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(api_labels));
    GtkListItemFactory *api_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(api_factory, "setup", G_CALLBACK(gpu_api_item_setup), p);
    g_signal_connect(api_factory, "bind", G_CALLBACK(gpu_api_item_bind), p);
    gtk_drop_down_set_list_factory(p->gpu_api, api_factory);
    g_object_unref(api_factory);
    row(video, tr("Графический API (GPU API)"),
        tr("Выбирает OpenGL или OpenGL ES для встроенного видео. Vulkan недоступен. Требуется перезапуск."),
        GTK_WIDGET(p->gpu_api));
    g_signal_connect(p->gpu_api, "notify::selected", G_CALLBACK(changed_gpu_api), p);
    p->keepaspect = add_switch(p, video, "keepaspect", tr("Сохранять пропорции"),
        tr("Сохраняет исходное соотношение сторон без растяжения изображения."));
    p->deinterlace = add_switch(p, video, "deinterlace", tr("Убирать чересстрочность"),
        tr("Для старых телезаписей и другого чересстрочного видео."));
    p->deband = add_switch(p, video, "deband", tr("Сглаживать цветовые полосы"),
        tr("Уменьшает ступенчатые переходы на градиентах."));
}

static void build_audio(Preferences *p)
{
    GtkWidget *audio = page(p->sections[1], tr("Звук"));
    p->audio_output = string_drop_down();
    row(audio, tr("Способ аудиовывода"),
        tr("Выбирает звуковую систему. Авто использует подходящий доступный способ вывода."),
        GTK_WIDGET(p->audio_output));
    g_signal_connect(p->audio_output, "notify::selected", G_CALLBACK(changed_audio), p);
    p->audio_device = string_drop_down();
    row(audio, tr("Устройство вывода"),
        tr("Выбирает колонки, наушники или другое устройство выбранной звуковой системы. Авто следует настройкам системы."),
        GTK_WIDGET(p->audio_device));
    g_signal_connect(p->audio_device, "notify::selected", G_CALLBACK(changed_audio), p);
    p->audio_delay = add_number(p, audio, "audio-delay", tr("Задержка звука"),
        tr("Положительное значение сдвигает звук позже видео."), -5, 5, 0.05);
    use_seconds_unit(p->audio_delay);
}

static void build_subtitles(Preferences *p)
{
    GtkWidget *subtitles = page(p->sections[2], tr("Субтитры"));
    p->sub_scale = add_number(p, subtitles, "sub-scale", tr("Масштаб субтитров"),
        tr("1,00 — исходный размер."), 0.5, 3, 0.05);
    p->sub_delay = add_number(p, subtitles, "sub-delay", tr("Задержка субтитров"),
        tr("Положительное значение показывает субтитры позже."), -10, 10, 0.1);
    use_seconds_unit(p->sub_delay);
}

static void extensions_changed(GObject *object, GParamSpec *property, gpointer data)
{
    (void)property;
    Preferences *p = data;
    if (gtk_expander_get_expanded(GTK_EXPANDER(object)) && !p->extensions_built) {
        p->extensions_built = TRUE;
        gboolean updating = p->updating;
        p->updating = TRUE;
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(card, "preferences-card");
        gtk_widget_set_margin_top(card, 8);
        gtk_box_append(GTK_BOX(p->extension_content), card);
        const char *titles[] = {
            tr("Видео"), tr("Звук"), tr("Изображения"), tr("Архивы"), tr("Плейлисты")
        };
        const char *descriptions[] = {
            tr("Расширения видео через запятую, без точек. Определяют категорию video."),
            tr("Расширения аудио через запятую, без точек. Влияют также на поиск внешних аудиодорожек."),
            tr("Расширения изображений через запятую, без точек. Используются также при поиске обложек."),
            tr("Расширения архивов через запятую, без точек. Определяют категорию archive."),
            tr("Расширения плейлистов через запятую, без точек. Определяют категорию playlist.")
        };
        for (guint i = 0; i < G_N_ELEMENTS(extension_options); ++i)
            p->extension_entries[i] = add_string(p, card, extension_options[i],
                titles[i], descriptions[i], FALSE);
        refresh_controls(p, "playback");
        p->updating = updating;
    }
    update_playlist_loader(p);
}

static void build_playback(Preferences *p)
{
    GtkWidget *playback = page(p->sections[3], tr("Воспроизведение"));
    p->loop = add_switch(p, playback, "loop-file", tr("Повторять текущий файл"),
        tr("После окончания начинает текущий файл заново."));
    GtkWidget *content = gtk_widget_get_parent(playback);
    GtkWidget *title = gtk_label_new(tr("Автосоздание плейлиста"));
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(title, "heading");
    gtk_box_append(GTK_BOX(content), title);
    GtkWidget *notice = gtk_label_new(tr("Изменения применяются при следующем открытии файла или папки. Перезапуск не нужен; текущая очередь не меняется."));
    gtk_label_set_xalign(GTK_LABEL(notice), 0);
    gtk_label_set_wrap(GTK_LABEL(notice), TRUE);
    gtk_widget_add_css_class(notice, "dim-label");
    gtk_box_append(GTK_BOX(content), notice);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "preferences-card");
    gtk_box_append(GTK_BOX(content), card);
    p->autocreate_playlist = string_drop_down();
    GtkWidget *labels = row(card, tr("Добавление соседних файлов"), NULL,
        GTK_WIDGET(p->autocreate_playlist));
    p->autocreate_description = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(p->autocreate_description, 0);
    gtk_label_set_wrap(p->autocreate_description, TRUE);
    gtk_label_set_max_width_chars(p->autocreate_description, 40);
    gtk_widget_add_css_class(GTK_WIDGET(p->autocreate_description), "dim-label");
    gtk_box_append(GTK_BOX(labels), GTK_WIDGET(p->autocreate_description));
    g_signal_connect(p->autocreate_playlist, "notify::selected", G_CALLBACK(changed_playlist_choice), p);
    p->directory_mode = string_drop_down();
    labels = row(card, tr("Обход папок"), NULL, GTK_WIDGET(p->directory_mode));
    p->directory_description = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(p->directory_description, 0);
    gtk_label_set_wrap(p->directory_description, TRUE);
    gtk_label_set_max_width_chars(p->directory_description, 40);
    gtk_widget_add_css_class(GTK_WIDGET(p->directory_description), "dim-label");
    gtk_box_append(GTK_BOX(labels), GTK_WIDGET(p->directory_description));
    g_signal_connect(p->directory_mode, "notify::selected", G_CALLBACK(changed_playlist_choice), p);
    p->directory_filter_types = add_string(p, card, "directory-filter-types", tr("Типы файлов"),
        tr("Категории через запятую: video, audio, image, archive, playlist. Пустое поле включает все файлы."), TRUE);
    p->extensions = GTK_EXPANDER(gtk_expander_new(tr("Расширения файлов")));
    p->extension_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_expander_set_child(p->extensions, p->extension_content);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(p->extensions));
    g_signal_connect(p->extensions, "notify::expanded", G_CALLBACK(extensions_changed), p);
}

static void build_keys(Preferences *p)
{
    p->keys_page = keys_page_new();
    gtk_widget_set_vexpand(p->keys_page, TRUE);
    gtk_box_append(GTK_BOX(p->sections[4]), p->keys_page);
    if (p->keys_input_path) {
        keys_page_configure(p->keys_page, p->keys_input_path,
            p->keys_defaults, p->keys_default_count,
            p->on_keys_change, p->keys_change_data);
        clear_keys_defaults(p);
        g_clear_pointer(&p->keys_input_path, g_free);
    }
    if (p->keys_error)
        keys_page_show_error(p->keys_page, p->keys_error);
}

static void build_interface(Preferences *p)
{
    GtkWidget *interface = page(p->sections[5], tr("Интерфейс"));
    const char *languages[] = {"English", "Русский", NULL};
    p->language = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(languages));
    row(interface, tr("Язык интерфейса"),
        tr("Меняет язык интерфейса и сохраняет выбор для следующих запусков."), GTK_WIDGET(p->language));
    g_signal_connect(p->language, "notify::selected", G_CALLBACK(changed_language), p);
    p->cursor_hide_delay = add_number(p, interface, "ui-cursor-hide-delay",
        tr("Скрывать курсор через"),
        tr("В полном экране — от последнего движения или щелчка мыши. Меню и перетаскивание удерживают курсор видимым, не продлевая отсчёт."),
        0.1, 10.0, 0.1);
    use_seconds_unit(p->cursor_hide_delay);
    gtk_spin_button_set_digits(p->cursor_hide_delay, 1);
    gtk_spin_button_set_snap_to_ticks(p->cursor_hide_delay, TRUE);
    p->timeline_previews = add_switch(p, interface, "timeline-previews", tr("Превью при наведении"),
        tr("Показывает кадр и время над полосой воспроизведения. Кадры загружаются по запросу."));
    p->allow_replace = add_switch(p, interface, "ui-allow-replace", tr("Разрешать замену файла"),
        tr("При открытии и перетаскивании разрешает заменить текущий файл. Если оба режима выключены, открытый файл сохраняется."));
    p->add_to_queue = add_switch(p, interface, "ui-add-to-queue", tr("Добавлять файлы в очередь"),
        tr("Добавляет открытые или перетащенные файлы в очередь; имеет приоритет над заменой. Пустой плеер всегда начинает воспроизведение."));
}

static void ensure_section(Preferences *p, const char *name)
{
    void (*const builders[])(Preferences *) = {
        build_video, build_audio, build_subtitles, build_playback,
        build_keys, build_interface
    };
    for (guint i = 0; i < G_N_ELEMENTS(section_names); ++i) {
        if (g_strcmp0(name, section_names[i]) != 0 || !p->sections[i] ||
            p->section_built[i])
            continue;
        p->section_built[i] = TRUE;
        gboolean updating = p->updating;
        p->updating = TRUE;
        builders[i](p);
        refresh_controls(p, name);
        p->updating = updating;
        if (i == 4)
            load_keys(p);
        return;
    }
}

GtkWindow *preferences_window_new(GtkWindow *parent,
                                  const PreferenceValues *values,
                                  const char *config_path,
                                  PreferencesChangeFunc on_change,
                                  PreferencesOpenConfigFunc on_open_config,
                                  gpointer user_data)
{
    g_return_val_if_fail(values != NULL, NULL);
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window, tr("Настройки"));
    gtk_window_set_default_size(window, 760, 600);
    gtk_widget_add_css_class(GTK_WIDGET(window), "preferences-window");
    if (parent) {
        gtk_window_set_application(window, gtk_window_get_application(parent));
        const char *icon_name = gtk_window_get_icon_name(parent);
        gtk_window_set_icon_name(window, icon_name ? icon_name : "io.github.emless.mpvgtk");
        gtk_window_set_transient_for(window, parent);
        gtk_window_set_destroy_with_parent(window, TRUE);
    }
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *title = gtk_label_new(tr("Настройки"));
    gtk_widget_add_css_class(title, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    gtk_window_set_titlebar(window, header);

    Preferences *p = g_new0(Preferences, 1);
    p->window = window;
    p->updating = TRUE;
    p->on_change = on_change;
    p->on_open_config = on_open_config;
    p->user_data = user_data;
    g_object_set_data_full(G_OBJECT(window), "mpv-preferences", p, preferences_free);

    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(window, layout);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_box_append(GTK_BOX(layout), body);
    GtkStack *stack = GTK_STACK(gtk_stack_new());
    p->stack = stack;
    gtk_stack_set_transition_type(stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(stack, 120);
    gtk_stack_set_vhomogeneous(stack, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(stack), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
    GtkWidget *sidebar = gtk_list_box_new();
    p->sidebar = GTK_LIST_BOX(sidebar);
    gtk_list_box_set_selection_mode(p->sidebar, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(sidebar, "navigation-sidebar");
    gtk_widget_add_css_class(sidebar, "preferences-sidebar");
    gtk_widget_set_size_request(sidebar, 160, -1);
    gtk_box_append(GTK_BOX(body), sidebar);
    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(stack));

    const char *titles[] = {
        tr("Видео"), tr("Звук"), tr("Субтитры"), tr("Воспроизведение"),
        tr("Клавиши"), tr("Интерфейс")
    };
    const char *icons[] = {
        "video-display-symbolic", "audio-volume-high-symbolic",
        "media-view-subtitles-symbolic", "media-playback-start-symbolic",
        "input-keyboard-symbolic", "preferences-system-symbolic"
    };
    for (guint i = 0; i < G_N_ELEMENTS(section_names); ++i) {
        p->sections[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        add_section(p, p->sections[i], section_names[i], titles[i], icons[i]);
    }
    g_signal_connect(sidebar, "row-selected", G_CALLBACK(section_selected), p);
    g_signal_connect(stack, "notify::visible-child-name",
                     G_CALLBACK(section_visible_changed), p);

    gtk_box_append(GTK_BOX(layout), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_add_css_class(footer, "preferences-footer");
    gtk_widget_set_margin_start(footer, 18);
    gtk_widget_set_margin_end(footer, 18);
    gtk_widget_set_margin_top(footer, 12);
    gtk_widget_set_margin_bottom(footer, 12);
    GtkWidget *config_labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(config_labels, TRUE);
    GtkWidget *saved = gtk_label_new(tr("Изменения сохраняются автоматически."));
    gtk_label_set_xalign(GTK_LABEL(saved), 0);
    gtk_label_set_wrap(GTK_LABEL(saved), TRUE);
    gtk_box_append(GTK_BOX(config_labels), saved);
    GtkWidget *path = gtk_label_new(config_path ? config_path : "");
    gtk_label_set_xalign(GTK_LABEL(path), 0);
    gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(path), TRUE);
    gtk_widget_set_tooltip_text(path, config_path);
    gtk_widget_add_css_class(path, "dim-label");
    gtk_box_append(GTK_BOX(config_labels), path);
    gtk_box_append(GTK_BOX(footer), config_labels);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    const char *filenames[] = {"mpv.conf", "input.conf"};
    const char *button_labels[] = {tr("Открыть mpv.conf"), tr("Открыть input.conf")};
    for (guint i = 0; i < G_N_ELEMENTS(filenames); ++i) {
        GtkWidget *button = gtk_button_new_with_label(button_labels[i]);
        gtk_widget_set_sensitive(button, on_open_config != NULL);
        g_object_set_data(G_OBJECT(button), "mpv-config-filename", (gpointer)filenames[i]);
        g_signal_connect(button, "clicked", G_CALLBACK(open_config), p);
        gtk_box_append(GTK_BOX(buttons), button);
    }
    gtk_box_append(GTK_BOX(footer), buttons);
    gtk_box_append(GTK_BOX(layout), footer);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(preferences_pressed), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(window, "close-request", G_CALLBACK(preferences_close_requested), NULL);
    g_signal_connect(window, "notify::visible", G_CALLBACK(audio_visibility_changed), p);
    g_signal_connect(window, "unrealize", G_CALLBACK(audio_window_unrealized), p);
    preferences_window_update(window, values);
    ensure_section(p, "video");
    p->updating = FALSE;
    return window;
}
