#include "gl_api.h"

GdkGLAPI gl_api_allowed_apis(const char *api)
{
    if (g_strcmp0(api, "opengl") == 0)
        return GDK_GL_API_GL;
    if (g_strcmp0(api, "opengl-es") == 0)
        return GDK_GL_API_GLES;
    return GDK_GL_API_GL | GDK_GL_API_GLES;
}

void gl_api_environment_begin(GlApiEnvironment *environment, const char *api)
{
    g_return_if_fail(environment != NULL);
    g_return_if_fail(!environment->active);

    GdkGLAPI allowed = gl_api_allowed_apis(api);
    if (allowed == (GDK_GL_API_GL | GDK_GL_API_GLES))
        return;

    environment->original_disable = g_strdup(g_getenv("GDK_DISABLE"));
    environment->active = TRUE;
    g_auto(GStrv) features = g_strsplit_set(
        environment->original_disable ? environment->original_disable : "",
        ":;, \t\r\n\v\f", -1);
    g_autoptr(GString) disable = g_string_new(NULL);
    gboolean inverted = FALSE;
    for (guint i = 0; features[i]; ++i) {
        /* Choose the API for the display's shared context too. A GtkGLArea
         * cannot reliably share resources across desktop GL and GLES. */
        if (!*features[i] || g_ascii_strcasecmp(features[i], "gl-api") == 0 ||
            g_ascii_strcasecmp(features[i], "gles-api") == 0)
            continue;
        if (g_ascii_strcasecmp(features[i], "all") == 0)
            inverted = TRUE;
        if (disable->len)
            g_string_append_c(disable, ':');
        g_string_append(disable, features[i]);
    }
    if (disable->len)
        g_string_append_c(disable, ':');
    /* With "all", GDK subtracts each named feature from the disabled set.
     * In that form, name the API to enable instead of the one to disable. */
    const char *api_flag = allowed == GDK_GL_API_GL ? "gles-api" : "gl-api";
    if (inverted)
        api_flag = allowed == GDK_GL_API_GL ? "gl-api" : "gles-api";
    g_string_append(disable, api_flag);
    g_setenv("GDK_DISABLE", disable->str, TRUE);
}

void gl_api_environment_end(GlApiEnvironment *environment)
{
    g_return_if_fail(environment != NULL);
    if (!environment->active)
        return;

    if (environment->original_disable)
        g_setenv("GDK_DISABLE", environment->original_disable, TRUE);
    else
        g_unsetenv("GDK_DISABLE");
    g_clear_pointer(&environment->original_disable, g_free);
    environment->active = FALSE;
}
