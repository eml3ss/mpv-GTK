#define _GNU_SOURCE
#include "i18n.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *language = "ru";
static char *interface_path;
static double cursor_hide_delay = 3.0;
static gboolean allow_replace = TRUE;
static gboolean add_to_queue;
static GHashTable *translations;
static gboolean translations_attempted;

/* Resources travel with the real executable, including when launched through
 * ~/.local/bin/mpv-gtk or from a different working directory. */
static char *translation_path(void)
{
    g_autofree char *executable = g_file_read_link("/proc/self/exe", NULL);
    if (!executable)
        return NULL;
    g_autofree char *directory = g_path_get_dirname(executable);
    return g_build_filename(directory, "locales", "en.ini", NULL);
}

static void load_translations(void)
{
    if (translations_attempted)
        return;
    translations_attempted = TRUE;
    g_autofree char *path = translation_path();
    g_autoptr(GError) error = NULL;
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!path || !g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error))
        return;

    g_autoptr(GHashTable) catalog = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_auto(GStrv) groups = g_key_file_get_groups(file, NULL);
    for (guint i = 0; groups[i]; ++i) {
        g_autofree char *source = g_key_file_get_string(file, groups[i], "source", &error);
        if (!source || !*source || !g_utf8_validate(source, -1, NULL))
            return;
        g_autofree char *translation = g_key_file_get_string(file, groups[i], "translation", &error);
        if (!translation || !*translation || !g_utf8_validate(translation, -1, NULL))
            return;
        g_hash_table_insert(catalog, g_steal_pointer(&source), g_steal_pointer(&translation));
    }
    translations = g_steal_pointer(&catalog);
}

static gboolean valid_cursor_hide_delay(double seconds)
{
    return isfinite(seconds) && seconds >= 0.1 && seconds <= 10.0;
}

static double normalized_cursor_hide_delay(double seconds)
{
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(text, sizeof(text), "%.1f", seconds);
    return g_ascii_strtod(text, NULL);
}

static gboolean valid_language(const char *code)
{
    return g_strcmp0(code, "ru") == 0 || g_strcmp0(code, "en") == 0;
}

const char *tr(const char *russian)
{
    if (!russian)
        return "";
    if (g_str_equal(language, "en")) {
        load_translations();
        const char *translation = translations
            ? g_hash_table_lookup(translations, russian) : NULL;
        /* Only text actually requested by widgets stays valid across language
         * switches; the full inactive catalogue can be released. */
        if (translation)
            return g_intern_string(translation);
    }
    return russian;
}

void i18n_set_language(const char *code)
{
    const char *selected = g_strcmp0(code, "en") == 0 ? "en" : "ru";
    if (g_str_equal(language, selected))
        return;
    g_clear_pointer(&translations, g_hash_table_unref);
    translations_attempted = FALSE;
    language = selected;
}

const char *i18n_language(void)
{
    return language;
}

static gboolean load_file(GKeyFile *file, GError **error)
{
    g_autoptr(GError) local_error = NULL;
    g_autofree char *contents = NULL;
    gsize length = 0;
    if (g_file_get_contents(interface_path, &contents, &length, &local_error)) {
        if (memchr(contents, '\0', length)) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                        "%s contains a NUL byte; refusing to rewrite it", interface_path);
            return FALSE;
        }
        return g_key_file_load_from_data(file, contents, length,
            G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, error);
    }
    if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
        !g_file_test(interface_path, G_FILE_TEST_IS_SYMLINK))
        return TRUE;
    g_propagate_error(error, g_steal_pointer(&local_error));
    return FALSE;
}

static gboolean load_boolean(GKeyFile *file, const char *key, gboolean fallback)
{
    g_autoptr(GError) error = NULL;
    gboolean value = g_key_file_get_boolean(file, "Interface", key, &error);
    return error ? fallback : value;
}

gboolean i18n_init(const char *mpv_config_path, GError **error)
{
    i18n_set_language("ru");
    cursor_hide_delay = 3.0;
    allow_replace = TRUE;
    add_to_queue = FALSE;
    g_clear_pointer(&interface_path, g_free);
    if (!mpv_config_path || !*mpv_config_path) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "A player config path is required for UI preferences");
        return FALSE;
    }
    g_autofree char *directory = g_path_get_dirname(mpv_config_path);
    interface_path = g_build_filename(directory, "interface.conf", NULL);
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!load_file(file, error))
        return FALSE;
    g_autofree char *code = g_key_file_get_string(file, "Interface", "language", NULL);
    i18n_set_language(code);
    double delay = g_key_file_get_double(file, "Interface", "cursor-hide-delay", NULL);
    if (valid_cursor_hide_delay(delay))
        cursor_hide_delay = normalized_cursor_hide_delay(delay);
    allow_replace = load_boolean(file, "allow-replace", TRUE);
    add_to_queue = load_boolean(file, "add-to-queue", FALSE);
    return TRUE;
}

static gboolean save_file(GKeyFile *file, GError **error)
{
    gsize length;
    g_autofree char *contents = g_key_file_to_data(file, &length, error);
    if (!contents)
        return FALSE;
    GStatBuf status;
    int mode = g_stat(interface_path, &status) == 0 ? (int)(status.st_mode & 0777) : 0600;
    return g_file_set_contents_full(interface_path, contents, (gssize)length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, mode, error);
}

static gboolean save_value(const char *key, const char *value, GError **error)
{
    g_autofree char *directory = g_path_get_dirname(interface_path);
    int lock = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int result = -1;
    if (lock >= 0) {
        do {
            result = flock(lock, LOCK_EX);
        } while (result < 0 && errno == EINTR);
    }
    if (lock < 0 || result < 0) {
        int saved_errno = errno;
        if (lock >= 0)
            close(lock);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "%s: %s", directory, g_strerror(saved_errno));
        return FALSE;
    }
    g_autoptr(GKeyFile) file = g_key_file_new();
    gboolean saved = load_file(file, error);
    if (saved) {
        g_key_file_set_value(file, "Interface", key, value);
        saved = save_file(file, error);
    }
    close(lock);
    return saved;
}

gboolean i18n_save_language(const char *code, GError **error)
{
    if (!interface_path || !valid_language(code)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "Initialize UI preferences and select ru or en before saving");
        return FALSE;
    }
    return save_value("language", code, error);
}

double i18n_cursor_hide_delay(void)
{
    return cursor_hide_delay;
}

gboolean i18n_save_cursor_hide_delay(double seconds, GError **error)
{
    if (!interface_path || !valid_cursor_hide_delay(seconds)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "Initialize UI preferences and select a cursor delay from 0.1 to 10 seconds before saving");
        return FALSE;
    }
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(text, sizeof(text), "%.1f", seconds);
    if (!save_value("cursor-hide-delay", text, error))
        return FALSE;
    cursor_hide_delay = g_ascii_strtod(text, NULL);
    return TRUE;
}

gboolean i18n_allow_replace(void)
{
    return allow_replace;
}

gboolean i18n_add_to_queue(void)
{
    return add_to_queue;
}

static gboolean save_boolean(const char *key, gboolean enabled, gboolean *cached,
                              GError **error)
{
    if (!interface_path) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "Initialize UI preferences before saving load policy");
        return FALSE;
    }
    enabled = !!enabled;
    if (!save_value(key, enabled ? "true" : "false", error))
        return FALSE;
    *cached = enabled;
    return TRUE;
}

gboolean i18n_save_allow_replace(gboolean enabled, GError **error)
{
    return save_boolean("allow-replace", enabled, &allow_replace, error);
}

gboolean i18n_save_add_to_queue(gboolean enabled, GError **error)
{
    return save_boolean("add-to-queue", enabled, &add_to_queue, error);
}

