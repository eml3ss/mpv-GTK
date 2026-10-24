#define _GNU_SOURCE
#include "config.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_file(const char *path)
{
    char *contents = NULL;
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    return contents;
}

static void save_setting(PlayerConfig *config, const char *name, const char *value)
{
    g_autoptr(GError) error = NULL;
    g_assert_true(config_set(config, name, value, &error));
    g_assert_no_error(error);
}

static void probe_executable(const char *binary, const char *legacy_dir,
                              const char *expected_path)
{
    g_auto(GStrv) environment = g_get_environ();
    environment = g_environ_unsetenv(environment, "MPV_GTK_CONFIG_FILE");
    environment = g_environ_setenv(environment, "XDG_CONFIG_HOME", legacy_dir, TRUE);
    char *arguments[] = {(char *)binary, "--probe", NULL};
    g_autofree char *output = NULL;
    g_autofree char *errors = NULL;
    g_autoptr(GError) error = NULL;
    int status;
    g_assert_true(g_spawn_sync(NULL, arguments, environment, G_SPAWN_DEFAULT,
        NULL, NULL, &output, &errors, &status, &error));
    g_assert_no_error(error);
    g_assert_true(g_spawn_check_wait_status(status, &error));
    g_assert_no_error(error);
    g_autofree char *directory = g_path_get_dirname(expected_path);
    g_autofree char *input_path = g_build_filename(directory, "input.conf", NULL);
    g_autofree char *expected = g_strdup_printf("%s\n%s", expected_path, input_path);
    g_assert_cmpstr(g_strstrip(output), ==, expected);
}

static void write_fixture(const char *path, const char *contents)
{
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
}

static char *renderer_ui(const char *legacy)
{
    return g_strdup_printf("# preserve UI comment\n[Interface]\nlanguage=en\n"
        "cursor-hide-delay=0.5\ncustom=keep\n%s\n[Other]\ngpu-api=keep-other\n", legacy);
}

static void assert_ui_migrated(const char *path)
{
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_autoptr(GError) error = NULL;
    g_assert_true(g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS, &error));
    g_assert_no_error(error);
    const char *obsolete[] = {"video-backend", "gpu-api", "gpu-renderer-api", "gpu-next-api"};
    for (guint i = 0; i < G_N_ELEMENTS(obsolete); i++)
        g_assert_false(g_key_file_has_key(file, "Interface", obsolete[i], NULL));
    g_autofree char *language = g_key_file_get_string(file, "Interface", "language", NULL);
    g_autofree char *custom = g_key_file_get_string(file, "Interface", "custom", NULL);
    g_autofree char *other = g_key_file_get_string(file, "Other", "gpu-api", NULL);
    g_assert_cmpstr(language, ==, "en");
    g_assert_cmpstr(custom, ==, "keep");
    g_assert_cmpstr(other, ==, "keep-other");
    g_assert_cmpfloat(g_key_file_get_double(file, "Interface", "cursor-hide-delay", NULL), ==, 0.5);
    g_autofree char *contents = read_file(path);
    g_assert_nonnull(strstr(contents, "# preserve UI comment"));
}

static void test_renderer_migration(const char *parent)
{
    g_autofree char *directory = g_build_filename(parent, "renderer-migration", NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    g_autofree char *path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *ui_path = g_build_filename(directory, "interface.conf", NULL);
    PlayerConfig config = {.path = g_strdup(path)};
    g_autoptr(GError) error = NULL;
    const char *unchanged = "# actual mpv settings\nvolume=55\n";
    write_fixture(path, unchanged);
    g_assert_true(config_migrate_renderer(&config, &error)); /* No UI file. */
    g_autofree char *no_legacy = renderer_ui("");
    write_fixture(ui_path, no_legacy);
    g_assert_true(config_migrate_renderer(&config, &error));
    g_autofree char *still_default = read_file(path);
    g_assert_cmpstr(still_default, ==, unchanged); /* No invented vo=libmpv. */
    g_autofree char *still_ui = read_file(ui_path);
    g_assert_cmpstr(still_ui, ==, no_legacy);

    const struct {
        const char *legacy;
        const char *vo;
        const char *api;
        const char *es;
    } mappings[] = {
        {"video-backend=gpu-next", "gpu-next", "vulkan", "auto"},
        {"video-backend=gpu-next\ngpu-next-api=opengl", "gpu-next", "opengl", "no"},
        {"video-backend=gpu\ngpu-renderer-api=opengl-es", "gpu", "opengl", "yes"},
        {"video-backend=libmpv\ngpu-api=auto", "libmpv", "auto", "auto"},
        {"video-backend=libmpv\ngpu-api=opengl", "libmpv", "opengl", "no"},
        {"video-backend=gpu\ngpu-renderer-api=vulkan", "gpu", "vulkan", "auto"},
        {"gpu-api=opengl-es", NULL, "opengl", "yes"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(mappings); i++) {
        const char *initial = "# vo=ignored comment\n# include=ignored.conf\nvolume=55\n[quiet]\nvolume=10\n";
        write_fixture(path, initial);
        g_assert_cmpint(g_chmod(path, 0640), ==, 0);
        g_autofree char *ui = renderer_ui(mappings[i].legacy);
        write_fixture(ui_path, ui);
        g_assert_true(config_migrate_renderer(&config, &error));
        g_assert_no_error(error);
        g_autofree char *actual = read_file(path);
        if (mappings[i].vo) {
            g_autofree char *expected = g_strdup_printf("\nvo=%s\n", mappings[i].vo);
            const char *option = strstr(actual, expected);
            g_assert_nonnull(option);
            g_assert_true(option < strstr(actual, "[quiet]"));
        } else {
            g_assert_null(strstr(actual, "\nvo="));
        }
        g_autofree char *api = g_strdup_printf("gpu-api=%s\n", mappings[i].api);
        g_autofree char *es = g_strdup_printf("opengl-es=%s\n", mappings[i].es);
        g_assert_nonnull(strstr(actual, api));
        g_assert_nonnull(strstr(actual, es));
        g_assert_nonnull(strstr(actual, "[quiet]\nvolume=10\n"));
        GStatBuf mode;
        g_assert_cmpint(g_stat(path, &mode), ==, 0);
        g_assert_cmpint(mode.st_mode & 0777, ==, 0640);
        assert_ui_migrated(ui_path);
        g_assert_true(config_migrate_renderer(&config, &error)); /* Idempotent. */
        g_autofree char *again = read_file(path);
        g_assert_cmpstr(actual, ==, again);
    }

    const char *explicit = "# actual settings win, including duplicate/negative forms\n"
        "vo=gpu\nvo=gpu-next\n[default]\nno-gpu-api\nopengl-es=no # chosen\n";
    g_autofree char *legacy = renderer_ui("video-backend=libmpv\ngpu-api=opengl-es\n"
                                         "gpu-next-api=vulkan\ngpu-renderer-api=auto");
    write_fixture(path, explicit);
    write_fixture(ui_path, legacy);
    g_assert_true(config_migrate_renderer(&config, &error));
    g_autofree char *explicit_after = read_file(path);
    g_assert_cmpstr(explicit_after, ==, explicit);
    assert_ui_migrated(ui_path);

    const char *indirect[] = {
        "include=renderer.conf\nvolume=55\n",
        "profile=cinema\n[cinema]\nvo=gpu\n",
        "[cinema]\ngpu-api=vulkan\n",
        "[cinema]\ninclude=renderer.conf\n",
        "[cinema]\nprofile-cond=width > 1920\n",
    };
    for (guint i = 0; i < G_N_ELEMENTS(indirect); i++) {
        write_fixture(path, indirect[i]);
        write_fixture(ui_path, legacy);
        g_assert_true(config_migrate_renderer(&config, &error));
        g_autofree char *actual = read_file(path);
        g_assert_cmpstr(actual, ==, indirect[i]);
        assert_ui_migrated(ui_path);
    }

    /* An inactive per-backend preference cannot invent an active backend. */
    write_fixture(path, unchanged);
    g_autofree char *inactive = renderer_ui("gpu-next-api=vulkan");
    write_fixture(ui_path, inactive);
    g_assert_true(config_migrate_renderer(&config, &error));
    g_autofree char *no_invented_backend = read_file(path);
    g_assert_cmpstr(no_invented_backend, ==, unchanged);

    /* Batch validation and replacement are one transaction. */
    const char *names[] = {"gpu-api", "opengl-es"};
    const char *values[] = {"opengl", "yes"};
    g_assert_true(config_set_options(&config, names, values, 2, &error));
    g_autofree char *batch = read_file(path);
    g_assert_nonnull(strstr(batch, "gpu-api=opengl\nopengl-es=yes\n"));
    const char *bad_names[] = {"volume", "invalid\noption"};
    g_assert_false(config_set_options(&config, bad_names, values, 2, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_autofree char *after_bad_batch = read_file(path);
    g_assert_cmpstr(batch, ==, after_bad_batch);

    if (geteuid() != 0) {
        write_fixture(path, unchanged);
        write_fixture(ui_path, legacy);
        g_assert_cmpint(g_chmod(directory, 0500), ==, 0);
        g_assert_false(config_migrate_renderer(&config, &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        g_autofree char *failed_ui = read_file(ui_path);
        g_autofree char *failed_mpv = read_file(path);
        g_assert_cmpstr(failed_ui, ==, legacy);
        g_assert_cmpstr(failed_mpv, ==, unchanged);
        g_assert_cmpint(g_chmod(directory, 0700), ==, 0);
    }
    g_assert_no_error(error);
    config_clear(&config);
    g_assert_cmpint(g_unlink(ui_path), ==, 0);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_list_settings(const char *parent)
{
    g_autofree char *directory = g_build_filename(parent, "string-lists", NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    g_autofree char *path = g_build_filename(directory, "mpv.conf", NULL);
    PlayerConfig config = {.path = g_strdup(path)};
    g_autoptr(GError) error = NULL;
    const char *original =
        "# keep header\r\n"
        "  video-exts=old # base note\r\n"
        "video-exts-append=added # append note\r\n"
        "video-exts-custom=untouched\r\n"
        "include=unchanged.conf\r\n"
        "video-exts-clr=yes\r\n"
        "[cinema]\r\nvideo-exts=profile\r\nvideo-exts-clr=yes\r\n"
        "[default]\r\n"
        " --video-exts-pre=first # prepend note\r\n"
        "video-exts-set=replace\r\nvideo-exts-toggle=other\r\n"
        "video-exts-del=old # deletion note\r\nvideo-exts-remove=other\r\n"
        "video-exts-add=last # last note\r\n";
    const char *cleared =
        "# keep header\r\n"
        "  # base note\r\n"
        "# append note\r\n"
        "video-exts-custom=untouched\r\n"
        "include=unchanged.conf\r\n"
        "[cinema]\r\nvideo-exts=profile\r\nvideo-exts-clr=yes\r\n"
        "[default]\r\n"
        " # prepend note\r\n"
        "# deletion note\r\n"
        "video-exts-clr=yes # last note\r\n";
    write_fixture(path, original);
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
    g_assert_true(config_set_list(&config, "video-exts", "", &error));
    g_assert_no_error(error);
    g_autofree char *empty = read_file(path);
    g_assert_cmpstr(empty, ==, cleared);
    GStatBuf mode;
    g_assert_cmpint(g_stat(path, &mode), ==, 0);
    g_assert_cmpint(mode.st_mode & 0777, ==, 0640);
    g_assert_true(config_set_list(&config, "video-exts", "mp4,mkv", &error));
    g_autofree char *nonempty = read_file(path);
    g_assert_nonnull(strstr(nonempty, "video-exts=\"mp4,mkv\" # last note\r\n"));
    g_assert_nonnull(strstr(nonempty, "[cinema]\r\nvideo-exts=profile\r\nvideo-exts-clr=yes\r\n"));
    g_assert_true(config_set_list(&config, "video-exts", "mp4,mkv", &error));
    g_autofree char *again = read_file(path);
    g_assert_cmpstr(again, ==, nonempty); /* No accumulated history or aliases. */
    g_assert_true(config_set_list(&config, "video-exts", "", &error));
    g_autofree char *empty_again = read_file(path);
    g_assert_cmpstr(empty_again, ==, cleared);

    write_fixture(path,
        "audio-exts=old # canonical\n"
        "audio-file-auto-exts-append=flac # legacy\n"
        "[profile]\naudio-file-auto-exts=keep-profile\n");
    g_assert_true(config_set_list(&config, "audio-file-auto-exts", "aac,opus", &error));
    g_autofree char *alias = read_file(path);
    g_assert_cmpstr(alias, ==,
        "# canonical\naudio-exts=\"aac,opus\" # legacy\n"
        "[profile]\naudio-file-auto-exts=keep-profile\n");
    write_fixture(path, "cover-art-auto-exts=jpg\nimage-exts-clr=yes # image note\n");
    g_assert_true(config_set_list(&config, "image-exts", "png", &error));
    g_autofree char *image = read_file(path);
    g_assert_cmpstr(image, ==, "image-exts=png # image note\n");
    write_fixture(path, "# no global family\n[profile]\narchive-exts=zip\n");
    g_assert_true(config_set_list(&config, "archive-exts", "", &error));
    g_autofree char *inserted = read_file(path);
    g_assert_cmpstr(inserted, ==,
        "# no global family\narchive-exts-clr=yes\n[profile]\narchive-exts=zip\n");
    g_assert_false(config_set_list(&config, "video-exts", "mp4\nvo=null", &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_autofree char *after_invalid = read_file(path);
    g_assert_cmpstr(after_invalid, ==, inserted);
    const char binary[] = "video-exts=mp4\n\0keep trailing bytes\n";
    g_assert_true(g_file_set_contents(path, binary, sizeof(binary) - 1, &error));
    g_assert_false(config_set_list(&config, "video-exts", "", &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_autofree char *unchanged = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents(path, &unchanged, &length, &error));
    g_assert_cmpmem(unchanged, length, binary, sizeof(binary) - 1);
    if (geteuid() != 0) {
        write_fixture(path, original);
        g_assert_cmpint(g_chmod(directory, 0500), ==, 0);
        g_assert_false(config_set_list(&config, "video-exts", "mkv", &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        g_autofree char *failed = read_file(path);
        g_assert_cmpstr(failed, ==, original);
        g_assert_cmpint(g_chmod(directory, 0700), ==, 0);
    }
    config_clear(&config);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_no_error(error);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    if (argc == 2 && g_str_equal(argv[1], "--probe")) {
        PlayerConfig config = {0};
        if (!config_init(&config, &error)) {
            g_printerr("%s\n", error->message);
            return 1;
        }
        g_print("%s\n%s\n", config_path(&config), config_input_path(&config));
        config_clear(&config);
        return 0;
    }
    g_autofree char *temporary = g_dir_make_tmp("mpv-gtk-config-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(temporary, "mpv.conf", NULL);
    g_autofree char *input_path = g_build_filename(temporary, "input.conf", NULL);
    g_assert_true(g_setenv("MPV_GTK_CONFIG_FILE", path, TRUE));
    PlayerConfig config = {0};
    g_assert_true(config_init(&config, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(config_path(&config), ==, path);
    g_assert_cmpstr(config_input_path(&config), ==, input_path);
    g_assert_null(config_input_path(NULL));
    g_autofree char *initial = read_file(path);
    g_autofree char *seed = read_file("data/mpv.conf");
    g_assert_cmpstr(initial, ==, seed);
    g_autofree char *initial_input = read_file(input_path);
    g_autofree char *input_seed = read_file("data/input.conf");
    g_assert_cmpstr(initial_input, ==, input_seed);
    g_auto(GStrv) input_lines = g_strsplit(initial_input, "\n", -1);
    for (guint i = 0; input_lines[i]; i++) {
        const char *line = g_strstrip(input_lines[i]);
        g_assert_true(!*line || *line == '#');
    }
    GStatBuf mode;
    g_assert_cmpint(g_stat(path, &mode), ==, 0);
    g_assert_cmpint(mode.st_mode & 0777, ==, 0600);
    g_assert_cmpint(g_stat(input_path, &mode), ==, 0);
    g_assert_cmpint(mode.st_mode & 0777, ==, 0600);
    const char *custom_input = "# custom keys stay intact\np cycle pause\n";
    g_assert_true(g_file_set_contents(input_path, custom_input, -1, &error));

    const char *original =
        "# untouched header\r\n"
        "hwdec=no # earlier duplicate\r\n"
        "unknown-option=custom # retain me\r\n"
        "  hwdec=auto-safe   # GPU choice\r\n"
        "no-deband # alias\r\n"
        "[film]\r\n"
        "hwdec=vaapi\r\n"
        "# profile comment\r\n";
    g_assert_true(g_file_set_contents(path, original, -1, &error));
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
    save_setting(&config, "hwdec", "auto");
    save_setting(&config, "deband", "yes");
    save_setting(&config, "sub-scale", "1.25");
    g_autofree char *changed = read_file(path);
    g_assert_nonnull(strstr(changed, "hwdec=no # earlier duplicate\r\n"));
    g_assert_nonnull(strstr(changed, "  hwdec=auto   # GPU choice\r\n"));
    g_assert_nonnull(strstr(changed, "unknown-option=custom # retain me\r\n"));
    g_assert_nonnull(strstr(changed, "deband=yes # alias\r\n"));
    g_assert_nonnull(strstr(changed, "sub-scale=1.25\r\n[film]\r\nhwdec=vaapi"));
    g_assert_cmpint(g_stat(path, &mode), ==, 0);
    g_assert_cmpint(mode.st_mode & 0777, ==, 0640);
    config_clear(&config);
    g_assert_true(config_init(&config, &error));
    g_autofree char *reopened = read_file(path);
    g_assert_cmpstr(changed, ==, reopened);
    g_autofree char *reopened_input = read_file(input_path);
    g_assert_cmpstr(reopened_input, ==, custom_input);

    const char *profiles = "# header\n[film]\nhwdec=vaapi\n[default]\nhwdec=no\n";
    g_assert_true(g_file_set_contents(path, profiles, -1, &error));
    save_setting(&config, "hwdec", "auto-safe");
    g_autofree char *profile_result = read_file(path);
    g_assert_cmpstr(profile_result, ==,
        "# header\n[film]\nhwdec=vaapi\n[default]\nhwdec=auto-safe\n");
    save_setting(&config, "title", "a # \"quoted\" value");
    g_autofree char *quoted = read_file(path);
    g_assert_nonnull(strstr(quoted, "title=\"a # \\\"quoted\\\" value\"\n"));

    g_assert_false(config_set(&config, "name\ninjected", "yes", &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_assert_false(config_set(&config, "hwdec", "auto\nvo=null", &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    const char binary[] = "hwdec=no\0unknown=yes\n";
    g_assert_true(g_file_set_contents(path, binary, sizeof(binary) - 1, &error));
    g_assert_false(config_set(&config, "hwdec", "auto", &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_autofree char *unchanged = NULL;
    gsize unchanged_size;
    g_assert_true(g_file_get_contents(path, &unchanged, &unchanged_size, &error));
    g_assert_cmpmem(unchanged, unchanged_size, binary, sizeof(binary) - 1);
    config_clear(&config);

    /* Execute a real copied binary through a symlink. Config belongs beside
     * /proc/self/exe, rather than the symlink or the current working directory. */
    g_autofree char *actual = g_build_filename(temporary, "actual", NULL);
    g_autofree char *legacy = g_build_filename(temporary, "legacy", NULL);
    g_autofree char *legacy_app = g_build_filename(legacy, "mpv-gtk", NULL);
    g_autofree char *legacy_file = g_build_filename(legacy_app, "mpv.conf", NULL);
    g_autofree char *legacy_input = g_build_filename(legacy_app, "input.conf", NULL);
    g_assert_cmpint(g_mkdir_with_parents(actual, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(legacy_app, 0700), ==, 0);
    g_assert_true(g_file_set_contents(legacy_file, "# migrated\nhwdec=no\n", -1, &error));
    g_assert_true(g_file_set_contents(legacy_input, "# migrated keys\nCtrl+x cycle pause\n", -1, &error));
    g_autofree char *self = g_file_read_link("/proc/self/exe", &error);
    g_autofree char *executable = NULL;
    gsize executable_size;
    g_assert_true(g_file_get_contents(self, &executable, &executable_size, &error));
    g_autofree char *copied = g_build_filename(actual, "player", NULL);
    g_assert_true(g_file_set_contents_full(copied, executable, (gssize)executable_size,
        G_FILE_SET_CONTENTS_CONSISTENT, 0700, &error));
    g_autofree char *alias = g_build_filename(temporary, "launcher", NULL);
    g_assert_cmpint(symlink(copied, alias), ==, 0);
    g_autofree char *real_config = g_build_filename(actual, "mpv.conf", NULL);
    g_autofree char *real_input = g_build_filename(actual, "input.conf", NULL);
    probe_executable(alias, legacy, real_config);
    g_autofree char *migrated = read_file(real_config);
    g_assert_cmpstr(migrated, ==, "# migrated\nhwdec=no\n");
    g_autofree char *migrated_input = read_file(real_input);
    g_assert_cmpstr(migrated_input, ==, "# migrated keys\nCtrl+x cycle pause\n");
    g_assert_true(g_file_set_contents(real_config, "# keep existing\n", -1, &error));
    g_assert_true(g_file_set_contents(real_input, "# keep existing keys\nm cycle pause\n", -1, &error));
    probe_executable(alias, legacy, real_config);
    g_autofree char *kept = read_file(real_config);
    g_assert_cmpstr(kept, ==, "# keep existing\n");
    g_autofree char *kept_input = read_file(real_input);
    g_assert_cmpstr(kept_input, ==, "# keep existing keys\nm cycle pause\n");
    g_assert_cmpint(g_unlink(alias), ==, 0);
    g_assert_cmpint(g_unlink(copied), ==, 0);
    g_assert_cmpint(g_unlink(real_config), ==, 0);
    g_assert_cmpint(g_unlink(real_input), ==, 0);
    g_assert_cmpint(g_unlink(legacy_file), ==, 0);
    g_assert_cmpint(g_unlink(legacy_input), ==, 0);
    g_assert_cmpint(g_rmdir(legacy_app), ==, 0);
    g_assert_cmpint(g_rmdir(legacy), ==, 0);
    g_assert_cmpint(g_rmdir(actual), ==, 0);
    test_renderer_migration(temporary);
    test_list_settings(temporary);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_unlink(input_path), ==, 0);
    g_assert_cmpint(g_rmdir(temporary), ==, 0);
    g_assert_no_error(error);
    puts("PASS config: defaults/bindings, comments/profiles, renderer migration, atomic batches, "
         "complete/empty string lists, legacy aliases, permissions and failure preservation");
    return 0;
}
