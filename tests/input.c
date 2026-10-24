#include "../src/input.h"

#include <locale.h>
#include <math.h>
#include <unistd.h>

typedef struct {
    mpv_handle *mpv;
    double volume;
    uint64_t next_id, reply;
} InputEngine;

static void poll_input(InputEngine *engine)
{
    mpv_event *event = mpv_wait_event(engine->mpv, .05);
    g_assert_cmpint(event->error, >=, 0);
    if (event->event_id == MPV_EVENT_COMMAND_REPLY)
        engine->reply = event->reply_userdata;
    else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
        mpv_event_property *property = event->data;
        if (g_str_equal(property->name, "volume") && property->format == MPV_FORMAT_DOUBLE)
            engine->volume = *(double *)property->data;
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
        mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
    }
}

static void await_volume(InputEngine *engine, double expected, uint64_t reply)
{
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (!isfinite(engine->volume) || fabs(engine->volume - expected) > .000001 ||
           engine->reply < reply) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_input(engine);
    }
}

static void refresh_bindings(InputEngine *engine, GHashTable *bindings)
{
    mpv_node node = {0};
    g_assert_cmpint(mpv_get_property(engine->mpv, "input-bindings", MPV_FORMAT_NODE, &node), >=, 0);
    input_bindings_update(bindings, &node);
    mpv_free_node_contents(&node);
}

static void press_key(InputEngine *engine, GHashTable *bindings, guint keyval,
                       GdkModifierType modifiers, const char *name, double volume)
{
    g_autofree char *key = input_key_name(keyval, modifiers);
    g_assert_cmpstr(key, ==, name);
    g_assert_true(input_bindings_contains(bindings, key));
    const char *args[] = {"keypress", key, NULL};
    uint64_t reply = ++engine->next_id;
    g_assert_cmpint(mpv_command_async(engine->mpv, reply, args), >=, 0);
    await_volume(engine, volume, reply);
}

static void check_key_names(void)
{
    const struct { guint keyval; GdkModifierType modifiers; const char *name; } cases[] = {
        {GDK_KEY_space, GDK_SHIFT_MASK, "SPACE"},
        {GDK_KEY_Tab, GDK_SHIFT_MASK | GDK_CONTROL_MASK, "Shift+Ctrl+TAB"},
        {GDK_KEY_ISO_Left_Tab, 0, "Shift+TAB"},
        {GDK_KEY_F11, GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK,
         "Shift+Ctrl+Alt+Meta+F11"},
        {GDK_KEY_eacute, GDK_LOCK_MASK, "é"},
        {GDK_KEY_Eacute, GDK_SHIFT_MASK | GDK_LOCK_MASK, "É"},
        {GDK_KEY_plus, GDK_SHIFT_MASK, "+"},
        {GDK_KEY_Page_Down, 0, "PGDWN"},
        {GDK_KEY_KP_Enter, 0, "KP_ENTER"},
        {GDK_KEY_KP_7, 0, "KP7"},
        {GDK_KEY_KP_Add, GDK_SHIFT_MASK, "Shift+KP_ADD"},
        {GDK_KEY_KP_Add, GDK_ALT_MASK, "Alt+KP_ADD"},
        {GDK_KEY_F, GDK_SHIFT_MASK | GDK_ALT_MASK, "Alt+F"},
        {GDK_KEY_Control_L, GDK_CONTROL_MASK, NULL},
        {GDK_KEY_Shift_L, 0, NULL},
        {GDK_KEY_Shift_R, GDK_SHIFT_MASK, NULL},
        {GDK_KEY_Alt_L, 0, NULL},
        {GDK_KEY_Alt_R, GDK_ALT_MASK, NULL}
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); ++i) {
        g_autofree char *name = input_key_name(cases[i].keyval, cases[i].modifiers);
        g_assert_cmpstr(name, ==, cases[i].name);
    }
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    check_key_names();
    char *filename = NULL;
    int fd = g_file_open_tmp("mpv-gtk-input-XXXXXX.conf", &filename, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_autofree char *input_path = filename;
    const char *contents =
        "SPACE set volume 17\n"
        "Ctrl+j set volume 23\n"
        "f ignore\n"
        "Shift+LEFT set volume 31\n"
        "Shift+f set volume 37\n"
        "Ctrl+Shift+j set volume 41\n"
        "é set volume 47\n"
        "Alt+é set volume 53\n"
        "SHARP set volume 59\n"
        "Ctrl++ set volume 61\n"
        "Alt+LEFT set volume 63\n"
        "Alt+f set volume 69\n"
        "Shift+KP_ADD set volume 71\n"
        "Alt+KP_ADD set volume 73\n"
        "Alt+Shift+f set volume 79\n"
        "a {inactive} set volume 67\n";
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(input_path, contents, -1, &error));
    g_assert_no_error(error);
    InputEngine engine = {.volume = NAN};
    engine.mpv = mpv_create();
    g_assert_nonnull(engine.mpv);
    const char *options[][2] = {
        {"config", "no"}, {"input-conf", input_path}, {"input-default-bindings", "no"},
        {"vo", "null"}, {"ao", "null"}, {"terminal", "no"}, {"load-scripts", "no"},
        {"volume", "5"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); ++i)
        g_assert_cmpint(mpv_set_option_string(engine.mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(engine.mpv), >=, 0);
    g_assert_cmpint(mpv_request_log_messages(engine.mpv, "error"), >=, 0);
    g_assert_cmpint(mpv_observe_property(engine.mpv, 1, "volume", MPV_FORMAT_DOUBLE), >=, 0);
    await_volume(&engine, 5, 0);
    g_autoptr(GHashTable) bindings = input_bindings_new();
    refresh_bindings(&engine, bindings);
    g_assert_cmpuint(g_hash_table_size(bindings), ==, 15);
    g_assert_false(input_bindings_contains(bindings, "q")); /* Disabled weak default. */
    g_assert_false(input_bindings_contains(bindings, "a")); /* Inactive strong section. */

    press_key(&engine, bindings, GDK_KEY_space, 0, "SPACE", 17);
    press_key(&engine, bindings, GDK_KEY_j, GDK_CONTROL_MASK, "Ctrl+j", 23);
    press_key(&engine, bindings, GDK_KEY_f, 0, "f", 23); /* Explicit ignore overrides GTK. */
    press_key(&engine, bindings, GDK_KEY_Left, GDK_SHIFT_MASK, "Shift+LEFT", 31);
    press_key(&engine, bindings, GDK_KEY_F, GDK_SHIFT_MASK, "F", 37);
    press_key(&engine, bindings, GDK_KEY_j, GDK_SHIFT_MASK | GDK_CONTROL_MASK, "Ctrl+J", 41);
    press_key(&engine, bindings, GDK_KEY_eacute, 0, "é", 47);
    press_key(&engine, bindings, GDK_KEY_eacute, GDK_ALT_MASK, "Alt+é", 53);
    press_key(&engine, bindings, GDK_KEY_numbersign, GDK_SHIFT_MASK, "SHARP", 59);
    press_key(&engine, bindings, GDK_KEY_plus, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "Ctrl++", 61);
    press_key(&engine, bindings, GDK_KEY_Left, GDK_ALT_MASK, "Alt+LEFT", 63);
    press_key(&engine, bindings, GDK_KEY_f, GDK_ALT_MASK, "Alt+f", 69);
    press_key(&engine, bindings, GDK_KEY_KP_Add, GDK_SHIFT_MASK, "Shift+KP_ADD", 71);
    press_key(&engine, bindings, GDK_KEY_KP_Add, GDK_ALT_MASK, "Alt+KP_ADD", 73);
    press_key(&engine, bindings, GDK_KEY_F, GDK_SHIFT_MASK | GDK_ALT_MASK, "Alt+F", 79);

    const char *enable[] = {"enable-section", "inactive", NULL};
    g_assert_cmpint(mpv_command(engine.mpv, enable), >=, 0);
    refresh_bindings(&engine, bindings);
    press_key(&engine, bindings, GDK_KEY_a, 0, "a", 67);
    const char *disable[] = {"disable-section", "inactive", NULL};
    g_assert_cmpint(mpv_command(engine.mpv, disable), >=, 0);
    refresh_bindings(&engine, bindings);
    g_assert_false(input_bindings_contains(bindings, "a"));

    input_bindings_update(bindings, NULL);
    g_assert_cmpuint(g_hash_table_size(bindings), ==, 0);
    refresh_bindings(&engine, bindings);
    g_assert_true(input_bindings_contains(bindings, "SPACE"));
    mpv_terminate_destroy(engine.mpv);
    g_assert_cmpint(unlink(input_path), ==, 0);
    puts("PASS input: real input.conf overrides, ignored defaults, active sections, "
         "Shift/Alt letters/arrows/keypad, Unicode normalization and async keypress routing");
    return 0;
}
