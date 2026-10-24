#ifndef MPV_GTK_KEYS_PAGE_H
#define MPV_GTK_KEYS_PAGE_H

#include <gtk/gtk.h>
#include <mpv/client.h>
#include "keybindings.h"

/* Accept and persist a local edit, then apply it asynchronously to mpv.
 * old_key is NULL when adding; reset removes only a managed override.
 * command is "ignore" when disabling and NULL when resetting. */
typedef gboolean (*KeysPageChangeFunc)(const char *old_key, const char *key,
                                       const char *command, gboolean reset,
                                       GError **error, gpointer user_data);

GtkWidget *keys_page_new(void);
void keys_page_configure(GtkWidget *page, const char *input_path,
                          const KeyBindingDefault *defaults, gsize count,
                          KeysPageChangeFunc on_change, gpointer user_data);

/* NULL preserves the previous snapshot. All event-owned strings are copied. */
void keys_page_update(GtkWidget *page, const mpv_node *bindings,
                      const mpv_node *commands);
void keys_page_show_error(GtkWidget *page, const char *message);

#endif
