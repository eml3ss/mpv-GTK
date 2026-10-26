/* Generate desktop artwork without a display, user media, or shared cache. */
#define _GNU_SOURCE
#include "../src/mpris_art.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    mpv_handle *mpv;
    MprisArt *art;
    char *uri, *icon_uri;
    gboolean loaded, restarted, live;
    guint callbacks;
} ArtHost;

static void artwork(const char *uri, gpointer data)
{
    ArtHost *host = data;
    g_assert_true(host->live);
    g_free(host->uri); host->uri = g_strdup(uri);
    if (uri && g_str_has_suffix(uri, ".svg")) {
        g_free(host->icon_uri); host->icon_uri = g_strdup(uri);
    }
    host->callbacks++;
}

static void pump(ArtHost *host, gint64 span)
{
    gint64 until = g_get_monotonic_time() + span;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        if (host->mpv) {
            mpv_event *event = mpv_wait_event(host->mpv, .002);
            if (event->event_id == MPV_EVENT_START_FILE) host->loaded = host->restarted = FALSE;
            else if (event->event_id == MPV_EVENT_FILE_LOADED) host->loaded = TRUE;
            else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) host->restarted = TRUE;
            else if (event->event_id == MPV_EVENT_END_FILE) host->loaded = FALSE;
            if (host->art) mpris_art_handle_event(host->art, event);
        } else g_usleep(1000);
    } while (g_get_monotonic_time() < until);
}

static void load(ArtHost *host, const char *filename)
{
    host->loaded = host->restarted = FALSE;
    const char *arguments[] = {"loadfile", filename, "replace", NULL};
    g_assert_cmpint(mpv_command_async(host->mpv, 0, arguments), >=, 0);
}

static void wait_loaded(ArtHost *host)
{
    gint64 deadline = g_get_monotonic_time() + 4000000;
    while (!host->loaded || !host->restarted) {
        if (g_get_monotonic_time() > deadline) g_error("Artwork fixture failed to load.");
        pump(host, 1000);
    }
}

static char *local_art_file(ArtHost *host)
{
    if (!host->uri) return NULL;
    char *filename = g_filename_from_uri(host->uri, NULL, NULL);
    if (filename && !g_file_test(filename, G_FILE_TEST_IS_REGULAR)) g_clear_pointer(&filename, g_free);
    return filename;
}

static gboolean color(ArtHost *host, gboolean red)
{
    g_autofree char *filename = local_art_file(host);
    if (!filename || !g_str_has_suffix(filename, ".png")) return FALSE;
    g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_file(filename, NULL);
    if (!pixbuf) return FALSE;
    int width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
    g_assert_cmpint(width, >, 0); g_assert_cmpint(height, >, 0);
    g_assert_cmpint(width, <=, 1024); g_assert_cmpint(height, <=, 1024);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const guchar *pixel = gdk_pixbuf_read_pixels(pixbuf) +
        gdk_pixbuf_get_rowstride(pixbuf) * (height / 2) + channels * (width / 2);
    return red ? pixel[0] > 180 && pixel[1] < 70 && pixel[2] < 70
        : pixel[2] > 180 && pixel[0] < 70 && pixel[1] < 70;
}

static void assert_quality(ArtHost *host, gboolean red)
{
    g_autofree char *filename = local_art_file(host);
    g_autoptr(GdkPixbuf) image = gdk_pixbuf_new_from_file(filename, NULL);
    g_assert_nonnull(image);
    int width = gdk_pixbuf_get_width(image), height = gdk_pixbuf_get_height(image);
    /* Real source detail must survive; enlarging an old 160x90 thumbnail
     * could satisfy dimensions alone but loses these four-pixel stripes. */
    g_assert_cmpint(width, ==, red ? 768 : 1024);
    g_assert_cmpint(height, ==, red ? 768 : 576);
    g_assert_cmpint(width, >, 160); g_assert_cmpint(height, >, 90);
    double scale = (double)width / (red ? 768 : 1280);
    int y = (int)lround((100.5 * scale) - .5);
    int channels = gdk_pixbuf_get_n_channels(image);
    const guchar *pixels = gdk_pixbuf_read_pixels(image);
    int stride = gdk_pixbuf_get_rowstride(image);
    for (int stripe = 2; stripe < 26; ++stripe) {
        int x = (int)lround(((40 + stripe * 4 + 2.5) * scale) - .5);
        const guchar *sample = pixels + y * stride + x * channels;
        int brightness = (sample[0] + sample[1] + sample[2]) / 3;
        if (stripe % 2) g_assert_cmpint(brightness, <, 70);
        else g_assert_cmpint(brightness, >, 180);
    }
}

static void wait_color(ArtHost *host, gboolean red)
{
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!color(host, red)) {
        if (g_get_monotonic_time() > deadline)
            g_error("No %s artwork frame: %s", red ? "red cover" : "blue video", host->uri ? host->uri : "none");
        pump(host, 1000);
    }
    assert_quality(host, red);
}

static char *wait_icon(ArtHost *host, gboolean current)
{
    gint64 deadline = g_get_monotonic_time() + 4000000;
    for (;;) {
        const char *uri = current ? host->uri : host->icon_uri;
        char *filename = uri ? g_filename_from_uri(uri, NULL, NULL) : NULL;
        if (filename && g_str_has_suffix(filename, ".svg")) {
            g_autofree char *contents = NULL;
            g_assert_true(g_file_get_contents(filename, &contents, NULL, NULL));
            g_assert_nonnull(strstr(contents, "<svg"));
            return filename;
        }
        g_free(filename);
        if (g_get_monotonic_time() > deadline) g_error("Artwork fallback icon was not published.");
        pump(host, 1000);
    }
}

static void private_file(const char *filename, int mode)
{
    GStatBuf info;
    g_assert_cmpint(g_stat(filename, &info), ==, 0);
    g_assert_cmpint(info.st_mode & 0777, ==, mode);
}

static void run_ffmpeg(char **arguments)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *diagnostic = NULL;
    int status;
    g_assert_true(g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, &diagnostic, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("Cannot generate artwork fixture: %s; %s", error->message, diagnostic ? diagnostic : "");
}

static void stripe_patch(GdkPixbuf *image)
{
    guchar *pixels = gdk_pixbuf_get_pixels(image);
    int stride = gdk_pixbuf_get_rowstride(image), channels = gdk_pixbuf_get_n_channels(image);
    for (int y = 40; y < 168; ++y) {
        for (int x = 40; x < 296; ++x) {
            guchar value = ((x - 40) / 4) % 2 ? 0 : 255;
            for (int channel = 0; channel < 3; ++channel)
                pixels[y * stride + x * channels + channel] = value;
        }
    }
}

static void fixtures(const char *video, const char *audio, const char *cover,
                       const char *cover_audio, const char *video_frame)
{
    g_autoptr(GdkPixbuf) image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 768, 768);
    gdk_pixbuf_fill(image, UINT32_C(0xff0000ff));
    stripe_patch(image);
    g_autoptr(GError) error = NULL;
    g_assert_true(gdk_pixbuf_save(image, cover, "png", &error, NULL));
    g_assert_no_error(error);
    g_autoptr(GdkPixbuf) frame = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 1280, 720);
    gdk_pixbuf_fill(frame, UINT32_C(0x0000ffff));
    stripe_patch(frame);
    g_assert_true(gdk_pixbuf_save(frame, video_frame, "png", &error, NULL));
    g_assert_no_error(error);
    char *video_args[] = {"ffmpeg", "-nostdin", "-v", "error", "-loop", "1", "-framerate", "20", "-i",
        (char *)video_frame, "-t", "3", "-c:v", "libx264", "-preset", "ultrafast", "-threads", "1",
        "-g", "20", "-pix_fmt", "yuv420p", (char *)video, NULL};
    run_ffmpeg(video_args);
    char *audio_args[] = {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=48000:duration=3", "-c:a", "flac", (char *)audio, NULL};
    run_ffmpeg(audio_args);
    char *cover_args[] = {"ffmpeg", "-nostdin", "-v", "error", "-i", (char *)audio,
        "-i", (char *)cover, "-map", "0:a", "-map", "1:v", "-c:a", "libmp3lame", "-c:v", "copy",
        "-id3v2_version", "3", "-metadata:s:v", "title=Album cover", "-disposition:v", "attached_pic",
        (char *)cover_audio, NULL};
    run_ffmpeg(cover_args);
}

static void set_option(mpv_handle *mpv, const char *name, const char *value, gboolean optional)
{
    int status = mpv_set_option_string(mpv, name, value);
    g_assert_true(status >= 0 || (optional && status == MPV_ERROR_OPTION_NOT_FOUND));
}

static mpv_handle *new_core(void)
{
    mpv_handle *mpv = mpv_create();
    g_assert_nonnull(mpv);
    const char *pairs[][2] = {{"config", "no"}, {"vo", "null"}, {"ao", "null"}, {"hwdec", "no"},
        {"idle", "yes"}, {"pause", "yes"}, {"terminal", "no"}, {"load-scripts", "no"}};
    for (guint i = 0; i < G_N_ELEMENTS(pairs); ++i) set_option(mpv, pairs[i][0], pairs[i][1], FALSE);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console", "load-auto-profiles",
        "load-select", "load-positioning", "load-commands", "load-context-menu"};
    for (guint i = 0; i < G_N_ELEMENTS(scripts); ++i) set_option(mpv, scripts[i], "no", TRUE);
    g_assert_cmpint(mpv_initialize(mpv), >=, 0);
    return mpv;
}

static void playback_unchanged(ArtHost *host)
{
    int paused = FALSE;
    double position = -1;
    g_assert_cmpint(mpv_get_property(host->mpv, "pause", MPV_FORMAT_FLAG, &paused), >=, 0);
    g_assert_true(paused);
    g_assert_cmpint(mpv_get_property(host->mpv, "time-pos", MPV_FORMAT_DOUBLE, &position), >=, 0);
    g_assert_cmpfloat(position, <, .15);
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
    setlocale(LC_NUMERIC, "C");
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    if (!ffmpeg) { puts("SKIP: FFmpeg is unavailable."); return 77; }
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-mpris-art-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *cache = g_build_filename(directory, "cache", NULL);
    g_setenv("XDG_CACHE_HOME", cache, TRUE);
    g_setenv("XDG_RUNTIME_DIR", directory, TRUE);
    g_autofree char *video = g_build_filename(directory, "Blue video.mkv", NULL);
    g_autofree char *audio = g_build_filename(directory, "Audio.flac", NULL);
    g_autofree char *cover = g_build_filename(directory, "Red cover.png", NULL);
    g_autofree char *cover_audio = g_build_filename(directory, "Covered audio.mp3", NULL);
    g_autofree char *video_frame = g_build_filename(directory, "Detailed video frame.png", NULL);
    fixtures(video, audio, cover, cover_audio, video_frame);
    ArtHost host = {.mpv = new_core(), .live = TRUE};
    load(&host, video);
    wait_loaded(&host);
    /* Production defers desktop integration until media is already visible. */
    host.art = mpris_art_new(host.mpv, artwork, &host);
    g_autofree char *icon = wait_icon(&host, FALSE);
    private_file(icon, 0600);
    g_autofree char *instance = g_path_get_dirname(icon);
    g_assert_true(g_str_has_prefix(instance, cache));
    private_file(instance, 0700);
    wait_color(&host, FALSE);
    g_autofree char *video_file = local_art_file(&host);
    private_file(video_file, 0600);
    playback_unchanged(&host);
    load(&host, cover_audio);
    wait_loaded(&host);
    wait_color(&host, TRUE);
    g_autofree char *cover_file = local_art_file(&host);
    private_file(cover_file, 0600);
    g_assert_cmpstr(cover_file, !=, video_file);
    playback_unchanged(&host);
    load(&host, audio);
    wait_loaded(&host);
    g_autofree char *audio_icon = wait_icon(&host, TRUE);
    g_assert_cmpstr(audio_icon, ==, icon);
    pump(&host, 250000);
    g_assert_false(color(&host, TRUE));
    g_assert_false(color(&host, FALSE));
    playback_unchanged(&host);
    /* A superseded video's asynchronous result must not replace the cover. */
    load(&host, video);
    load(&host, cover_audio);
    wait_loaded(&host);
    wait_color(&host, TRUE);
    pump(&host, 250000);
    g_assert_true(color(&host, TRUE));
    mpris_art_free(host.art); host.art = NULL; host.live = FALSE;
    gint64 deadline = g_get_monotonic_time() + 4000000;
    while (g_file_test(instance, G_FILE_TEST_EXISTS)) {
        if (g_get_monotonic_time() > deadline) g_error("Artwork cache was not removed after shutdown.");
        pump(&host, 10000);
    }
    /* Dispose while a new query/decode/write can still be pending. */
    host.live = TRUE;
    host.art = mpris_art_new(host.mpv, artwork, &host);
    mpris_art_free(host.art); host.art = NULL; host.live = FALSE;
    mpv_terminate_destroy(host.mpv); host.mpv = NULL;
    pump(&host, 200000);
    g_free(host.uri); g_free(host.icon_uri);
    remove_tree(directory);
    puts("PASS MPRIS artwork: detailed 1024x576 video/768x768 cover PNGs, fallback, private cache, source replacement and safe teardown.");
    return 0;
}
