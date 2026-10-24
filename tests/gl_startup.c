/* Observe the real pre-GTK stock engine at main()'s GTK boundary, then fail
 * display initialization deliberately, including with early preloaded media.
 * No real display, GTK window or render context is created. */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <mpv/client.h>
#include <sys/eventfd.h>
#include <unistd.h>

static gboolean test_gtk_init_check(void);
static int observed_initialize(mpv_handle *handle);
static void observed_destroy(mpv_handle *handle);
static int observed_eventfd(unsigned int value, int flags);
static int observed_close(int fd);
#define gtk_init_check test_gtk_init_check
#define mpv_initialize observed_initialize
#define mpv_terminate_destroy observed_destroy
#define eventfd observed_eventfd
#define close observed_close
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main
#undef close
#undef eventfd
#undef mpv_terminate_destroy
#undef mpv_initialize
#undef gtk_init_check

#include <glib/gstdio.h>

static mpv_handle *initialized_handle;
static const char *expected_api, *expected_es;
static const char *expected_disable, *expected_renderer;
static guint engine_initializations, engine_destructions, gtk_initializations, diagnostic_calls;
static guint wake_closes;
static int wake_fd = -1;
static const char *startup_media;
static guint early_preload_checks;

static int observed_eventfd(unsigned int value, int flags)
{
    g_assert_cmpint(wake_fd, ==, -1);
    wake_fd = eventfd(value, flags);
    return wake_fd;
}

static int observed_close(int fd)
{
    if (fd == wake_fd) {
        wake_closes++;
        wake_fd = -1;
    }
    return close(fd);
}

static int observed_initialize(mpv_handle *handle)
{
    int result = mpv_initialize(handle);
    if (result >= 0) {
        g_assert_null(initialized_handle);
        initialized_handle = handle;
        engine_initializations++;
    }
    return result;
}

static void observed_destroy(mpv_handle *handle)
{
    g_assert_true(handle == initialized_handle);
    initialized_handle = NULL;
    engine_destructions++;
    mpv_terminate_destroy(handle);
}

static void assert_engine_option(const char *name, const char *expected)
{
    char *actual = mpv_get_property_string(initialized_handle, name);
    g_assert_nonnull(actual);
    g_assert_cmpstr(actual, ==, expected);
    mpv_free(actual);
}

static gboolean test_gtk_init_check(void)
{
    gtk_initializations++;
    g_assert_nonnull(initialized_handle);
    assert_engine_option("options/vo", "libmpv");
    assert_engine_option("options/force-window", "no");
    assert_engine_option("options/gpu-api", expected_api);
    assert_engine_option("options/opengl-es", expected_es);
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.9);
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, expected_disable);
    g_assert_cmpstr(g_getenv("GSK_RENDERER"), ==, expected_renderer);
    if (startup_media) {
        /* main() must queue media before calling GTK. Inspect the real stock
         * engine while its new hook is held: demux has completed but the VO
         * and FILE_LOADED cannot precede the absent GtkGLArea context. */
        gboolean preloaded = FALSE;
        gint64 until = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
        while (!preloaded) {
            g_assert_cmpint(g_get_monotonic_time(), <, until);
            mpv_event *event = mpv_wait_event(initialized_handle, .02);
            g_assert_cmpint(event->error, >=, 0);
            g_assert_cmpint(event->event_id, !=, MPV_EVENT_FILE_LOADED);
            g_assert_cmpint(event->event_id, !=, MPV_EVENT_END_FILE);
            if (event->event_id == MPV_EVENT_HOOK) {
                mpv_event_hook *hook = event->data;
                g_assert_cmpuint(event->reply_userdata, ==, RENDER_LOAD_HOOK_ID);
                g_assert_cmpstr(hook->name, ==, "on_preloaded");
                preloaded = TRUE;
            }
        }
        mpv_node tracks = {0};
        g_assert_cmpint(mpv_get_property(initialized_handle, "track-list", MPV_FORMAT_NODE,
                                        &tracks), >=, 0);
        g_assert_cmpint(tracks.format, ==, MPV_FORMAT_NODE_ARRAY);
        g_assert_cmpint(tracks.u.list->num, ==, 1);
        g_assert_cmpstr(node_string(&tracks.u.list->values[0], "type"), ==, "video");
        mpv_free_node_contents(&tracks);
        char *vo = NULL;
        g_assert_cmpint(mpv_get_property(initialized_handle, "current-vo", MPV_FORMAT_STRING,
                                        &vo), ==, MPV_ERROR_PROPERTY_UNAVAILABLE);
        g_assert_null(vo);
        early_preload_checks++;
        /* Leave the actual hook pending. The failed-display return must
         * still tear down the core and eventfd without a window or hang. */
    }
    return FALSE;
}

static void capture_diagnostic(const char *message)
{
    g_assert_cmpstr(message, ==,
        "mpv-gtk: не удалось подключиться к графической сессии Wayland\n");
    diagnostic_calls++;
}

static void restore_variable(const char *name, const char *value)
{
    if (value)
        g_setenv(name, value, TRUE);
    else
        g_unsetenv(name);
}

static void write_fixture(const char *path, const char *contents)
{
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
}

static void assert_file_unchanged(const char *path, const char *expected)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *actual = NULL;
    g_assert_true(g_file_get_contents(path, &actual, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(actual, ==, expected);
}

static void run_startup(const char *path, const char *input, const char *ui,
                         const char *configuration, const char *normalized_api,
                         const char *api, const char *es,
                         const char *disable, const char *renderer_override)
{
    const char *ui_contents = "# Keep UI choices separate\n[Interface]\nlanguage=ru\n"
                              "cursor-hide-delay=1.9\n[Other]\ncustom=yes\n";
    const char *input_contents = "# Keep user bindings\nSPACE cycle pause\n";
    write_fixture(path, configuration);
    write_fixture(input, input_contents);
    write_fixture(ui, ui_contents);
    restore_variable("GDK_DISABLE", disable);
    restore_variable("GSK_RENDERER", renderer_override);
    expected_api = api;
    expected_es = es;
    expected_disable = disable;
    expected_renderer = renderer_override ? renderer_override : "opengl";
    if (g_str_equal(normalized_api, "opengl"))
        expected_disable = disable ? "dmabuf:gles-api" : "gles-api";
    else if (g_str_equal(normalized_api, "opengl-es"))
        expected_disable = disable ? "dmabuf:gl-api" : "gl-api";
    engine_initializations = engine_destructions = gtk_initializations = diagnostic_calls = 0;
    wake_closes = early_preload_checks = 0;
    char *arguments[] = {"mpv-gtk-gl-startup-test", (char *)startup_media, NULL};
    g_assert_cmpint(mpv_gtk_program_main(startup_media ? 2 : 1, arguments), ==, 77);
    g_assert_cmpuint(engine_initializations, ==, 1);
    g_assert_cmpuint(engine_destructions, ==, 1);
    g_assert_cmpuint(wake_closes, ==, 1);
    g_assert_cmpint(wake_fd, ==, -1);
    g_assert_null(initialized_handle);
    g_assert_cmpuint(gtk_initializations, ==, 1);
    g_assert_cmpuint(diagnostic_calls, ==, 1);
    g_assert_cmpuint(early_preload_checks, ==, startup_media ? 1 : 0);
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, disable);
    g_assert_cmpstr(g_getenv("GSK_RENDERER"), ==, expected_renderer);
    assert_file_unchanged(path, configuration);
    assert_file_unchanged(input, input_contents);
    assert_file_unchanged(ui, ui_contents);
}

int main(void)
{
    g_autofree char *original_config = g_strdup(g_getenv("MPV_GTK_CONFIG_FILE"));
    g_autofree char *original_disable = g_strdup(g_getenv("GDK_DISABLE"));
    g_autofree char *original_renderer = g_strdup(g_getenv("GSK_RENDERER"));
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-gl-startup-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input = g_build_filename(directory, "input.conf", NULL);
    g_autofree char *ui = g_build_filename(directory, "interface.conf", NULL);
    g_autofree char *included = g_build_filename(directory, "renderer.conf", NULL);
    g_autofree char *media = g_build_filename(directory, "early GTK failure.mp4", NULL);
    g_setenv("MPV_GTK_CONFIG_FILE", path, TRUE);
    GPrintFunc original_printerr = g_set_printerr_handler(capture_diagnostic);
    const struct { const char *vo, *api, *es, *normalized, *disable; } choices[] = {
        {"libmpv", "auto", "auto", "auto", NULL},
        {"gpu", "opengl", "no", "opengl", NULL},
        {"gpu-next", "opengl", "yes", "opengl-es", "gl-api; dmabuf,gles-api"},
        {"gpu-next", "vulkan", "auto", "auto", NULL}
    };
    for (guint i = 0; i < G_N_ELEMENTS(choices); i++) {
        g_autofree char *configuration = g_strdup_printf(
            "# Preserve renderer options\nhwdec=no\nao=null\nvo=%s\ngpu-api=%s\nopengl-es=%s\n",
            choices[i].vo, choices[i].api, choices[i].es);
        run_startup(path, input, ui, configuration, choices[i].normalized,
                    choices[i].api, choices[i].es, choices[i].disable, NULL);
    }

    write_fixture(included, "[renderer]\nvo=libmpv\ngpu-api=opengl\nopengl-es=yes\n");
    g_autofree char *profile = g_strdup_printf(
        "hwdec=no\nao=null\ninclude=%s\nprofile=renderer\n", included);
    run_startup(path, input, ui, profile, "opengl-es", "opengl", "yes",
                "gl-api; dmabuf,gles-api", "vulkan");
    write_fixture(included, "vo=gpu\ngpu-api=opengl\nopengl-es=no\n");
    g_autofree char *include = g_strdup_printf("hwdec=no\nao=null\ninclude=%s\n", included);
    run_startup(path, input, ui, include, "opengl", "opengl", "no",
                "gl-api; dmabuf,gles-api", "cairo");
    write_fixture(included, "vo=gpu-next\ngpu-api=vulkan\n");
    g_autofree char *deferred = g_strdup_printf(
        "hwdec=no\nao=null\ninclude=%s\nvo=libmpv\ngpu-api=opengl\nopengl-es=no\n", included);
    run_startup(path, input, ui, deferred, "opengl", "vulkan", "no", NULL, NULL);

    char *ffmpeg[] = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "color=c=black:size=32x32:rate=1:duration=2",
        "-c:v", "mpeg4", "-threads", "1", media, NULL};
    g_autofree char *messages = NULL;
    int fixture_status = 0;
    g_assert_true(g_spawn_sync(NULL, ffmpeg, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, &messages, &fixture_status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(fixture_status, &error))
        g_error("startup fixture: %s; %s", error->message, messages ? messages : "");
    startup_media = media;
    run_startup(path, input, ui,
        "hwdec=no\nao=null\ngpu-api=auto\nopengl-es=auto\n",
        "auto", "auto", "auto", NULL, NULL);
    startup_media = NULL;

    g_set_printerr_handler(original_printerr);
    restore_variable("MPV_GTK_CONFIG_FILE", original_config);
    restore_variable("GDK_DISABLE", original_disable);
    restore_variable("GSK_RENDERER", original_renderer);
    g_assert_cmpint(g_unlink(media), ==, 0);
    g_assert_cmpint(g_unlink(included), ==, 0);
    g_assert_cmpint(g_unlink(ui), ==, 0);
    g_assert_cmpint(g_unlink(input), ==, 0);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS GL startup: forced embedded VO after stock options/includes/profiles, "
         "shared GL API chosen before GTK, environment restoration, "
         "early preloaded video before the display boundary, unchanged configs "
         "and failed-display cleanup while the render hook is held");
    return 0;
}
