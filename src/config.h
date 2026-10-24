#ifndef MPV_GTK_CONFIG_H
#define MPV_GTK_CONFIG_H

#include <glib.h>

typedef struct {
    char *path;
    char *input_path;
} PlayerConfig;

/* Initialize a zeroed structure. An existing config is never replaced. */
gboolean config_init(PlayerConfig *config, GError **error);
gboolean config_set(PlayerConfig *config, const char *name, const char *value,
                    GError **error);
/* Commit all options in one atomic replacement, or leave the file unchanged. */
gboolean config_set_options(PlayerConfig *config, const char *const *names,
                            const char *const *values, gsize count, GError **error);
/* Save a complete CSV string list. An empty list uses name-clr=yes; all
 * conflicting global list actions are normalized, preserving comments and
 * named profiles. The read/modify/write is one locked atomic transaction. */
gboolean config_set_list(PlayerConfig *config, const char *name, const char *value,
                         GError **error);
/* Migrate obsolete renderer fields from sibling interface.conf before loading
 * mpv.conf. Explicit mpv options and indirect/profile configuration win. */
gboolean config_migrate_renderer(PlayerConfig *config, GError **error);
const char *config_path(const PlayerConfig *config);
const char *config_input_path(const PlayerConfig *config);
void config_clear(PlayerConfig *config);

#endif
