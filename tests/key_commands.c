#include "key_commands.h"
#include "i18n.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>

static const mpv_node *field(const mpv_node *node, const char *name)
{
    g_assert_cmpint(node->format, ==, MPV_FORMAT_NODE_MAP);
    for (int i = 0; i < node->u.list->num; i++)
        if (g_str_equal(node->u.list->keys[i], name))
            return &node->u.list->values[i];
    g_error("Missing runtime field: %s", name);
}

static const KeyCommandSpec *find_spec(GPtrArray *specs, const char *name)
{
    for (guint i = 0; i < specs->len; i++) {
        const KeyCommandSpec *spec = g_ptr_array_index(specs, i);
        if (g_str_equal(spec->name, name))
            return spec;
    }
    g_error("Missing command in snapshot: %s", name);
}

static void check_snapshot(const mpv_node *raw, GPtrArray *specs)
{
    g_assert_cmpuint(specs->len, ==, raw->u.list->num);
    for (guint i = 0; i < specs->len; i++) {
        const mpv_node *command = &raw->u.list->values[i];
        const KeyCommandSpec *spec = g_ptr_array_index(specs, i);
        const mpv_node *name = field(command, "name");
        const mpv_node *args = field(command, "args");
        const mpv_node *vararg = field(command, "vararg");
        g_assert_cmpstr(spec->name, ==, name->u.string);
        g_assert_true(key_command_is_known(spec->name));
        g_assert_cmpuint(spec->arguments->len, ==, args->u.list->num);
        g_assert_cmpint(spec->vararg, ==, !!vararg->u.flag);
        g_assert_true(g_str_has_prefix(spec->signature, spec->name));
        g_assert_nonnull(spec->description);
        g_assert_true(*spec->description != '\0');
        g_assert_cmpstr(spec->description, !=, key_command_help("future-unknown-command"));
        g_assert_true(*spec->category != '\0');
        g_assert_true(*spec->example != '\0');
        g_assert_true(g_utf8_validate(spec->description, -1, NULL));
        for (guint j = 0; j < spec->arguments->len; j++) {
            const KeyCommandArgument *copy = g_ptr_array_index(spec->arguments, j);
            const mpv_node *argument = &args->u.list->values[j];
            g_assert_cmpstr(copy->name, ==, field(argument, "name")->u.string);
            g_assert_cmpstr(copy->type, ==, field(argument, "type")->u.string);
            g_assert_cmpint(copy->optional, ==, !!field(argument, "optional")->u.flag);
            g_assert_nonnull(strstr(spec->signature, copy->name));
            g_assert_nonnull(strstr(spec->signature, copy->type));
        }
    }
    g_assert_cmpstr(find_spec(specs, "ignore")->signature, ==, "ignore");
    g_assert_cmpstr(find_spec(specs, "quit")->signature, ==, "quit [code=Integer]");
    g_assert_true(find_spec(specs, "run")->vararg);
    g_assert_nonnull(strstr(find_spec(specs, "run")->signature, "args=String..."));
    g_assert_nonnull(strstr(find_spec(specs, "seek")->signature, "target=Time"));
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    mpv_handle *mpv = mpv_create();
    g_assert_nonnull(mpv);
    const char *options[][2] = {
        {"config", "no"}, {"vo", "null"}, {"ao", "null"},
        {"terminal", "no"}, {"load-scripts", "no"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(options); i++)
        g_assert_cmpint(mpv_set_option_string(mpv, options[i][0], options[i][1]), >=, 0);
    g_assert_cmpint(mpv_initialize(mpv), >=, 0);
    mpv_node runtime = {0};
    g_assert_cmpint(mpv_get_property(mpv, "command-list", MPV_FORMAT_NODE, &runtime), >=, 0);
    g_assert_cmpint(runtime.format, ==, MPV_FORMAT_NODE_ARRAY);
    g_assert_cmpint(runtime.u.list->num, >, 0);
    i18n_set_language("ru");
    g_autoptr(GPtrArray) russian = key_command_list_from_node(&runtime);
    check_snapshot(&runtime, russian);
    i18n_set_language("en");
    g_autoptr(GPtrArray) english = key_command_list_from_node(&runtime);
    check_snapshot(&runtime, english);
    const char *retained_help = key_command_help("seek");
    g_assert_cmpstr(retained_help, ==,
        "Seeks by a time or percentage; flags select relative, absolute, or exact seeking.");
    for (guint i = 0; i < english->len; i++) {
        const KeyCommandSpec *ru = g_ptr_array_index(russian, i);
        const KeyCommandSpec *en = g_ptr_array_index(english, i);
        g_assert_cmpstr(ru->description, !=, en->description);
        g_assert_cmpstr(ru->category, !=, en->category);
    }
    guint command_count = english->len;
    mpv_free_node_contents(&runtime);
    mpv_terminate_destroy(mpv);
    /* A completed snapshot must remain usable after engine-owned nodes die. */
    g_assert_cmpstr(find_spec(english, "run")->name, ==, "run");
    g_assert_cmpstr(find_spec(english, "quit")->signature, ==, "quit [code=Integer]");

    gsize preset_count;
    const KeyCommandPreset *presets = key_command_presets(&preset_count);
    g_assert_cmpuint(preset_count, >=, 30);
    for (gsize i = 0; i < preset_count; i++) {
        g_assert_cmpstr(tr(presets[i].title), !=, presets[i].title);
        g_assert_cmpstr(tr(presets[i].help), !=, presets[i].help);
        g_assert_cmpstr(tr(presets[i].category), !=, presets[i].category);
        g_autofree char *description = key_command_description(presets[i].command);
        g_assert_cmpstr(description, ==, tr(presets[i].help));
    }
    g_autofree char *pause = key_command_description(" no-osd\t cycle  pause ");
    g_assert_cmpstr(pause, ==, "Pauses or resumes playback.");
    g_autofree char *seek = key_command_description("seek -5");
    g_assert_cmpstr(seek, ==, "Seeks backward by 5 seconds.");
    g_autofree char *app = key_command_description("script-message mpv-gtk open-file");
    g_assert_cmpstr(app, ==, "Opens the GTK file selection dialog.");
    /* Cover operation semantics beyond exact presets: different magnitudes,
     * numeric spellings, aliases, a ratio, and a named built-in script action. */
    g_autofree char *contrast = key_command_description("add contrast 1");
    g_assert_cmpstr(contrast, ==, "Increases contrast by 1.");
    g_autofree char *volume = key_command_description("add volume -2");
    g_assert_cmpstr(volume, ==, "Decreases volume by 2.");
    g_autofree char *delay = key_command_description("add audio-delay 0.100");
    g_assert_cmpstr(delay, ==, "Moves audio 0.1 s later relative to video.");
    g_autofree char *normal_speed = key_command_description("set speed 1.0");
    g_assert_cmpstr(normal_speed, ==, "Restores playback speed to 1×.");
    g_autofree char *slower = key_command_description("multiply speed 1/1.1");
    g_assert_cmpstr(slower, ==, "Slows playback: multiplier 0.909091.");
    g_autofree char *track = key_command_description("cycle sub down");
    g_assert_cmpstr(track, ==, "Selects the previous subtitle track.");
    g_autofree char *modes = key_command_description("cycle-values hwdec no auto");
    g_assert_cmpstr(modes, ==, "Cycles hardware decoding mode through these values: no, auto.");
    g_autofree char *selector = key_command_description("script-binding select/select-aid");
    g_assert_cmpstr(selector, ==, "Opens the audio track selector.");
    i18n_set_language("ru");
    /* The public help pointer outlives the inactive translation catalogue. */
    g_assert_cmpstr(retained_help, ==,
        "Seeks by a time or percentage; flags select relative, absolute, or exact seeking.");
    g_assert_cmpstr(key_command_help("seek"), ==,
        "Перематывает на указанное время или долю файла; режим задаётся флагами.");
    g_autofree char *deinterlace = key_command_description("cycle deinterlace");
    g_assert_cmpstr(deinterlace, ==, "Переключает устранение чересстрочности.");
    g_autofree char *chapter = key_command_description("add chapter -1");
    g_assert_cmpstr(chapter, ==, "Переходит к предыдущей главе.");
    g_autofree char *console = key_command_description("script-binding commands/open");
    g_assert_cmpstr(console, ==, "Открывает консоль команд mpv.");
    i18n_set_language("en");
    g_autofree char *empty = key_command_description(NULL);
    g_assert_cmpstr(empty, ==, "No command is assigned.");
    g_autofree char *unknown = key_command_description("future-unknown-command argument");
    g_assert_false(key_command_is_known("future-unknown-command"));
    g_assert_cmpstr(unknown, ==, key_command_help("future-unknown-command"));
    mpv_node invalid = {.format = MPV_FORMAT_NONE};
    g_autoptr(GPtrArray) absent = key_command_list_from_node(&invalid);
    g_assert_cmpuint(absent->len, ==, 0);
    i18n_set_language("ru");
    printf("PASS command catalogue: %u runtime commands, argument signatures, RU/EN property/script semantics, %zu presets; no commands executed\n",
           command_count, preset_count);
    return 0;
}
