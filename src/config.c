#define _GNU_SOURCE
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const char defaults[] =
    "# mpv GTK: settings next to the executable.\n"
    "# Settings changed in the player are saved here; other options are preserved.\n"
    "vo=libmpv\n"
    "gpu-api=auto\n"
    "opengl-es=auto\n"
    "hwdec=auto-safe\n"
    "keepaspect=yes\n"
    "deinterlace=no\n"
    "deband=no\n"
    "audio-delay=0\n"
    "sub-delay=0\n"
    "sub-scale=1\n"
    "loop-file=no\n"
    "volume=50\n";

static const char input_defaults[] =
    "# mpv GTK: custom input bindings next to the executable.\n"
    "# Remove the leading # to enable an example, then restart the player.\n"
    "# Custom bindings take priority over GTK player shortcuts for the same key.\n"
    "# p cycle pause\n"
    "# Ctrl+s screenshot\n"
    "# Alt+RIGHT seek 30\n"
    "# MBTN_LEFT cycle pause\n"
    "# MBTN_RIGHT show-text \"${media-title}\"\n";

static gboolean file_error(GError **error, const char *path)
{
    int saved_errno = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                "%s: %s", path, g_strerror(saved_errno));
    return FALSE;
}

/* Lock the directory rather than a file which atomic replacement would unlink.
 * All instances of this application then read/modify/write the same revision. */
static int lock_directory(const char *path, GError **error)
{
    g_autofree char *directory = g_path_get_dirname(path);
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        file_error(error, directory);
        return -1;
    }
    int result;
    do {
        result = flock(fd, LOCK_EX);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        file_error(error, directory);
        close(fd);
        return -1;
    }
    return fd;
}

static gboolean write_config(const char *path, const char *contents, GError **error)
{
    GStatBuf status;
    int mode = g_stat(path, &status) == 0 ? (int)(status.st_mode & 0777) : 0600;
    return g_file_set_contents_full(path, contents, -1,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, mode, error);
}

static gboolean seed_file(const char *path, const char *filename,
                           const char *seed, gboolean migrate, GError **error)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) read_error = NULL;
    if (g_file_get_contents(path, &contents, NULL, &read_error))
        return TRUE;
    if (!g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) ||
        g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        g_propagate_error(error, g_steal_pointer(&read_error));
        return FALSE;
    }
    g_autofree char *legacy = migrate
        ? g_build_filename(g_get_user_config_dir(), "mpv-gtk", filename, NULL) : NULL;
    if (legacy && g_file_test(legacy, G_FILE_TEST_EXISTS)) {
        g_clear_error(&read_error);
        if (!g_file_get_contents(legacy, &contents, NULL, &read_error)) {
            g_propagate_error(error, g_steal_pointer(&read_error));
            return FALSE;
        }
    }
    return write_config(path, contents ? contents : seed, error);
}

gboolean config_init(PlayerConfig *config, GError **error)
{
    g_return_val_if_fail(config != NULL, FALSE);
    if (config->path && config->input_path)
        return TRUE;
    const char *override = g_getenv("MPV_GTK_CONFIG_FILE");
    if (!config->path) {
        if (override && *override) {
            config->path = g_canonicalize_filename(override, NULL);
        } else {
            g_autofree char *executable = g_file_read_link("/proc/self/exe", error);
            if (!executable)
                return FALSE;
            g_autofree char *directory = g_path_get_dirname(executable);
            config->path = g_build_filename(directory, "mpv.conf", NULL);
        }
    }
    g_autofree char *directory = g_path_get_dirname(config->path);
    if (!config->input_path)
        config->input_path = g_build_filename(directory, "input.conf", NULL);
    if (g_mkdir_with_parents(directory, 0700) < 0) {
        file_error(error, directory);
        config_clear(config);
        return FALSE;
    }
    int lock = lock_directory(config->path, error);
    if (lock < 0) {
        config_clear(config);
        return FALSE;
    }
    gboolean migrate = !override || !*override;
    gboolean result = seed_file(config->path, "mpv.conf", defaults, migrate, error) &&
                      seed_file(config->input_path, "input.conf", input_defaults, migrate, error);
    close(lock);
    if (!result)
        config_clear(config);
    return result;
}

static const char *skip_space(const char *text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    return text;
}

static gboolean matches_option(const char *line, const char *name)
{
    const char *start = skip_space(line);
    if (g_str_has_prefix(start, "--"))
        start += 2;
    if (g_str_has_prefix(start, "no-"))
        start += 3;
    gsize length = strlen(name);
    return strncmp(start, name, length) == 0 &&
           (!start[length] || strchr("= \t\r#", start[length]) != NULL);
}

static char *quote_value(const char *value)
{
    gboolean simple = *value != '\0';
    for (const char *p = value; *p; p++)
        if (!g_ascii_isalnum(*p) && !strchr("_./:+-", *p))
            simple = FALSE;
    if (simple)
        return g_strdup(value);
    GString *quoted = g_string_new("\"");
    for (const char *p = value; *p; p++) {
        if (*p == '\\' || *p == '"')
            g_string_append_c(quoted, '\\');
        g_string_append_c(quoted, *p);
    }
    g_string_append_c(quoted, '"');
    return g_string_free(quoted, FALSE);
}

static const char *option_comment(const char *line)
{
    char quote = 0;
    gboolean escaped = FALSE;
    for (const char *p = skip_space(line); *p; p++) {
        if (escaped) {
            escaped = FALSE;
        } else if (*p == '\\') {
            escaped = TRUE;
        } else if (*p == quote) {
            quote = 0;
        } else if (!quote && (*p == '\'' || *p == '"')) {
            quote = *p;
        } else if (!quote && *p == '#') {
            return p;
        }
    }
    return NULL;
}

static char *replace_option(const char *line, const char *name, const char *value)
{
    const char *start = skip_space(line), *comment = option_comment(line);
    while (comment && comment > start && (comment[-1] == ' ' || comment[-1] == '\t'))
        comment--;
    gsize length = strlen(line);
    gboolean crlf = length && line[length - 1] == '\r';
    g_autofree char *indent = g_strndup(line, (gsize)(start - line));
    g_autofree char *quoted = quote_value(value);
    return g_strdup_printf("%s%s=%s%s%s%s", indent, name, quoted,
        comment && *comment == '#' ? " " : "", comment ? comment : "",
        crlf && !comment ? "\r" : "");
}

static gboolean valid_option(const char *name, const char *value, GError **error)
{
    if (!name || !*name || !value || strpbrk(value, "\r\n")) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "Invalid config option or multiline value");
        return FALSE;
    }
    for (const char *p = name; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') {
            g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                                "Invalid config option name");
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean read_text(const char *path, char **contents, GError **error)
{
    gsize length;
    if (!g_file_get_contents(path, contents, &length, error))
        return FALSE;
    if (memchr(*contents, '\0', length)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "%s contains a NUL byte; refusing to rewrite it", path);
        g_clear_pointer(contents, g_free);
        return FALSE;
    }
    return TRUE;
}

static char *edit_option(const char *contents, const char *name, const char *value)
{
    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    guint count = g_strv_length(lines), insert_at = count;
    int last = -1;
    gboolean global = TRUE;
    const char *newline = strstr(contents, "\r\n") ? "\r\n" : "\n";
    for (guint i = 0; i < count; i++) {
        const char *start = skip_space(lines[i]);
        if (*start == '[') {
            if (global)
                insert_at = i;
            const char *end = strchr(start, ']');
            global = end && end - start == 8 && strncmp(start, "[default]", 9) == 0;
            if (global)
                insert_at = count;
        } else if (global && matches_option(lines[i], name)) {
            last = (int)i;
        }
    }
    if (last >= 0) {
        char *replacement = replace_option(lines[last], name, value);
        g_free(lines[last]);
        lines[last] = replacement;
    }
    GString *output = g_string_new(NULL);
    for (guint i = 0; i <= count; i++) {
        if (last < 0 && i == insert_at) {
            if (output->len && output->str[output->len - 1] != '\n')
                g_string_append(output, newline);
            g_autofree char *quoted = quote_value(value);
            g_string_append_printf(output, "%s=%s%s", name, quoted, newline);
        }
        if (i < count) {
            g_string_append(output, lines[i]);
            if (i + 1 < count)
                g_string_append_c(output, '\n');
        }
    }
    return g_string_free(output, FALSE);
}

gboolean config_set_options(PlayerConfig *config, const char *const *names,
                            const char *const *values, gsize count, GError **error)
{
    g_return_val_if_fail(config && config->path, FALSE);
    if (count && (!names || !values)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "Config option names and values are required");
        return FALSE;
    }
    for (gsize i = 0; i < count; i++)
        if (!valid_option(names[i], values[i], error))
            return FALSE;
    if (!count)
        return TRUE;
    int lock = lock_directory(config->path, error);
    if (lock < 0)
        return FALSE;
    g_autofree char *contents = NULL;
    gboolean result = read_text(config->path, &contents, error);
    for (gsize i = 0; result && i < count; i++) {
        char *next = edit_option(contents, names[i], values[i]);
        g_free(contents);
        contents = next;
    }
    if (result)
        result = write_config(config->path, contents, error);
    close(lock);
    return result;
}

gboolean config_set(PlayerConfig *config, const char *name, const char *value,
                    GError **error)
{
    const char *names[] = {name}, *values[] = {value};
    return config_set_options(config, names, values, 1, error);
}

static gboolean list_action_option(const char *line, const char *name)
{
    if (matches_option(line, name))
        return TRUE;
    static const char *const actions[] = {
        "add", "append", "pre", "clr", "del", "set", "toggle", "remove"
    };
    for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
        g_autofree char *option = g_strconcat(name, "-", actions[i], NULL);
        if (matches_option(line, option))
            return TRUE;
    }
    return FALSE;
}

static const struct { const char *name, *alias; } list_aliases[] = {
    {"audio-exts", "audio-file-auto-exts"},
    {"image-exts", "cover-art-auto-exts"}
};

static gboolean list_family_option(const char *line, const char *name)
{
    if (list_action_option(line, name))
        return TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(list_aliases); i++)
        if (g_str_equal(name, list_aliases[i].name) &&
            list_action_option(line, list_aliases[i].alias))
            return TRUE;
    return FALSE;
}

static char *edit_list(const char *contents, const char *name, const char *value)
{
    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    guint count = g_strv_length(lines);
    g_autofree gboolean *family = g_new0(gboolean, count);
    gboolean global = TRUE;
    int last = -1;
    for (guint i = 0; i < count; i++) {
        const char *start = skip_space(lines[i]);
        if (*start == '[') {
            const char *end = strchr(start, ']');
            global = end && end - start == 8 && strncmp(start, "[default]", 9) == 0;
        } else if (global && list_family_option(lines[i], name)) {
            family[i] = TRUE;
            last = (int)i;
        }
    }
    g_autofree char *canonical = *value ? g_strdup(name) : g_strconcat(name, "-clr", NULL);
    const char *saved_value = *value ? value : "yes";
    if (last < 0)
        return edit_option(contents, canonical, saved_value);
    GString *output = g_string_new(NULL);
    for (guint i = 0; i < count; i++) {
        if ((int)i == last) {
            g_autofree char *replacement = replace_option(lines[i], canonical, saved_value);
            g_string_append(output, replacement);
        } else if (family[i]) {
            const char *comment = option_comment(lines[i]);
            if (!comment)
                continue;
            const char *start = skip_space(lines[i]);
            g_string_append_len(output, lines[i], start - lines[i]);
            g_string_append(output, comment);
        } else {
            g_string_append(output, lines[i]);
        }
        if (i + 1 < count)
            g_string_append_c(output, '\n');
    }
    return g_string_free(output, FALSE);
}

gboolean config_set_list(PlayerConfig *config, const char *name, const char *value,
                         GError **error)
{
    g_return_val_if_fail(config && config->path, FALSE);
    if (!valid_option(name, value, error))
        return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(list_aliases); i++)
        if (g_str_equal(name, list_aliases[i].alias))
            name = list_aliases[i].name;
    int lock = lock_directory(config->path, error);
    if (lock < 0)
        return FALSE;
    g_autofree char *contents = NULL;
    gboolean result = read_text(config->path, &contents, error);
    if (result) {
        g_autofree char *updated = edit_list(contents, name, value);
        result = write_config(config->path, updated, error);
    }
    close(lock);
    return result;
}

static gboolean default_header(const char *line)
{
    const char *end = strchr(line, ']');
    return end && end - line == 8 && strncmp(line, "[default]", 9) == 0;
}

/* Includes, activated profiles, and profile-specific renderer choices may
 * supply settings whose effective value cannot be established from this file. */
static gboolean renderer_precedence(const char *contents, gboolean explicit[3])
{
    static const char *const targets[] = {"vo", "gpu-api", "opengl-es"};
    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    gboolean global = TRUE, indirect = FALSE;
    for (guint i = 0; lines[i]; i++) {
        const char *line = skip_space(lines[i]);
        if (*line == '[') {
            global = default_header(line);
            continue;
        }
        if (matches_option(line, "include") || matches_option(line, "profile") ||
            g_str_has_prefix(line, "profile-"))
            indirect = TRUE;
        for (guint j = 0; j < G_N_ELEMENTS(targets); j++) {
            if (matches_option(line, targets[j])) {
                if (global)
                    explicit[j] = TRUE;
                else
                    indirect = TRUE;
            }
        }
    }
    return indirect;
}

gboolean config_migrate_renderer(PlayerConfig *config, GError **error)
{
    g_return_val_if_fail(config && config->path, FALSE);
    static const char *const obsolete[] = {
        "video-backend", "gpu-api", "gpu-renderer-api", "gpu-next-api"
    };
    static const char *const targets[] = {"vo", "gpu-api", "opengl-es"};
    g_autofree char *directory = g_path_get_dirname(config->path);
    g_autofree char *ui_path = g_build_filename(directory, "interface.conf", NULL);
    int lock = lock_directory(config->path, error);
    if (lock < 0)
        return FALSE;
    g_autoptr(GError) read_error = NULL;
    g_autofree char *ui_contents = NULL;
    if (!read_text(ui_path, &ui_contents, &read_error)) {
        gboolean absent = g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
                          !g_file_test(ui_path, G_FILE_TEST_IS_SYMLINK);
        if (!absent)
            g_propagate_error(error, g_steal_pointer(&read_error));
        close(lock);
        return absent;
    }
    g_autoptr(GKeyFile) ui = g_key_file_new();
    if (!g_key_file_load_from_data(ui, ui_contents, -1,
            G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, error)) {
        close(lock);
        return FALSE;
    }
    gboolean present = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(obsolete); i++)
        present |= g_key_file_has_key(ui, "Interface", obsolete[i], NULL);
    if (!present) {
        close(lock);
        return TRUE;
    }
    g_autofree char *contents = NULL;
    if (!read_text(config->path, &contents, error)) {
        close(lock);
        return FALSE;
    }
    gboolean explicit[3] = {FALSE};
    gboolean indirect = renderer_precedence(contents, explicit);
    g_autofree char *backend = g_key_file_get_string(ui, "Interface", "video-backend", NULL);
    if (backend)
        g_strstrip(backend);
    const char *values[3] = {NULL};
    const char *api_key = "gpu-api";
    if (g_strcmp0(backend, "libmpv") == 0 || g_strcmp0(backend, "gpu") == 0 ||
        g_strcmp0(backend, "gpu-next") == 0)
        values[0] = backend;
    if (g_strcmp0(backend, "gpu") == 0)
        api_key = "gpu-renderer-api";
    else if (g_strcmp0(backend, "gpu-next") == 0)
        api_key = "gpu-next-api";
    g_autofree char *api = g_key_file_get_string(ui, "Interface", api_key, NULL);
    if (api)
        g_strstrip(api);
    if (!api && g_strcmp0(backend, "gpu-next") == 0)
        api = g_strdup("vulkan"); /* The old gpu-next default, not a new default. */
    if (g_strcmp0(api, "opengl-es") == 0) {
        values[1] = "opengl";
        values[2] = "yes";
    } else if (g_strcmp0(api, "opengl") == 0 || g_strcmp0(api, "desktop") == 0) {
        values[1] = "opengl";
        values[2] = "no";
    } else if (g_strcmp0(api, "auto") == 0 || g_strcmp0(api, "vulkan") == 0) {
        values[1] = api;
        values[2] = "auto";
    }
    for (guint i = 0; !indirect && i < G_N_ELEMENTS(targets); i++) {
        if (values[i] && !explicit[i]) {
            char *next = edit_option(contents, targets[i], values[i]);
            g_free(contents);
            contents = next;
        }
    }
    /* Commit the actual configuration first. A failed write leaves all legacy
     * keys available for retry; a later UI-write failure is safely idempotent. */
    gboolean result = write_config(config->path, contents, error);
    if (result) {
        for (guint i = 0; i < G_N_ELEMENTS(obsolete); i++)
            g_key_file_remove_key(ui, "Interface", obsolete[i], NULL);
        g_autofree char *updated_ui = g_key_file_to_data(ui, NULL, error);
        result = updated_ui && write_config(ui_path, updated_ui, error);
    }
    close(lock);
    return result;
}

const char *config_path(const PlayerConfig *config)
{
    return config ? config->path : NULL;
}

const char *config_input_path(const PlayerConfig *config)
{
    return config ? config->input_path : NULL;
}

void config_clear(PlayerConfig *config)
{
    if (config) {
        g_clear_pointer(&config->path, g_free);
        g_clear_pointer(&config->input_path, g_free);
    }
}
