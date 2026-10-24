/* Exercise the actual settings callback and initialization path without
 * constructing GTK widgets or requiring a display. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <string.h>

typedef struct {
    const char *name;
    mpv_format format;
    const char *text;
    int flag;
    double number;
} Setting;

static const Setting settings[] = {
    {.name = "hwdec", .format = MPV_FORMAT_STRING, .text = "no"},
    {.name = "keepaspect", .format = MPV_FORMAT_FLAG, .flag = 0},
    {.name = "deinterlace", .format = MPV_FORMAT_FLAG, .flag = 1},
    {.name = "deband", .format = MPV_FORMAT_FLAG, .flag = 1},
    {.name = "loop-file", .format = MPV_FORMAT_STRING, .text = "inf"},
    {.name = "audio-delay", .format = MPV_FORMAT_DOUBLE, .number = 0.25},
    {.name = "sub-delay", .format = MPV_FORMAT_DOUBLE, .number = 0.5},
    {.name = "sub-scale", .format = MPV_FORMAT_DOUBLE, .number = 1.5},
};

static gboolean settings_property(const char *name)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(settings); ++i) {
        if (g_str_equal(name, settings[i].name))
            return TRUE;
    }
    return FALSE;
}

static void assert_setting(const Setting *setting, mpv_event_property *property)
{
    g_assert_cmpstr(property->name, ==, setting->name);
    g_assert_cmpint(property->format, ==, setting->format);
    g_assert_nonnull(property->data);
    if (setting->format == MPV_FORMAT_STRING)
        g_assert_cmpstr(*(const char **)property->data, ==, setting->text);
    else if (setting->format == MPV_FORMAT_FLAG)
        g_assert_cmpint(*(int *)property->data, ==, setting->flag);
    else
        g_assert_cmpfloat_with_epsilon(*(double *)property->data, setting->number, 1e-9);
}

static void poll_settings(Player *p, unsigned expected_sets, gboolean read_back)
{
    unsigned sets = 0, reads = 0;
    gboolean requested = FALSE;
    gboolean received[G_N_ELEMENTS(settings)] = {FALSE};
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (reads < (read_back ? G_N_ELEMENTS(settings) : 0) || sets < expected_sets || !requested) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        if (sets == expected_sets && !requested) {
            requested = TRUE;
            if (read_back) {
                for (unsigned i = 0; i < G_N_ELEMENTS(settings); ++i)
                    g_assert_cmpint(mpv_get_property_async(p->mpv, 100 + i,
                        settings[i].name, settings[i].format), >=, 0);
            }
        }
        mpv_event *event = mpv_wait_event(p->mpv, 0.05);
        if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY) {
            g_assert_cmpint(event->error, >=, 0);
            sets++;
        } else if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY) {
            g_assert_cmpint(event->error, >=, 0);
            g_assert_cmpuint(event->reply_userdata, >=, 100);
            guint64 index = event->reply_userdata - 100;
            g_assert_cmpuint(index, <, G_N_ELEMENTS(settings));
            g_assert_false(received[index]);
            received[index] = TRUE;
            assert_setting(&settings[index], event->data);
            /* Exercise the same cache update as real engine observations. */
            property_changed(p, event->data);
            reads++;
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *property = event->data;
            if (settings_property(property->name))
                property_changed(p, property);
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *log = event->data;
            g_error("libmpv %s: %s", log->prefix, log->text);
        } else {
            g_assert_cmpint(event->error, >=, 0);
        }
    }
    g_assert_cmpuint(sets, ==, expected_sets);
    g_assert_cmpuint(p->error_count, ==, 0);
    g_assert_cmpstr(p->preference_values.hwdec, ==, "no");
    g_assert_false(p->preference_values.keepaspect);
    g_assert_true(p->preference_values.deinterlace);
    g_assert_true(p->preference_values.deband);
    g_assert_true(p->preference_values.loop);
    g_assert_cmpfloat_with_epsilon(p->preference_values.audio_delay, 0.25, 1e-9);
    g_assert_cmpfloat_with_epsilon(p->preference_values.sub_delay, 0.5, 1e-9);
    g_assert_cmpfloat_with_epsilon(p->preference_values.sub_scale, 1.5, 1e-9);
}

static void close_test_player(Player *p)
{
    p->closing = TRUE;
    mpv_set_wakeup_callback(p->mpv, NULL, NULL);
    mpv_terminate_destroy(p->mpv);
    close(p->wake_fd);
    if (p->gl_library)
        dlclose(p->gl_library);
    config_clear(&p->config);
    g_free(p->hwdec);
    g_free(p->hwdec_current);
    g_free(p->pending_hwdec);
    g_free(p->audio_output);
    g_free(p->audio_device);
}

static void check_cuda_interop_policy(void)
{
    const struct { const char *hwdec; gboolean compatible; } hardware[] = {
        {NULL, FALSE}, {"", TRUE}, {"no", TRUE}, {"yes", TRUE},
        {"auto", TRUE}, {"auto-safe", TRUE}, {"auto-unsafe", TRUE},
        {"auto-copy", TRUE}, {"auto-copy-safe", TRUE}, {"auto-copy-unsafe", TRUE},
        {"nvdec", TRUE}, {"cuda", TRUE}, {"h264-nvdec", TRUE},
        {"hevc_cuvid-cuda", TRUE}, {"nvdec-copy", TRUE}, {"cuda-copy", TRUE},
        {"vaapi-copy", TRUE}, {"h264-vaapi-copy", TRUE}, {"vdpau-copy", TRUE},
        {"vulkan-copy", TRUE}, {"qsv-copy", TRUE}, {"v4l2m2m-copy", TRUE},
        {"nvdec,vaapi-copy", TRUE}, {"nvdec, no,vaapi", TRUE}, {"no,vaapi", TRUE},
        {"nvdec,,auto-copy", TRUE}, {"vaapi", FALSE}, {"h264-vaapi", FALSE},
        {"vdpau", FALSE}, {"drm", FALSE}, {"vulkan", FALSE}, {"qsv", FALSE},
        {"rkmpp", FALSE}, {"nvdec,vaapi,no", FALSE}, {"auto,vaapi", FALSE},
        {"vaapi-copy,vulkan", FALSE}, {"unsupported", FALSE}
    };
    for (guint i = 0; i < G_N_ELEMENTS(hardware); ++i) {
        g_assert_cmpint(cuda_interop_handles(hardware[i].hwdec), ==, hardware[i].compatible);
        g_assert_cmpint(prefer_cuda_interop("NVIDIA Corporation", "auto", hardware[i].hwdec),
                        ==, hardware[i].compatible);
    }
    const struct { const char *vendor, *interop; gboolean optimized; } contexts[] = {
        {"NVIDIA Corporation", "auto", TRUE}, {"nViDiA Corporation", "", TRUE},
        {"Mesa", "auto", FALSE}, {"Intel", "auto", FALSE},
        {"AMD", "auto", FALSE}, {NULL, "auto", FALSE}, {"", "auto", FALSE},
        {"NVIDIA Corporation", NULL, FALSE}, {"NVIDIA Corporation", "all", FALSE},
        {"NVIDIA Corporation", "no", FALSE}, {"NVIDIA Corporation", "cuda", FALSE},
        {"NVIDIA Corporation", "vaapi", FALSE}
    };
    for (guint i = 0; i < G_N_ELEMENTS(contexts); ++i)
        g_assert_cmpint(prefer_cuda_interop(contexts[i].vendor, contexts[i].interop, "nvdec"),
                        ==, contexts[i].optimized);
}

/* A fresh native snapshot exercises the same cache path as reopening the
 * lazy settings page. The engine stays idle: no GPU decoder is simulated. */
static void read_hardware_setting(Player *p, const char *expected, guint expected_sets)
{
    const uint64_t request = 7000;
    g_assert_cmpint(mpv_get_property_async(p->mpv, request, "hwdec", MPV_FORMAT_STRING), >=, 0);
    gboolean received = FALSE;
    guint sets = 0;
    gint64 until = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!received || sets < expected_sets) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        mpv_event *event = mpv_wait_event(p->mpv, .02);
        g_assert_cmpint(event->error, >=, 0);
        if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata == request) {
            mpv_event_property *property = event->data;
            g_assert_cmpstr(property->name, ==, "hwdec");
            g_assert_cmpint(property->format, ==, MPV_FORMAT_STRING);
            g_assert_cmpstr(*(char **)property->data, ==, expected);
            property_changed(p, property);
            received = TRUE;
        } else if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY &&
                   event->reply_userdata == HWDEC_SET_ID) {
            sets++;
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *property = event->data;
            if (g_str_equal(property->name, "hwdec"))
                property_changed(p, property);
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *log = event->data;
            g_error("libmpv %s: %s", log->prefix, log->text);
        }
    }
    g_assert_cmpuint(sets, ==, expected_sets);
    g_assert_cmpstr(p->hwdec, ==, expected);
}

static void check_deferred_hardware(const char *directory, const char *input)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = g_build_filename(directory, "hardware.conf", NULL);
    g_autofree char *invalid_path = g_build_filename(directory, "hardware-failed.conf", NULL);
    g_assert_true(g_file_set_contents(path,
        "# Preserve hardware choices\nhwdec=nvdec\nao=null\n", -1, &error));
    g_assert_no_error(error);
    Player p = {.wake_fd = -1};
    p.config.path = g_strdup(path);
    p.config.input_path = g_strdup(input);
    g_assert_true(init_mpv(&p));
    read_hardware_setting(&p, "nvdec", 0);
    /* Model the capability of a successfully created restricted renderer.
     * Actual hardware decoding and GL vendor detection are not claimed here. */
    p.cuda_interop = TRUE;
    p.preference_values.hwdec_known = TRUE;
    const char *choice = "vaapi";
    preference_changed("hwdec", MPV_FORMAT_STRING, &choice, &p);
    g_assert_cmpstr(p.pending_hwdec, ==, "vaapi");
    g_assert_cmpstr(p.preference_values.hwdec, ==, "vaapi");
    g_assert_true(p.preference_values.hwdec_restart_required);
    g_assert_false(p.preference_values.hwdec_pending);
    g_assert_true(p.preference_values.hwdec_known);
    read_hardware_setting(&p, "nvdec", 0);
    g_assert_cmpstr(p.preference_values.hwdec, ==, "vaapi");
    g_assert_true(p.preference_values.hwdec_restart_required);
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
    g_assert_nonnull(strstr(saved, "# Preserve hardware choices\n"));
    g_assert_nonnull(strstr(saved, "hwdec=vaapi\n"));

    choice = "vdpau";
    preference_changed("hwdec", MPV_FORMAT_STRING, &choice, &p);
    read_hardware_setting(&p, "nvdec", 0);
    g_assert_cmpstr(p.pending_hwdec, ==, "vdpau");
    g_assert_cmpstr(p.preference_values.hwdec, ==, "vdpau");
    const char invalid[] = "hwdec=nvdec\n\0preserve invalid bytes\n";
    g_assert_true(g_file_set_contents(invalid_path, invalid, sizeof(invalid) - 1, &error));
    g_assert_no_error(error);
    char *regular_path = p.config.path;
    p.config.path = g_strdup(invalid_path);
    choice = "no"; /* Failed saving must not cancel the pending mode or apply this live. */
    preference_changed("hwdec", MPV_FORMAT_STRING, &choice, &p);
    g_assert_cmpuint(p.error_count, ==, 1);
    read_hardware_setting(&p, "nvdec", 0);
    g_assert_cmpstr(p.pending_hwdec, ==, "vdpau");
    g_assert_cmpstr(p.preference_values.hwdec, ==, "vdpau");
    g_assert_true(p.preference_values.hwdec_restart_required);
    g_autofree char *unchanged = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents(invalid_path, &unchanged, &length, &error));
    g_assert_cmpmem(unchanged, length, invalid, sizeof(invalid) - 1);
    g_free(p.config.path);
    p.config.path = regular_path;

    const char *live[] = {"nvdec", "no", "vaapi-copy", "auto-copy", "nvdec"};
    for (guint i = 0; i < G_N_ELEMENTS(live); ++i) {
        choice = live[i];
        preference_changed("hwdec", MPV_FORMAT_STRING, &choice, &p);
        g_assert_null(p.pending_hwdec);
        g_assert_false(p.preference_values.hwdec_restart_required);
        read_hardware_setting(&p, choice, 1);
        g_assert_cmpstr(p.preference_values.hwdec, ==, choice);
    }
    choice = "vaapi";
    preference_changed("hwdec", MPV_FORMAT_STRING, &choice, &p);
    read_hardware_setting(&p, "nvdec", 0);
    g_assert_cmpstr(p.pending_hwdec, ==, "vaapi");
    close_test_player(&p);

    Player reopened = {.wake_fd = -1};
    reopened.config.path = g_strdup(path);
    reopened.config.input_path = g_strdup(input);
    g_assert_true(init_mpv(&reopened));
    read_hardware_setting(&reopened, "vaapi", 0);
    g_assert_cmpstr(reopened.preference_values.hwdec, ==, "vaapi");
    g_assert_null(reopened.pending_hwdec);
    g_assert_false(reopened.preference_values.hwdec_restart_required);
    /* A renderer which was not narrowed retains normal live selection. */
    choice = "vdpau";
    preference_changed("hwdec", MPV_FORMAT_STRING, &choice, &reopened);
    read_hardware_setting(&reopened, "vdpau", 1);
    g_assert_null(reopened.pending_hwdec);
    g_assert_false(reopened.preference_values.hwdec_restart_required);
    close_test_player(&reopened);
    g_assert_cmpint(unlink(invalid_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    puts("PASS hardware preferences: NVIDIA/copy/list policy, deferred modes persist without mutating the engine, snapshots preserve pending choice, live compatible modes, atomic failure and restart readback");
}

/* Keep the engine idle so even the automatic output choice cannot open a
 * sound device. Read the actual properties after the queued live setters. */
static void read_audio_settings(Player *p, const char *output, const char *device)
{
    const char *names[] = {"ao", "audio-device"};
    const char *expected[] = {output, device};
    gboolean received[G_N_ELEMENTS(names)] = {FALSE};
    for (guint i = 0; i < G_N_ELEMENTS(names); i++)
        g_assert_cmpint(mpv_get_property_async(p->mpv, 5000 + i,
                                               names[i], MPV_FORMAT_STRING), >=, 0);
    guint reads = 0;
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (reads < G_N_ELEMENTS(names)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        mpv_event *event = mpv_wait_event(p->mpv, 0.05);
        if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY) {
            guint64 index = event->reply_userdata - 5000;
            g_assert_cmpuint(index, <, G_N_ELEMENTS(names));
            g_assert_cmpint(event->error, >=, 0);
            g_assert_false(received[index]);
            received[index] = TRUE;
            mpv_event_property *property = event->data;
            g_assert_cmpstr(property->name, ==, names[index]);
            g_assert_cmpint(property->format, ==, MPV_FORMAT_STRING);
            g_assert_nonnull(property->data);
            g_assert_cmpstr(*(char **)property->data, ==, expected[index]);
            property_changed(p, property);
            reads++;
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *property = event->data;
            if (g_str_equal(property->name, "ao") || g_str_equal(property->name, "audio-device"))
                property_changed(p, property);
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *log = event->data;
            g_error("libmpv %s: %s", log->prefix, log->text);
        } else {
            g_assert_cmpint(event->error, >=, 0);
        }
    }
    g_assert_cmpstr(p->preference_values.audio_output, ==, *output ? output : "auto");
    g_assert_cmpstr(p->preference_values.audio_device, ==, device);
}

static void check_audio_settings(const char *directory, const char *input)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *path = g_build_filename(directory, "audio.conf", NULL);
    g_autofree char *failed_path = g_build_filename(directory, "audio-failed.conf", NULL);
    /* No media is loaded: this stored device name only exercises the real
     * option and the host's incompatible-backend reset, without probing it. */
    g_assert_true(g_file_set_contents(path,
        "# Preserve this comment\nhwdec=no\nao=\naudio-device=pulse/mpv-gtk-test-device\n",
        -1, &error));
    g_assert_no_error(error);
    Player p = {.wake_fd = -1};
    p.config.path = g_strdup(path);
    p.config.input_path = g_strdup(input);
    g_assert_true(init_mpv(&p));
    read_audio_settings(&p, "", "pulse/mpv-gtk-test-device");

    const char *output = "null";
    preference_changed("ao", MPV_FORMAT_STRING, &output, &p);
    /* The UI reflects a successful transaction immediately, before replies. */
    g_assert_cmpstr(p.preference_values.audio_output, ==, "null");
    g_assert_cmpstr(p.preference_values.audio_device, ==, "auto");
    read_audio_settings(&p, "null", "auto");
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(saved, "# Preserve this comment\n"));
    g_assert_nonnull(strstr(saved, "ao=null\n"));
    g_assert_nonnull(strstr(saved, "audio-device=auto\n"));
    g_assert_null(strstr(saved, "pulse/mpv-gtk-test-device"));
    close_test_player(&p);

    Player reopened = {.wake_fd = -1};
    reopened.config.path = g_strdup(path);
    reopened.config.input_path = g_strdup(input);
    g_assert_true(init_mpv(&reopened));
    read_audio_settings(&reopened, "null", "auto");
    output = "auto";
    preference_changed("ao", MPV_FORMAT_STRING, &output, &reopened);
    g_assert_cmpstr(reopened.preference_values.audio_output, ==, "auto");
    read_audio_settings(&reopened, "", "auto");
    g_clear_pointer(&saved, g_free);
    g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
    g_assert_no_error(error);
    /* Both ao= and ao="" denote the empty list; reopening below checks the
     * parsed value without constraining the config writer's quoting. */
    g_assert_null(strstr(saved, "ao=auto"));
    g_assert_nonnull(strstr(saved, "audio-device=auto\n"));

    /* A bare backend name is a valid stock device selector. Exercise the
     * separate device callback too, then restore automatic device choice. */
    const char *device = "null";
    preference_changed("audio-device", MPV_FORMAT_STRING, &device, &reopened);
    g_assert_cmpstr(reopened.preference_values.audio_device, ==, "null");
    read_audio_settings(&reopened, "", "null");
    g_clear_pointer(&saved, g_free);
    g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(saved, "audio-device=null\n"));
    device = "auto";
    preference_changed("audio-device", MPV_FORMAT_STRING, &device, &reopened);
    read_audio_settings(&reopened, "", "auto");

    const char invalid[] = "# Preserve invalid bytes\n\0ao=null\n";
    g_assert_true(g_file_set_contents(failed_path, invalid, sizeof(invalid) - 1, &error));
    g_assert_no_error(error);
    char *valid_path = reopened.config.path;
    reopened.config.path = g_strdup(failed_path);
    output = "null";
    preference_changed("ao", MPV_FORMAT_STRING, &output, &reopened);
    g_assert_cmpuint(reopened.error_count, ==, 1);
    g_assert_cmpstr(reopened.preference_values.audio_output, ==, "auto");
    g_assert_cmpstr(reopened.preference_values.audio_device, ==, "auto");
    read_audio_settings(&reopened, "", "auto");
    g_autofree char *unchanged = NULL;
    gsize length = 0;
    g_assert_true(g_file_get_contents(failed_path, &unchanged, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(unchanged, length, invalid, sizeof(invalid) - 1);
    g_free(reopened.config.path);
    reopened.config.path = valid_path;
    close_test_player(&reopened);

    Player automatic = {.wake_fd = -1};
    automatic.config.path = g_strdup(path);
    automatic.config.input_path = g_strdup(input);
    g_assert_true(init_mpv(&automatic));
    read_audio_settings(&automatic, "", "auto");
    close_test_player(&automatic);
    g_assert_cmpint(unlink(failed_path), ==, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    puts("PASS audio preferences: stock live AO/device readback, incompatible device reset, "
         "empty-list Auto, atomic save failure and reopened settings without opening audio output");
}

int main(void)
{
    check_cuda_interop_policy();
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("mpv-gtk-preferences-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *input_path = g_build_filename(directory, "input.conf", NULL);
    g_autofree char *interface_path = g_build_filename(directory, "interface.conf", NULL);
    g_autofree char *failed_path = g_build_filename(directory, "failed.conf", NULL);
    g_assert_true(g_file_set_contents(path,
        "# Keep renderer settings in the real mpv config\nvo=libmpv\ngpu-api=auto\nopengl-es=auto\n", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_setenv("MPV_GTK_CONFIG_FILE", path, TRUE));
    Player p = {.wake_fd = -1};
    g_assert_true(init_mpv(&p));
    g_assert_true(i18n_init(path, &error));
    g_assert_no_error(error);
    g_autofree char *before_ui = NULL;
    g_assert_true(g_file_get_contents(path, &before_ui, NULL, &error));
    double cursor_delay = 1.74;
    preference_changed("ui-cursor-hide-delay", MPV_FORMAT_DOUBLE, &cursor_delay, &p);
    g_assert_cmpfloat(p.preference_values.cursor_hide_delay, ==, 1.7);
    g_assert_cmpint(p.cursor_state.delay_us, ==, 1700000);
    g_autofree char *after_cursor = NULL;
    g_assert_true(g_file_get_contents(path, &after_cursor, NULL, &error));
    g_assert_cmpstr(after_cursor, ==, before_ui);
    char *engine_api = mpv_get_property_string(p.mpv, "options/gpu-api");
    char *engine_es = mpv_get_property_string(p.mpv, "options/opengl-es");
    g_assert_nonnull(engine_api);
    g_assert_nonnull(engine_es);
    const char *api = "opengl", *es = "yes";
    preference_changed("gpu-api", MPV_FORMAT_STRING, &api, &p);
    g_autofree char *staged = NULL;
    g_assert_true(g_file_get_contents(path, &staged, NULL, &error));
    g_assert_cmpstr(staged, ==, before_ui);
    preference_changed("opengl-es", MPV_FORMAT_STRING, &es, &p);
    g_assert_null(p.pending_gpu_api);
    g_assert_cmpstr(p.preference_values.gpu_api, ==, "opengl-es");
    /* Changing saved preferences must never swap the implementation under an
     * existing handle or change the active display transport mid-session. */
    g_assert_cmpstr(p.gl_api, ==, "auto");
    char *current_api = mpv_get_property_string(p.mpv, "options/gpu-api");
    char *current_es = mpv_get_property_string(p.mpv, "options/opengl-es");
    g_assert_cmpstr(current_api, ==, engine_api);
    g_assert_cmpstr(current_es, ==, engine_es);
    mpv_free(current_api);
    mpv_free(current_es);
    mpv_free(engine_api);
    mpv_free(engine_es);
    g_autofree char *after_ui = NULL;
    g_assert_true(g_file_get_contents(path, &after_ui, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(after_ui, "vo=libmpv\n"));
    g_assert_nonnull(strstr(after_ui, "gpu-api=opengl\n"));
    g_assert_nonnull(strstr(after_ui, "opengl-es=yes\n"));
    g_autofree char *ui = NULL;
    g_assert_true(g_file_get_contents(interface_path, &ui, NULL, &error));
    g_assert_null(strstr(ui, "gpu-api"));
    g_assert_null(strstr(ui, "video-backend"));
    for (unsigned i = 0; i < G_N_ELEMENTS(settings); ++i) {
        const Setting *setting = &settings[i];
        const void *value = setting->format == MPV_FORMAT_STRING ? (const void *)&setting->text
                          : setting->format == MPV_FORMAT_FLAG ? (const void *)&setting->flag
                          : (const void *)&setting->number;
        preference_changed(setting->name, setting->format, value, &p);
    }
    poll_settings(&p, G_N_ELEMENTS(settings), TRUE);
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
    g_assert_no_error(error);
    const char *expected[] = {
        "hwdec=no", "keepaspect=no", "deinterlace=yes", "deband=yes",
        "loop-file=inf", "audio-delay=0.25", "sub-delay=0.5", "sub-scale=1.5"
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(expected); ++i)
        g_assert_nonnull(strstr(saved, expected[i]));
    /* A batch-save failure must roll back the displayed graphics API;
     * neither staging nor failure may change the current engine options. */
    const char invalid[] = "vo=libmpv\n\0preserve trailing bytes\n";
    g_assert_true(g_file_set_contents(failed_path, invalid, sizeof(invalid) - 1, &error));
    char *regular_path = p.config.path;
    p.config.path = g_strdup(failed_path);
    api = "opengl";
    es = "no";
    preference_changed("gpu-api", MPV_FORMAT_STRING, &api, &p);
    preference_changed("opengl-es", MPV_FORMAT_STRING, &es, &p);
    g_assert_null(p.pending_gpu_api);
    g_assert_cmpstr(p.preference_values.gpu_api, ==, "opengl-es");
    g_assert_cmpuint(p.error_count, ==, 1);
    g_autofree char *unchanged = NULL;
    gsize unchanged_length = 0;
    g_assert_true(g_file_get_contents(failed_path, &unchanged, &unchanged_length, &error));
    g_assert_cmpmem(unchanged, unchanged_length, invalid, sizeof(invalid) - 1);
    g_free(p.config.path);
    p.config.path = regular_path;
    close_test_player(&p);

    Player reopened = {.wake_fd = -1};
    g_assert_true(init_mpv(&reopened));
    g_assert_true(i18n_init(path, &error));
    g_assert_no_error(error);
    g_assert_cmpfloat(i18n_cursor_hide_delay(), ==, 1.7);
    g_assert_cmpstr(reopened.gl_api, ==, "opengl-es");
    poll_settings(&reopened, 0, TRUE);
    close_test_player(&reopened);
    check_audio_settings(directory, input_path);
    check_deferred_hardware(directory, input_path);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(unlink(input_path), ==, 0);
    g_assert_cmpint(unlink(interface_path), ==, 0);
    g_assert_cmpint(unlink(failed_path), ==, 0);
    g_assert_cmpint(rmdir(directory), ==, 0);
    puts("PASS preferences: eight async settings, embedded OpenGL/ES option pairs, atomic save rollback, cursor delay and restart persistence without mutating the active engine");
    return 0;
}
