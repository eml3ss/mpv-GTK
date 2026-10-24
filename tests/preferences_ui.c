/* Audio model/parser coverage runs without a display. Native control and
 * window visibility/loader checks run when GTK can connect to a display. */
#include "../src/preferences.c"

#include <locale.h>
#include <stdio.h>

typedef struct {
    GtkWindow *window;
    PreferenceValues values;
    guint writes;
    double last_value;
    gboolean fail_next;
    guint keys_loads;
    guint gpu_writes;
    const char *last_gpu_api;
    const char *pending_gpu_api, *last_opengl_es;
    guint es_writes;
    guint audio_writes, audio_loads, audio_unloads;
    guint playlist_writes, playlist_groups, playlist_load_changes, frontend_writes;
} Capture;

static void capture_playlist_load(GtkWindow *window, guint groups, gpointer data)
{
    Capture *capture = data;
    g_assert_true(window == capture->window);
    g_assert_true(groups == 0 || groups == PREFERENCES_PLAYLIST_BASIC ||
        groups == (PREFERENCES_PLAYLIST_BASIC | PREFERENCES_PLAYLIST_EXTENSIONS));
    capture->playlist_groups = groups;
    capture->playlist_load_changes++;
}

static const char **capture_playlist_field(Capture *capture, const char *name)
{
    const struct { const char *name; const char **field; } options[] = {
        {"autocreate-playlist", &capture->values.autocreate_playlist},
        {"directory-mode", &capture->values.directory_mode},
        {"directory-filter-types", &capture->values.directory_filter_types},
        {"video-exts", &capture->values.video_exts}, {"audio-exts", &capture->values.audio_exts},
        {"image-exts", &capture->values.image_exts}, {"archive-exts", &capture->values.archive_exts},
        {"playlist-exts", &capture->values.playlist_exts}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i) {
        if (g_str_equal(name, options[i].name))
            return options[i].field;
    }
    return NULL;
}

static void capture_audio_load(GtkWindow *window, gboolean active, gpointer data)
{
    Capture *capture = data;
    g_assert_true(window == capture->window);
    if (active)
        capture->audio_loads++;
    else
        capture->audio_unloads++;
}

static void capture_keys_load(GtkWindow *window, gpointer data)
{
    Capture *capture = data;
    capture->keys_loads++;
    g_assert_true(window == capture->window);
    Preferences *p = g_object_get_data(G_OBJECT(window), "mpv-preferences");
    g_assert_nonnull(p->keys_page);
    const KeyBindingDefault defaults[] = {{"SPACE", "cycle pause", "Play / pause"}};
    preferences_window_configure_keys(window, "/tmp/mpv-gtk-ui-test/input.conf",
        defaults, G_N_ELEMENTS(defaults), NULL, NULL);
}

static void capture_change(const char *name, mpv_format format,
                           const void *value, gpointer data)
{
    Capture *capture = data;
    const char **playlist_field = capture_playlist_field(capture, name);
    if (playlist_field) {
        g_assert_cmpint(format, ==, MPV_FORMAT_STRING);
        capture->playlist_writes++;
        if (capture->fail_next)
            capture->fail_next = FALSE;
        else
            *playlist_field = g_intern_string(*(const char *const *)value);
        preferences_window_update(capture->window, &capture->values);
        return;
    }
    if (g_str_equal(name, "ui-allow-replace") || g_str_equal(name, "ui-add-to-queue")) {
        g_assert_cmpint(format, ==, MPV_FORMAT_FLAG);
        capture->frontend_writes++;
        if (g_str_equal(name, "ui-allow-replace"))
            capture->values.allow_replace = !!*(const int *)value;
        else
            capture->values.add_to_queue = !!*(const int *)value;
        preferences_window_update(capture->window, &capture->values);
        return;
    }
    if (g_str_equal(name, "ao") || g_str_equal(name, "audio-device")) {
        g_assert_cmpint(format, ==, MPV_FORMAT_STRING);
        capture->audio_writes++;
        const char *selected = g_intern_string(*(const char *const *)value);
        if (capture->fail_next) {
            capture->fail_next = FALSE;
        } else if (g_str_equal(name, "ao")) {
            capture->values.audio_output = selected;
            if (!g_str_equal(audio_value(capture->values.audio_device), "auto") &&
                !audio_device_matches(capture->values.audio_device, selected))
                capture->values.audio_device = "auto";
        } else {
            capture->values.audio_device = selected;
        }
        preferences_window_update(capture->window, &capture->values);
        return;
    }
    if (g_str_equal(name, "gpu-api")) {
        g_assert_cmpint(format, ==, MPV_FORMAT_STRING);
        g_assert_null(capture->pending_gpu_api);
        capture->gpu_writes++;
        capture->last_gpu_api = *(const char *const *)value;
        capture->pending_gpu_api = capture->last_gpu_api;
        return;
    }
    if (g_str_equal(name, "opengl-es")) {
        g_assert_cmpint(format, ==, MPV_FORMAT_STRING);
        g_assert_nonnull(capture->pending_gpu_api);
        capture->es_writes++;
        capture->last_opengl_es = *(const char *const *)value;
        if (capture->fail_next) {
            capture->fail_next = FALSE;
        } else {
            capture->values.gpu_api = g_str_equal(capture->pending_gpu_api, "opengl") &&
                g_str_equal(capture->last_opengl_es, "yes") ? "opengl-es" : capture->pending_gpu_api;
        }
        capture->pending_gpu_api = NULL;
        preferences_window_update(capture->window, &capture->values);
        return;
    }
    g_assert_cmpstr(name, ==, "ui-cursor-hide-delay");
    g_assert_cmpint(format, ==, MPV_FORMAT_DOUBLE);
    capture->writes++;
    capture->last_value = *(const double *)value;
    if (capture->fail_next) {
        capture->fail_next = FALSE;
        /* Match the host's failed-save rollback, including its reentrancy. */
        preferences_window_update(capture->window, &capture->values);
    } else {
        capture->values.cursor_hide_delay = capture->last_value;
    }
}

static void assert_value(GtkSpinButton *control, double expected)
{
    g_assert_cmpfloat_with_epsilon(gtk_spin_button_get_value(control), expected, 1e-9);
}

static void check_cursor_delay_input(void)
{
    const char *valid[] = {"1.0", "1.0 s", "1s", "  1.0  s  "};
    for (guint i = 0; i < G_N_ELEMENTS(valid); ++i) {
        double value = 0;
        g_assert_true(parse_seconds(valid[i], &value));
        g_assert_cmpfloat_with_epsilon(value, 1.0, 1e-9);
    }
    const char *invalid[] = {
        "", " ", "s", "1.0 ss", "1 s s", "1 ms", "1 seconds",
        "1s2", "1,0 s", "nan", "inf", "1e999 s"
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        double value = 12;
        g_assert_false(parse_seconds(invalid[i], &value));
        g_assert_cmpfloat(value, ==, 12);
    }
    double negative;
    g_assert_true(parse_seconds("-0.15 s", &negative));
    g_assert_cmpfloat_with_epsilon(negative, -0.15, 1e-9);
    puts("PASS seconds input: plain numbers, s suffix, negative delays and invalid text");
}

static void check_audio_models(void)
{
    char *output_names[] = {"pipewire", "pulse", "alsa", "pcm", "lavc", "pipewire", "null"};
    mpv_node output_values[G_N_ELEMENTS(output_names)];
    for (guint i = 0; i < G_N_ELEMENTS(output_names); ++i)
        output_values[i] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = output_names[i]};
    mpv_node_list output_list = {.num = G_N_ELEMENTS(output_values), .values = output_values};
    mpv_node outputs = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &output_list};
    g_autoptr(GPtrArray) choices = audio_read_choices(&outputs, TRUE);
    g_assert_cmpuint(choices->len, ==, 4);
    g_autoptr(GPtrArray) names = NULL;
    guint selected;
    g_autoptr(GtkStringList) model = audio_make_model(choices, "pulse", NULL, &names, &selected);
    g_assert_cmpuint(selected, ==, 2);
    g_assert_cmpstr(gtk_string_list_get_string(model, 0), ==, tr("Авто"));
    g_assert_cmpstr(gtk_string_list_get_string(model, selected), ==, "PulseAudio");
    g_assert_cmpstr(g_ptr_array_index(names, selected), ==, "pulse");
    g_clear_object(&model);
    g_clear_pointer(&names, g_ptr_array_unref);
    model = audio_make_model(choices, "custom-output", NULL, &names, &selected);
    g_assert_cmpuint(selected, ==, 5);
    g_assert_cmpstr(g_ptr_array_index(names, selected), ==, "custom-output");

    char *device_names[] = {"pipewire/usb", "pulse/speaker", "alsa", "alsa/default", "pipewirex/wrong"};
    char *keys[] = {"name", "description"};
    mpv_node device_fields[G_N_ELEMENTS(device_names)][2];
    mpv_node_list device_maps[G_N_ELEMENTS(device_names)];
    mpv_node device_values[G_N_ELEMENTS(device_names)];
    for (guint i = 0; i < G_N_ELEMENTS(device_names); ++i) {
        device_fields[i][0] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = device_names[i]};
        device_fields[i][1] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = "Default"};
        device_maps[i] = (mpv_node_list){.num = 2, .keys = keys, .values = device_fields[i]};
        device_values[i] = (mpv_node){.format = MPV_FORMAT_NODE_MAP, .u.list = &device_maps[i]};
    }
    mpv_node_list device_list = {.num = G_N_ELEMENTS(device_values), .values = device_values};
    mpv_node devices = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &device_list};
    g_autoptr(GPtrArray) device_choices = audio_read_choices(&devices, FALSE);
    g_clear_object(&model);
    g_clear_pointer(&names, g_ptr_array_unref);
    model = audio_make_model(device_choices, "pipewire/usb", "pipewire", &names, &selected);
    g_assert_cmpuint(names->len, ==, 2);
    g_assert_cmpuint(selected, ==, 1);
    g_assert_true(audio_device_matches("alsa", "alsa"));
    g_assert_false(audio_device_matches("pipewirex/wrong", "pipewire"));
    g_clear_object(&model);
    g_clear_pointer(&names, g_ptr_array_unref);
    model = audio_make_model(device_choices, "pulse/speaker", "auto", &names, &selected);
    g_assert_cmpuint(names->len, ==, 6);
    g_assert_cmpstr(gtk_string_list_get_string(model, selected), ==, "Default · PulseAudio");
    g_clear_object(&model);
    g_clear_pointer(&names, g_ptr_array_unref);
    model = audio_make_model(device_choices, "pipewire/unplugged", "pipewire", &names, &selected);
    g_assert_cmpuint(names->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(names, selected), ==, "pipewire/unplugged");
    g_clear_object(&model);
    g_clear_pointer(&names, g_ptr_array_unref);
    model = audio_make_model(NULL, "pipewire/unplugged", "pipewire", &names, &selected);
    g_assert_cmpuint(names->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(names, selected), ==, "pipewire/unplugged");
    puts("PASS audio models: stock AO choices, encoding outputs excluded, device filtering, bare AO names, Auto labels and preserved custom/unplugged selections");
}

static void check_filter_types(void)
{
    const struct { const char *text, *expected; } valid[] = {
        {"", ""}, {"   ", ""}, {"video,audio", "video,audio"},
        {" video, audio , playlist ", "video,audio,playlist"},
        {"video,audio,image,archive,playlist", "video,audio,image,archive,playlist"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(valid); ++i) {
        g_autofree char *value = NULL;
        g_assert_true(normalize_filter_types(valid[i].text, &value));
        g_assert_cmpstr(value, ==, valid[i].expected);
    }
    const char *invalid[] = {"video,", ",audio", "video,,audio", "movies", "Video", "video;audio", "video,\naudio"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        g_autofree char *value = NULL;
        g_assert_false(normalize_filter_types(invalid[i], &value));
        g_assert_null(value);
    }
    puts("PASS playlist filter input: actual media categories, whitespace normalization, explicit empty-all and invalid-list rejection");
}

static GtkWidget *descendant_of_type(GtkWidget *widget, GType type)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type))
        return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = descendant_of_type(child, type);
        if (found)
            return found;
    }
    return NULL;
}

static void wait_mapping(GtkWidget *widget, gboolean mapped)
{
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (gtk_widget_get_mapped(widget) != mapped && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_true(gtk_widget_get_mapped(widget) == mapped);
}

static void activate_dropdown_row(GtkDropDown *control, guint position)
{
    /* Exercise GtkListView's actual activation path: the crash happened after
     * the synchronous host callback returned, not in set_selected alone. */
    g_assert_true(gtk_widget_activate(GTK_WIDGET(control)));
    GtkWidget *popover = descendant_of_type(GTK_WIDGET(control), GTK_TYPE_POPOVER);
    g_assert_nonnull(popover);
    wait_mapping(popover, TRUE);
    GtkWidget *list = descendant_of_type(popover, GTK_TYPE_LIST_VIEW);
    g_assert_nonnull(list);
    g_assert_true(gtk_selection_model_select_item(
        gtk_list_view_get_model(GTK_LIST_VIEW(list)), position, TRUE));
    g_assert_true(gtk_widget_activate_action(list, "list.activate-item", "u", position));
    wait_mapping(popover, FALSE);
}

static void check_playlist_activation(void)
{
    GtkSettings *settings = gtk_settings_get_default();
    gboolean animations;
    g_object_get(settings, "gtk-enable-animations", &animations, NULL);
    g_object_set(settings, "gtk-enable-animations", FALSE, NULL);
    Capture capture = {.values = {.hwdec = "no", .gpu_api = "auto",
        .language = "en", .cursor_hide_delay = 3, .autocreate_playlist = "no",
        .directory_mode = "auto", .directory_filter_types = "video,audio"}};
    capture.window = preferences_window_new(NULL, &capture.values,
        "/tmp/mpv-gtk-ui-test/mpv.conf", capture_change, NULL, &capture);
    Preferences *p = g_object_get_data(G_OBJECT(capture.window), "mpv-preferences");
    gtk_stack_set_visible_child_name(p->stack, "playback");
    gtk_window_present(capture.window);
    wait_mapping(GTK_WIDGET(p->autocreate_playlist), TRUE);
    g_autoptr(GListModel) autocreate = g_object_ref(gtk_drop_down_get_model(p->autocreate_playlist));
    g_autoptr(GListModel) directory = g_object_ref(gtk_drop_down_get_model(p->directory_mode));
    const guint autoplay_selections[] = {1, 2, 0, 2, 1};
    for (guint i = 0; i < G_N_ELEMENTS(autoplay_selections); ++i) {
        guint selected = autoplay_selections[i];
        activate_dropdown_row(p->autocreate_playlist, selected);
        g_assert_cmpstr(capture.values.autocreate_playlist, ==, autocreate_options[selected]);
        g_assert_true(gtk_drop_down_get_model(p->autocreate_playlist) == autocreate);
    }
    const guint directory_selections[] = {1, 2, 3, 0};
    for (guint i = 0; i < G_N_ELEMENTS(directory_selections); ++i) {
        guint selected = directory_selections[i];
        activate_dropdown_row(p->directory_mode, selected);
        g_assert_cmpstr(capture.values.directory_mode, ==, directory_options[selected]);
        g_assert_true(gtk_drop_down_get_model(p->directory_mode) == directory);
    }
    guint writes = capture.playlist_writes;
    PreferenceValues snapshot = capture.values;
    snapshot.autocreate_playlist = "custom-mode";
    snapshot.directory_mode = "custom-directory-mode";
    preferences_window_update(capture.window, &snapshot);
    g_assert_true(gtk_drop_down_get_model(p->autocreate_playlist) == autocreate);
    g_assert_true(gtk_drop_down_get_model(p->directory_mode) == directory);
    g_assert_cmpuint(g_list_model_get_n_items(autocreate), ==, 4);
    g_assert_cmpuint(g_list_model_get_n_items(directory), ==, 5);
    activate_dropdown_row(p->autocreate_playlist, 0);
    activate_dropdown_row(p->directory_mode, 1);
    g_assert_cmpstr(capture.values.autocreate_playlist, ==, "no");
    g_assert_cmpstr(capture.values.directory_mode, ==, "lazy");
    g_assert_cmpuint(capture.playlist_writes, ==, writes + 2);
    activate_dropdown_row(p->autocreate_playlist, G_N_ELEMENTS(autocreate_options));
    activate_dropdown_row(p->directory_mode, G_N_ELEMENTS(directory_options));
    g_assert_cmpuint(capture.playlist_writes, ==, writes + 2);
    g_assert_cmpuint(gtk_drop_down_get_selected(p->autocreate_playlist), ==, 0);
    g_assert_cmpuint(gtk_drop_down_get_selected(p->directory_mode), ==, 1);
    capture.fail_next = TRUE;
    activate_dropdown_row(p->autocreate_playlist, 2);
    g_assert_cmpstr(capture.values.autocreate_playlist, ==, "no");
    g_assert_cmpuint(gtk_drop_down_get_selected(p->autocreate_playlist), ==, 0);
    g_assert_true(gtk_drop_down_get_model(p->autocreate_playlist) == autocreate);
    g_assert_true(gtk_drop_down_get_model(p->directory_mode) == directory);
    gtk_window_destroy(capture.window);
    g_object_set(settings, "gtk-enable-animations", animations, NULL);
    puts("PASS playlist popup activation: all stock modes, synchronous save refresh, stable models, read-only custom entries and failed-save rollback");
}

static void check_playlist_widgets(void)
{
    Capture capture = {.values = {.hwdec = "no", .gpu_api = "auto",
        .language = "en", .cursor_hide_delay = 3, .allow_replace = TRUE}};
    capture.window = preferences_window_new(NULL, &capture.values,
        "/tmp/mpv-gtk-ui-test/mpv.conf", capture_change, NULL, &capture);
    Preferences *p = g_object_get_data(G_OBJECT(capture.window), "mpv-preferences");
    preferences_window_set_playlist_loader(capture.window, capture_playlist_load, &capture);
    g_assert_null(p->autocreate_playlist);
    g_assert_cmpuint(capture.playlist_load_changes, ==, 0);
    gtk_stack_set_visible_child_name(p->stack, "playback");
    g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(p->autocreate_playlist)));
    g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(p->directory_mode)));
    g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(p->directory_filter_types)));
    g_assert_false(p->extensions_built);
    for (guint i = 0; i < G_N_ELEMENTS(p->extension_entries); ++i)
        g_assert_null(p->extension_entries[i]);
    gtk_widget_set_visible(GTK_WIDGET(capture.window), TRUE);
    g_assert_cmpuint(capture.playlist_groups, ==, PREFERENCES_PLAYLIST_BASIC);
    capture.values.autocreate_playlist = "no";
    capture.values.directory_mode = "auto";
    capture.values.directory_filter_types = "video,audio";
    preferences_window_update(capture.window, &capture.values);
    g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(p->autocreate_playlist)));
    g_assert_cmpuint(capture.playlist_writes, ==, 0);
    GtkEntry *types = p->directory_filter_types;
    gtk_editable_set_text(GTK_EDITABLE(types), " audio,video ");
    PreferenceValues observed = capture.values;
    observed.directory_filter_types = "playlist";
    preferences_window_update(capture.window, &observed);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(types)), ==, " audio,video ");
    GtkWidget *delegate = GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(types)));
    gtk_root_set_focus(GTK_ROOT(capture.window), delegate);
    g_signal_emit_by_name(types, "activate");
    g_assert_cmpstr(capture.values.directory_filter_types, ==, "audio,video");
    g_assert_null(gtk_root_get_focus(GTK_ROOT(capture.window)));
    g_assert_cmpuint(capture.playlist_writes, ==, 1);
    gtk_editable_set_text(GTK_EDITABLE(types), "video,unknown");
    g_assert_false(commit_string(types));
    StringEdit *edit = g_object_get_data(G_OBJECT(types), "mpv-string-edit");
    g_assert_true(gtk_widget_get_visible(GTK_WIDGET(edit->error)));
    g_assert_cmpuint(capture.playlist_writes, ==, 1);
    gtk_editable_set_text(GTK_EDITABLE(types), "");
    string_focus_left(NULL, types);
    g_assert_cmpstr(capture.values.directory_filter_types, ==, "");
    g_assert_cmpuint(capture.playlist_writes, ==, 2);
    gtk_editable_set_text(GTK_EDITABLE(types), "image");
    preferences_window_commit(capture.window);
    g_assert_cmpuint(capture.playlist_writes, ==, 3);
    capture.fail_next = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(types), "audio");
    g_signal_emit_by_name(types, "activate");
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(types)), ==, "image");
    g_assert_cmpuint(capture.playlist_writes, ==, 4);
    observed = capture.values;
    observed.autocreate_playlist = "custom-mode";
    observed.directory_mode = "custom-directory-mode";
    preferences_window_update(capture.window, &observed);
    g_assert_cmpstr(gtk_string_list_get_string(GTK_STRING_LIST(
        gtk_drop_down_get_model(p->autocreate_playlist)),
        gtk_drop_down_get_selected(p->autocreate_playlist)), ==, "custom-mode");
    g_assert_cmpuint(capture.playlist_writes, ==, 4);
    preferences_window_update(capture.window, &capture.values);
    gtk_drop_down_set_selected(p->autocreate_playlist, 1);
    gtk_drop_down_set_selected(p->directory_mode, 2);
    g_assert_cmpstr(capture.values.autocreate_playlist, ==, "filter");
    g_assert_cmpstr(capture.values.directory_mode, ==, "recursive");
    g_assert_cmpuint(capture.playlist_writes, ==, 6);
    gtk_expander_set_expanded(p->extensions, TRUE);
    g_assert_true(p->extensions_built);
    g_assert_cmpuint(capture.playlist_groups, ==,
        PREFERENCES_PLAYLIST_BASIC | PREFERENCES_PLAYLIST_EXTENSIONS);
    g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(p->extension_entries[0])));
    capture.values.video_exts = "mkv,myCustomExtension";
    capture.values.audio_exts = "flac,opus";
    capture.values.image_exts = "png,jpg";
    capture.values.archive_exts = "zip";
    capture.values.playlist_exts = "m3u,m3u8";
    preferences_window_update(capture.window, &capture.values);
    GtkEntry *video_exts = p->extension_entries[0];
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(video_exts)), ==, "mkv,myCustomExtension");
    gtk_editable_set_text(GTK_EDITABLE(video_exts), "mkv,anotherCustomExtension");
    g_signal_emit_by_name(video_exts, "activate");
    g_assert_cmpstr(capture.values.video_exts, ==, "mkv,anotherCustomExtension");
    g_assert_cmpuint(capture.playlist_writes, ==, 7);
    gtk_expander_set_expanded(p->extensions, FALSE);
    g_assert_cmpuint(capture.playlist_groups, ==, PREFERENCES_PLAYLIST_BASIC);
    gtk_expander_set_expanded(p->extensions, TRUE);
    g_assert_true(p->extension_entries[0] == video_exts);
    preferences_window_show_interface(capture.window);
    g_assert_cmpuint(capture.playlist_groups, ==, 0);
    g_assert_true(gtk_switch_get_active(p->allow_replace));
    g_assert_false(gtk_switch_get_active(p->add_to_queue));
    gtk_switch_set_active(p->add_to_queue, TRUE);
    gtk_switch_set_active(p->allow_replace, FALSE);
    g_assert_true(capture.values.add_to_queue);
    g_assert_false(capture.values.allow_replace);
    g_assert_cmpuint(capture.frontend_writes, ==, 2);
    gtk_stack_set_visible_child_name(p->stack, "playback");
    g_assert_cmpuint(capture.playlist_groups, ==,
        PREFERENCES_PLAYLIST_BASIC | PREFERENCES_PLAYLIST_EXTENSIONS);
    gtk_widget_set_visible(GTK_WIDGET(capture.window), FALSE);
    g_assert_cmpuint(capture.playlist_groups, ==, 0);
    gtk_widget_set_visible(GTK_WIDGET(capture.window), TRUE);
    gtk_window_destroy(capture.window);
    g_assert_cmpuint(capture.playlist_groups, ==, 0);
    puts("PASS playlist UI: lazy subscriptions/extensions, unknown controls, real option callbacks, empty lists, dirty edits, validation, save rollback and file-opening switches");
}

static void check_deferred_decoder_status(void)
{
    const char *languages[] = {"ru", "en"};
    for (guint i = 0; i < G_N_ELEMENTS(languages); ++i) {
        i18n_set_language(languages[i]);
        PreferenceValues active = {.hwdec = "nvdec", .hwdec_current = "nvdec",
            .media_loaded = TRUE, .has_video = TRUE, .hwdec_known = TRUE};
        g_autofree char *native_status = preferences_decoder_status(&active);
        PreferenceValues deferred = active;
        deferred.hwdec = "vaapi";
        deferred.hwdec_pending = deferred.hwdec_restart_required = TRUE;
        g_autofree char *deferred_status = preferences_decoder_status(&deferred);
        g_assert_cmpstr(deferred_status, ==, native_status);

        /* A saved future selection must not change the explanation of the
         * actual software decoder or turn missing observations into facts. */
        deferred.hwdec_current = "no";
        g_autofree char *software_status = preferences_decoder_status(&deferred);
        deferred.hwdec = "no";
        g_autofree char *other_selection = preferences_decoder_status(&deferred);
        g_assert_cmpstr(other_selection, ==, software_status);
        active.hwdec_known = deferred.hwdec_known = FALSE;
        g_autofree char *unknown_status = preferences_decoder_status(&active);
        g_autofree char *deferred_unknown = preferences_decoder_status(&deferred);
        g_assert_cmpstr(deferred_unknown, ==, unknown_status);
    }
    i18n_set_language("ru");
    puts("PASS deferred decoder: saved choices retain the actual native/software/unknown status in RU and EN");
}

int main(void)
{
    check_cursor_delay_input();
    check_audio_models();
    check_filter_types();
    check_deferred_decoder_status();
    if (!gtk_init_check()) {
        puts("SKIP preferences UI: no usable display");
        return 77;
    }
    /* The application uses C numeric locale after initializing libmpv. */
    setlocale(LC_NUMERIC, "C");
    check_playlist_activation();
    check_playlist_widgets();
    Capture capture = {.values = {
        .hwdec = "no", .language = "en", .gpu_api = "opengl-es",
        .audio_output = "auto", .audio_device = "auto", .keepaspect = TRUE,
        .audio_delay = 0.15, .sub_delay = 0.2, .sub_scale = 1.2,
        .cursor_hide_delay = 2.5
    }};
    capture.window = preferences_window_new(NULL, &capture.values,
        "/tmp/mpv-gtk-ui-test/mpv.conf", capture_change, NULL, &capture);
    Preferences *p = g_object_get_data(G_OBJECT(capture.window), "mpv-preferences");
    g_assert_nonnull(p->hwdec);
    g_assert_nonnull(p->gpu_api);
    g_assert_cmpuint(gtk_drop_down_get_selected(p->gpu_api), ==, 2);
    g_assert_cmpuint(capture.gpu_writes, ==, 0);
    g_assert_null(p->audio_output);
    g_assert_null(p->audio_device);
    preferences_window_set_audio_loader(capture.window, capture_audio_load, &capture);
    g_assert_cmpuint(capture.audio_loads, ==, 0);
    mpv_node output_values[] = {
        {.format = MPV_FORMAT_STRING, .u.string = "pipewire"},
        {.format = MPV_FORMAT_STRING, .u.string = "pulse"}
    };
    mpv_node_list output_list = {.num = 2, .values = output_values};
    mpv_node outputs = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &output_list};
    char *device_keys[] = {"name", "description"};
    mpv_node device_fields[][2] = {
        {{.format = MPV_FORMAT_STRING, .u.string = "pipewire/usb"},
         {.format = MPV_FORMAT_STRING, .u.string = "USB headphones"}},
        {{.format = MPV_FORMAT_STRING, .u.string = "pulse/speaker"},
         {.format = MPV_FORMAT_STRING, .u.string = "Speakers"}}
    };
    mpv_node_list device_maps[] = {
        {.num = 2, .keys = device_keys, .values = device_fields[0]},
        {.num = 2, .keys = device_keys, .values = device_fields[1]}
    };
    mpv_node device_values[] = {
        {.format = MPV_FORMAT_NODE_MAP, .u.list = &device_maps[0]},
        {.format = MPV_FORMAT_NODE_MAP, .u.list = &device_maps[1]}
    };
    mpv_node_list device_list = {.num = 2, .values = device_values};
    mpv_node devices = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &device_list};
    preferences_window_update_audio_outputs(capture.window, &outputs, &devices);
    g_assert_null(p->audio_output);
    g_assert_null(p->audio_delay);
    g_assert_null(p->sub_delay);
    g_assert_null(p->sub_scale);
    g_assert_null(p->loop);
    g_assert_null(p->language);
    g_assert_null(p->cursor_hide_delay);
    g_assert_null(p->keys_page);
    for (guint i = 1; i < G_N_ELEMENTS(p->sections); ++i)
        g_assert_null(gtk_widget_get_first_child(p->sections[i]));
    preferences_window_set_keys_loader(capture.window, capture_keys_load, &capture);
    preferences_window_update_keys(capture.window, NULL, NULL);
    preferences_window_commit(capture.window);
    g_assert_cmpuint(capture.keys_loads, ==, 0);
    g_assert_cmpuint(capture.writes, ==, 0);
    /* A not-yet-created page receives the latest snapshot, and retains its
     * own copy of strings whose engine event has already been released. */
    PreferenceValues pending = capture.values;
    char *temporary_hwdec = g_strdup("auto-copy");
    char *temporary_gpu_api = g_strdup("opengl");
    pending.hwdec = temporary_hwdec;
    pending.gpu_api = temporary_gpu_api;
    pending.audio_delay = 0.45;
    preferences_window_update(capture.window, &pending);
    g_free(temporary_hwdec);
    g_free(temporary_gpu_api);
    g_assert_cmpstr(p->values.hwdec, ==, "auto-copy");
    g_assert_cmpstr(p->values.gpu_api, ==, "opengl");
    g_assert_cmpuint(gtk_drop_down_get_selected(p->gpu_api), ==, 1);
    g_assert_cmpuint(capture.gpu_writes, ==, 0);
    g_assert_null(p->audio_delay);
    gtk_stack_set_visible_child_name(p->stack, "audio");
    assert_value(p->audio_delay, 0.45);
    g_assert_nonnull(p->audio_output);
    g_assert_nonnull(p->audio_device);
    g_assert_cmpuint(p->audio_output_values->len, ==, 3);
    g_assert_cmpuint(p->audio_device_values->len, ==, 3);
    g_assert_cmpuint(capture.audio_writes, ==, 0);
    gtk_drop_down_set_selected(p->audio_output, 1);
    g_assert_cmpuint(capture.audio_writes, ==, 1);
    g_assert_cmpstr(capture.values.audio_output, ==, "pipewire");
    g_assert_cmpuint(p->audio_device_values->len, ==, 2);
    gtk_drop_down_set_selected(p->audio_device, 1);
    g_assert_cmpuint(capture.audio_writes, ==, 2);
    g_assert_cmpstr(capture.values.audio_device, ==, "pipewire/usb");
    mpv_node_list empty_list = {0};
    mpv_node empty = {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &empty_list};
    preferences_window_update_audio_outputs(capture.window, NULL, &empty);
    g_assert_cmpstr(g_ptr_array_index(p->audio_device_values,
        gtk_drop_down_get_selected(p->audio_device)), ==, "pipewire/usb");
    g_assert_cmpuint(capture.audio_writes, ==, 2);
    preferences_window_update_audio_outputs(capture.window, NULL, &devices);
    g_assert_cmpuint(p->audio_output_values->len, ==, 3);
    g_assert_cmpuint(capture.audio_writes, ==, 2);
    capture.fail_next = TRUE;
    gtk_drop_down_set_selected(p->audio_output, 2);
    g_assert_cmpuint(capture.audio_writes, ==, 3);
    g_assert_cmpuint(gtk_drop_down_get_selected(p->audio_output), ==, 1);
    gtk_drop_down_set_selected(p->audio_output, 2);
    g_assert_cmpuint(capture.audio_writes, ==, 4);
    g_assert_cmpstr(capture.values.audio_output, ==, "pulse");
    g_assert_cmpstr(capture.values.audio_device, ==, "auto");
    g_assert_cmpuint(capture.audio_loads, ==, 0);
    gtk_widget_set_visible(GTK_WIDGET(capture.window), TRUE);
    g_assert_cmpuint(capture.audio_loads, ==, 1);
    gtk_widget_set_visible(GTK_WIDGET(capture.window), FALSE);
    g_assert_cmpuint(capture.audio_unloads, ==, 1);
    gtk_widget_set_visible(GTK_WIDGET(capture.window), TRUE);
    g_assert_cmpuint(capture.audio_loads, ==, 2);
    gtk_stack_set_visible_child_name(p->stack, "video");
    g_assert_cmpuint(capture.audio_unloads, ==, 2);
    gtk_widget_set_visible(GTK_WIDGET(capture.window), FALSE);
    g_assert_cmpuint(capture.audio_writes, ==, 4);
    g_assert_null(p->sub_delay);
    g_assert_null(p->keys_page);
    preferences_window_update(capture.window, &capture.values);
    g_assert_cmpuint(gtk_drop_down_get_selected(p->gpu_api), ==, 2);
    gtk_drop_down_set_selected(p->gpu_api, 0);
    g_assert_cmpuint(capture.gpu_writes, ==, 1);
    g_assert_cmpstr(capture.last_gpu_api, ==, "auto");
    g_assert_cmpstr(capture.last_opengl_es, ==, "auto");
    capture.fail_next = TRUE;
    gtk_drop_down_set_selected(p->gpu_api, 2);
    g_assert_cmpuint(capture.gpu_writes, ==, 2);
    g_assert_cmpstr(capture.last_gpu_api, ==, "opengl");
    g_assert_cmpstr(capture.last_opengl_es, ==, "yes");
    g_assert_cmpuint(gtk_drop_down_get_selected(p->gpu_api), ==, 0);
    gtk_drop_down_set_selected(p->gpu_api, 1);
    g_assert_cmpuint(capture.gpu_writes, ==, 3);
    g_assert_cmpstr(capture.last_gpu_api, ==, "opengl");
    g_assert_cmpstr(capture.last_opengl_es, ==, "no");
    /* Vulkan remains visible for clarity, but the embedded Render API must
     * reject it even for direct programmatic selection. */
    g_assert_cmpuint(g_list_model_get_n_items(gtk_drop_down_get_model(p->gpu_api)), ==, 4);
    g_assert_cmpstr(gtk_string_list_get_string(GTK_STRING_LIST(
        gtk_drop_down_get_model(p->gpu_api)), 3), ==, "Vulkan");
    g_assert_nonnull(gtk_drop_down_get_list_factory(p->gpu_api));
    gtk_drop_down_set_selected(p->gpu_api, 3);
    g_assert_cmpuint(gtk_drop_down_get_selected(p->gpu_api), ==, 1);
    g_assert_cmpuint(capture.gpu_writes, ==, 3);
    gtk_drop_down_set_selected(p->gpu_api, 2);
    g_assert_cmpuint(capture.gpu_writes, ==, 4);
    g_assert_cmpstr(capture.last_gpu_api, ==, "opengl");
    g_assert_cmpstr(capture.last_opengl_es, ==, "yes");
    g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(p->gpu_api)));
    g_assert_cmpuint(gtk_drop_down_get_selected(p->gpu_api), ==, 2);
    g_assert_cmpuint(capture.gpu_writes, ==, 4);
    g_assert_cmpuint(capture.es_writes, ==, capture.gpu_writes);
    g_assert_null(capture.pending_gpu_api);
    gtk_list_box_select_row(p->sidebar, gtk_list_box_get_row_at_index(p->sidebar, 2));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(p->stack), ==, "subtitles");
    g_assert_nonnull(p->sub_delay);
    g_assert_null(p->cursor_hide_delay);
    preferences_window_show_interface(capture.window);
    GtkSpinButton *control = p->cursor_hide_delay;
    /* set_digits() emits value-changed in GTK: construction must not save
     * the spin button's initial lower bound over the configured delay. */
    g_assert_cmpuint(capture.writes, ==, 0);
    assert_value(control, 2.5);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(p->audio_delay)), ==, "0.15 s");
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(p->sub_delay)), ==, "0.20 s");
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(control)), ==, "2.5 s");
    g_assert_false(gtk_spin_button_get_numeric(control));

    gtk_editable_set_text(GTK_EDITABLE(control), "1.7");
    g_assert_cmpuint(capture.writes, ==, 0);
    assert_value(control, 2.5);
    g_assert_true(number_key_pressed(NULL, GDK_KEY_Return, 0, 0, control));
    g_assert_cmpuint(capture.writes, ==, 1);
    assert_value(control, 1.7);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(control)), ==, "1.7 s");
    g_assert_cmpfloat_with_epsilon(capture.last_value, 1.7, 1e-9);
    preferences_window_commit(capture.window);
    g_assert_cmpuint(capture.writes, ==, 1);

    gtk_editable_set_text(GTK_EDITABLE(control), "3.4 s");
    number_focus_left(NULL, control);
    g_assert_cmpuint(capture.writes, ==, 2);
    assert_value(control, 3.4);
    gtk_editable_set_text(GTK_EDITABLE(control), "4.5");
    preferences_window_commit(capture.window);
    g_assert_cmpuint(capture.writes, ==, 3);
    assert_value(control, 4.5);

    gtk_spin_button_spin(control, GTK_SPIN_STEP_FORWARD, 0.1);
    g_assert_cmpuint(capture.writes, ==, 4);
    assert_value(control, 4.6);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(control)), ==, "4.6 s");
    gtk_spin_button_spin(control, GTK_SPIN_STEP_BACKWARD, 0.1);
    g_assert_cmpuint(capture.writes, ==, 5);
    assert_value(control, 4.5);

    gtk_editable_set_text(GTK_EDITABLE(control), "6.3");
    PreferenceValues observed = capture.values;
    observed.cursor_hide_delay = 3.2;
    observed.audio_delay = 0.7;
    preferences_window_update(capture.window, &observed);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(control)), ==, "6.3");
    assert_value(control, 4.5);
    assert_value(p->audio_delay, 0.7);
    g_assert_cmpuint(capture.writes, ==, 5);
    g_assert_true(number_key_pressed(NULL, GDK_KEY_KP_Enter, 0, 0, control));
    g_assert_cmpuint(capture.writes, ==, 6);
    assert_value(control, 6.3);

    preferences_window_show_interface(capture.window);
    GtkWidget *text = GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(control)));
    gtk_root_set_focus(GTK_ROOT(capture.window), text);
    g_assert_true(gtk_root_get_focus(GTK_ROOT(capture.window)) == text);
    capture.fail_next = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(control), "7.4");
    g_assert_true(number_key_pressed(NULL, GDK_KEY_ISO_Enter, 0, 0, control));
    g_assert_cmpuint(capture.writes, ==, 7);
    assert_value(control, 6.3);
    g_assert_null(gtk_root_get_focus(GTK_ROOT(capture.window)));
    gtk_editable_set_text(GTK_EDITABLE(control), "7.4");
    g_assert_true(number_key_pressed(NULL, GDK_KEY_Return, 0, 0, control));
    g_assert_cmpuint(capture.writes, ==, 8);
    assert_value(control, 7.4);
    /* Equivalent text, repeated Enter and close must not duplicate saves. */
    gtk_editable_set_text(GTK_EDITABLE(control), "7.4");
    number_key_pressed(NULL, GDK_KEY_Return, 0, 0, control);
    g_assert_false(preferences_close_requested(capture.window, NULL));
    g_assert_cmpuint(capture.writes, ==, 8);
    /* Constructing another section must not roll back the just-saved edit
     * while its asynchronous engine observation is still in flight. */
    gtk_stack_set_visible_child_name(p->stack, "playback");
    assert_value(control, 7.4);
    gtk_stack_set_visible_child_name(p->stack, "keys");
    g_assert_cmpuint(capture.keys_loads, ==, 1);
    GtkWidget *keys = p->keys_page;
    gtk_stack_set_visible_child_name(p->stack, "audio");
    gtk_stack_set_visible_child_name(p->stack, "keys");
    g_assert_cmpuint(capture.keys_loads, ==, 1);
    g_assert_true(p->keys_page == keys);
    gtk_window_destroy(capture.window);

    capture.window = preferences_window_new(NULL, &capture.values,
        "/tmp/mpv-gtk-ui-test/mpv.conf", capture_change, NULL, &capture);
    p = g_object_get_data(G_OBJECT(capture.window), "mpv-preferences");
    g_assert_cmpuint(capture.writes, ==, 8);
    g_assert_cmpuint(capture.gpu_writes, ==, 4);
    g_assert_cmpuint(gtk_drop_down_get_selected(p->gpu_api), ==, 2);
    g_assert_null(p->cursor_hide_delay);
    preferences_window_show_interface(capture.window);
    assert_value(p->cursor_hide_delay, 7.4);
    gtk_editable_set_text(GTK_EDITABLE(p->cursor_hide_delay), "8.6");
    g_assert_false(preferences_close_requested(capture.window, NULL));
    g_assert_cmpuint(capture.writes, ==, 9);
    assert_value(p->cursor_hide_delay, 8.6);
    gtk_window_destroy(capture.window);
    puts("PASS preferences UI: silent audio catalogue refresh/hotplug, AO/device filtering and rollback, lazy audio subscriptions, GPU API, lazy sections/Keys, owned snapshots and numeric commits");
    return 0;
}
