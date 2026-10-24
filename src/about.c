#include "about.h"
#include "i18n.h"

static gboolean about_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                  guint keycode, GdkModifierType state,
                                  gpointer data)
{
    (void)controller;
    (void)keycode;
    (void)state;
    if (keyval != GDK_KEY_Escape)
        return FALSE;
    gtk_window_close(GTK_WINDOW(data));
    return TRUE;
}

static GtkWidget *component_row(const char *title, const char *value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_margin_start(row, 14);
    gtk_widget_set_margin_end(row, 14);
    gtk_widget_set_margin_top(row, 12);
    gtk_widget_set_margin_bottom(row, 12);

    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row), label);

    GtkWidget *detail = gtk_label_new(value);
    gtk_widget_add_css_class(detail, "dim-label");
    gtk_box_append(GTK_BOX(row), detail);
    return row;
}

GtkWindow *about_window_new(GtkWindow *parent, const char *version)
{
    g_return_val_if_fail(GTK_IS_WINDOW(parent), NULL);

    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_application(window, gtk_window_get_application(parent));
    gtk_window_set_transient_for(window, parent);
    gtk_window_set_destroy_with_parent(window, TRUE);
    gtk_window_set_modal(window, TRUE);
    gtk_window_set_resizable(window, FALSE);
    gtk_window_set_default_size(window, 380, -1);
    gtk_window_set_title(window, tr("О программе"));
    gtk_window_set_icon_name(window, "io.github.emless.mpvgtk");
    gtk_widget_add_css_class(GTK_WIDGET(window), "mpv-about");

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), gtk_label_new(""));
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header), ":close");
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_widget_add_css_class(header, "flat");
    gtk_window_set_titlebar(window, header);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 600);
    gtk_window_set_child(window, scroll);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content, 24);
    gtk_widget_set_margin_end(content, 24);
    gtk_widget_set_margin_top(content, 24);
    gtk_widget_set_margin_bottom(content, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

    GtkWidget *icon = gtk_image_new_from_icon_name("io.github.emless.mpvgtk");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 96);
    gtk_widget_set_margin_bottom(icon, 8);
    gtk_box_append(GTK_BOX(content), icon);

    GtkWidget *name = gtk_label_new("mpv GTK");
    gtk_widget_add_css_class(name, "title-1");
    gtk_box_append(GTK_BOX(content), name);

    GtkWidget *description = gtk_label_new(tr("Медиаплеер на GTK и mpv."));
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 32);
    gtk_label_set_justify(GTK_LABEL(description), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(content), description);

    GtkWidget *badge = gtk_label_new(version ? version : "");
    gtk_widget_add_css_class(badge, "about-version");
    gtk_widget_set_halign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(content), badge);

    GtkWidget *components_title = gtk_label_new(tr("Компоненты"));
    gtk_label_set_xalign(GTK_LABEL(components_title), 0);
    gtk_widget_add_css_class(components_title, "heading");
    gtk_widget_set_margin_top(components_title, 16);
    gtk_box_append(GTK_BOX(content), components_title);

    GtkWidget *components = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(components, "preferences-card");
    g_autofree char *gtk_version = g_strdup_printf("GTK %u.%u.%u",
        gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
    gtk_box_append(GTK_BOX(components), component_row(tr("Интерфейс"), gtk_version));
    gtk_box_append(GTK_BOX(components), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(components), component_row(tr("Воспроизведение"), "mpv"));
    gtk_box_append(GTK_BOX(content), components);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(about_key_pressed), window);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);
    return window;
}
