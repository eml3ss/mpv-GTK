#ifndef MPV_GTK_KEYBINDINGS_H
#define MPV_GTK_KEYBINDINGS_H

#include <glib.h>
#include <mpv/client.h>
#include <stdint.h>

#define KEYBINDINGS_APP_SECTION "mpv-gtk-defaults"
#define KEYBINDINGS_EDIT_SECTION "mpv-gtk-edits"

typedef struct {
    const char *key;
    const char *command;
    const char *comment;
} KeyBindingDefault;

typedef struct {
    char *key;
    char *command;
    char *comment;
    char *section;
    gboolean weak;
    gboolean active;
    gboolean app_default;
    int64_t priority;
} KeyBinding;

/* One owned row per key, including disabled mpv defaults. Active engine
 * bindings win; observed frontend availability is preserved, and supplied
 * defaults fill keys whose frontend row is absent from the snapshot.
 * The returned array owns its rows and strings; release with g_ptr_array_unref. */
GPtrArray *keybindings_snapshot(const mpv_node *node,
                               const KeyBindingDefault *defaults, gsize count);

/* Edits live in a marked global {default} block. Original input.conf lines,
 * comments, duplicate bindings and named sections are preserved. Reset removes
 * only the managed override, restoring the original file binding or fallback.
 * These functions check single-line syntax, not mpv command validity. */
gboolean keybindings_set(const char *path, const char *key, const char *command,
                         GError **error);
gboolean keybindings_reset(const char *path, const char *key, GError **error);

/* An atomic rename suppresses the old key with "ignore" and binds the new key
 * in the same file replacement. Passing the same key is equivalent to set. */
gboolean keybindings_rename(const char *path, const char *old_key, const char *key,
                            const char *command, GError **error);

/* Owned define-section contents for edited keys only, without {default}
 * directives. Empty blocks return a nonempty comment so replacing a live
 * section preserves its place in the enabled-section stack. */
char *keybindings_managed_contents(const char *path, GError **error);

#endif
