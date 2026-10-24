/* Real libmpv checks for optional subscriptions and on-demand snapshots.
 * No GTK display or user configuration is used. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <glib/gstdio.h>

typedef struct {
    GHashTable *properties;
    guint playback_replies;
    double audio_delay;
} Events;

static void collect_events(Player *p, Events *events)
{
    gint64 deadline = g_get_monotonic_time() + 120000;
    while (g_get_monotonic_time() < deadline) {
        mpv_event *event = mpv_wait_event(p->mpv, 0.01);
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *property = event->data;
            g_hash_table_add(events->properties, g_strdup(property->name));
            if (g_str_equal(property->name, "audio-delay") &&
                property->format == MPV_FORMAT_DOUBLE && property->data)
                events->audio_delay = *(double *)property->data;
        } else if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY &&
                   event->reply_userdata >= PLAYBACK_REFRESH_BASE &&
                   event->reply_userdata < PLAYBACK_REFRESH_BASE + G_N_ELEMENTS(playback_properties)) {
            events->playback_replies++;
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *log = event->data;
            g_error("libmpv %s: %s", log->prefix, log->text);
        }
    }
}

static void assert_group(const Events *events, const ObservedProperty *properties,
                         gsize count, gboolean present)
{
    for (gsize i = 0; i < count; ++i)
        g_assert_cmpint(g_hash_table_contains(events->properties, properties[i].name), ==, present);
}

static void check_queue_widgets(Player *p)
{
    if (!gtk_init_check()) {
        puts("SKIP lazy queue UI: no usable display");
        return;
    }
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(root);
    p->sidebar = gtk_revealer_new();
    gtk_revealer_set_transition_duration(GTK_REVEALER(p->sidebar), 0);
    g_signal_connect(p->sidebar, "notify::reveal-child", G_CALLBACK(queue_reveal_changed), p);
    g_signal_connect(p->sidebar, "notify::child-revealed", G_CALLBACK(queue_reveal_changed), p);
    gtk_box_append(GTK_BOX(root), p->sidebar);
    p->previous_button = gtk_button_new();
    p->next_button = gtk_button_new();
    gtk_box_append(GTK_BOX(root), p->previous_button);
    gtk_box_append(GTK_BOX(root), p->next_button);
    g_assert_null(p->playlist);
    g_assert_null(gtk_revealer_get_child(GTK_REVEALER(p->sidebar)));
    p->playlist_count = 0;
    update_playlist_navigation(p);
    g_assert_false(gtk_widget_get_visible(p->previous_button));
    g_assert_false(gtk_widget_get_visible(p->next_button));
    p->playlist_count = 1;
    update_playlist_navigation(p);
    g_assert_false(gtk_widget_get_visible(p->previous_button));
    g_assert_false(gtk_widget_get_visible(p->next_button));
    p->playlist_count = 2;
    update_playlist_navigation(p);
    g_assert_true(gtk_widget_get_visible(p->previous_button));
    g_assert_true(gtk_widget_get_visible(p->next_button));
    for (int i = 0; i < 2; ++i) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->sidebar), TRUE);
        g_assert_nonnull(p->playlist);
        g_assert_nonnull(p->queue_label);
        g_assert_true(p->playlist_observed);
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->sidebar), FALSE);
        g_assert_false(p->playlist_observed);
        g_assert_null(p->playlist);
        g_assert_null(p->queue_label);
        g_assert_null(gtk_revealer_get_child(GTK_REVEALER(p->sidebar)));
    }
    p->area = GTK_GL_AREA(gtk_gl_area_new());
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(p->area));
    p->error_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    p->error_label = gtk_label_new("");
    gtk_box_append(GTK_BOX(p->error_bar), p->error_label);
    gtk_box_append(GTK_BOX(root), p->error_bar);
    gtk_widget_set_visible(p->error_bar, FALSE);
    g_autoptr(GError) context_error = g_error_new_literal(
        GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE, "Context creation failed (test)");
    gtk_gl_area_set_error(p->area, context_error);
    p->ready = TRUE;
    /* The error argument is owned by the area and freed during the call. */
    gl_context_failed(p, p->area, gtk_gl_area_get_error(p->area));
    g_assert_null(gtk_gl_area_get_error(p->area));
    g_assert_false(p->ready);
    g_assert_true(p->render_failed);
    g_assert_true(gtk_widget_get_visible(p->error_bar));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(p->error_label)), ==, context_error->message);
    g_object_unref(root);
    p->sidebar = p->previous_button = p->next_button = NULL;
    p->area = NULL;
    p->error_bar = p->error_label = NULL;
    puts("PASS lazy queue UI: allocation, release, reopen and empty-queue navigation");
    puts("PASS GL error UI: message retained in upper bar, native center error cleared");
}

int main(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-lazy-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input = g_build_filename(directory, "input.conf", NULL);
    g_assert_true(g_file_set_contents(path, "vo=libmpv\nhwdec=no\naudio-delay=0.25\n", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(input, "SPACE cycle pause\n", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_setenv("MPV_GTK_CONFIG_FILE", path, TRUE));
    Player p = {.wake_fd = -1};
    g_assert_true(init_mpv(&p));
    Events events = {.properties = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL)};
    collect_events(&p, &events);
    assert_group(&events, preferences_properties, G_N_ELEMENTS(preferences_properties), FALSE);
    assert_group(&events, decoder_properties, G_N_ELEMENTS(decoder_properties), FALSE);
    assert_group(&events, audio_properties, G_N_ELEMENTS(audio_properties), FALSE);
    assert_group(&events, playback_properties, G_N_ELEMENTS(playback_properties), FALSE);
    g_assert_false(g_hash_table_contains(events.properties, "playlist"));
    g_assert_true(g_hash_table_contains(events.properties, "volume"));
    g_assert_true(g_hash_table_contains(events.properties, "fullscreen"));
    g_assert_cmpuint(events.playback_replies, ==, 0);

    observe_group(&p, preferences_properties, G_N_ELEMENTS(preferences_properties),
                  PREFERENCES_OBSERVE_BASE, TRUE, &p.preferences_observed);
    collect_events(&p, &events);
    assert_group(&events, preferences_properties, G_N_ELEMENTS(preferences_properties), TRUE);
    g_assert_cmpfloat_with_epsilon(events.audio_delay, 0.25, 1e-9);
    g_hash_table_remove_all(events.properties);
    observe_group(&p, preferences_properties, G_N_ELEMENTS(preferences_properties),
                  PREFERENCES_OBSERVE_BASE, TRUE, &p.preferences_observed);
    collect_events(&p, &events);
    g_assert_cmpuint(g_hash_table_size(events.properties), ==, 0);

    double delay = 0.75;
    g_assert_cmpint(mpv_set_property_async(p.mpv, 50000, "audio-delay", MPV_FORMAT_DOUBLE, &delay), >=, 0);
    collect_events(&p, &events);
    g_assert_true(g_hash_table_contains(events.properties, "audio-delay"));
    g_assert_cmpfloat_with_epsilon(events.audio_delay, 0.75, 1e-9);
    observe_group(&p, preferences_properties, G_N_ELEMENTS(preferences_properties),
                  PREFERENCES_OBSERVE_BASE, FALSE, &p.preferences_observed);
    g_hash_table_remove_all(events.properties);
    delay = 1.25;
    g_assert_cmpint(mpv_set_property_async(p.mpv, 50001, "audio-delay", MPV_FORMAT_DOUBLE, &delay), >=, 0);
    collect_events(&p, &events);
    g_assert_false(g_hash_table_contains(events.properties, "audio-delay"));
    observe_group(&p, preferences_properties, G_N_ELEMENTS(preferences_properties),
                  PREFERENCES_OBSERVE_BASE, TRUE, &p.preferences_observed);
    collect_events(&p, &events);
    g_assert_cmpfloat_with_epsilon(events.audio_delay, 1.25, 1e-9);
    observe_group(&p, preferences_properties, G_N_ELEMENTS(preferences_properties),
                  PREFERENCES_OBSERVE_BASE, FALSE, &p.preferences_observed);

    observe_group(&p, decoder_properties, G_N_ELEMENTS(decoder_properties),
                  DECODER_OBSERVE_BASE, TRUE, &p.decoder_observed);
    collect_events(&p, &events);
    assert_group(&events, decoder_properties, G_N_ELEMENTS(decoder_properties), TRUE);
    observe_group(&p, decoder_properties, G_N_ELEMENTS(decoder_properties),
                  DECODER_OBSERVE_BASE, FALSE, &p.decoder_observed);
    const ObservedProperty playlist_property = {"playlist", MPV_FORMAT_NODE};
    observe_group(&p, &playlist_property, 1, PLAYLIST_OBSERVE_ID, TRUE, &p.playlist_observed);
    collect_events(&p, &events);
    g_assert_true(g_hash_table_contains(events.properties, "playlist"));
    observe_group(&p, &playlist_property, 1, PLAYLIST_OBSERVE_ID, FALSE, &p.playlist_observed);

    mpv_event reconfigure = {.event_id = MPV_EVENT_VIDEO_RECONFIG};
    handle_event(&p, &reconfigure);
    reconfigure.event_id = MPV_EVENT_PLAYBACK_RESTART;
    handle_event(&p, &reconfigure);
    collect_events(&p, &events);
    g_assert_cmpuint(events.playback_replies, ==, 0);
    request_playback_values(&p);
    collect_events(&p, &events);
    g_assert_cmpuint(events.playback_replies, ==, G_N_ELEMENTS(playback_properties));
    check_queue_widgets(&p);

    mpv_set_wakeup_callback(p.mpv, NULL, NULL);
    mpv_terminate_destroy(p.mpv);
    close(p.wake_fd);
    if (p.gl_library)
        dlclose(p.gl_library);
    config_clear(&p.config);
    g_hash_table_unref(events.properties);
    g_assert_cmpint(g_remove(input), ==, 0);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS lazy resources: no optional startup subscriptions, on-demand snapshots, unsubscribe and fresh state on reopen");
    return 0;
}
