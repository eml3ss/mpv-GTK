#ifndef MPV_GTK_INPUT_H
#define MPV_GTK_INPUT_H

#include <gdk/gdk.h>
#include <mpv/client.h>

/* Keep only explicit, currently active bindings; mpv exposes inactive
 * built-in defaults in input-bindings even when defaults are disabled. */
GHashTable *input_bindings_new(void);
void input_bindings_update(GHashTable *bindings, const mpv_node *node);
gboolean input_bindings_contains(GHashTable *bindings, const char *key_name);

/* The caller owns the returned mpv key name. Modifier-only or unsupported
 * keys return NULL. Lock state never becomes a key-binding modifier. */
char *input_key_name(guint keyval, GdkModifierType modifiers);

#endif
