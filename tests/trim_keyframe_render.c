/* Exercise Trim keyframe controls while the real libmpv Render API has a
 * pending frame. The software renderer uses the same VO synchronization as
 * OpenGL without creating a window, display connection, or GPU context. */
#define _GNU_SOURCE
#include <mpv/client.h>
#include <mpv/render.h>

static mpv_event *render_wait_event(mpv_handle *mpv, double timeout);
#define mpv_wait_event render_wait_event
#define main trim_keyframe_fixture_main
#include "trim_keyframes.c"
#undef main
#undef mpv_wait_event

static mpv_render_context *software_renderer;
static gboolean pending_frame;
static guint rendered_frames;
static _Alignas(64) guchar software_pixels[16 * 16 * 4];

static void render_pending_frame(void)
{
    if (!software_renderer) return;
    pending_frame |= (mpv_render_context_update(software_renderer) & MPV_RENDER_UPDATE_FRAME) != 0;
    if (!pending_frame) return;
    int size[] = {16, 16}, block = 0;
    size_t stride = 16 * 4;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size}, {MPV_RENDER_PARAM_SW_FORMAT, "rgb0"},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride}, {MPV_RENDER_PARAM_SW_POINTER, software_pixels},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block}, {MPV_RENDER_PARAM_INVALID, NULL}
    };
    g_assert_cmpint(mpv_render_context_render(software_renderer, params), >=, 0);
    pending_frame = FALSE;
    ++rendered_frames;
}

static mpv_event *render_wait_event(mpv_handle *mpv, double timeout)
{
    render_pending_frame();
    return mpv_wait_event(mpv, timeout);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setlocale(LC_NUMERIC, "C");
    g_autofree char *ffmpeg = g_find_program_in_path("ffmpeg");
    g_autofree char *ffprobe = g_find_program_in_path("ffprobe");
    if (!ffmpeg || !ffprobe) { puts("SKIP: FFmpeg tools are unavailable."); return 77; }
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-trim-render-XXXXXX", NULL);
    g_assert_nonnull(directory);
    g_autofree char *filename = g_build_filename(directory, "keyframes.mp4", NULL);
    g_autofree char *config = g_build_filename(directory, "mpv.conf", NULL);
    g_assert_true(g_file_set_contents(config, "", 0, NULL));
    generate_media(filename);
    KeyframeHost host = {0};
    host.mpv = mpv_create();
    g_assert_nonnull(host.mpv);
    const char *options[][2] = {{"config", "no"}, {"vo", "libmpv"}, {"ao", "null"},
        {"hwdec", "no"}, {"pause", "yes"}, {"idle", "yes"}, {"keep-open", "yes"},
        {"terminal", "no"}, {"load-scripts", "no"}, {"untimed", "no"}, {"osd-level", "0"}};
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i) set_option(host.mpv, options[i][0], options[i][1], FALSE);
    const char *scripts[] = {"osc", "ytdl", "load-stats-overlay", "load-console", "load-auto-profiles",
        "load-select", "load-positioning", "load-commands", "load-context-menu"};
    for (guint i = 0; i < G_N_ELEMENTS(scripts); ++i) set_option(host.mpv, scripts[i], "no", TRUE);
    g_assert_cmpint(mpv_initialize(host.mpv), >=, 0);
    mpv_render_param create[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW}, {MPV_RENDER_PARAM_INVALID, NULL}
    };
    int status = mpv_render_context_create(&software_renderer, host.mpv, create);
    if (status < 0) {
        g_print("SKIP software Render API: %s\n", mpv_error_string(status));
        mpv_terminate_destroy(host.mpv);
        remove_tree(directory);
        return 77;
    }
    host.backend = trim_backend_new(host.mpv, config);
    load(&host, filename, "8.2");
    trim_backend_set_enabled(host.backend, TRUE);
    await_keyframes(&host);
    paused(&host, FALSE);
    pump(&host, 100000);
    double longest_ms = 0;
    for (int iteration = 0; iteration < 3; ++iteration) {
        gint64 deadline = g_get_monotonic_time() + 2000000;
        while (!(mpv_render_context_update(software_renderer) & MPV_RENDER_UPDATE_FRAME)) {
            if (g_get_monotonic_time() > deadline) g_error("No pending software-rendered frame.");
            g_usleep(1000);
        }
        pending_frame = TRUE;
        g_usleep(10000);
        const char *previous[] = {"previous"};
        /* Queue one jump without drawing, let its deferred dispatch run, and
         * click again while the core may be waiting for its previous frame.
         * A synchronous property read here can stall for VO timeouts. */
        trim_backend_action(host.backend, "keyframe", 1, previous);
        g_usleep(22000);
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(10000);
        gint64 began = g_get_monotonic_time();
        trim_backend_action(host.backend, "keyframe", 1, previous);
        double elapsed_ms = (g_get_monotonic_time() - began) / 1000.0;
        longest_ms = MAX(longest_ms, elapsed_ms);
        g_assert_cmpfloat(elapsed_ms, <, 150);
        guint frames_before = rendered_frames;
        pump(&host, 180000);
        g_assert_true(host.loaded);
        g_assert_cmpuint(rendered_frames, >, frames_before);
    }
    trim_backend_free(host.backend); host.backend = NULL;
    render_pending_frame();
    mpv_render_context_free(software_renderer); software_renderer = NULL;
    mpv_terminate_destroy(host.mpv);
    remove_tree(directory);
    g_print("PASS Render API keyframes: overlapping clicks below %.3f ms, playback keeps rendering; no GUI or GPU.\n",
            longest_ms);
    return 0;
}
