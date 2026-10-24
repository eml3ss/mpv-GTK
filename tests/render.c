/* Exercise the application's real GL resolver and libmpv renderer with Mesa
 * surfaceless EGL. This covers shaders/FBOs, not GTK, Wayland, or NVIDIA. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <epoxy/egl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>

static void require_render(bool success, const char *operation)
{
    if (!success) {
        g_printerr("FAIL offscreen render: %s (EGL error 0x%x)\n",
                   operation, eglGetError());
        exit(1);
    }
}

static void require_mpv(int result, const char *operation)
{
    if (result < 0) {
        g_printerr("FAIL offscreen render: %s: %s\n",
                   operation, mpv_error_string(result));
        exit(1);
    }
}

static char *render_clip(unsigned fps, unsigned seconds)
{
    char *filename = NULL;
    int fd = g_file_open_tmp("mpv-gtk-render-XXXXXX.mp4", &filename, NULL);
    require_render(fd >= 0, "create temporary clip");
    close(fd);
    g_autofree char *source = g_strdup_printf("testsrc2=size=320x180:rate=%u:duration=%u", fps, seconds);
    char *args[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", source,
        "-c:v", "mpeg4", "-q:v", "4", "-pix_fmt", "yuv420p", "-an",
        filename, NULL
    };
    int status = -1;
    g_autofree char *errors = NULL;
    g_autoptr(GError) error = NULL;
    gboolean spawned = g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH,
                                    NULL, NULL, NULL, &errors, &status, &error);
    if (!spawned || !g_spawn_check_wait_status(status, &error)) {
        g_printerr("ffmpeg: %s%s\n", error ? error->message : "", errors ? errors : "");
        unlink(filename);
        require_render(false, "generate color clip");
    }
    return filename;
}

static bool colorful_frame(uint64_t *checksum)
{
    unsigned char pixels[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    require_render(glGetError() == GL_NO_ERROR, "read rendered RGBA pixels");
    unsigned bright = 0, colored = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < sizeof(pixels); ++i) {
        hash ^= pixels[i];
        hash *= UINT64_C(1099511628211);
    }
    for (unsigned i = 0; i < sizeof(pixels); i += 4) {
        int r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
        if (r > 25 || g > 25 || b > 25)
            bright++;
        if (abs(r - g) > 40 || abs(g - b) > 40 || abs(r - b) > 40)
            colored++;
    }
    *checksum = hash;
    return bright > 256 && colored > 256;
}

static bool opaque_black_frame(void)
{
    unsigned char pixels[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    require_render(glGetError() == GL_NO_ERROR, "read cleared RGBA pixels");
    for (unsigned i = 0; i < sizeof(pixels); i += 4) {
        if (pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0 ||
            pixels[i + 3] != 255)
            return false;
    }
    return true;
}

/* Follow real playback state without constructing GTK widgets. Calling the
 * application's actual render callback below then exercises the same cached
 * framebuffer that GtkGLArea would retain between explicit redraws. */
static bool state_redraw;
static guint media_starts, media_loads, preload_hooks;
static bool cached_binary_loaded;

static void playback_events(Player *player, bool *eof)
{
    for (;;) {
        mpv_event *event = mpv_wait_event(player->mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        require_mpv(event->error, "mpv event");
        switch (event->event_id) {
        case MPV_EVENT_HOOK:
            require_render(event->reply_userdata == RENDER_LOAD_HOOK_ID,
                           "only the registered renderer barrier is dispatched");
            preload_hooks++;
            handle_event(player, event);
            break;
        case MPV_EVENT_START_FILE:
            media_starts++;
            player->timeline.loaded = FALSE;
            player->idle = FALSE;
            state_redraw = true;
            break;
        case MPV_EVENT_FILE_LOADED:
            media_loads++;
            player->timeline.loaded = TRUE;
            player->idle = FALSE;
            state_redraw = true;
            break;
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *end = event->data;
            require_mpv(end->error, "media end");
            player->timeline.loaded = FALSE;
            state_redraw = true;
            if (end->reason == MPV_END_FILE_REASON_EOF)
                *eof = true;
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property *property = event->data;
            if (property->format != MPV_FORMAT_FLAG || !property->data)
                break;
            if (g_str_equal(property->name, "idle-active")) {
                state_redraw |= player->idle != !!*(int *)property->data;
                player->idle = !!*(int *)property->data;
            } else if (g_str_equal(property->name, "pause"))
                player->timeline.paused = !!*(int *)property->data;
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            mpv_event_log_message *log = event->data;
            if (strstr(log->text, "Loading binary program succeeded."))
                cached_binary_loaded = true;
            if (log->log_level <= MPV_LOG_LEVEL_ERROR) {
                g_printerr("libmpv %s: %s", log->prefix, log->text);
                require_render(false, "libmpv reported shader/playback error");
            }
            break;
        }
        default:
            break;
        }
    }
}

static void application_frame(Player *player, GLuint fbo)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, 64, 64);
    require_render(render(NULL, NULL, player), "application render callback");
    require_render(!player->render_failed && glGetError() == GL_NO_ERROR,
                   "application frame has no rendering errors");
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

static bool updated_frame(Player *player, GLuint fbo)
{
    uint64_t updates = mpv_render_context_update(player->renderer);
    /* Production also queues GtkGLArea redraws on FILE_LOADED/END_FILE and
     * idle changes. A renderer update can precede the corresponding client
     * event; service that explicit state redraw even without a new frame. */
    if (!(updates & MPV_RENDER_UPDATE_FRAME) && !state_redraw)
        return false;
    state_redraw = false;
    application_frame(player, fbo);
    return true;
}

static mpv_handle *render_engine(const char *cache_directory, bool debug)
{
    mpv_handle *mpv = mpv_create();
    require_render(mpv != NULL, "create libmpv");
    const char *options[][2] = {
        {"config", "no"}, {"vo", "libmpv"}, {"ao", "null"}, {"hwdec", "no"},
        {"terminal", "no"}, {"load-scripts", "no"}, {"video-timing-offset", "0"},
        {"idle", "yes"}, {"keep-open", "no"}, {"gpu-shader-cache-dir", cache_directory}
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(options); ++i)
        require_mpv(mpv_set_option_string(mpv, options[i][0], options[i][1]), options[i][0]);
    require_mpv(mpv_initialize(mpv), "initialize libmpv");
    require_mpv(mpv_request_log_messages(mpv, debug ? "debug" : "error"), "request renderer logs");
    require_mpv(mpv_hook_add(mpv, RENDER_LOAD_HOOK_ID, "on_preloaded", 0), "register lazy renderer hook");
    require_mpv(mpv_observe_property(mpv, 1, "idle-active", MPV_FORMAT_FLAG), "observe idle state");
    require_mpv(mpv_observe_property(mpv, 2, "pause", MPV_FORMAT_FLAG), "observe pause state");
    return mpv;
}

static guint cached_programs(const char *directory)
{
    g_autoptr(GDir) files = g_dir_open(directory, 0, NULL);
    require_render(files != NULL, "read isolated shader cache");
    guint count = 0;
    const char *name;
    while ((name = g_dir_read_name(files))) {
        g_autofree char *path = g_build_filename(directory, name, NULL);
        g_autofree char *contents = NULL;
        gsize length = 0;
        require_render(g_file_get_contents(path, &contents, &length, NULL), "read cached native shader");
        require_render(g_str_has_prefix(contents, "mpv shader cache v1\n") &&
                       length > strlen("mpv shader cache v1\n"), "cache contains a native program binary");
        count++;
    }
    return count;
}

static void remove_shader_cache(const char *directory)
{
    g_autoptr(GDir) files = g_dir_open(directory, 0, NULL);
    require_render(files != NULL, "read shader cache for cleanup");
    const char *name;
    while ((name = g_dir_read_name(files))) {
        g_autofree char *path = g_build_filename(directory, name, NULL);
        require_render(unlink(path) == 0, "remove isolated shader binary");
    }
    require_render(rmdir(directory) == 0, "remove isolated shader cache directory");
}

typedef struct {
    gboolean loaded, position_known, quit_end, shutdown;
    double position;
    guint render_starvation_logs;
} RenderShutdown;

static void shutdown_render_events(Player *player, RenderShutdown *state)
{
    for (;;) {
        mpv_event *event = mpv_wait_event(player->mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            return;
        require_mpv(event->error, "shutdown event");
        switch (event->event_id) {
        case MPV_EVENT_HOOK:
            handle_event(player, event);
            break;
        case MPV_EVENT_START_FILE:
            player->timeline.loaded = FALSE;
            player->idle = FALSE;
            break;
        case MPV_EVENT_FILE_LOADED:
            player->timeline.loaded = TRUE;
            state->loaded = TRUE;
            state_redraw = true;
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property *property = event->data;
            if (!property->data)
                break;
            if (property->format == MPV_FORMAT_FLAG && g_str_equal(property->name, "pause"))
                player->timeline.paused = !!*(int *)property->data;
            else if (property->format == MPV_FORMAT_DOUBLE && g_str_equal(property->name, "time-pos")) {
                state->position_known = TRUE;
                state->position = *(double *)property->data;
            }
            break;
        }
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *end = event->data;
            require_render(end->reason == MPV_END_FILE_REASON_QUIT && end->error >= 0,
                           "the tested media ends only by native quit");
            state->quit_end = TRUE;
            player->timeline.loaded = FALSE;
            /* Native watch-later saving precedes END_FILE. Consume its final
             * frame without depending on another GTK snapshot being painted. */
            handle_event(player, event);
            require_render(player->quit_requested && player->close_prepared && !player->closing,
                           "final file event prepares close while the native core is serviced");
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            handle_event(player, event);
            require_render(player->shutdown_received && player->closing,
                           "native shutdown acknowledges final host cleanup");
            state->shutdown = TRUE;
            break;
        case MPV_EVENT_LOG_MESSAGE: {
            mpv_event_log_message *log = event->data;
            if (strstr(log->text, "not being called") || strstr(log->text, "or stuck"))
                state->render_starvation_logs++;
            require_render(log->log_level > MPV_LOG_LEVEL_ERROR,
                           "native shutdown has no playback or shader errors");
            break;
        }
        default:
            break;
        }
    }
}

static void shutdown_render_wait(Player *player, RenderShutdown *state)
{
    struct pollfd descriptor = {.fd = player->wake_fd, .events = POLLIN};
    poll(&descriptor, 1, 10);
    uint64_t wakeups;
    while (read(player->wake_fd, &wakeups, sizeof(wakeups)) > 0) {}
    shutdown_render_events(player, state);
}

static void check_render_shutdown(const char *filename, const char *cache_directory,
                                   void *library, GLuint fbo, bool pending, bool use_es)
{
    g_autofree char *watch_directory = g_dir_make_tmp("mpv-gtk-render-watch-XXXXXX", NULL);
    require_render(watch_directory != NULL, "create private render shutdown state");
    mpv_handle *mpv = render_engine(cache_directory, true);
    require_mpv(mpv_set_option_string(mpv, "gpu-hwdec-interop", "no"),
                "isolate software render shutdown from hardware driver probing");
    require_mpv(mpv_set_option_string(mpv, "watch-later-dir", watch_directory),
                "save shutdown position only in the test tree");
    require_mpv(mpv_set_option_string(mpv, "watch-later-options", "start"),
                "isolate the saved playback position");
    require_mpv(mpv_set_option_string(mpv, "pause", "yes"), "start shutdown fixture paused");
    require_mpv(mpv_observe_property(mpv, 3, "time-pos", MPV_FORMAT_DOUBLE), "observe saved position");
    Player player = {.mpv = mpv, .gl_library = library, .ready = TRUE, .idle = TRUE,
        .wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    require_render(player.wake_fd >= 0, "create shutdown notification descriptor");
    mpv_set_wakeup_callback(mpv, wake_main, &player);
    require_render(create_renderer(&player), "create real shutdown render context");
    const char *load[] = {"loadfile", filename, "replace", NULL};
    require_mpv(mpv_command_async(mpv, 1, load), "load real shutdown video");
    RenderShutdown state = {0};
    bool colored = false;
    uint64_t checksum = 0;
    state_redraw = false;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!(state.loaded && player.timeline.paused && colored)) {
        require_render(g_get_monotonic_time() < deadline, "paused shutdown fixture becomes ready");
        shutdown_render_wait(&player, &state);
        if (updated_frame(&player, fbo))
            colored = colorful_frame(&checksum) && player.timeline.loaded;
    }
    state.position_known = FALSE;
    const char *seek[] = {"seek", "1", "absolute+exact", NULL};
    require_mpv(mpv_command_async(mpv, 2, seek), "set a positive saved position");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!state.position_known || fabs(state.position - 1) > .05) {
        require_render(g_get_monotonic_time() < deadline, "shutdown seek completes");
        shutdown_render_wait(&player, &state);
        updated_frame(&player, fbo);
    }
    if (pending) {
        int pause = 0;
        require_mpv(mpv_set_property_async(mpv, 3, "pause", MPV_FORMAT_FLAG, &pause),
                    "queue a real video frame before quitting");
        mpv_render_frame_info info = {0};
        deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (!(info.flags & MPV_RENDER_FRAME_INFO_PRESENT)) {
            require_render(g_get_monotonic_time() < deadline, "native next frame is queued");
            shutdown_render_wait(&player, &state);
            mpv_render_context_update(player.renderer);
            require_mpv(mpv_render_context_get_info(player.renderer,
                (mpv_render_param){MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info}), "inspect queued frame");
        }
    }
    /* Check starvation during shutdown, rather than treating shader compiler
     * or scheduler latency during unrelated first-frame setup as this bug. */
    state.render_starvation_logs = 0;
    const char *quit[] = {"quit-watch-later", NULL};
    require_mpv(mpv_command_async(mpv, 4, quit), "quit with native watch-later saving");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!state.shutdown) {
        require_render(g_get_monotonic_time() < deadline, "cooperative renderer shutdown completes");
        shutdown_render_wait(&player, &state);
    }
    require_render(state.quit_end, "native quit reports its final file event before shutdown");
    /* This is the production pre-free drain. It needs no valid FBO/viewport
     * and must release the VO's queued presentation before disabling callbacks. */
    discard_render_frame(&player);
    mpv_render_frame_info drained = {0};
    require_mpv(mpv_render_context_get_info(player.renderer,
        (mpv_render_param){MPV_RENDER_PARAM_NEXT_FRAME_INFO, &drained}), "inspect final drain");
    require_render(!(drained.flags & MPV_RENDER_FRAME_INFO_PRESENT),
                   "pre-free drain leaves no pending presentation");
    mpv_render_context_set_update_callback(player.renderer, NULL, NULL);
    mpv_render_context_free(player.renderer);
    player.renderer = NULL;
    shutdown_render_events(&player, &state);
    require_render(state.render_starvation_logs == 0,
                   "shutdown does not use mpv's lost-render timeout");
    mpv_set_wakeup_callback(mpv, NULL, NULL);
    mpv_terminate_destroy(mpv);
    close(player.wake_fd);
    g_autofree char *digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, filename, -1);
    g_autofree char *upper = g_ascii_strup(digest, -1);
    g_autofree char *saved_path = g_build_filename(watch_directory, upper, NULL);
    g_autofree char *saved = NULL;
    require_render(g_file_get_contents(saved_path, &saved, NULL, NULL), "native quit saved its position");
    const char *start = strstr(saved, "start=");
    require_render(start != NULL, "native watch-later record contains start");
    double position = g_ascii_strtod(start + strlen("start="), NULL);
    require_render(pending ? position >= .95 && position < 2 : fabs(position - 1) < .05,
                   "discarding presentation does not lose the saved playback position");
    g_autoptr(GDir) files = g_dir_open(watch_directory, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(files))) {
        g_autofree char *path = g_build_filename(watch_directory, name, NULL);
        require_render(unlink(path) == 0, "remove private watch-later entry");
    }
    g_clear_pointer(&files, g_dir_close);
    require_render(rmdir(watch_directory) == 0, "remove private watch-later directory");
    printf("PASS offscreen %s shutdown: %s native quit, consumed presentation, saved position, no lost-render timeout\n",
           use_es ? "GLES" : "GL", pending ? "pending-frame" : "paused");
}

static void render_api(const char *filename, const char *shutdown_filename, bool use_es)
{
    state_redraw = false;
    media_starts = media_loads = preload_hooks = 0;
    cached_binary_loaded = false;
    g_autofree char *cache_directory = g_dir_make_tmp("mpv-gtk-shader-cache-XXXXXX", NULL);
    require_render(cache_directory != NULL, "create isolated shader cache");
    EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                                  EGL_DEFAULT_DISPLAY, NULL);
    require_render(display != EGL_NO_DISPLAY, "create surfaceless EGL display");
    EGLint major, minor;
    require_render(eglInitialize(display, &major, &minor), "initialize EGL");
    require_render(eglBindAPI(use_es ? EGL_OPENGL_ES_API : EGL_OPENGL_API), "bind GL API");

    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, use_es ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint count = 0;
    require_render(eglChooseConfig(display, config_attrs, &config, 1, &count) && count == 1,
                   "choose RGBA8 EGL configuration");
    EGLint desktop_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    EGLint es_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                         use_es ? es_attrs : desktop_attrs);
    require_render(context != EGL_NO_CONTEXT, use_es ? "create GLES 3.0 context" : "create GL 3.3 core context");
    EGLint surface_attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    require_render(surface != EGL_NO_SURFACE, "create offscreen EGL surface");
    require_render(eglMakeCurrent(display, surface, surface, context), "make EGL context current");
    require_render(epoxy_is_desktop_gl() == !use_es, "selected GL API matches request");
    require_render(epoxy_gl_version() >= (use_es ? 30 : 33), "selected GL version meets request");
    bool binary_cache_supported = use_es || epoxy_gl_version() >= 41 ||
        epoxy_has_gl_extension("GL_ARB_get_program_binary");
    GLint binary_formats = 0;
    if (binary_cache_supported) {
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binary_formats);
        require_render(glGetError() == GL_NO_ERROR, "query supported program binary formats");
    }

    printf("Offscreen %s: %s / %s\n", use_es ? "GLES" : "GL",
           glGetString(GL_VERSION), glGetString(GL_RENDERER));

    GLuint texture = 0, fbo = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    require_render(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                   "RGBA8 framebuffer is complete");
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    void *library = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    require_render(library != NULL, "open GL resolver fallback library");
    mpv_handle *mpv = render_engine(cache_directory, false);
    Player player = {.mpv = mpv, .gl_library = library, .wake_fd = -1, .idle = TRUE};
    application_frame(&player, fbo);
    require_render(!player.renderer && opaque_black_frame(),
                   "empty player clears black without allocating a libmpv renderer");
    const char *load[] = {"loadfile", filename, "replace", NULL};
    require_mpv(mpv_command_async(mpv, 1, load), "load generated color clip");

    bool colored = false, eof = false;
    uint64_t checksum = 0;
    gint64 deadline = g_get_monotonic_time() + 10000000;
    while (!player.render_load_hook && g_get_monotonic_time() < deadline) {
        playback_events(&player, &eof);
        if (!player.render_load_hook)
            g_usleep(5000);
    }
    require_render(player.render_load_hook && !player.renderer && !player.timeline.loaded,
                   "load hook waits for GL readiness before video output initialization");
    require_render(media_starts == 1 && media_loads == 0 && preload_hooks == 1,
                   "the initial file starts exactly once and stops at the renderer barrier");
    mpv_node tracks = {0};
    require_mpv(mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &tracks),
                "read demuxed metadata while the renderer barrier is held");
    bool video_known = false;
    if (tracks.format == MPV_FORMAT_NODE_ARRAY && tracks.u.list) {
        for (int i = 0; i < tracks.u.list->num; ++i) {
            const mpv_node *track = &tracks.u.list->values[i];
            video_known |= g_str_equal(node_string(track, "type"), "video") &&
                *node_string(track, "codec") != '\0';
        }
    }
    mpv_free_node_contents(&tracks);
    require_render(video_known, "demuxing and video track discovery finish before a renderer exists");
    char *current_vo = mpv_get_property_string(mpv, "current-vo");
    require_render(current_vo == NULL, "the preloaded barrier precedes native video output creation");
    mpv_free(current_vo);
    deadline = g_get_monotonic_time() + 50000;
    while (g_get_monotonic_time() < deadline) {
        playback_events(&player, &eof);
        require_render(!player.timeline.loaded && !player.renderer && player.render_load_hook,
                       "holding the preloaded hook prevents premature video output initialization");
        g_usleep(5000);
    }
    /* The test owns a surfaceless EGL context in place of GtkGLArea. Use the
     * same context-creation helper, then the actual production hook release. */
    require_render(create_renderer(&player), "lazily create real mpv GL renderer");
    player.ready = TRUE;
    continue_render_load(&player);
    require_render(!player.render_load_hook && player.renderer,
                   "ready renderer releases the exact pending load hook");
    mpv_render_context *renderer = player.renderer;
    deadline = g_get_monotonic_time() + 10000000;
    while (!colored && g_get_monotonic_time() < deadline) {
        playback_events(&player, &eof);
        if (updated_frame(&player, fbo))
            colored = colorful_frame(&checksum) && player.timeline.loaded;
        if (!colored)
            g_usleep(5000);
    }
    require_render(player.timeline.loaded && colored && player.render_calls > 0,
                   "decoded frame produces nonblack color pixels before timeout");
    require_render(media_starts == 1 && media_loads == 1 && preload_hooks == 1,
                   "one initial file produces one successful load and its first real video frame");

    int paused = 1;
    require_mpv(mpv_set_property_async(mpv, 3, "pause", MPV_FORMAT_FLAG, &paused), "pause clip");
    deadline = g_get_monotonic_time() + 2000000;
    while (!player.timeline.paused && g_get_monotonic_time() < deadline) {
        playback_events(&player, &eof);
        updated_frame(&player, fbo);
        if (!player.timeline.paused)
            g_usleep(5000);
    }
    require_render(player.timeline.paused && player.timeline.loaded && !player.idle,
                   "real clip enters paused state");
    application_frame(&player, fbo);
    require_render(colorful_frame(&checksum), "paused redraw preserves the video frame");

    /* Client events and renderer updates are asynchronous: an END_FILE or
     * START_FILE can invalidate the frontend's media while libmpv still holds
     * its last decoded frame. This boundary must clear even that retained
     * image, rather than relying on libmpv's eventual idle redraw. */
    player.timeline.loaded = FALSE;
    application_frame(&player, fbo);
    require_render(opaque_black_frame(), "unloaded media clears a retained libmpv frame");
    player.timeline.loaded = TRUE;
    application_frame(&player, fbo);
    require_render(colorful_frame(&checksum), "loaded paused media can redraw after a clear");

    paused = 0;
    require_mpv(mpv_set_property_async(mpv, 4, "pause", MPV_FORMAT_FLAG, &paused), "resume clip");
    deadline = g_get_monotonic_time() + 10000000;
    while (!(eof && player.idle) && g_get_monotonic_time() < deadline) {
        playback_events(&player, &eof);
        updated_frame(&player, fbo);
        if (!(eof && player.idle))
            g_usleep(5000);
    }
    require_render(eof && player.idle && !player.timeline.loaded, "real clip finishes and enters idle");
    /* GtkGLArea auto-render is disabled. The explicit redraw requested by the
     * idle/END_FILE handler must erase its previous video texture completely. */
    application_frame(&player, fbo);
    require_render(opaque_black_frame(), "EOF redraw clears the cached frame to opaque black");
    application_frame(&player, fbo);
    require_render(opaque_black_frame(), "later idle redraw cannot restore the last video frame");

    colored = false;
    eof = false;
    /* EOF was verified above. Keep the explicit reload's first frame around
     * until the independent renderer/client notifications have both arrived. */
    require_mpv(mpv_set_option_string(mpv, "pause", "yes"), "retain reloaded first frame");
    require_mpv(mpv_command_async(mpv, 5, load), "load clip again after EOF");
    deadline = g_get_monotonic_time() + 10000000;
    while (!colored && g_get_monotonic_time() < deadline) {
        playback_events(&player, &eof);
        if (updated_frame(&player, fbo))
            colored = colorful_frame(&checksum) && player.timeline.loaded && !player.idle;
        if (!colored)
            g_usleep(5000);
    }
    if (!colored)
        g_printerr("Reload state: loaded=%d idle=%d eof=%d hook=%" G_GUINT64_FORMAT
                   " ready=%d render_failed=%d calls=%" G_GUINT64_FORMAT "\n",
                   player.timeline.loaded, player.idle, eof, player.render_load_hook,
                   player.ready, player.render_failed, player.render_calls);
    require_render(colored, "loading after EOF restores video rendering");
    require_render(player.renderer == renderer && media_starts == 2 && media_loads == 2 && preload_hooks == 2,
                   "the explicit second load reuses its renderer without an extra startup load");
    printf("PASS offscreen %s: real 1 fps frame, RGBA8 pixels, checksum %016" PRIx64 "\n",
           use_es ? "GLES" : "GL", checksum);
    printf("PASS offscreen %s: paused frame, opaque EOF clear, reload after EOF\n",
           use_es ? "GLES" : "GL");
    printf("PASS offscreen %s: preloaded metadata before VO, exact load counts, GL barrier and reused renderer\n",
           use_es ? "GLES" : "GL");

    mpv_render_context_free(renderer);
    mpv_terminate_destroy(mpv);
    if (binary_formats > 0) {
        guint programs = cached_programs(cache_directory);
        require_render(programs > 0, "the cold engine persists compiled shader programs");
        mpv_handle *warm_mpv = render_engine(cache_directory, true);
        Player warm = {.mpv = warm_mpv, .gl_library = library, .wake_fd = -1, .ready = TRUE, .idle = TRUE};
        require_render(create_renderer(&warm), "create a fresh renderer for cached programs");
        /* Cache log delivery and rendering are asynchronous. Retain the
         * decoded frame while waiting for both, rather than racing the short
         * fixture's EOF if the test process is temporarily descheduled. */
        require_mpv(mpv_set_option_string(warm_mpv, "pause", "yes"), "retain cached-engine first frame");
        require_mpv(mpv_command_async(warm_mpv, 1, load), "load clip in a fresh cached engine");
        cached_binary_loaded = colored = eof = false;
        state_redraw = false;
        deadline = g_get_monotonic_time() + 10000000;
        while (!(colored && cached_binary_loaded) && g_get_monotonic_time() < deadline) {
            playback_events(&warm, &eof);
            if (updated_frame(&warm, fbo))
                colored = colorful_frame(&checksum) && warm.timeline.loaded;
            if (!(colored && cached_binary_loaded))
                g_usleep(5000);
        }
        if (!(colored && cached_binary_loaded))
            g_printerr("Warm shader state: colored=%d binary=%d loaded=%d idle=%d eof=%d "
                       "hook=%" G_GUINT64_FORMAT " ready=%d calls=%" G_GUINT64_FORMAT "\n",
                       colored, cached_binary_loaded, warm.timeline.loaded, warm.idle, eof,
                       warm.render_load_hook, warm.ready, warm.render_calls);
        require_render(colored && cached_binary_loaded,
                       "a fresh engine loads a real cached program and produces color video");
        mpv_render_context_free(warm.renderer);
        mpv_terminate_destroy(warm_mpv);
        printf("PASS offscreen %s shader cache: %u native binaries persisted, fresh-engine binary load and color frame\n",
               use_es ? "GLES" : "GL", programs);
    } else {
        printf("SKIP offscreen %s shader-cache subset: the driver exposes no program binary formats\n",
               use_es ? "GLES" : "GL");
    }
    check_render_shutdown(shutdown_filename, cache_directory, library, fbo, false, use_es);
    check_render_shutdown(shutdown_filename, cache_directory, library, fbo, true, use_es);
    remove_shader_cache(cache_directory);
    dlclose(library);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    require_render(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT),
                   "release EGL context");
    require_render(eglDestroySurface(display, surface), "destroy EGL surface");
    require_render(eglDestroyContext(display, context), "destroy EGL context");
    require_render(eglTerminate(display), "terminate EGL display");
}

static void failed_render_hook(const char *filename)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-render-hook-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *config = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input = g_build_filename(directory, "input.conf", NULL);
    g_autofree char *previous_config = g_strdup(g_getenv("MPV_GTK_CONFIG_FILE"));
    g_autofree char *cache_directory = g_build_filename(directory, "shaders", NULL);
    g_autofree char *configuration = g_strdup_printf(
        "vo=libmpv\nhwdec=no\nforce-window=yes\ngpu-shader-cache-dir=%s\n", cache_directory);
    g_assert_true(g_file_set_contents(config, configuration, -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_setenv("MPV_GTK_CONFIG_FILE", config, TRUE));
    Player player = {.wake_fd = -1, .test_seconds = 1};
    require_render(init_mpv(&player), "initialize actual host with lazy renderer hook");
    mpv_handle *mpv = player.mpv;
    require_mpv(mpv_get_property_async(mpv, 900, "force-window", MPV_FORMAT_STRING),
                "read effective frontend force-window option");
    gboolean checked_window = FALSE;
    gint64 property_deadline = g_get_monotonic_time() + 1000000;
    while (!checked_window && g_get_monotonic_time() < property_deadline) {
        mpv_event *event = mpv_wait_event(mpv, .01);
        if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata == 900) {
            require_mpv(event->error, "read effective frontend force-window option");
            mpv_event_property *property = event->data;
            require_render(property->format == MPV_FORMAT_STRING && property->data &&
                           g_strcmp0(*(char **)property->data, "no") == 0,
                           "GTK owns the window even when mpv.conf requests an early video output");
            checked_window = TRUE;
        }
    }
    require_render(checked_window, "frontend force-window option reply arrives");
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        /* A failed GTK GL realization and a close before realization both
         * have to release a pending hook instead of leaving the core stuck. */
        player.render_failed = attempt == 0;
        player.closing = attempt == 1;
        gboolean hook_seen = FALSE, failed = FALSE, replied = FALSE;
        const char *load[] = {"loadfile", filename, "replace", NULL};
        require_mpv(mpv_command_async(mpv, 1, load), "load with unavailable renderer");
        gint64 deadline = g_get_monotonic_time() + 5000000;
        while (!failed && g_get_monotonic_time() < deadline) {
            mpv_event *event = mpv_wait_event(mpv, .01);
            if (event->event_id == MPV_EVENT_HOOK) {
                handle_event(&player, event);
                hook_seen = TRUE;
                require_render(!player.render_load_hook && !player.renderer,
                               "failure and shutdown release the load hook without a renderer");
            } else if (event->event_id == MPV_EVENT_END_FILE) {
                mpv_event_end_file *end = event->data;
                failed = end->reason == MPV_END_FILE_REASON_ERROR && end->error < 0;
            }
        }
        require_render(hook_seen && failed, "failed renderer reaches a terminal media error before timeout");
        require_mpv(mpv_get_property_async(mpv, 2, "volume", MPV_FORMAT_DOUBLE),
                    "query core after renderer failure");
        deadline = g_get_monotonic_time() + 1000000;
        while (!replied && g_get_monotonic_time() < deadline) {
            mpv_event *event = mpv_wait_event(mpv, .01);
            if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata == 2) {
                require_mpv(event->error, "core remains responsive after released hook");
                replied = TRUE;
            }
        }
        require_render(replied, "core accepts asynchronous requests after renderer failure");
    }
    mpv_set_wakeup_callback(mpv, NULL, NULL);
    mpv_terminate_destroy(mpv);
    close(player.wake_fd);
    dlclose(player.gl_library);
    config_clear(&player.config);
    require_render(unlink(config) == 0 && unlink(input) == 0 && rmdir(directory) == 0,
                   "remove isolated render hook configuration");
    if (previous_config)
        g_setenv("MPV_GTK_CONFIG_FILE", previous_config, TRUE);
    else
        g_unsetenv("MPV_GTK_CONFIG_FILE");
    puts("PASS lazy renderer hook: GL failure and close release the barrier; core remains responsive");
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    require_render(g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", TRUE), "select software rendering for test");
    require_render(g_setenv("MESA_GL_VERSION_OVERRIDE", "3.3", TRUE), "test desktop GL 3.3 shader path");
    require_render(g_setenv("MESA_GLES_VERSION_OVERRIDE", "3.0", TRUE), "test GLES 3.0 shader path");
    require_render(g_setenv("MESA_SHADER_CACHE_DISABLE", "true", TRUE), "isolate the driver cache from user data");
    g_autofree char *filename = render_clip(1, 2);
    g_autofree char *shutdown_filename = render_clip(10, 4);
    render_api(filename, shutdown_filename, false);
    render_api(filename, shutdown_filename, true);
    failed_render_hook(filename);
    require_render(unlink(filename) == 0, "remove temporary clip");
    require_render(unlink(shutdown_filename) == 0, "remove temporary shutdown clip");
    puts("GTK/Wayland context creation and NVIDIA rendering were not tested.");
    return 0;
}
