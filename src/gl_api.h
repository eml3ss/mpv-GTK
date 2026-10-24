#ifndef MPV_GTK_GL_API_H
#define MPV_GTK_GL_API_H

#include <gdk/gdk.h>

typedef struct {
    char *original_disable;
    gboolean active;
} GlApiEnvironment;

GdkGLAPI gl_api_allowed_apis(const char *api);

/* Initialize to zero, begin before GTK initialization, and end immediately
 * afterwards. Restoring the environment keeps child windows/processes free to
 * select a different API from the current saved preference. */
void gl_api_environment_begin(GlApiEnvironment *environment, const char *api);
void gl_api_environment_end(GlApiEnvironment *environment);

#endif
