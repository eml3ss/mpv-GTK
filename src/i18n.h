#ifndef MPV_GTK_I18N_H
#define MPV_GTK_I18N_H

#include <glib.h>

/* Russian source strings remain valid keys and the default UI language.
 * English is loaded from locales/en.ini beside the executable on its first
 * lookup, and released when deselected. Missing resources use the source text.
 * Returned translations remain valid across language switches. */
const char *tr(const char *russian);
void i18n_set_language(const char *code);
const char *i18n_language(void);

/* UI preferences live in interface.conf beside the supplied mpv.conf.
 * Initialization tolerates a missing file and defaults invalid codes to ru.
 * Saving does not change the active language; refresh it after a successful save. */
gboolean i18n_init(const char *mpv_config_path, GError **error);
gboolean i18n_save_language(const char *code, GError **error);

/* Fullscreen cursor inactivity delay in seconds, persisted separately from mpv.
 * Missing or invalid values use 3 seconds. Successful saves update this value. */
double i18n_cursor_hide_delay(void);
gboolean i18n_save_cursor_hide_delay(double seconds, GError **error);

/* Frontend policy for incoming files/URLs. Both are independent Interface
 * booleans: allow-replace defaults true, add-to-queue defaults false. These
 * preferences never enter mpv.conf. Successful saves update the getters. */
gboolean i18n_allow_replace(void);
gboolean i18n_add_to_queue(void);
gboolean i18n_save_allow_replace(gboolean enabled, GError **error);
gboolean i18n_save_add_to_queue(gboolean enabled, GError **error);

#endif
