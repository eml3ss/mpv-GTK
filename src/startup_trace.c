#include "startup_trace.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gsize initialized;
static gint enabled;
static gint64 started;
static GMutex output_lock;

void startup_trace_begin(void)
{
    if (g_once_init_enter(&initialized)) {
        const char *setting = getenv("MPV_GTK_STARTUP_TRACE");
        if (setting && strcmp(setting, "1") == 0) {
            started = g_get_monotonic_time();
            g_atomic_int_set(&enabled, TRUE);
        }
        g_once_init_leave(&initialized, 1);
    }
}

gboolean startup_trace_enabled(void)
{
    return g_atomic_int_get(&enabled);
}

void startup_trace_mark(const char *stage)
{
    if (!g_atomic_int_get(&enabled))
        return;

    g_mutex_lock(&output_lock);
    gint64 elapsed_us = g_get_monotonic_time() - started;
    fprintf(stderr, "mpv-gtk startup: +%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT
            " ms | %s\n", elapsed_us / 1000, elapsed_us % 1000, stage);
    g_mutex_unlock(&output_lock);
}
