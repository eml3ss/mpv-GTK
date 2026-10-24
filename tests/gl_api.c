#include "../src/gl_api.h"

#include <stdio.h>

static void set_disable(const char *value)
{
    if (value)
        g_setenv("GDK_DISABLE", value, TRUE);
    else
        g_unsetenv("GDK_DISABLE");
}

/* GDK's debug-variable parser makes "all" invert the listed feature bits.
 * Evaluate the two API bits to check the effective result, not just spelling. */
static GdkGLAPI disabled_apis(const char *value)
{
    GdkGLAPI listed = 0;
    gboolean inverted = FALSE;
    g_auto(GStrv) tokens = g_strsplit_set(value, ":;, \t\r\n\v\f", -1);
    for (guint i = 0; tokens[i]; ++i) {
        if (g_ascii_strcasecmp(tokens[i], "gl-api") == 0)
            listed |= GDK_GL_API_GL;
        else if (g_ascii_strcasecmp(tokens[i], "gles-api") == 0)
            listed |= GDK_GL_API_GLES;
        else if (g_ascii_strcasecmp(tokens[i], "all") == 0)
            inverted = TRUE;
    }
    return inverted ? (GDK_GL_API_GL | GDK_GL_API_GLES) & ~listed : listed;
}

static void check_selection(const char *original, const char *api,
                            const char *expected)
{
    set_disable(original);
    GlApiEnvironment environment = {0};
    gl_api_environment_begin(&environment, api);
    g_assert_true(environment.active);
    g_assert_cmpstr(environment.original_disable, ==, original);
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, expected);
    GdkGLAPI selected = gl_api_allowed_apis(api);
    GdkGLAPI disabled = disabled_apis(g_getenv("GDK_DISABLE"));
    g_assert_cmpuint(disabled & selected, ==, 0);
    g_assert_cmpuint(disabled, ==,
                     (GDK_GL_API_GL | GDK_GL_API_GLES) & ~selected);
    gl_api_environment_end(&environment);
    g_assert_false(environment.active);
    g_assert_null(environment.original_disable);
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, original);
    gl_api_environment_end(&environment);
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, original);
}

static void check_auto(const char *original, const char *api)
{
    set_disable(original);
    const char *unchanged = g_getenv("GDK_DISABLE");
    GlApiEnvironment environment = {0};
    gl_api_environment_begin(&environment, api);
    g_assert_false(environment.active);
    g_assert_null(environment.original_disable);
    g_assert_true(g_getenv("GDK_DISABLE") == unchanged);
    gl_api_environment_end(&environment);
    g_assert_true(g_getenv("GDK_DISABLE") == unchanged);
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, original);
}

int main(void)
{
    g_autofree char *original = g_strdup(g_getenv("GDK_DISABLE"));
    g_assert_cmpuint(gl_api_allowed_apis("opengl"), ==, GDK_GL_API_GL);
    g_assert_cmpuint(gl_api_allowed_apis("opengl-es"), ==, GDK_GL_API_GLES);
    g_assert_cmpuint(gl_api_allowed_apis("auto"), ==,
                     GDK_GL_API_GL | GDK_GL_API_GLES);
    g_assert_cmpuint(gl_api_allowed_apis(NULL), ==,
                     GDK_GL_API_GL | GDK_GL_API_GLES);
    g_assert_cmpuint(gl_api_allowed_apis("unknown"), ==,
                     GDK_GL_API_GL | GDK_GL_API_GLES);

    check_selection(NULL, "opengl", "gles-api");
    check_selection(NULL, "opengl-es", "gl-api");
    check_selection("", "opengl", "gles-api");
    check_selection("", "opengl-es", "gl-api");
    check_selection("dmabuf:offload", "opengl", "dmabuf:offload:gles-api");
    check_selection("dmabuf:offload", "opengl-es", "dmabuf:offload:gl-api");
    check_selection("gl-api", "opengl", "gles-api");
    check_selection("gles-api", "opengl-es", "gl-api");
    check_selection("gles-api:gl-api:gles-api:gl-api", "opengl", "gles-api");
    check_selection("gles-api:gl-api:gles-api:gl-api", "opengl-es", "gl-api");
    check_selection(" dmabuf;gl-api,offload\tgles-api\nthreads\r\v\f",
                     "opengl", "dmabuf:offload:threads:gles-api");
    check_selection("all:gl-api:unknown:gles-api:gl", "opengl-es",
                     "all:unknown:gl:gles-api");
    check_selection("all", "opengl", "all:gl-api");
    check_selection("all", "opengl-es", "all:gles-api");
    check_selection("all:gl-api:gles-api:dmabuf", "opengl",
                     "all:dmabuf:gl-api");
    check_selection("GL-API:DMABUF:GLES-API:Offload", "opengl",
                     "DMABUF:Offload:gles-api");
    check_selection("GL-API:DMABUF:GLES-API:Offload", "opengl-es",
                     "DMABUF:Offload:gl-api");
    check_selection("ALL:GL-API:unknown:GLES-API:Offload", "opengl",
                     "ALL:unknown:Offload:gl-api");
    check_selection("aLl:GL-API:unknown:gles-API:Offload", "opengl-es",
                     "aLl:unknown:Offload:gles-api");

    check_auto(NULL, "auto");
    check_auto("", "auto");
    check_auto(" gl-api;dmabuf,gles-api\toffload ", "auto");
    check_auto("all", NULL);
    check_auto("unknown", "unknown");

    /* A second process inherits the original environment, so changing the
     * saved preference and choosing New window cannot inherit the old API. */
    set_disable("dmabuf");
    GlApiEnvironment environment = {0};
    gl_api_environment_begin(&environment, "opengl");
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, "dmabuf:gles-api");
    gl_api_environment_end(&environment);
    gl_api_environment_begin(&environment, "opengl-es");
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, "dmabuf:gl-api");
    gl_api_environment_end(&environment);
    g_assert_cmpstr(g_getenv("GDK_DISABLE"), ==, "dmabuf");
    set_disable(original);

    puts("PASS GL API: strict desktop/ES selection, shared-context environment, "
         "preserved features, inverted all and case-insensitive flags, untouched Auto, "
         "unset/empty restoration and changed window preference");
    return 0;
}
