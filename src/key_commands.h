#ifndef MPV_GTK_KEY_COMMANDS_H
#define MPV_GTK_KEY_COMMANDS_H

#include <glib.h>
#include <mpv/client.h>

typedef struct {
    char *name;
    char *type;
    gboolean optional;
} KeyCommandArgument;

typedef struct {
    char *name;
    char *signature;
    char *description;
    char *category;
    char *example;
    GPtrArray *arguments; /* Owned KeyCommandArgument pointers. */
    gboolean vararg;
} KeyCommandSpec;

typedef struct {
    const char *command;
    const char *title;
    const char *help;
    const char *category;
} KeyCommandPreset;

/* The runtime command-list NODE is copied. Free the returned array with
 * g_ptr_array_unref(); all specs, argument arrays and strings belong to it.
 * Descriptions are translated using the current UI language at snapshot time. */
GPtrArray *key_command_list_from_node(const mpv_node *commands);

/* Base-command lookups return translated static strings. Unknown future
 * commands retain their runtime signature and receive an explicit fallback. */
gboolean key_command_is_known(const char *base);
const char *key_command_help(const char *base);
const char *key_command_example(const char *base);
const char *key_command_category(const char *base);

/* Describe a full binding; semantic presets and application script messages
 * receive specific explanations. The caller frees the returned string. */
char *key_command_description(const char *command);

/* Static preset fields are Russian source strings: use tr() for display. */
const KeyCommandPreset *key_command_presets(gsize *count);

#endif
