#include "track_notice.h"

typedef struct {
    GtkRevealer *revealer;
    GtkLabel *title, *detail;
    guint hide_source;
} TrackNotice;

static void cancel_hide(TrackNotice *p)
{
    if (p->hide_source) {
        g_source_remove(p->hide_source);
        p->hide_source = 0;
    }
}

static void notice_free(gpointer data)
{
    TrackNotice *p = data;
    cancel_hide(p);
    g_free(p);
}

static gboolean hide_notice(gpointer data)
{
    TrackNotice *p = data;
    p->hide_source = 0;
    gtk_revealer_set_reveal_child(p->revealer, FALSE);
    return G_SOURCE_REMOVE;
}

static void notice_unmapped(GtkWidget *widget, gpointer data)
{
    (void)data;
    track_notice_clear(widget);
}

static GtkLabel *notice_label(void)
{
    GtkLabel *label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(label, 0);
    gtk_label_set_wrap(label, TRUE);
    gtk_label_set_wrap_mode(label, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(label, 36);
    gtk_widget_set_can_target(GTK_WIDGET(label), FALSE);
    return label;
}

GtkWidget *track_notice_new(GtkOverlay *overlay)
{
    g_return_val_if_fail(GTK_IS_OVERLAY(overlay), NULL);
    TrackNotice *p = g_new0(TrackNotice, 1);
    GtkWidget *notice = gtk_revealer_new();
    p->revealer = GTK_REVEALER(notice);
    gtk_revealer_set_transition_type(p->revealer, GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(p->revealer, 120);
    gtk_widget_set_can_target(notice, FALSE);
    gtk_widget_set_focusable(notice, FALSE);
    gtk_widget_set_halign(notice, GTK_ALIGN_END);
    gtk_widget_set_valign(notice, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(notice, 14);
    gtk_widget_set_margin_end(notice, 14);
    gtk_widget_set_margin_start(notice, 14);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_can_target(content, FALSE);
    gtk_widget_add_css_class(content, "playback-info-overlay");
    p->title = notice_label();
    gtk_widget_add_css_class(GTK_WIDGET(p->title), "heading");
    p->detail = notice_label();
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(p->title));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(p->detail));
    gtk_revealer_set_child(p->revealer, content);
    g_object_set_data_full(G_OBJECT(notice), "mpv-track-notice", p, notice_free);
    g_signal_connect(notice, "unmap", G_CALLBACK(notice_unmapped), NULL);
    gtk_overlay_add_overlay(overlay, notice);
    gtk_overlay_set_measure_overlay(overlay, notice, FALSE);
    gtk_overlay_set_clip_overlay(overlay, notice, TRUE);
    return notice;
}

void track_notice_show(GtkWidget *notice, const char *title, const char *detail)
{
    g_return_if_fail(GTK_IS_REVEALER(notice));
    TrackNotice *p = g_object_get_data(G_OBJECT(notice), "mpv-track-notice");
    g_return_if_fail(p != NULL);
    cancel_hide(p);
    g_autofree char *valid_title = g_utf8_make_valid(title ? title : "", -1);
    g_autofree char *valid_detail = g_utf8_make_valid(detail ? detail : "", -1);
    gtk_label_set_text(p->title, valid_title);
    gtk_label_set_text(p->detail, valid_detail);
    gtk_widget_set_visible(GTK_WIDGET(p->title), *valid_title != '\0');
    gtk_widget_set_visible(GTK_WIDGET(p->detail), *valid_detail != '\0');
    gtk_revealer_set_reveal_child(p->revealer, TRUE);
    p->hide_source = g_timeout_add(800, hide_notice, p);
    g_source_set_name_by_id(p->hide_source, "mpv-gtk-track-notice");
}

void track_notice_clear(GtkWidget *notice)
{
    g_return_if_fail(GTK_IS_REVEALER(notice));
    TrackNotice *p = g_object_get_data(G_OBJECT(notice), "mpv-track-notice");
    g_return_if_fail(p != NULL);
    cancel_hide(p);
    gtk_revealer_set_reveal_child(p->revealer, FALSE);
}
