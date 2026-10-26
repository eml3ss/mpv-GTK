/* Exercise persistent decoded hover frames, passive GTK presentation,
 * cancellation, and the persisted toggle using isolated generated media. */
#define _GNU_SOURCE
#include "../src/seek_preview.h"
#include "../src/i18n.h"

#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    mpv_handle *mpv;
    SeekPreview *preview;
    GtkWindow *window;
    GtkWidget *scale, *custom;
    gboolean loaded, restarted;
    double playback_position;
} PreviewHost;

static void set_option(mpv_handle *mpv, const char *name, const char *value, gboolean optional)
{
    int status = mpv_set_option_string(mpv, name, value);
    g_assert_true(status >= 0 || (optional && status == MPV_ERROR_OPTION_NOT_FOUND));
}

static void poll_host(PreviewHost *host)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    mpv_event *event = mpv_wait_event(host->mpv, .005);
    if (event->event_id == MPV_EVENT_START_FILE) host->loaded = host->restarted = FALSE;
    else if (event->event_id == MPV_EVENT_FILE_LOADED) host->loaded = TRUE;
    else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) host->restarted = TRUE;
    if (host->preview) seek_preview_handle_event(host->preview, event);
}

static void pump_for(PreviewHost *host, gint64 delay)
{
    gint64 deadline = g_get_monotonic_time() + delay;
    while (g_get_monotonic_time() < deadline) poll_host(host);
}

static void wait_loaded(PreviewHost *host)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host->loaded || !host->restarted) {
        if (g_get_monotonic_time() > deadline) g_error("Preview fixture did not load.");
        poll_host(host);
    }
}

static GtkWidget *find_widget(GtkWidget *root, GType type)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(root, type)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *match = find_widget(child, type);
        if (match) return match;
    }
    return NULL;
}

static GtkEventController *motion_controller(GtkWidget *widget)
{
    g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        GtkEventController *controller = g_list_model_get_item(controllers, i);
        if (g_strcmp0(gtk_event_controller_get_name(controller), "timeline-preview-motion") == 0) {
            /* This check supplies its own coordinates through signals. Do not
             * let the desktop pointer crossing a newly placed popup replace
             * that synthetic hover with unrelated motion/leave events. */
            gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_NONE);
            return controller;
        }
        g_object_unref(controller);
    }
    g_error("Preview motion controller is not attached.");
    return NULL;
}

static void hover_at(GtkWidget *widget, double fraction, double y, gboolean enter)
{
    g_autoptr(GtkEventController) controller = motion_controller(widget);
    double x = gtk_widget_get_width(widget) * fraction;
    g_signal_emit_by_name(controller, enter ? "enter" : "motion", x, y);
}

static void hover(GtkWidget *widget, double fraction, gboolean enter)
{
    hover_at(widget, fraction, gtk_widget_get_height(widget) / 2.0, enter);
}

static void leave(GtkWidget *widget)
{
    g_autoptr(GtkEventController) controller = motion_controller(widget);
    g_signal_emit_by_name(controller, "leave");
}

static GdkTexture *preview_texture(GtkWidget *timeline)
{
    GtkWidget *popover = find_widget(timeline, GTK_TYPE_POPOVER);
    if (!popover || !gtk_widget_get_mapped(popover)) return NULL;
    GtkWidget *picture = find_widget(popover, GTK_TYPE_PICTURE);
    GdkPaintable *paintable = picture ? gtk_picture_get_paintable(GTK_PICTURE(picture)) : NULL;
    return GDK_IS_TEXTURE(paintable) ? GDK_TEXTURE(paintable) : NULL;
}

static gboolean card_allocated(GtkWidget *timeline)
{
    GtkWidget *popover = find_widget(timeline, GTK_TYPE_POPOVER);
    if (!popover || !gtk_widget_get_mapped(popover) || gtk_widget_get_width(popover) <= 0 ||
        gtk_widget_get_height(popover) <= 0) return FALSE;
    GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(popover));
    GtkWidget *picture = find_widget(popover, GTK_TYPE_PICTURE);
    GtkNative *native = gtk_widget_get_native(popover);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    return content && gtk_widget_get_mapped(content) && gtk_widget_get_width(content) > 0 &&
        gtk_widget_get_height(content) > 0 && picture && gtk_widget_get_mapped(picture) &&
        gtk_widget_get_width(picture) > 0 && gtk_widget_get_height(picture) > 0 &&
        surface && gdk_surface_get_mapped(surface) && gdk_surface_get_width(surface) > 0 &&
        gdk_surface_get_height(surface) > 0;
}

static gboolean has_color(GdkTexture *texture, gboolean blue)
{
    if (!texture) return FALSE;
    int width = gdk_texture_get_width(texture), height = gdk_texture_get_height(texture);
    g_assert_cmpint(width, >, 0);
    g_assert_cmpint(height, >, 0);
    g_assert_cmpint(width, <=, 160);
    g_assert_cmpint(height, <=, 90);
    g_autoptr(GdkTextureDownloader) download = gdk_texture_downloader_new(texture);
    gdk_texture_downloader_set_format(download, GDK_MEMORY_R8G8B8A8);
    gsize stride;
    g_autoptr(GBytes) bytes = gdk_texture_downloader_download_bytes(download, &stride);
    const guchar *data = g_bytes_get_data(bytes, NULL);
    const guchar *pixel = data + stride * (height / 2) + 4 * (width / 2);
    if (pixel[3] < 180) return FALSE;
    return blue ? pixel[2] > 180 && pixel[0] < 70 && pixel[1] < 70
        : pixel[0] > 180 && pixel[2] < 70 && pixel[1] < 70;
}

static void wait_color(PreviewHost *host, GtkWidget *timeline, gboolean blue)
{
    gint64 deadline = g_get_monotonic_time() + 6000000;
    while (!card_allocated(timeline) || !has_color(preview_texture(timeline), blue)) {
        if (g_get_monotonic_time() > deadline) {
            GtkWidget *popover = find_widget(timeline, GTK_TYPE_POPOVER);
            GtkWidget *content = popover ? gtk_popover_get_child(GTK_POPOVER(popover)) : NULL;
            g_error("No visible decoded %s hover frame: popover=%dx%d content=%dx%d mapped=%d.",
                blue ? "blue" : "red", popover ? gtk_widget_get_width(popover) : -1,
                popover ? gtk_widget_get_height(popover) : -1, content ? gtk_widget_get_width(content) : -1,
                content ? gtk_widget_get_height(content) : -1, popover ? gtk_widget_get_mapped(popover) : 0);
        }
        poll_host(host);
    }
    GtkWidget *popover = find_widget(timeline, GTK_TYPE_POPOVER);
    g_assert_false(gtk_popover_get_autohide(GTK_POPOVER(popover)));
    g_assert_false(gtk_widget_get_can_target(popover));
    GtkWidget *label = find_widget(popover, GTK_TYPE_LABEL);
    g_assert_nonnull(label);
    g_assert_true(g_regex_match_simple(blue ? "^00:0[23]$" : "^00:0[01]$",
        gtk_label_get_text(GTK_LABEL(label)), 0, 0));
}

static void wait_clock(PreviewHost *host, GtkWidget *timeline, gboolean blue)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    GtkWidget *popover = NULL;
    for (;;) {
        popover = find_widget(timeline, GTK_TYPE_POPOVER);
        GtkWidget *label = popover ? find_widget(popover, GTK_TYPE_LABEL) : NULL;
        GtkWidget *content = popover ? gtk_popover_get_child(GTK_POPOVER(popover)) : NULL;
        GtkWidget *picture = popover ? find_widget(popover, GTK_TYPE_PICTURE) : NULL;
        if (popover && gtk_widget_get_mapped(popover) && label && gtk_widget_get_mapped(label) &&
            gtk_widget_get_width(label) > 0 && content && gtk_widget_get_width(content) > 0 &&
            gtk_widget_get_width(content) < 140 && gtk_widget_get_height(content) < 70 &&
            picture && !gtk_widget_get_mapped(picture) &&
            g_regex_match_simple(blue ? "^00:0[23]$" : "^00:0[01]$",
                gtk_label_get_text(GTK_LABEL(label)), 0, 0)) break;
        if (g_get_monotonic_time() > deadline) g_error("No compact timestamp hover.");
        poll_host(host);
    }
    GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(popover));
    GtkWidget *picture = find_widget(popover, GTK_TYPE_PICTURE);
    GtkWidget *spinner = find_widget(popover, GTK_TYPE_SPINNER);
    g_assert_true(gtk_popover_get_has_arrow(GTK_POPOVER(popover)));
    g_assert_false(gtk_popover_get_autohide(GTK_POPOVER(popover)));
    g_assert_false(gtk_widget_get_can_target(popover));
    g_assert_true(gtk_widget_has_css_class(popover, "time-only"));
    g_assert_nonnull(picture);
    g_assert_null(gtk_picture_get_paintable(GTK_PICTURE(picture)));
    g_assert_false(gtk_widget_get_mapped(picture));
    g_assert_false(gtk_widget_get_visible(gtk_widget_get_parent(picture)));
    g_assert_false(gtk_widget_get_visible(spinner));
    g_assert_false(gtk_spinner_get_spinning(GTK_SPINNER(spinner)));
    g_assert_cmpint(gtk_widget_get_width(content), <, 140);
    g_assert_cmpint(gtk_widget_get_height(content), <, 70);
}

static guint decoder_threads(void)
{
    g_autoptr(GDir) tasks = g_dir_open("/proc/self/task", 0, NULL);
    if (!tasks) return 0;
    guint count = 0;
    const char *task;
    while ((task = g_dir_read_name(tasks))) {
        g_autofree char *path = g_build_filename("/proc/self/task", task, "comm", NULL);
        g_autofree char *name = NULL;
        if (g_file_get_contents(path, &name, NULL, NULL) &&
            g_str_equal(g_strstrip(name), "preview-decoder")) ++count;
    }
    return count;
}

static void assert_playback_unchanged(PreviewHost *host)
{
    int paused = 0;
    double position = 0;
    g_assert_cmpint(mpv_get_property(host->mpv, "pause", MPV_FORMAT_FLAG, &paused), >=, 0);
    g_assert_true(paused);
    g_assert_cmpint(mpv_get_property(host->mpv, "time-pos", MPV_FORMAT_DOUBLE, &position), >=, 0);
    g_assert_cmpfloat(fabs(position - host->playback_position), <, .05);
}

static guint call_count(const char *filename)
{
    g_autofree char *contents = NULL;
    gsize length = 0;
    return g_file_get_contents(filename, &contents, &length, NULL) ? (guint)length : 0;
}

static void wait_hidden(PreviewHost *host, GtkWidget *popover)
{
    gint64 deadline = g_get_monotonic_time() + 1500000;
    while (popover && gtk_widget_get_mapped(popover)) {
        if (g_get_monotonic_time() > deadline) g_error("Hover preview did not hide.");
        poll_host(host);
    }
}

static void check_vertical_bounds(PreviewHost *host, GtkWidget *timeline,
                                   GtkWidget *popover, double pending_fraction)
{
    /* Leave while a new target's refinement can still be pending. Neither
     * late frames nor horizontal motion outside the bar may reopen it. */
    hover(timeline, pending_fraction, FALSE);
    pump_for(host, 80000);
    hover_at(timeline, .75, -8, FALSE);
    wait_hidden(host, popover);
    pump_for(host, 250000);
    g_assert_null(preview_texture(timeline));
    hover_at(timeline, .25, -8, FALSE);
    pump_for(host, 100000);
    g_assert_false(gtk_widget_get_mapped(popover));
    g_assert_null(preview_texture(timeline));
    /* Motion back inside is sufficient even without a separate enter. */
    hover(timeline, .75, FALSE);
    wait_color(host, timeline, TRUE);
    hover_at(timeline, .75, gtk_widget_get_height(timeline) + 8, FALSE);
    wait_hidden(host, popover);
    pump_for(host, 250000);
    g_assert_null(preview_texture(timeline));
    hover_at(timeline, .25, gtk_widget_get_height(timeline) + 8, FALSE);
    pump_for(host, 100000);
    g_assert_false(gtk_widget_get_mapped(popover));
    g_assert_null(preview_texture(timeline));
    hover(timeline, .25, FALSE);
    wait_color(host, timeline, FALSE);
    assert_playback_unchanged(host);
}

static void save_card(PreviewHost *host, GtkWidget *popover)
{
    const char *filename = g_getenv("MPV_GTK_PREVIEW_SCREENSHOT");
    if (!filename || !*filename) return;
    GtkNative *native = gtk_widget_get_native(popover);
    g_assert_nonnull(native);
    GtkWidget *content = gtk_popover_get_child(GTK_POPOVER(popover));
    GdkPaintable *paintable = gtk_widget_paintable_new(content);
    gtk_widget_queue_draw(content);
    pump_for(host, 100000);
    int width = gtk_widget_get_width(content), height = gtk_widget_get_height(content);
    g_print("Preview card mapped=%d, surface mapped=%d, popover=%dx%d, content=%dx%d\n",
        gtk_widget_get_mapped(popover), gdk_surface_get_mapped(gtk_native_get_surface(native)),
        gtk_widget_get_width(popover), gtk_widget_get_height(popover), width, height);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    if (!node) { g_object_unref(paintable); return; }
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture *texture = gsk_renderer_render_texture(gtk_native_get_renderer(native), node, &bounds);
    g_assert_nonnull(texture);
    g_assert_true(gdk_texture_save_to_png(texture, filename));
    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
}

static void generate_media(const char *ffmpeg, const char *filename)
{
    char *args[] = {(char *)ffmpeg, "-nostdin", "-v", "error",
        "-f", "lavfi", "-i", "color=c=red:s=320x180:r=24:d=2",
        "-f", "lavfi", "-i", "color=c=blue:s=320x180:r=24:d=2",
        "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0,format=yuv420p[v]",
        "-map", "[v]", "-c:v", "libx264", "-preset", "ultrafast", "-g", "24",
        "-threads", "1", (char *)filename, NULL};
    int status;
    g_autofree char *diagnostic = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, args, NULL, 0, NULL, NULL, NULL, &diagnostic, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("Cannot generate hover fixture: %s; %s", error->message, diagnostic ? diagnostic : "");
}

static void generate_audio(const char *ffmpeg, const char *filename)
{
    char *args[] = {(char *)ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=8000:duration=4", "-c:a", "pcm_s16le", (char *)filename, NULL};
    int status;
    g_autofree char *diagnostic = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_spawn_sync(NULL, args, NULL, 0, NULL, NULL, NULL, &diagnostic, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("Cannot generate audio hover fixture: %s; %s", error->message, diagnostic ? diagnostic : "");
}

static void remove_tree(const char *path)
{
    GStatBuf info;
    if (g_lstat(path, &info) != 0) return;
    if (!S_ISDIR(info.st_mode)) { g_assert_cmpint(g_unlink(path), ==, 0); return; }
    g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
    g_assert_nonnull(directory);
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        remove_tree(child);
    }
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

int main(void)
{
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    gtk_disable_setlocale();
    gdk_set_allowed_backends("wayland");
    g_setenv("GSK_RENDERER", "opengl", FALSE);
    if (!gtk_init_check()) { puts("SKIP: no usable native Wayland display."); return 77; }
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    if (!ffmpeg) { puts("SKIP: FFmpeg is unavailable."); return 77; }
    const char *empty[] = {NULL};
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_set_search_path(theme, empty);
    const char *resources[] = {"/org/gtk/libgtk/icons/", NULL};
    gtk_icon_theme_set_resource_path(theme, resources);
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-hover-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *config = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *interface = g_build_filename(directory, "interface.conf", NULL);
    const char *settings = "# retain preview comment\n[Interface]\nfuture-option=keep\n[Other]\ncustom=yes\n";
    g_assert_true(g_file_set_contents(config, "# isolated mpv settings\n", -1, &error));
    g_assert_true(g_file_set_contents(interface, settings, -1, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(interface, 0640), ==, 0);
    g_assert_true(i18n_init(config, &error));
    g_assert_true(i18n_timeline_previews());
    g_assert_true(i18n_save_timeline_previews(FALSE, &error));
    g_assert_true(i18n_init(config, &error));
    g_assert_false(i18n_timeline_previews());
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(interface, &saved, NULL, &error));
    g_assert_nonnull(strstr(saved, "timeline-previews=false"));
    g_assert_nonnull(strstr(saved, "# retain preview comment"));
    g_assert_nonnull(strstr(saved, "future-option=keep"));
    g_assert_nonnull(strstr(saved, "custom=yes"));
    GStatBuf metadata;
    g_assert_cmpint(g_stat(interface, &metadata), ==, 0);
    g_assert_cmpint(metadata.st_mode & 0777, ==, 0640);
    g_autofree char *source = g_build_filename(directory, "hover Unicode Ω.mp4", NULL);
    generate_media(ffmpeg, source);
    g_autofree char *audio = g_build_filename(directory, "audio hover.wav", NULL);
    generate_audio(ffmpeg, audio);
    g_autofree char *tools = g_build_filename(directory, "tools", NULL);
    g_assert_cmpint(g_mkdir(tools, 0700), ==, 0);
    g_autofree char *counter = g_build_filename(directory, "ffmpeg-count", NULL);
    g_autofree char *wrapper = g_build_filename(tools, "ffmpeg", NULL);
    g_autofree char *quoted_counter = g_shell_quote(counter);
    g_autofree char *quoted_ffmpeg = g_shell_quote(ffmpeg);
    /* Generation above uses the real tool. Any subsequent invocation would
     * reveal a regression to launching a decoder process for each request. */
    g_autofree char *script = g_strdup_printf("#!/bin/sh\nprintf x >> %s\nexec %s \"$@\"\n",
        quoted_counter, quoted_ffmpeg);
    g_assert_true(g_file_set_contents(wrapper, script, -1, &error));
    g_assert_cmpint(g_chmod(wrapper, 0700), ==, 0);
    g_autofree char *path = g_strdup(g_getenv("PATH"));
    g_autofree char *with_tools = g_strdup_printf("%s:%s", tools, path ? path : "");
    g_assert_true(g_setenv("PATH", with_tools, TRUE));
    PreviewHost host = {.playback_position = .4};
    host.mpv = mpv_create();
    g_assert_nonnull(host.mpv);
    set_option(host.mpv, "config", "no", FALSE);
    set_option(host.mpv, "vo", "null", FALSE);
    set_option(host.mpv, "ao", "null", FALSE);
    set_option(host.mpv, "idle", "yes", FALSE);
    set_option(host.mpv, "pause", "yes", FALSE);
    set_option(host.mpv, "terminal", "no", FALSE);
    set_option(host.mpv, "load-scripts", "no", FALSE);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console", "load-auto-profiles",
        "load-select", "load-positioning", "load-commands", "load-context-menu", NULL};
    for (guint i = 0; scripts[i]; ++i) set_option(host.mpv, scripts[i], "no", TRUE);
    g_assert_cmpint(mpv_initialize(host.mpv), >=, 0);
    const char *load[] = {"loadfile", source, NULL};
    g_assert_cmpint(mpv_command_async(host.mpv, 0, load), >=, 0);
    wait_loaded(&host);
    const char *seek[] = {"seek", "0.4", "absolute+exact", NULL};
    host.restarted = FALSE;
    g_assert_cmpint(mpv_command_async(host.mpv, 0, seek), >=, 0);
    wait_loaded(&host);
    host.window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(host.window, 640, 350);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_window_set_child(host.window, box);
    GtkWidget *space = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(space, TRUE);
    gtk_box_append(GTK_BOX(box), space);
    host.scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 4, .01);
    gtk_scale_set_draw_value(GTK_SCALE(host.scale), FALSE);
    gtk_box_append(GTK_BOX(box), host.scale);
    host.custom = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(host.custom), 32);
    gtk_box_append(GTK_BOX(box), host.custom);
    host.preview = seek_preview_new(host.mpv);
    seek_preview_attach(host.preview, host.scale, TRUE);
    seek_preview_attach(host.preview, host.custom, FALSE);
    GtkEventController *scale_motion = motion_controller(host.scale);
    GtkEventController *custom_motion = motion_controller(host.custom);
    g_object_unref(scale_motion);
    g_object_unref(custom_motion);
    gtk_widget_set_tooltip_text(host.custom, "Trim timeline");
    seek_preview_set_enabled(host.preview, i18n_timeline_previews());
    gtk_window_present(host.window);
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (!gtk_widget_get_mapped(host.scale) || gtk_widget_get_width(host.scale) < 100) {
        if (g_get_monotonic_time() > deadline) g_error("Preview test window did not map.");
        poll_host(&host);
    }
    hover(host.scale, .25, TRUE);
    wait_clock(&host, host.scale, FALSE);
    g_assert_cmpuint(decoder_threads(), ==, 0); /* Disabled frames never start a decoder. */
    hover(host.scale, .75, FALSE);
    wait_clock(&host, host.scale, TRUE);
    g_assert_cmpuint(call_count(counter), ==, 0);
    assert_playback_unchanged(&host);
    g_assert_true(i18n_save_timeline_previews(TRUE, &error));
    g_assert_true(i18n_init(config, &error));
    g_assert_true(i18n_timeline_previews());
    seek_preview_set_enabled(host.preview, i18n_timeline_previews());
    hover(host.scale, .25, TRUE);
    wait_color(&host, host.scale, FALSE);
    g_assert_cmpuint(call_count(counter), ==, 0);
    assert_playback_unchanged(&host);
    hover(host.scale, .75, FALSE);
    wait_color(&host, host.scale, TRUE);
    assert_playback_unchanged(&host);
    g_assert_cmpuint(call_count(counter), ==, 0);
    gint64 cached_started = g_get_monotonic_time();
    hover(host.scale, .25, FALSE);
    wait_color(&host, host.scale, FALSE);
    double cached_ms = (g_get_monotonic_time() - cached_started) / 1000.0;
    g_print("Cached hover ready in %.3f ms.\n", cached_ms);
    pump_for(&host, 200000);
    g_assert_cmpuint(call_count(counter), ==, 0);
    /* Change targets while the earlier request's exact refinement can still
     * be pending. A late frame must not replace the most recent red target. */
    hover(host.scale, .70, FALSE);
    pump_for(&host, 70000);
    hover(host.scale, .30, FALSE);
    wait_color(&host, host.scale, FALSE);
    pump_for(&host, 250000);
    g_assert_true(has_color(preview_texture(host.scale), FALSE));
    assert_playback_unchanged(&host);
    g_assert_cmpuint(call_count(counter), ==, 0);
    GtkWidget *popover = find_widget(host.scale, GTK_TYPE_POPOVER);
    g_assert_nonnull(popover);
    check_vertical_bounds(&host, host.scale, popover, .40);
    pump_for(&host, 100000);
    g_assert_true(card_allocated(host.scale));
    save_card(&host, popover);
    leave(host.scale);
    wait_hidden(&host, popover);
    hover(host.custom, .75, TRUE);
    wait_color(&host, host.custom, TRUE);
    g_assert_false(gtk_widget_get_has_tooltip(host.custom));
    g_assert_true(find_widget(host.custom, GTK_TYPE_POPOVER) == popover);
    assert_playback_unchanged(&host);
    check_vertical_bounds(&host, host.custom, popover, .90);
    hover(host.custom, .75, FALSE);
    wait_color(&host, host.custom, TRUE);
    seek_preview_set_enabled(host.preview, FALSE);
    wait_clock(&host, host.custom, TRUE); /* Changing the preference updates a visible card. */
    g_assert_false(gtk_widget_get_has_tooltip(host.custom));
    hover(host.custom, .25, TRUE);
    pump_for(&host, 250000);
    wait_clock(&host, host.custom, FALSE);
    g_assert_cmpuint(call_count(counter), ==, 0);
    assert_playback_unchanged(&host);
    leave(host.custom);
    wait_hidden(&host, popover);
    g_assert_true(gtk_widget_get_has_tooltip(host.custom));
    seek_preview_set_enabled(host.preview, TRUE);
    /* The persistent provider first supplies a keyframe and refines after
     * the target settles. Reload while that work can still be pending. */
    hover(host.scale, .40, TRUE);
    pump_for(&host, 80000);
    assert_playback_unchanged(&host);
    g_assert_cmpint(mpv_command_async(host.mpv, 0, load), >=, 0);
    host.loaded = host.restarted = FALSE;
    wait_loaded(&host);
    pump_for(&host, 450000);
    wait_hidden(&host, popover);
    g_assert_null(preview_texture(host.scale));
    g_assert_cmpuint(call_count(counter), ==, 0);
    g_assert_cmpint(mpv_get_property(host.mpv, "time-pos", MPV_FORMAT_DOUBLE, &host.playback_position), >=, 0);
    hover(host.custom, .62, TRUE);
    pump_for(&host, 80000);
    seek_preview_set_enabled(host.preview, FALSE);
    wait_clock(&host, host.custom, TRUE);
    pump_for(&host, 200000);
    g_assert_null(preview_texture(host.custom));
    wait_clock(&host, host.custom, TRUE); /* A late frame cannot replace the clock. */
    g_assert_false(gtk_widget_get_has_tooltip(host.custom));
    assert_playback_unchanged(&host);
    g_assert_cmpuint(call_count(counter), ==, 0);
    const char *load_audio[] = {"loadfile", audio, "replace", NULL};
    host.loaded = host.restarted = FALSE;
    g_assert_cmpint(mpv_command_async(host.mpv, 0, load_audio), >=, 0);
    wait_loaded(&host);
    g_assert_cmpint(mpv_get_property(host.mpv, "time-pos", MPV_FORMAT_DOUBLE, &host.playback_position), >=, 0);
    hover(host.custom, .75, TRUE);
    wait_clock(&host, host.custom, TRUE);
    assert_playback_unchanged(&host);
    seek_preview_set_enabled(host.preview, TRUE);
    wait_clock(&host, host.custom, TRUE); /* Audio keeps time even without a video track. */
    hover(host.custom, .80, TRUE);
    pump_for(&host, 80000);
    g_autoptr(GtkEventController) stopped_motion = motion_controller(host.custom);
    seek_preview_shutdown(host.preview);
    wait_hidden(&host, popover);
    g_signal_emit_by_name(stopped_motion, "enter", gtk_widget_get_width(host.custom) * .75, 16.0);
    pump_for(&host, 450000);
    g_assert_cmpuint(call_count(counter), ==, 0);
    g_assert_null(preview_texture(host.custom));
    assert_playback_unchanged(&host);
    gtk_window_destroy(host.window);
    seek_preview_free(host.preview);
    host.preview = NULL;
    /* Deliver any already queued worker completions after the view is gone. */
    pump_for(&host, 250000);
    mpv_terminate_destroy(host.mpv);
    if (path) g_setenv("PATH", path, TRUE); else g_unsetenv("PATH");
    g_assert_no_error(error);
    remove_tree(directory);
    puts("PASS persistent hover previews: disabled frames keep compact clock without a decoder, runtime toggles, decoded frames, shared cache, unchanged playback, cancellation and shutdown.");
    return 0;
}
