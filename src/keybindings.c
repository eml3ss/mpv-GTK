#define _GNU_SOURCE
#include "keybindings.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK_BEGIN "# >>> mpv-gtk managed key bindings"
#define BLOCK_END "# <<< mpv-gtk managed key bindings"

static const mpv_node *field(const mpv_node *node, const char *name)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
        return NULL;
    for (int i = 0; i < node->u.list->num; ++i)
        if (g_str_equal(node->u.list->keys[i], name))
            return &node->u.list->values[i];
    return NULL;
}

static const char *string_field(const mpv_node *node, const char *name)
{
    const mpv_node *value = field(node, name);
    return value && value->format == MPV_FORMAT_STRING ? value->u.string : "";
}

static void binding_free(gpointer data)
{
    KeyBinding *binding = data;
    g_free(binding->key);
    g_free(binding->command);
    g_free(binding->comment);
    g_free(binding->section);
    g_free(binding);
}

static KeyBinding *binding_from_node(const mpv_node *item)
{
    const mpv_node *weak = field(item, "is_weak");
    const mpv_node *priority = field(item, "priority");
    KeyBinding *binding = g_new0(KeyBinding, 1);
    binding->key = g_strdup(string_field(item, "key"));
    binding->command = g_strdup(string_field(item, "cmd"));
    binding->comment = g_strdup(string_field(item, "comment"));
    binding->section = g_strdup(string_field(item, "section"));
    binding->weak = weak && weak->format == MPV_FORMAT_FLAG && weak->u.flag;
    binding->priority = priority && priority->format == MPV_FORMAT_INT64
                      ? priority->u.int64 : -1;
    binding->active = binding->priority >= 0;
    binding->app_default = g_str_equal(binding->section, KEYBINDINGS_APP_SECTION);
    return binding;
}

static gboolean preferred(const KeyBinding *candidate, const KeyBinding *old)
{
    if (candidate->active != old->active)
        return candidate->active;
    if (candidate->weak != old->weak)
        return !candidate->weak;
    if (candidate->priority != old->priority)
        return candidate->priority > old->priority;
    return !g_str_equal(old->section, "default") &&
            g_str_equal(candidate->section, "default");
}

static gint compare_bindings(gconstpointer a, gconstpointer b)
{
    const KeyBinding *left = *(KeyBinding *const *)a;
    const KeyBinding *right = *(KeyBinding *const *)b;
    return g_strcmp0(left->key, right->key);
}

GPtrArray *keybindings_snapshot(const mpv_node *node,
                               const KeyBindingDefault *defaults, gsize count)
{
    g_autoptr(GHashTable) rows = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, binding_free);
    /* Remember real frontend rows even if an inactive user row with the
     * same key wins the display tie. Their observed status is authoritative. */
    g_autoptr(GHashTable) app_rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (node && node->format == MPV_FORMAT_NODE_ARRAY && node->u.list) {
        for (int i = 0; i < node->u.list->num; ++i) {
            const mpv_node *item = &node->u.list->values[i];
            const char *key = string_field(item, "key");
            if (!key || !*key)
                continue;
            KeyBinding *binding = binding_from_node(item);
            if (binding->app_default)
                g_hash_table_replace(app_rows, g_strdup(key), (gpointer)item);
            KeyBinding *old = g_hash_table_lookup(rows, key);
            if (!old || preferred(binding, old))
                g_hash_table_replace(rows, g_strdup(key), binding);
            else
                binding_free(binding);
        }
    }
    for (gsize i = 0; defaults && i < count; ++i) {
        const KeyBindingDefault *fallback = &defaults[i];
        if (!fallback->key || !*fallback->key)
            continue;
        KeyBinding *old = g_hash_table_lookup(rows, fallback->key);
        if (old && old->active && !old->weak) {
            if (old->app_default && fallback->comment) {
                g_free(old->comment);
                old->comment = g_strdup(fallback->comment);
            }
            continue;
        }
        const mpv_node *actual_app = g_hash_table_lookup(app_rows, fallback->key);
        if (actual_app) {
            if (old && old->active)
                continue;
            KeyBinding *binding = binding_from_node(actual_app);
            if (fallback->comment) {
                g_free(binding->comment);
                binding->comment = g_strdup(fallback->comment);
            }
            g_hash_table_replace(rows, g_strdup(binding->key), binding);
            continue;
        }
        KeyBinding *binding = g_new0(KeyBinding, 1);
        binding->key = g_strdup(fallback->key);
        binding->command = g_strdup(fallback->command ? fallback->command : "ignore");
        binding->comment = g_strdup(fallback->comment ? fallback->comment : "");
        binding->section = g_strdup(KEYBINDINGS_APP_SECTION);
        binding->active = binding->app_default = TRUE;
        g_hash_table_replace(rows, g_strdup(binding->key), binding);
    }
    GPtrArray *result = g_ptr_array_new_with_free_func(binding_free);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, rows);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_hash_table_iter_steal(&iter);
        g_free(key);
        g_ptr_array_add(result, value);
    }
    g_ptr_array_sort(result, compare_bindings);
    return result;
}

static gboolean invalid(GError **error, const char *message)
{
    g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, message);
    return FALSE;
}

static gboolean valid_key(const char *key, GError **error)
{
    if (!key || !*key || *key == '#' || !g_utf8_validate(key, -1, NULL))
        return invalid(error, "A valid key name is required");
    for (const char *p = key; *p; p = g_utf8_next_char(p))
        if (g_unichar_isspace(g_utf8_get_char(p)) || g_unichar_iscntrl(g_utf8_get_char(p)) ||
            *p == '{' || *p == '}')
            return invalid(error, "A key name cannot contain whitespace, control characters or section braces");
    return TRUE;
}

static gboolean valid_command(const char *command, GError **error)
{
    if (!command || !*command || !g_utf8_validate(command, -1, NULL) ||
        strpbrk(command, "\r\n"))
        return invalid(error, "A nonempty, single-line command is required");
    const char *p = command;
    while (g_ascii_isspace(*p)) p++;
    if (!*p || *p == '#')
        return invalid(error, "A command cannot be empty or only a comment");
    return TRUE;
}

static gboolean marker(const char *line, const char *text)
{
    gsize length = strcspn(line, "\r\n");
    return strlen(text) == length && strncmp(line, text, length) == 0;
}

static GPtrArray *split_lines(const char *contents)
{
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    while (*contents) {
        const char *end = strchr(contents, '\n');
        gsize length = end ? (gsize)(end - contents) + 1 : strlen(contents);
        g_ptr_array_add(lines, g_strndup(contents, length));
        contents += length;
    }
    return lines;
}

static gboolean find_block(GPtrArray *lines, int *begin, int *end, GError **error)
{
    *begin = *end = -1;
    for (guint i = 0; i < lines->len; ++i) {
        if (marker(g_ptr_array_index(lines, i), BLOCK_BEGIN)) {
            if (*begin >= 0)
                return invalid(error, "input.conf contains duplicate managed key-binding blocks");
            *begin = (int)i;
        } else if (marker(g_ptr_array_index(lines, i), BLOCK_END)) {
            if (*begin < 0 || *end >= 0)
                return invalid(error, "input.conf contains an unmatched managed key-binding marker");
            *end = (int)i;
        }
    }
    if ((*begin >= 0) != (*end >= 0))
        return invalid(error, "input.conf contains an unfinished managed key-binding block");
    return TRUE;
}

static gboolean managed_key(const char *line, const char *key, const char **command)
{
    const char *start = line;
    while (*start == ' ' || *start == '\t') start++;
    const char *end = start;
    while (*end && !g_ascii_isspace(*end)) end++;
    if ((gsize)(end - start) != strlen(key) || strncmp(start, key, (gsize)(end - start)))
        return FALSE;
    while (*end == ' ' || *end == '\t') end++;
    if (strncmp(end, "{default}", 9) || !g_ascii_isspace(end[9]))
        return FALSE;
    end += 9;
    while (*end == ' ' || *end == '\t') end++;
    if (command) *command = end;
    return TRUE;
}

static const char *trailing_comment(const char *command)
{
    char quote = 0;
    gboolean escaped = FALSE;
    for (const char *p = command; *p && *p != '\r' && *p != '\n'; ++p) {
        if (escaped) escaped = FALSE;
        else if (*p == '\\') escaped = TRUE;
        else if (quote && *p == quote) quote = 0;
        else if (!quote && (*p == '\'' || *p == '"')) quote = *p;
        else if (!quote && *p == '#') return p;
    }
    return NULL;
}

static void set_override(GPtrArray *lines, int begin, int *end, const char *key,
                          const char *command, const char *newline)
{
    int last = -1;
    const char *old_command = NULL;
    for (int i = begin + 1; i < *end; ++i)
        if (managed_key(g_ptr_array_index(lines, i), key, &old_command))
            last = i;
    g_autofree char *comment = NULL;
    if (last >= 0 && !trailing_comment(command)) {
        managed_key(g_ptr_array_index(lines, last), key, &old_command);
        const char *start = trailing_comment(old_command);
        if (start) comment = g_strndup(start, strcspn(start, "\r\n"));
    }
    char *line = g_strdup_printf("%s {default} %s%s%s%s", key, command,
        comment ? " " : "", comment ? comment : "", newline);
    if (last >= 0) {
        g_free(g_ptr_array_index(lines, last));
        g_ptr_array_index(lines, last) = line;
    } else {
        g_ptr_array_insert(lines, (guint)*end, line);
        (*end)++;
    }
}

static gboolean change(const char *path, const char *old_key, const char *key,
                         const char *command, gboolean reset, GError **error)
{
    if (!path || !*path)
        return invalid(error, "An input.conf path is required");
    if (!valid_key(key, error) ||
        (old_key && !valid_key(old_key, error)) || (!reset && !valid_command(command, error)))
        return FALSE;
    g_autofree char *directory = g_path_get_dirname(path);
    int lock = g_open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (lock < 0 || flock(lock, LOCK_EX) < 0) {
        int saved_errno = errno;
        if (lock >= 0) close(lock);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "%s: %s", directory, g_strerror(saved_errno));
        return FALSE;
    }
    g_autofree char *contents = NULL;
    gsize length = 0;
    g_autoptr(GError) read_error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        if (!g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) ||
            g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
            g_propagate_error(error, g_steal_pointer(&read_error));
            close(lock);
            return FALSE;
        }
        contents = g_strdup("");
    }
    if (memchr(contents, '\0', length)) {
        close(lock);
        return invalid(error, "input.conf contains a NUL byte; refusing to rewrite it");
    }
    g_autoptr(GPtrArray) lines = split_lines(contents);
    int begin, end;
    if (!find_block(lines, &begin, &end, error)) {
        close(lock);
        return FALSE;
    }
    if (reset && begin < 0) {
        close(lock);
        return TRUE;
    }
    const char *newline = strstr(contents, "\r\n") ? "\r\n" : "\n";
    if (begin < 0) {
        if (lines->len) {
            char *last = g_ptr_array_index(lines, lines->len - 1);
            if (!g_str_has_suffix(last, "\n")) {
                char *terminated = g_strconcat(last, newline, NULL);
                g_free(last);
                g_ptr_array_index(lines, lines->len - 1) = terminated;
            }
        }
        begin = (int)lines->len;
        g_ptr_array_add(lines, g_strconcat(BLOCK_BEGIN, newline, NULL));
        end = (int)lines->len;
        g_ptr_array_add(lines, g_strconcat(BLOCK_END, newline, NULL));
    }
    if (reset) {
        for (int i = end - 1; i > begin; --i)
            if (managed_key(g_ptr_array_index(lines, i), key, NULL))
                g_ptr_array_remove_index(lines, (guint)i);
    } else {
        if (old_key && !g_str_equal(old_key, key))
            set_override(lines, begin, &end, old_key, "ignore", newline);
        set_override(lines, begin, &end, key, command, newline);
    }
    GString *output = g_string_new(NULL);
    for (guint i = 0; i < lines->len; ++i)
        g_string_append(output, g_ptr_array_index(lines, i));
    GStatBuf status;
    int mode = g_stat(path, &status) == 0 ? (int)(status.st_mode & 0777) : 0600;
    gboolean result = g_file_set_contents_full(path, output->str, (gssize)output->len,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, mode, error);
    g_string_free(output, TRUE);
    close(lock);
    return result;
}

gboolean keybindings_set(const char *path, const char *key, const char *command, GError **error)
{
    return change(path, NULL, key, command, FALSE, error);
}

gboolean keybindings_reset(const char *path, const char *key, GError **error)
{
    return change(path, NULL, key, NULL, TRUE, error);
}

gboolean keybindings_rename(const char *path, const char *old_key, const char *key,
                            const char *command, GError **error)
{
    return change(path, old_key, key, command, FALSE, error);
}

char *keybindings_managed_contents(const char *path, GError **error)
{
    g_autofree char *contents = NULL;
    gsize length;
    if (!path || !*path) {
        invalid(error, "An input.conf path is required");
        return NULL;
    }
    if (!g_file_get_contents(path, &contents, &length, error))
        return NULL;
    if (memchr(contents, '\0', length)) {
        invalid(error, "input.conf contains a NUL byte; refusing to parse it");
        return NULL;
    }
    g_autoptr(GPtrArray) lines = split_lines(contents);
    int begin, end;
    if (!find_block(lines, &begin, &end, error))
        return NULL;
    GString *result = g_string_new("# mpv-gtk edited bindings\n");
    for (int i = begin + 1; begin >= 0 && i < end; ++i) {
        const char *line = g_ptr_array_index(lines, i);
        const char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (!*start || *start == '#' || *start == '\r' || *start == '\n') {
            g_string_append(result, line);
            continue;
        }
        const char *key_end = start;
        while (*key_end && !g_ascii_isspace(*key_end)) key_end++;
        g_autofree char *key = g_strndup(start, (gsize)(key_end - start));
        const char *command = NULL;
        if (!managed_key(line, key, &command))
            continue; /* Preserve opaque file lines without changing their section. */
        g_string_append_printf(result, "%s %s", key, command);
        if (!g_str_has_suffix(command, "\n"))
            g_string_append_c(result, '\n');
    }
    return g_string_free(result, FALSE);
}
