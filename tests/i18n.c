#include "i18n.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char *read_file(const char *path)
{
    char *contents = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    return contents;
}

static void check_lazy_catalog(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *executable = g_file_read_link("/proc/self/exe", &error);
    g_assert_no_error(error);
    g_autofree char *directory = g_path_get_dirname(executable);
    g_autofree char *locale_directory = g_build_filename(directory, "locales", NULL);
    g_autofree char *path = g_build_filename(locale_directory, "en.ini", NULL);
    const char *first = "[0]\nsource=Настройки\ntranslation=Settings\n\n"
                        "[1]\nsource=Видео\ntranslation=Video\n";
    const char *second = "[0]\nsource=Настройки\ntranslation=Updated settings\n";
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    g_assert_cmpstr(tr("Настройки"), ==, "Настройки");
    i18n_set_language("en");
    /* Selection alone must not load (or cache the absence of) a catalogue. */
    g_assert_cmpint(g_mkdir(locale_directory, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, first, -1, &error));
    const char *retained = tr("Настройки");
    g_assert_cmpstr(retained, ==, "Settings");
    g_assert_cmpstr(tr("Видео"), ==, "Video");

    i18n_set_language("ru");
    g_assert_cmpstr(tr("Настройки"), ==, "Настройки");
    g_assert_cmpstr(retained, ==, "Settings");
    g_assert_true(g_file_set_contents(path, second, -1, &error));
    i18n_set_language("en");
    g_assert_cmpstr(tr("Настройки"), ==, "Updated settings");
    g_assert_cmpstr(retained, ==, "Settings");

    /* Unloading removes the catalogue, not previously returned text. Missing
     * and malformed files fall back to Russian without using the old table. */
    i18n_set_language("ru");
    g_assert_cmpint(g_unlink(path), ==, 0);
    i18n_set_language("en");
    g_assert_cmpstr(tr("Настройки"), ==, "Настройки");
    g_assert_true(g_file_set_contents(path, first, -1, &error));
    g_assert_cmpstr(tr("Настройки"), ==, "Настройки");
    i18n_set_language("ru");
    i18n_set_language("en");
    g_assert_cmpstr(tr("Настройки"), ==, "Settings");
    const char *invalid[] = {
        "malformed ini content",
        "[0]\nsource=Настройки\n",
        "[0]\nsource=Настройки\ntranslation=\xff\n"
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        i18n_set_language("ru");
        g_assert_true(g_file_set_contents(path, invalid[i], -1, &error));
        i18n_set_language("en");
        g_assert_cmpstr(tr("Настройки"), ==, "Настройки");
        g_assert_cmpstr(retained, ==, "Settings");
    }
    i18n_set_language("ru");
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(locale_directory), ==, 0);
    g_assert_no_error(error);
}

static void check_relocated_catalog(const char *directory)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *binary = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents("/proc/self/exe", &binary, &length, &error));
    g_autofree char *fixture = g_build_filename(directory, "test-i18n-relocated", NULL);
    g_assert_true(g_file_set_contents(fixture, binary, (gssize)length, &error));
    g_assert_cmpint(g_chmod(fixture, 0700), ==, 0);
    char *arguments[] = {fixture, "--check-lazy-catalog", NULL};
    int status = 0;
    /* Deliberately launch outside the executable/resource directory. */
    g_assert_true(g_spawn_sync("/", arguments, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, NULL, NULL, &status, &error));
    g_assert_true(g_spawn_check_wait_status(status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_unlink(fixture), ==, 0);
}

static void check_load_policy(const char *directory, const char *mpv_path, const char *ui_path)
{
    g_autoptr(GError) error = NULL;
    const char *custom = "# keep policy comment\n[Interface]\nlanguage=en\n"
        "cursor-hide-delay=2.4\nallow-replace=false\nadd-to-queue=true\n"
        "future-option=keep\n[Other]\ncustom=yes\n";
    g_assert_true(g_file_set_contents(ui_path, custom, -1, &error));
    g_assert_cmpint(g_chmod(ui_path, 0640), ==, 0);
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_false(i18n_allow_replace());
    g_assert_true(i18n_add_to_queue());
    for (guint replace = 0; replace < 2; replace++) {
        for (guint append = 0; append < 2; append++) {
            g_assert_true(i18n_save_allow_replace(replace, &error));
            g_assert_true(i18n_save_add_to_queue(append, &error));
            g_assert_cmpint(i18n_allow_replace(), ==, replace);
            g_assert_cmpint(i18n_add_to_queue(), ==, append);
            g_assert_true(i18n_init(mpv_path, &error));
            g_assert_cmpint(i18n_allow_replace(), ==, replace);
            g_assert_cmpint(i18n_add_to_queue(), ==, append);
            g_assert_cmpstr(i18n_language(), ==, "en");
            g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 2.4);
        }
    }
    g_autofree char *saved = read_file(ui_path);
    g_assert_nonnull(strstr(saved, "# keep policy comment"));
    g_assert_nonnull(strstr(saved, "future-option=keep"));
    g_assert_nonnull(strstr(saved, "[Other]\ncustom=yes"));
    GStatBuf mode;
    g_assert_cmpint(g_stat(ui_path, &mode), ==, 0);
    g_assert_cmpint(mode.st_mode & 0777, ==, 0640);

    const struct { const char *replace, *append; gboolean expected_replace, expected_append; } invalid[] = {
        {"bad", "true", TRUE, TRUE},
        {"false", "bad", FALSE, FALSE},
        {"", "", TRUE, FALSE}
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autofree char *contents = g_strdup_printf(
            "[Interface]\nallow-replace=%s\nadd-to-queue=%s\n",
            invalid[i].replace, invalid[i].append);
        g_assert_true(g_file_set_contents(ui_path, contents, -1, &error));
        g_assert_true(i18n_init(mpv_path, &error));
        g_assert_cmpint(i18n_allow_replace(), ==, invalid[i].expected_replace);
        g_assert_cmpint(i18n_add_to_queue(), ==, invalid[i].expected_append);
    }

    g_assert_true(g_file_set_contents(ui_path, custom, -1, &error));
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_true(g_file_set_contents(ui_path, "malformed ini", -1, &error));
    g_assert_false(i18n_save_allow_replace(TRUE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(i18n_save_add_to_queue(FALSE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(i18n_allow_replace());
    g_assert_true(i18n_add_to_queue());
    g_autofree char *malformed = read_file(ui_path);
    g_assert_cmpstr(malformed, ==, "malformed ini");
    const char binary[] = "[Interface]\nallow-replace=false\n\0keep unknown bytes\n";
    g_assert_true(g_file_set_contents(ui_path, binary, sizeof(binary) - 1, &error));
    g_assert_false(i18n_save_allow_replace(TRUE, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_assert_false(i18n_allow_replace());
    g_assert_true(i18n_add_to_queue());
    g_autofree char *unchanged = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents(ui_path, &unchanged, &length, &error));
    g_assert_cmpmem(unchanged, length, binary, sizeof(binary) - 1);
    if (geteuid() != 0) {
        g_assert_true(g_file_set_contents(ui_path, custom, -1, &error));
        g_assert_cmpint(g_chmod(directory, 0500), ==, 0);
        g_assert_false(i18n_save_allow_replace(TRUE, &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        g_assert_false(i18n_save_add_to_queue(FALSE, &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        g_assert_false(i18n_allow_replace());
        g_assert_true(i18n_add_to_queue());
        g_autofree char *failed = read_file(ui_path);
        g_assert_cmpstr(failed, ==, custom);
        g_assert_cmpint(g_chmod(directory, 0700), ==, 0);
    }
    g_assert_cmpint(g_unlink(ui_path), ==, 0);
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_true(i18n_allow_replace());
    g_assert_false(i18n_add_to_queue());
    g_assert_true(i18n_save_add_to_queue(TRUE, &error));
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_true(i18n_allow_replace());
    g_assert_true(i18n_add_to_queue());
    g_assert_no_error(error);
}

int main(int argc, char **argv)
{
    if (argc == 2 && g_str_equal(argv[1], "--check-lazy-catalog")) {
        check_lazy_catalog();
        return 0;
    }
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-i18n-XXXXXX", &error);
    g_assert_no_error(error);
    check_relocated_catalog(directory);
    g_autofree char *mpv_path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *ui_path = g_build_filename(directory, "interface.conf", NULL);
    const char *mpv_contents = "# video settings remain separate\nhwdec=auto-safe\n";
    g_assert_true(g_file_set_contents(mpv_path, mpv_contents, -1, &error));
    g_assert_true(g_setenv("LANG", "en_US.UTF-8", TRUE));
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 3.0);
    g_assert_true(i18n_allow_replace());
    g_assert_false(i18n_add_to_queue());
    g_assert_cmpstr(tr("Настройки"), ==, "Настройки");
    g_assert_false(g_file_test(ui_path, G_FILE_TEST_EXISTS));

    g_assert_true(i18n_save_language("en", &error));
    g_assert_no_error(error);
    /* The host refreshes widgets only after saving has succeeded. */
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_cmpstr(i18n_language(), ==, "en");
    g_assert_cmpstr(tr("Настройки"), ==, "Settings");
    g_assert_cmpstr(tr("Субтитры"), ==, "Subtitles");
    g_assert_cmpstr(tr("Аппаратное декодирование"), ==, "Hardware decoding");
    g_assert_cmpstr(tr("Информация о файле"), ==, "File information");
    g_assert_cmpstr(tr("Скрывать курсор через"), ==, "Hide cursor after");
    g_autofree char *queue = g_strdup_printf(tr("Очередь · %d"), 7);
    g_assert_cmpstr(queue, ==, "Queue · 7");
    const char *unknown = "Непереведённая строка · Ω";
    g_assert_true(tr(unknown) == unknown);
    g_assert_true(g_utf8_validate(tr("Звук · M\nПКМ — аудиодорожки"), -1, NULL));
    g_assert_cmpstr(tr(NULL), ==, "");
    GStatBuf status;
    g_assert_cmpint(g_stat(ui_path, &status), ==, 0);
    g_assert_cmpint(status.st_mode & 0777, ==, 0600);

    const char *custom = "# retain UI comment\n[Interface]\nlanguage=en\n"
                         "future-option=keep\n\n[Other]\ncustom=yes\n";
    g_assert_true(g_file_set_contents(ui_path, custom, -1, &error));
    g_assert_cmpint(g_chmod(ui_path, 0640), ==, 0);
    g_assert_true(i18n_save_language("ru", &error));
    g_autofree char *saved = read_file(ui_path);
    g_assert_nonnull(strstr(saved, "# retain UI comment"));
    g_assert_nonnull(strstr(saved, "language=ru"));
    g_assert_nonnull(strstr(saved, "future-option=keep"));
    g_assert_nonnull(strstr(saved, "[Other]\ncustom=yes"));
    g_assert_cmpint(g_stat(ui_path, &status), ==, 0);
    g_assert_cmpint(status.st_mode & 0777, ==, 0640);
    g_assert_true(i18n_save_cursor_hide_delay(1.7, &error));
    g_assert_no_error(error);
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.7);
    g_clear_pointer(&saved, g_free);
    saved = read_file(ui_path);
    g_assert_nonnull(strstr(saved, "# retain UI comment"));
    g_assert_nonnull(strstr(saved, "language=ru"));
    g_assert_nonnull(strstr(saved, "future-option=keep"));
    g_assert_nonnull(strstr(saved, "[Other]\ncustom=yes"));
    g_assert_cmpint(g_stat(ui_path, &status), ==, 0);
    g_assert_cmpint(status.st_mode & 0777, ==, 0640);
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.7);
    g_assert_cmpstr(tr("Настройки"), ==, "Настройки");
    g_assert_true(i18n_save_language("en", &error));
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_cmpstr(i18n_language(), ==, "en");
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.7);
    g_assert_true(i18n_save_language("ru", &error));
    g_assert_true(i18n_init(mpv_path, &error));
    g_clear_pointer(&saved, g_free);
    saved = read_file(ui_path);
    g_autofree char *mpv_after = read_file(mpv_path);
    g_assert_cmpstr(mpv_after, ==, mpv_contents);

    g_assert_false(i18n_save_language("de", &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_autofree char *after_invalid = read_file(ui_path);
    g_assert_cmpstr(after_invalid, ==, saved);
    const double invalid_delays[] = {0, -1, 0.09, 10.01, NAN, INFINITY, -INFINITY};
    for (guint i = 0; i < G_N_ELEMENTS(invalid_delays); ++i) {
        g_assert_false(i18n_save_cursor_hide_delay(invalid_delays[i], &error));
        g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
        g_clear_error(&error);
        g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.7);
        g_autofree char *unchanged = read_file(ui_path);
        g_assert_cmpstr(unchanged, ==, saved);
    }
    const double boundaries[] = {0.1, 10.0};
    for (guint i = 0; i < G_N_ELEMENTS(boundaries); ++i) {
        g_assert_true(i18n_save_cursor_hide_delay(boundaries[i], &error));
        g_assert_true(i18n_init(mpv_path, &error));
        g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, boundaries[i]);
    }
    /* A sequence of 0.1 steps need not produce an exact decimal double.
     * Persist the same one-decimal value the control displays and applies. */
    double stepped = 0.1;
    for (guint i = 0; i < 9; ++i)
        stepped += 0.1;
    g_assert_true(i18n_save_cursor_hide_delay(stepped, &error));
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.0);
    g_autofree char *canonical = read_file(ui_path);
    g_assert_nonnull(strstr(canonical, "cursor-hide-delay=1.0\n"));
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.0);
    g_assert_true(i18n_save_cursor_hide_delay(1.74, &error));
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.7);
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.7);
    const char *invalid_stored[] = {"bad", "", "0", "-1", "10.1", "nan", "inf", "-inf"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid_stored); ++i) {
        g_autofree char *contents = g_strdup_printf(
            "[Interface]\nlanguage=en\ncursor-hide-delay=%s\n", invalid_stored[i]);
        g_assert_true(g_file_set_contents(ui_path, contents, -1, &error));
        g_assert_true(i18n_init(mpv_path, &error));
        g_assert_cmpstr(i18n_language(), ==, "en");
        g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 3.0);
    }
    i18n_set_language("en");
    g_assert_cmpstr(tr("Видео"), ==, "Video");
    i18n_set_language("unsupported");
    g_assert_cmpstr(i18n_language(), ==, "ru");
    i18n_set_language(NULL);
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_true(g_file_set_contents(ui_path, "[Interface]\nlanguage=invalid\n", -1, &error));
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 3.0);
    g_assert_true(g_file_set_contents(ui_path, "[Other]\ncustom=yes\n", -1, &error));
    g_assert_true(i18n_init(mpv_path, &error));
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 3.0);

    g_assert_true(i18n_save_cursor_hide_delay(2.5, &error));
    g_assert_true(g_file_set_contents(ui_path, "malformed ini content", -1, &error));
    g_assert_false(i18n_save_cursor_hide_delay(4.0, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 2.5);
    g_assert_false(i18n_init(mpv_path, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpstr(i18n_language(), ==, "ru");
    g_assert_false(i18n_save_language("en", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_autofree char *malformed = read_file(ui_path);
    g_assert_cmpstr(malformed, ==, "malformed ini content");
    g_autofree char *missing_parent = g_build_filename(directory, "missing", "mpv.conf", NULL);
    g_assert_true(i18n_init(missing_parent, &error));
    g_assert_false(i18n_save_cursor_hide_delay(4.0, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 3.0);
    g_assert_false(i18n_init(NULL, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_assert_false(i18n_save_cursor_hide_delay(4.0, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_assert_false(i18n_save_language("en", &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);

    g_assert_false(i18n_save_allow_replace(FALSE, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_assert_false(i18n_save_add_to_queue(TRUE, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    check_load_policy(directory, mpv_path, ui_path);

    g_assert_cmpint(g_unlink(ui_path), ==, 0);
    g_assert_cmpint(g_unlink(mpv_path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS i18n: lazy relocated RU/EN catalogue, language switching, stable pointers, "
         "UI/cursor/load-policy persistence, independent boolean defaults, permissions and save rollback");
    return 0;
}
