#ifndef MPV_GTK_STARTUP_TRACE_H
#define MPV_GTK_STARTUP_TRACE_H

#include <glib.h>

/* Call at main entry, before starting worker threads. Disabled unless
 * MPV_GTK_STARTUP_TRACE is exactly "1". Marks are safe from any thread.
 * These times exclude the dynamic loader and other work before main. */
void startup_trace_begin(void);
gboolean startup_trace_enabled(void);
void startup_trace_mark(const char *stage);

#endif
