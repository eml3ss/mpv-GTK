/* Exercise the real stock engine without a display. GtkGLArea always owns
 * presentation: user VO options must never select a second mpv window. */
#define _GNU_SOURCE
#include <mpv/client.h>
#include <mpv/render.h>

static int observed_hook_add(mpv_handle *handle, uint64_t userdata,
                             const char *name, int priority);
static int observed_render_create(mpv_render_context **context,
                                  mpv_handle *handle, mpv_render_param *params);
#define mpv_hook_add observed_hook_add
#define mpv_render_context_create observed_render_create
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main
#undef mpv_render_context_create
#undef mpv_hook_add

#include <glib/gstdio.h>

static guint render_hook_registrations, preloaded_hook_registrations, early_hook_registrations;
static guint render_context_creations;

static int observed_hook_add(mpv_handle *handle, uint64_t userdata,
                             const char *name, int priority)
{
    int result = mpv_hook_add(handle, userdata, name, priority);
    if (result >= 0 && userdata == RENDER_LOAD_HOOK_ID) {
        render_hook_registrations++;
        preloaded_hook_registrations += g_str_equal(name, "on_preloaded");
        early_hook_registrations += g_str_equal(name, "on_load");
    }
    return result;
}

static int observed_render_create(mpv_render_context **context,
                                  mpv_handle *handle, mpv_render_param *params)
{
    (void)context;
    (void)handle;
    (void)params;
    render_context_creations++;
    g_assert_not_reached(); /* No current GL context exists in this test. */
    return MPV_ERROR_UNSUPPORTED;
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

static void assert_option(mpv_handle *mpv, const char *name, const char *expected)
{
    char *actual = mpv_get_property_string(mpv, name);
    g_assert_nonnull(actual);
    g_assert_cmpstr(actual, ==, expected);
    mpv_free(actual);
}

static void assert_engine(const char *path, const char *input,
                           const char *contents, const char *api,
                           const char *es, const char *normalized_api,
                           double volume)
{
    write_fixture(path, contents);
    Player p = {.wake_fd = -1};
    p.config.path = g_strdup(path);
    p.config.input_path = g_strdup(input);
    render_hook_registrations = preloaded_hook_registrations = early_hook_registrations = 0;
    render_context_creations = 0;
    g_assert_true(init_mpv_core(&p));
    g_assert_nonnull(p.mpv);
    assert_option(p.mpv, "options/vo", "libmpv");
    assert_option(p.mpv, "options/force-window", "no");
    assert_option(p.mpv, "options/gpu-api", api);
    assert_option(p.mpv, "options/opengl-es", es);
    assert_option(p.mpv, "options/hwdec", "no");
    g_autofree char *shader_cache = g_build_filename(g_get_user_cache_dir(), "mpv-gtk", "shaders", NULL);
    assert_option(p.mpv, "options/gpu-shader-cache-dir", shader_cache);
    g_assert_false(g_file_test(shader_cache, G_FILE_TEST_EXISTS));
    g_assert_cmpstr(p.gl_api, ==, normalized_api);
    double actual_volume = -1;
    g_assert_cmpint(mpv_get_property(p.mpv, "volume", MPV_FORMAT_DOUBLE,
                                    &actual_volume), >=, 0);
    g_assert_cmpfloat(actual_volume, ==, volume);
    g_assert_false(p.ready);
    g_assert_null(p.renderer);
    g_assert_null(p.area);
    g_assert_false(ensure_renderer(&p));
    g_assert_cmpuint(render_hook_registrations, ==, 1);
    g_assert_cmpuint(preloaded_hook_registrations, ==, 1);
    g_assert_cmpuint(early_hook_registrations, ==, 0);
    g_assert_cmpuint(render_context_creations, ==, 0);
    char *current_vo = mpv_get_property_string(p.mpv, "current-vo");
    g_assert_null(current_vo); /* No media or video window was instantiated. */
    p.closing = TRUE;
    mpv_set_wakeup_callback(p.mpv, NULL, NULL);
    mpv_terminate_destroy(p.mpv);
    if (p.gl_library)
        dlclose(p.gl_library);
    if (p.wake_fd >= 0)
        close(p.wake_fd);
    config_clear(&p.config);
    assert_file_unchanged(path, contents);
    assert_file_unchanged(input, "# no user bindings\n");
}

static void assert_shader_cache_override(const char *path, const char *input,
                                         const char *cache, gboolean enabled)
{
    g_autofree char *contents = g_strdup_printf(
        "hwdec=no\nao=null\ngpu-shader-cache-dir=%s\ngpu-shader-cache=%s\n",
        cache, enabled ? "yes" : "no");
    write_fixture(path, contents);
    Player p = {.wake_fd = -1};
    p.config.path = g_strdup(path);
    p.config.input_path = g_strdup(input);
    g_assert_true(init_mpv_core(&p));
    assert_option(p.mpv, "options/gpu-shader-cache-dir", cache);
    assert_option(p.mpv, "options/gpu-shader-cache", enabled ? "yes" : "no");
    g_assert_null(p.renderer);
    g_assert_false(g_file_test(cache, G_FILE_TEST_EXISTS));
    mpv_terminate_destroy(p.mpv);
    if (p.gl_library)
        dlclose(p.gl_library);
    close(p.wake_fd);
    config_clear(&p.config);
    assert_file_unchanged(path, contents);
    assert_file_unchanged(input, "# no user bindings\n");
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-embedded-engine-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input = g_build_filename(directory, "input.conf", NULL);
    g_autofree char *included = g_build_filename(directory, "included.conf", NULL);
    g_autofree char *cache_home = g_build_filename(directory, "cache-home", NULL);
    g_autofree char *custom_cache = g_build_filename(directory, "custom-shaders", NULL);
    g_assert_true(g_setenv("XDG_CACHE_HOME", cache_home, TRUE));
    write_fixture(input, "# no user bindings\n");

    /* Stock's unspecified gpu-api is an empty priority list. The UI presents
     * Auto; the engine file remains unchanged even for unsupported native VOs. */
    assert_engine(path, input, "hwdec=no\nao=null\n", "", "auto", "auto", 50);
    const struct { const char *vo, *api, *es, *normalized; } choices[] = {
        {"gpu", "opengl", "no", "opengl"},
        {"gpu-next", "opengl", "yes", "opengl-es"},
        {"libmpv", "auto", "auto", "auto"},
        {"gpu-next", "vulkan", "auto", "auto"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(choices); i++) {
        g_autofree char *contents = g_strdup_printf(
            "# Keep the user's stock options\nhwdec=no\nao=null\nvo=%s\n"
            "gpu-api=%s\nopengl-es=%s\nvolume=37\nforce-window=yes\n",
            choices[i].vo, choices[i].api, choices[i].es);
        assert_engine(path, input, contents, choices[i].api, choices[i].es,
                      choices[i].normalized, 37);
    }

    const char *include_contents = "vo=gpu\ngpu-api=opengl\nopengl-es=yes\nvolume=63\n";
    write_fixture(included, include_contents);
    g_autofree char *include = g_strdup_printf("hwdec=no\nao=null\ninclude=%s\n", included);
    assert_engine(path, input, include, "opengl", "yes", "opengl-es", 63);
    assert_file_unchanged(included, include_contents);

    const char *profile_contents =
        "[renderer-test]\nvo=gpu-next\ngpu-api=vulkan\nopengl-es=auto\nvolume=27\n";
    write_fixture(included, profile_contents);
    g_autofree char *profile = g_strdup_printf(
        "hwdec=no\nao=null\ninclude=%s\nprofile=renderer-test\n", included);
    assert_engine(path, input, profile, "vulkan", "auto", "auto", 27);
    assert_file_unchanged(included, profile_contents);

    const char *deferred_contents = "vo=gpu\ngpu-api=vulkan\nvolume=61\n";
    write_fixture(included, deferred_contents);
    /* Stock processes this include during initialize(), after direct file
     * options. The application must enforce embedding after that evaluation. */
    g_autofree char *deferred = g_strdup_printf(
        "hwdec=no\nao=null\ninclude=%s\nvo=libmpv\ngpu-api=opengl\n"
        "opengl-es=no\nvolume=29\n", included);
    assert_engine(path, input, deferred, "vulkan", "no", "opengl", 61);
    assert_file_unchanged(included, deferred_contents);

    assert_shader_cache_override(path, input, custom_cache, TRUE);
    assert_shader_cache_override(path, input, custom_cache, FALSE);

    g_assert_cmpint(g_unlink(included), ==, 0);
    g_assert_cmpint(g_unlink(input), ==, 0);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS embedded engine: stock options/includes/profiles preserved, VO always libmpv, "
         "OpenGL API selection, one preloaded hook, lazy private shader cache with user overrides, "
         "no renderer before GtkGLArea readiness");
    return 0;
}
