#include "../src/keybindings.h"

#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <string.h>

typedef struct {
    mpv_handle *mpv;
    double volume;
    uint64_t sent, replied;
} Engine;

static const KeyBindingDefault defaults[] = {
    {"SPACE", "set volume 19", "Frontend play action"},
    {"z", "set volume 7", "Frontend action"},
    {"d", "set volume 53", "Frontend action"}
};

static void poll_engine(Engine *engine)
{
    mpv_event *event = mpv_wait_event(engine->mpv, .02);
    g_assert_cmpint(event->error, >=, 0);
    if (event->event_id == MPV_EVENT_COMMAND_REPLY)
        engine->replied = event->reply_userdata;
    else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
        mpv_event_property *property = event->data;
        if (property->format == MPV_FORMAT_DOUBLE && g_str_equal(property->name, "volume"))
            engine->volume = *(double *)property->data;
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
        mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
    }
}

static void await_volume(Engine *engine, double expected, uint64_t reply)
{
    gint64 until = g_get_monotonic_time() + 3000000;
    while (!isfinite(engine->volume) || fabs(engine->volume - expected) > .000001 ||
           engine->replied < reply) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        poll_engine(engine);
    }
}

static void press(Engine *engine, const char *key, double expected)
{
    const char *args[] = {"keypress", key, NULL};
    uint64_t reply = ++engine->sent;
    g_assert_cmpint(mpv_command_async(engine->mpv, reply, args), >=, 0);
    await_volume(engine, expected, reply);
}

static void reload_bindings(Engine *engine, const char *path)
{
    /* A nonempty comment clears strong globals without deleting the enabled
     * default section. The separate frontend section remains below it. */
    const char *clear[] = {"define-section", "default", "# mpv-gtk reload\n", "force", NULL};
    const char *load[] = {"load-input-conf", path, NULL};
    g_assert_cmpint(mpv_command(engine->mpv, clear), >=, 0);
    g_assert_cmpint(mpv_command(engine->mpv, load), >=, 0);
}

static void apply_managed(Engine *engine, const char *path)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *contents = keybindings_managed_contents(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(contents);
    g_assert_cmpuint(strlen(contents), >, 0);
    g_assert_null(strstr(contents, "{default}"));
    const char *define[] = {"define-section", KEYBINDINGS_EDIT_SECTION, contents, "force", NULL};
    const char *enable[] = {"enable-section", KEYBINDINGS_EDIT_SECTION, NULL};
    g_assert_cmpint(mpv_command(engine->mpv, define), >=, 0);
    g_assert_cmpint(mpv_command(engine->mpv, enable), >=, 0);
}

static GPtrArray *snapshot(Engine *engine)
{
    mpv_node node = {0};
    g_assert_cmpint(mpv_get_property(engine->mpv, "input-bindings", MPV_FORMAT_NODE, &node), >=, 0);
    GPtrArray *rows = keybindings_snapshot(&node, defaults, G_N_ELEMENTS(defaults));
    mpv_free_node_contents(&node);
    return rows;
}

static const KeyBinding *find(GPtrArray *rows, const char *key)
{
    for (guint i = 0; i < rows->len; ++i) {
        const KeyBinding *binding = g_ptr_array_index(rows, i);
        if (g_str_equal(binding->key, key))
            return binding;
    }
    g_error("Missing key-binding row: %s", key);
    return NULL;
}

static void save(const char *path, const char *key, const char *command)
{
    g_autoptr(GError) error = NULL;
    g_assert_true(keybindings_set(path, key, command, &error));
    g_assert_no_error(error);
}

static void reset(const char *path, const char *key)
{
    g_autoptr(GError) error = NULL;
    g_assert_true(keybindings_reset(path, key, &error));
    g_assert_no_error(error);
}

static void preserved(const char *path, const char *original)
{
    g_autofree char *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_true(g_str_has_prefix(contents, original));
    GStatBuf status;
    g_assert_cmpint(g_stat(path, &status), ==, 0);
    g_assert_cmpint(status.st_mode & 0777, ==, 0640);
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-keybindings-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(directory, "input.conf", NULL);
    const char *original =
        "# Personal bindings; keep comments, spacing and duplicate history.\r\n"
        "SPACE set volume 11 # earlier duplicate\r\n"
        "  SPACE   set volume 17  # final original binding\r\n"
        "f ignore # explicit suppression\r\n"
        "x {inactive} set volume 71 # retain named sections\r\n"
        "t {advanced} set volume 79\r\n"
        "u {advanced} set volume 81\r\n"
        "é set volume 37\r\n"
        "k show-text \"a # b ${media-title}\" # quoted argument\r\n"
        "g-p set volume 83 # native multikey sequence\r\n";
    g_assert_true(g_file_set_contents(path, original, -1, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(path, 0640), ==, 0);
    Engine engine = {.volume = NAN};
    engine.mpv = mpv_create();
    g_assert_nonnull(engine.mpv);
    const char *options[][2] = {
        {"config", "no"}, {"input-conf", path}, {"input-default-bindings", "no"},
        {"load-scripts", "no"}, {"terminal", "no"}, {"vo", "null"}, {"ao", "null"},
        {"volume", "5"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(engine.mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(engine.mpv), >=, 0);
    g_assert_cmpint(mpv_request_log_messages(engine.mpv, "error"), >=, 0);
    g_assert_cmpint(mpv_observe_property(engine.mpv, 1, "volume", MPV_FORMAT_DOUBLE), >=, 0);
    await_volume(&engine, 5, 0);
    const char *frontend[] = {"define-section", KEYBINDINGS_APP_SECTION,
        "SPACE set volume 19\nz set volume 7\nd set volume 53\n", "force", NULL};
    const char *enable_frontend[] = {"enable-section", KEYBINDINGS_APP_SECTION, NULL};
    const char *enable_default[] = {"enable-section", "default", NULL};
    g_assert_cmpint(mpv_command(engine.mpv, frontend), >=, 0);
    g_assert_cmpint(mpv_command(engine.mpv, enable_frontend), >=, 0);
    g_assert_cmpint(mpv_command(engine.mpv, enable_default), >=, 0);

    g_autoptr(GPtrArray) rows = snapshot(&engine);
    g_assert_cmpuint(rows->len, >, 100); /* Disabled mpv defaults remain discoverable. */
    g_assert_true(find(rows, "q")->weak);
    g_assert_false(find(rows, "q")->active);
    g_assert_true(find(rows, "d")->app_default);
    g_assert_true(find(rows, "d")->active);
    g_assert_false(find(rows, "d")->weak);
    g_assert_false(find(rows, "SPACE")->app_default);
    g_assert_cmpstr(find(rows, "SPACE")->command, ==, "set volume 17");
    g_assert_cmpstr(find(rows, "f")->command, ==, "ignore");
    g_assert_false(find(rows, "x")->active);
    const char *disable_frontend[] = {"disable-section", KEYBINDINGS_APP_SECTION, NULL};
    g_assert_cmpint(mpv_command(engine.mpv, disable_frontend), >=, 0);
    g_autoptr(GPtrArray) inactive_rows = snapshot(&engine);
    g_assert_true(find(inactive_rows, "d")->app_default);
    g_assert_false(find(inactive_rows, "d")->active);
    g_assert_cmpint(find(inactive_rows, "d")->priority, ==, -1);
    g_assert_true(find(inactive_rows, "SPACE")->active);
    g_assert_false(find(inactive_rows, "SPACE")->app_default);
    /* Disabling both sections also must not turn the inactive frontend
     * fallback into a synthetic active row behind the inactive user binding. */
    const char *disable_default[] = {"disable-section", "default", NULL};
    g_assert_cmpint(mpv_command(engine.mpv, disable_default), >=, 0);
    g_autoptr(GPtrArray) all_inactive = snapshot(&engine);
    g_assert_true(find(all_inactive, "SPACE")->app_default);
    g_assert_false(find(all_inactive, "SPACE")->active);
    g_assert_cmpint(find(all_inactive, "SPACE")->priority, ==, -1);
    g_assert_cmpint(mpv_command(engine.mpv, enable_frontend), >=, 0);
    g_autoptr(GPtrArray) reactivated = snapshot(&engine);
    g_assert_true(find(reactivated, "d")->app_default);
    g_assert_true(find(reactivated, "d")->active);
    g_assert_cmpint(find(reactivated, "d")->priority, >=, 0);
    g_assert_cmpint(mpv_command(engine.mpv, enable_default), >=, 0);
    press(&engine, "SPACE", 17);
    press(&engine, "g", 17);
    press(&engine, "p", 83); /* All events forwarded lets mpv resolve sequences. */

    save(path, "SPACE", "set volume 23");
    save(path, "SPACE", "set volume 29 # managed note");
    save(path, "SPACE", "set volume 31");
    save(path, "n", "show-text \"# ${media-title}\" # keep literal syntax");
    preserved(path, original);
    g_autofree char *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "SPACE {default} set volume 31 # managed note\r\n"));
    g_assert_nonnull(strstr(contents, "n {default} show-text \"# ${media-title}\" # keep literal syntax\r\n"));
    reload_bindings(&engine, path);
    press(&engine, "SPACE", 31);
    reset(path, "SPACE");
    reload_bindings(&engine, path);
    press(&engine, "SPACE", 17);

    g_assert_true(keybindings_rename(path, "SPACE", "Ctrl+j", "set volume 41", &error));
    g_assert_no_error(error);
    preserved(path, original);
    reload_bindings(&engine, path);
    press(&engine, "Ctrl+j", 41);
    press(&engine, "SPACE", 41); /* Renaming suppresses the old original binding. */
    reset(path, "SPACE");
    reset(path, "Ctrl+j");
    reload_bindings(&engine, path);
    press(&engine, "SPACE", 17);
    save(path, "z", "set volume 47");
    reload_bindings(&engine, path);
    press(&engine, "z", 47);
    reset(path, "z");
    reload_bindings(&engine, path);
    press(&engine, "z", 7); /* Frontend fallback survives and is restored. */

    const char *enable_advanced[] = {"enable-section", "advanced", NULL};
    g_assert_cmpint(mpv_command(engine.mpv, enable_advanced), >=, 0);
    reload_bindings(&engine, path);
    press(&engine, "t", 79); /* Named user sections and their active order survive. */
    save(path, "t", "set volume 88");
    reload_bindings(&engine, path);
    press(&engine, "t", 79); /* A global edit alone loses to the advanced section. */
    apply_managed(&engine, path);
    press(&engine, "t", 88); /* Only edited keys gain the highest live priority. */
    press(&engine, "u", 81); /* Other advanced bindings keep their original behavior. */
    reset(path, "t");
    reload_bindings(&engine, path);
    apply_managed(&engine, path);
    press(&engine, "t", 79);
    preserved(path, original);

    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_false(keybindings_set(path, "Ctrl+\nj", "set volume 1", &error));
    g_clear_error(&error);
    g_assert_false(keybindings_set(path, "j", "set volume 1\nq quit", &error));
    g_clear_error(&error);
    g_autofree char *unchanged = NULL;
    g_assert_true(g_file_get_contents(path, &unchanged, NULL, NULL));
    g_assert_cmpstr(unchanged, ==, contents);

    g_autofree char *broken = g_build_filename(directory, "broken.conf", NULL);
    g_assert_true(g_file_set_contents(broken,
        "# >>> mpv-gtk managed key bindings\nt {default} set volume 88\n", -1, &error));
    g_assert_no_error(error);
    g_autofree char *invalid_contents = keybindings_managed_contents(broken, &error);
    g_assert_null(invalid_contents);
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);

    mpv_terminate_destroy(engine.mpv);
    g_assert_cmpint(g_remove(broken), ==, 0);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    puts("PASS keybindings: all-key model, preserved input.conf, atomic rename, native sequences, reset, edited-key priority over advanced sections");
    return 0;
}
