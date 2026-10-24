/* Test-only native Wayland compositor: no X11 and no application changes. */
#define _POSIX_C_SOURCE 200809L
#define WLR_USE_UNSTABLE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cairo.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

struct server {
    struct wl_display *display;
    struct wlr_renderer *renderer;
    struct wlr_scene *scene;
    struct wlr_scene_output *scene_output;
    struct wl_listener new_toplevel, frame, commit;
    struct wl_event_source *timer;
    pid_t child;
    unsigned total_toplevels, mapped_toplevels, video_frames;
    int child_status;
    bool child_exited, timed_out, screenshot;
    struct timespec started;
    const char *png;
};

struct view {
    struct server *server;
    struct wlr_xdg_toplevel *toplevel;
    struct wl_listener commit, map, destroy;
};

static int unsupported(const char *stage)
{
    fprintf(stderr, "SKIP headless Wayland: %s (%s)\n", stage, strerror(errno));
    return 77;
}

static void configure(struct wl_listener *listener, void *data)
{
    (void)data;
    struct view *view = wl_container_of(listener, view, commit);
    if (view->toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(view->toplevel, 1080, 720);
        wlr_xdg_toplevel_set_activated(view->toplevel, true);
    }
}

static void mapped(struct wl_listener *listener, void *data)
{
    (void)data;
    struct view *view = wl_container_of(listener, view, map);
    view->server->mapped_toplevels++;
}

static void destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    struct view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->destroy.link);
    free(view);
}

static void new_toplevel(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;
    server->total_toplevels++;
    struct view *view = calloc(1, sizeof(*view));
    assert(view);
    view->server = server;
    view->toplevel = toplevel;
    wlr_scene_xdg_surface_create(&server->scene->tree, toplevel->base);
    view->commit.notify = configure;
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    view->map.notify = mapped;
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    view->destroy.notify = destroyed;
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
}

static void output_commit(struct wl_listener *listener, void *data)
{
    struct server *server = wl_container_of(listener, server, commit);
    struct wlr_output_event_commit *event = data;
    if (!event->state->buffer || !server->mapped_toplevels)
        return;
    pixman_image_t *image = wlr_pixman_renderer_get_buffer_image(
        server->renderer, event->state->buffer);
    if (!image)
        return;
    int width = pixman_image_get_width(image), height = pixman_image_get_height(image);
    int stride = pixman_image_get_stride(image);
    uint8_t *pixels = (uint8_t *)pixman_image_get_data(image);
    unsigned saturated = 0;
    /* The video occupies the middle of the 1080x720 application window.
     * Theme backgrounds and the small empty-player icon cannot satisfy this. */
    for (int y = 180; y < 540 && y < height; y++) {
        const uint32_t *row = (const uint32_t *)(pixels + y * stride);
        for (int x = 250; x < 830 && x < width; x++) {
            int red = (row[x] >> 16) & 255, green = (row[x] >> 8) & 255, blue = row[x] & 255;
            if ((red > 90 || green > 90 || blue > 90) &&
                (abs(red - green) > 70 || abs(green - blue) > 70))
                saturated++;
        }
    }
    if (saturated < 12000)
        return;
    server->video_frames++;
    if (!server->screenshot) {
        cairo_surface_t *surface = cairo_image_surface_create_for_data(
            pixels, CAIRO_FORMAT_RGB24, width, height, stride);
        server->screenshot = cairo_surface_write_to_png(surface, server->png) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(surface);
    }
}

static void output_frame(struct wl_listener *listener, void *data)
{
    (void)data;
    struct server *server = wl_container_of(listener, server, frame);
    wlr_scene_output_commit(server->scene_output, NULL);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(server->scene_output, &now);
}

static int poll_child(void *data)
{
    struct server *server = data;
    pid_t result = waitpid(server->child, &server->child_status, WNOHANG);
    if (result == server->child) {
        server->child_exited = true;
        wl_display_terminate(server->display);
        return 0;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - server->started.tv_sec > 16) {
        server->timed_out = true;
        wl_display_terminate(server->display);
        return 0;
    }
    wl_event_source_timer_update(server->timer, 50);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: headless-wayland PLAYER CLIP SCREENSHOT.png\n");
        return 2;
    }
    struct server server = {.png = argv[3]};
    wlr_log_init(WLR_ERROR, NULL);
    server.display = wl_display_create();
    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    server.renderer = wlr_pixman_renderer_create();
    if (!backend || !server.renderer || !wlr_renderer_init_wl_display(server.renderer, server.display))
        return unsupported("headless backend or Pixman display initialization");
    struct wlr_allocator *allocator = wlr_allocator_autocreate(backend, server.renderer);
    if (!allocator)
        return unsupported("Pixman output allocator");
    wlr_compositor_create(server.display, 6, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);
    wlr_viewporter_create(server.display);
    wlr_seat_create(server.display, "headless-test");
    struct wlr_xdg_shell *shell = wlr_xdg_shell_create(server.display, 3);
    server.scene = wlr_scene_create();
    server.new_toplevel.notify = new_toplevel;
    wl_signal_add(&shell->events.new_toplevel, &server.new_toplevel);
    struct wlr_output *output = wlr_headless_add_output(backend, 1280, 900);
    if (!output || !wlr_output_init_render(output, allocator, server.renderer))
        return unsupported("headless output render initialization");
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, 1280, 900, 60000);
    if (!wlr_output_commit_state(output, &state))
        return unsupported("headless output mode commit");
    wlr_output_state_finish(&state);
    struct wlr_output_layout *layout = wlr_output_layout_create(server.display);
    struct wlr_output_layout_output *layout_output = wlr_output_layout_add_auto(layout, output);
    server.scene_output = wlr_scene_output_create(server.scene, output);
    struct wlr_scene_output_layout *scene_layout = wlr_scene_attach_output_layout(server.scene, layout);
    wlr_scene_output_layout_add_output(scene_layout, layout_output, server.scene_output);
    server.frame.notify = output_frame;
    wl_signal_add(&output->events.frame, &server.frame);
    server.commit.notify = output_commit;
    wl_signal_add(&output->events.commit, &server.commit);
    if (!wlr_backend_start(backend))
        return unsupported("headless backend start");
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0)
        return unsupported("Wayland client socketpair");
    server.child = fork();
    if (server.child < 0)
        return unsupported("player subprocess fork");
    if (!server.child) {
        close(sockets[0]);
        if (sockets[1] != 3) { dup2(sockets[1], 3); close(sockets[1]); }
        fcntl(3, F_SETFD, 0);
        setenv("WAYLAND_SOCKET", "3", 1);
        unsetenv("WAYLAND_DISPLAY");
        unsetenv("DISPLAY");
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("GSK_RENDERER", "gl", 1);
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 1);
        execl(argv[1], argv[1], "--self-test=6", argv[2], (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    if (!wl_client_create(server.display, sockets[0])) {
        int saved_errno = errno;
        kill(server.child, SIGTERM);
        waitpid(server.child, &server.child_status, 0);
        errno = saved_errno;
        return unsupported("Wayland client attachment");
    }
    clock_gettime(CLOCK_MONOTONIC, &server.started);
    server.timer = wl_event_loop_add_timer(loop, poll_child, &server);
    wl_event_source_timer_update(server.timer, 50);
    while (!server.child_exited && !server.timed_out) {
        wl_event_loop_dispatch(loop, 50);
        wl_display_flush_clients(server.display);
    }
    if (!server.child_exited) { kill(server.child, SIGTERM); waitpid(server.child, &server.child_status, 0); }
    wl_display_destroy_clients(server.display);
    wl_display_destroy(server.display);
    if (!WIFEXITED(server.child_status) || WEXITSTATUS(server.child_status) != 0)
        return 1;
    if (server.total_toplevels != 1 || !server.mapped_toplevels || !server.video_frames || !server.screenshot) {
        fprintf(stderr, "No proof: toplevels=%u mapped=%u video frames=%u screenshot=%d\n",
                server.total_toplevels, server.mapped_toplevels, server.video_frames, server.screenshot);
        return server.total_toplevels > 1 ? 1 : 77;
    }
    printf("PASS native Wayland GUI: one xdg_toplevel, %u composited video frames; %s\n",
           server.video_frames, server.png);
    return 0;
}
