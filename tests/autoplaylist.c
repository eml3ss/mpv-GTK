/* Exercise evaluated mpv.conf, the real preference callback, and incoming-file
 * policy against stock libmpv. The renderer hook is released with null outputs;
 * this test never constructs GTK widgets or opens sound/video devices. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

#include <glib/gstdio.h>

#define AUTOPLAYLIST_BARRIER UINT64_C(70000)

typedef struct {
    char *root, *media, *subdir;
    char *wav, *flac, *video, *image, *custom, *text, *hidden, *nested;
    guint cases;
} PlaylistFixture;

typedef struct {
    Player player;
    guint starts, loads, ends;
    gboolean restarted, barrier;
} PlaylistHost;

static void ffmpeg(char *args[])
{
    g_autoptr(GError) error = NULL;
    g_autofree char *messages = NULL;
    int status = -1;
    g_assert_true(g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, &messages, &status, &error));
    g_assert_no_error(error);
    if (!g_spawn_check_wait_status(status, &error))
        g_error("ffmpeg: %s; %s", error->message, messages ? messages : "");
}

static void copy_file(const char *source, const char *destination)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *contents = NULL;
    gsize length;
    g_assert_true(g_file_get_contents(source, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(destination, contents, length, &error));
    g_assert_no_error(error);
}

static PlaylistFixture fixture_new(void)
{
    PlaylistFixture f = {0};
    g_autoptr(GError) error = NULL;
    f.root = g_dir_make_tmp("mpv-gtk-autoplaylist-XXXXXX", &error);
    g_assert_no_error(error);
    f.media = g_build_filename(f.root, "media", NULL);
    f.subdir = g_build_filename(f.media, "subdir", NULL);
    g_assert_cmpint(g_mkdir_with_parents(f.subdir, 0700), ==, 0);
    f.wav = g_build_filename(f.media, "track2.wav", NULL);
    f.flac = g_build_filename(f.media, "track10.flac", NULL);
    f.video = g_build_filename(f.media, "movie3.mp4", NULL);
    f.image = g_build_filename(f.media, "cover4.png", NULL);
    f.custom = g_build_filename(f.media, "custom5.xyz", NULL);
    f.text = g_build_filename(f.media, "ignored.txt", NULL);
    f.hidden = g_build_filename(f.media, ".hidden.wav", NULL);
    f.nested = g_build_filename(f.subdir, "track1.wav", NULL);
    char *audio[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=8000:duration=30", "-c:a", "pcm_s16le", f.wav, NULL
    };
    ffmpeg(audio);
    char *flac[] = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                    "-i", f.wav, "-c:a", "flac", f.flac, NULL};
    ffmpeg(flac);
    char *video[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "color=c=red:size=32x32:rate=1:duration=1", "-c:v", "mpeg4", "-an", f.video, NULL
    };
    ffmpeg(video);
    char *image[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "color=c=green:size=4x4:rate=1", "-frames:v", "1", "-threads", "1", f.image, NULL
    };
    ffmpeg(image);
    copy_file(f.wav, f.custom); /* Valid audio with an initially unknown extension. */
    copy_file(f.wav, f.hidden);
    copy_file(f.wav, f.nested);
    g_assert_true(g_file_set_contents(f.text, "Not a media file.\n", -1, &error));
    g_assert_no_error(error);
    return f;
}

static void remove_tree(const char *path)
{
    g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
    g_assert_nonnull(directory);
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR))
            remove_tree(child);
        else
            g_assert_cmpint(g_remove(child), ==, 0);
    }
    g_clear_pointer(&directory, g_dir_close);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void fixture_free(PlaylistFixture *f)
{
    remove_tree(f->root);
    g_free(f->root); g_free(f->media); g_free(f->subdir);
    g_free(f->wav); g_free(f->flac); g_free(f->video); g_free(f->image);
    g_free(f->custom); g_free(f->text); g_free(f->hidden); g_free(f->nested);
}

static void poll_host(PlaylistHost *host)
{
    Player *p = &host->player;
    mpv_event *event = mpv_wait_event(p->mpv, 0.02);
    switch (event->event_id) {
    case MPV_EVENT_HOOK: {
        mpv_event_hook *hook = event->data;
        /* Only the renderer barrier is bypassed: all loading, playlist
         * expansion, option evaluation and decoding use the stock engine. */
        g_assert_cmpint(mpv_hook_continue(p->mpv, hook->id), >=, 0);
        break;
    }
    case MPV_EVENT_START_FILE:
        host->starts++;
        host->restarted = FALSE;
        p->timeline.loaded = FALSE;
        p->media_loading = TRUE;
        break;
    case MPV_EVENT_FILE_LOADED:
        host->loads++;
        p->timeline.loaded = TRUE;
        p->media_loading = FALSE;
        p->idle = FALSE;
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        host->restarted = TRUE;
        break;
    case MPV_EVENT_END_FILE: {
        mpv_event_end_file *end = event->data;
        g_assert_cmpint(end->error, >=, 0);
        g_assert_cmpint(end->reason, !=, MPV_END_FILE_REASON_ERROR);
        host->ends++;
        p->timeline.loaded = FALSE;
        p->media_loading = FALSE;
        p->idle = TRUE;
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *property = event->data;
        if (!property->data || property->format == MPV_FORMAT_NONE)
            break;
        if (g_str_equal(property->name, "time-pos"))
            p->timeline.position = *(double *)property->data;
        else if (g_str_equal(property->name, "duration"))
            p->timeline.duration = *(double *)property->data;
        else if (g_str_equal(property->name, "pause"))
            p->timeline.paused = !!*(int *)property->data;
        else if (g_str_equal(property->name, "playlist-count"))
            p->playlist_count = *(int64_t *)property->data;
        else if (g_str_equal(property->name, "playlist-pos"))
            p->playlist_position = *(int64_t *)property->data;
        break;
    }
    case MPV_EVENT_GET_PROPERTY_REPLY:
        if (event->reply_userdata == AUTOPLAYLIST_BARRIER) {
            g_assert_cmpint(event->error, >=, 0);
            host->barrier = TRUE;
        }
        break;
    case MPV_EVENT_COMMAND_REPLY:
    case MPV_EVENT_SET_PROPERTY_REPLY:
        g_assert_cmpint(event->error, >=, 0);
        break;
    case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *log = event->data;
        g_error("libmpv %s: %s", log->prefix, log->text);
        break;
    }
    default:
        break;
    }
}

static void barrier(PlaylistHost *host)
{
    host->barrier = FALSE;
    g_assert_cmpint(mpv_get_property_async(host->player.mpv, AUTOPLAYLIST_BARRIER,
                                         "playlist-count", MPV_FORMAT_INT64), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host->barrier) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_host(host);
    }
    g_assert_cmpuint(host->player.error_count, ==, 0);
}

static PlaylistHost host_new(const char *config_path)
{
    PlaylistHost host = {0};
    Player *p = &host.player;
    p->wake_fd = -1;
    p->test_seconds = 1;
    p->pending = g_ptr_array_new_with_free_func(g_free);
    p->config.path = g_strdup(config_path);
    g_autofree char *directory = g_path_get_dirname(config_path);
    p->config.input_path = g_build_filename(directory, "input.conf", NULL);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(p->config.input_path, "", 0, &error));
    g_assert_no_error(error);
    g_assert_true(i18n_init(config_path, &error));
    g_assert_no_error(error);
    g_assert_true(init_mpv(p));
    g_assert_cmpint(mpv_set_option_string(p->mpv, "vo", "null"), >=, 0);
    g_assert_cmpint(mpv_set_option_string(p->mpv, "ao", "null"), >=, 0);
    p->ready = TRUE;
    /* init_mpv copied a stack address into its wakeup callback. This host is
     * returned by value and polled directly, so it must not retain that pointer. */
    mpv_set_wakeup_callback(p->mpv, NULL, NULL);
    return host;
}

static void host_free(PlaylistHost *host)
{
    Player *p = &host->player;
    p->closing = TRUE;
    mpv_terminate_destroy(p->mpv);
    if (p->wake_fd >= 0)
        close(p->wake_fd);
    if (p->gl_library)
        dlclose(p->gl_library);
    config_clear(&p->config);
    g_clear_pointer(&p->pending, g_ptr_array_unref);
    g_queue_clear_full(&p->pending_commands, (GDestroyNotify)g_strfreev);
    g_free(p->hwdec); g_free(p->hwdec_current);
    g_free(p->audio_output); g_free(p->audio_device);
    for (guint i = 0; i < G_N_ELEMENTS(p->playlist_values); ++i)
        g_free(p->playlist_values[i]);
}

static char *case_config(PlaylistFixture *f, const char *options)
{
    g_autofree char *name = g_strdup_printf("case%u", ++f->cases);
    g_autofree char *directory = g_build_filename(f->root, name, NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    char *path = g_build_filename(directory, "mpv.conf", NULL);
    g_autofree char *contents = g_strconcat("hwdec=no\nao=null\npause=yes\n",
                                           options, NULL);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
    return path;
}

static void assert_option(Player *p, const char *name, const char *expected)
{
    char *value = mpv_get_property_string(p->mpv, name);
    g_assert_cmpstr(value, ==, expected);
    mpv_free(value);
}

static void assert_list(Player *p, const char *name, const char *const *expected, guint count)
{
    mpv_node node = {0};
    g_assert_cmpint(mpv_get_property(p->mpv, name, MPV_FORMAT_NODE, &node), >=, 0);
    g_assert_cmpint(node.format, ==, MPV_FORMAT_NODE_ARRAY);
    g_assert_nonnull(node.u.list);
    g_assert_cmpint(node.u.list->num, ==, count);
    for (guint i = 0; i < count; ++i) {
        g_assert_cmpint(node.u.list->values[i].format, ==, MPV_FORMAT_STRING);
        g_assert_cmpstr(node.u.list->values[i].u.string, ==, expected[i]);
    }
    mpv_free_node_contents(&node);
}

static const mpv_node *field(const mpv_node *map, const char *key)
{
    g_assert_cmpint(map->format, ==, MPV_FORMAT_NODE_MAP);
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_str_equal(map->u.list->keys[i], key))
            return &map->u.list->values[i];
    g_error("Missing playlist entry field: %s", key);
    return NULL;
}

static void assert_playlist(PlaylistHost *host, const char *const *paths,
                             guint count, guint current)
{
    mpv_node node = {0};
    g_assert_cmpint(mpv_get_property(host->player.mpv, "playlist", MPV_FORMAT_NODE, &node), >=, 0);
    g_assert_cmpint(node.format, ==, MPV_FORMAT_NODE_ARRAY);
    g_assert_cmpint(node.u.list->num, ==, count);
    for (guint i = 0; i < count; ++i) {
        const mpv_node *filename = field(&node.u.list->values[i], "filename");
        g_assert_cmpint(filename->format, ==, MPV_FORMAT_STRING);
        g_assert_cmpstr(filename->u.string, ==, paths[i]);
    }
    mpv_free_node_contents(&node);
    int64_t position = -1;
    g_assert_cmpint(mpv_get_property(host->player.mpv, "playlist-pos", MPV_FORMAT_INT64, &position), >=, 0);
    g_assert_cmpint(position, ==, current);
}

static GPtrArray *paths_new(const char *const *paths, guint count)
{
    GPtrArray *array = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < count; ++i)
        g_ptr_array_add(array, g_strdup(paths[i]));
    return array;
}

static void open_files(PlaylistHost *host, const char *const *paths, guint count)
{
    g_autoptr(GPtrArray) array = paths_new(paths, count);
    guint loaded = host->loads;
    g_assert_true(open_paths(&host->player, array));
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (host->loads == loaded || !host->player.timeline.loaded || !host->restarted) {
        if (g_get_monotonic_time() >= deadline)
            g_error("Open timed out: loads=%u/%u starts=%u ends=%u loaded=%d restarting=%d",
                    host->loads, loaded, host->starts, host->ends,
                    host->player.timeline.loaded, host->restarted);
        poll_host(host);
    }
    barrier(host);
}

static void open_file(PlaylistHost *host, const char *path)
{
    const char *paths[] = {path};
    open_files(host, paths, 1);
}

static void change_text(PlaylistHost *host, const char *name, const char *value)
{
    preference_changed(name, MPV_FORMAT_STRING, &value, &host->player);
    barrier(host);
}

static void set_policy(PlaylistHost *host, gboolean replace, gboolean append)
{
    int allow = replace, add = append;
    preference_changed("ui-allow-replace", MPV_FORMAT_FLAG, &allow, &host->player);
    preference_changed("ui-add-to-queue", MPV_FORMAT_FLAG, &add, &host->player);
    g_assert_cmpint(i18n_allow_replace(), ==, replace);
    g_assert_cmpint(i18n_add_to_queue(), ==, append);
}

static void set_paused_position(PlaylistHost *host, double position)
{
    int pause = 1;
    g_assert_cmpint(mpv_set_property(host->player.mpv, "pause", MPV_FORMAT_FLAG, &pause), >=, 0);
    char number[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(number, sizeof(number), position);
    const char *seek[] = {"seek", number, "absolute+exact", NULL};
    g_assert_cmpint(mpv_command_async(host->player.mpv, 0, seek), >=, 0);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host->player.timeline.paused || fabs(host->player.timeline.position - position) > .1) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_host(host);
    }
    barrier(host);
}

static void check_modes(PlaylistFixture *f)
{
    const char *modes[] = {"no", "filter", "same"};
    for (guint i = 0; i < G_N_ELEMENTS(modes); ++i) {
        g_autofree char *options = g_strdup_printf(
            "autocreate-playlist=%s\ndirectory-mode=ignore\ndirectory-filter-types=video\n", modes[i]);
        g_autofree char *config = case_config(f, options);
        PlaylistHost host = host_new(config);
        assert_option(&host.player, "autocreate-playlist", modes[i]);
        open_file(&host, f->wav);
        const char *single[] = {f->wav};
        const char *filter[] = {f->video, f->wav};
        const char *same[] = {f->wav, f->flac};
        if (i == 0) assert_playlist(&host, single, 1, 0);
        if (i == 1) assert_playlist(&host, filter, 2, 1);
        if (i == 2) {
            assert_playlist(&host, same, 2, 0);
            g_autofree char *uri = g_filename_to_uri(f->flac, NULL, NULL);
            g_assert_nonnull(uri);
            open_file(&host, uri);
            assert_playlist(&host, same, 2, 1);
            open_file(&host, f->custom);
            const char *unknown[] = {f->custom};
            assert_playlist(&host, unknown, 1, 0);
        }
        host_free(&host);
    }
}

static void check_live_settings(PlaylistFixture *f)
{
    g_autofree char *config = case_config(f, "directory-mode=ignore\ndirectory-filter-types=audio\n");
    PlaylistHost host = host_new(config);
    assert_option(&host.player, "autocreate-playlist", "no");
    open_file(&host, f->wav);
    set_paused_position(&host, 4);
    const char *single[] = {f->wav};
    change_text(&host, "autocreate-playlist", "same");
    assert_option(&host.player, "autocreate-playlist", "same");
    assert_playlist(&host, single, 1, 0); /* Live change does not rebuild a queue. */
    open_file(&host, f->wav);
    const char *audio[] = {f->wav, f->flac};
    assert_playlist(&host, audio, 2, 0);
    const char *names[] = {"video-exts", "audio-exts", "image-exts", "archive-exts", "playlist-exts"};
    const char *values[] = {"mp4", "wav,flac,xyz", "png", "zip", "m3u"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i)
        change_text(&host, names[i], values[i]);
    const char *extensions[] = {"wav", "flac", "xyz"};
    assert_list(&host.player, "audio-exts", extensions, 3);
    assert_playlist(&host, audio, 2, 0);
    open_file(&host, f->wav);
    const char *custom_audio[] = {f->custom, f->wav, f->flac};
    assert_playlist(&host, custom_audio, 3, 1);
    host_free(&host);

    host = host_new(config); /* Re-evaluate the callback's actual persisted mpv.conf. */
    assert_option(&host.player, "autocreate-playlist", "same");
    assert_list(&host.player, "audio-exts", extensions, 3);
    open_file(&host, f->wav);
    assert_playlist(&host, custom_audio, 3, 1);
    change_text(&host, "autocreate-playlist", "filter");
    change_text(&host, "directory-filter-types", "");
    assert_list(&host.player, "directory-filter-types", NULL, 0);
    assert_playlist(&host, custom_audio, 3, 1);
    open_file(&host, f->wav);
    const char *all[] = {f->image, f->custom, f->text, f->video, f->wav, f->flac};
    assert_playlist(&host, all, 6, 4);
    host_free(&host);

    host = host_new(config);
    assert_list(&host.player, "directory-filter-types", NULL, 0);
    open_file(&host, f->wav);
    assert_playlist(&host, all, 6, 4);
    change_text(&host, "directory-filter-types", "audio");
    const char *type[] = {"audio"};
    assert_list(&host.player, "directory-filter-types", type, 1);
    open_file(&host, f->wav);
    assert_playlist(&host, custom_audio, 3, 1);
    host_free(&host);

    host = host_new(config); /* A saved -clr must not override the later list. */
    assert_list(&host.player, "directory-filter-types", type, 1);
    host_free(&host);
}

static void check_directories(PlaylistFixture *f)
{
    g_autofree char *config = case_config(f,
        "autocreate-playlist=no\ndirectory-mode=ignore\ndirectory-filter-types=audio\n");
    PlaylistHost host = host_new(config);
    open_file(&host, f->media);
    const char *flat[] = {f->wav, f->flac};
    assert_playlist(&host, flat, 2, 0);
    change_text(&host, "directory-mode", "lazy");
    open_file(&host, f->media);
    const char *lazy[] = {f->wav, f->flac, f->subdir};
    assert_playlist(&host, lazy, 3, 0);
    change_text(&host, "directory-mode", "recursive");
    open_file(&host, f->media);
    const char *recursive[] = {f->wav, f->flac, f->nested};
    assert_playlist(&host, recursive, 3, 0);
    change_text(&host, "directory-mode", "auto");
    open_file(&host, f->media);
    assert_playlist(&host, lazy, 3, 0); /* Default shuffle=no makes auto lazy. */
    host_free(&host);
}

static void check_policies(PlaylistFixture *f)
{
    g_autofree char *config = case_config(f, "autocreate-playlist=no\ndirectory-mode=ignore\n");
    PlaylistHost host = host_new(config);
    g_assert_true(i18n_allow_replace());
    g_assert_false(i18n_add_to_queue());
    set_policy(&host, FALSE, FALSE);
    open_file(&host, f->wav); /* Both off must still permit the first open. */
    set_paused_position(&host, 5);
    const char *incoming[] = {f->flac};
    g_autoptr(GPtrArray) paths = paths_new(incoming, 1);
    guint loads = host.loads, starts = host.starts;
    g_assert_false(open_paths(&host.player, paths));
    barrier(&host);
    const char *one[] = {f->wav};
    assert_playlist(&host, one, 1, 0);
    g_assert_cmpuint(host.loads, ==, loads);
    g_assert_cmpuint(host.starts, ==, starts);
    assert_option(&host.player, "pause", "yes");
    g_assert_cmpfloat_with_epsilon(host.player.timeline.position, 5, .1);

    set_policy(&host, TRUE, TRUE); /* Add-to-queue wins over Allow replace. */
    g_assert_true(open_paths(&host.player, paths));
    barrier(&host);
    const char *two[] = {f->wav, f->flac};
    assert_playlist(&host, two, 2, 0);
    g_assert_cmpuint(host.loads, ==, loads);
    g_assert_cmpuint(host.starts, ==, starts);
    assert_option(&host.player, "pause", "yes");
    g_assert_cmpfloat_with_epsilon(host.player.timeline.position, 5, .1);

    set_policy(&host, TRUE, FALSE);
    open_file(&host, f->flac);
    const char *replaced[] = {f->flac};
    assert_playlist(&host, replaced, 1, 0);
    assert_option(&host.player, "pause", "no");
    host_free(&host);

    host = host_new(config);
    set_policy(&host, FALSE, FALSE);
    const char *first[] = {f->wav};
    g_autoptr(GPtrArray) initial = paths_new(first, 1);
    g_assert_true(open_paths(&host.player, initial));
    g_assert_true(host.player.media_loading);
    g_assert_false(open_paths(&host.player, paths)); /* Loading is already active. */
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!host.loads || !host.restarted) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        poll_host(&host);
    }
    barrier(&host);
    assert_playlist(&host, one, 1, 0);

    /* Before libmpv/GTK initialization completes, pending incoming paths
     * already count as active media for the same replacement policy. */
    Player deferred = {0};
    g_assert_true(open_paths(&deferred, initial));
    g_assert_cmpuint(deferred.pending->len, ==, 1);
    g_assert_false(open_paths(&deferred, paths));
    g_assert_cmpuint(deferred.pending->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(deferred.pending, 0), ==, f->wav);
    set_policy(&host, FALSE, TRUE);
    g_assert_true(open_paths(&deferred, paths));
    g_assert_cmpuint(deferred.pending->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(deferred.pending, 1), ==, f->flac);
    g_ptr_array_unref(deferred.pending);
    host_free(&host);

    host = host_new(config);
    g_assert_false(i18n_allow_replace());
    g_assert_true(i18n_add_to_queue());
    open_file(&host, f->wav); /* Queue-only policy must still start an idle player. */
    assert_playlist(&host, one, 1, 0);
    assert_option(&host.player, "pause", "no");
    host_free(&host);

    g_autofree char *batch_config = case_config(f,
        "autocreate-playlist=same\ndirectory-mode=lazy\ndirectory-filter-types=audio\n");
    host = host_new(batch_config);
    open_files(&host, two, 2);
    assert_playlist(&host, two, 2, 0);
    assert_option(&host.player, "autocreate-playlist", "same");
    host_free(&host);
}

int main(void)
{
    setlocale(LC_NUMERIC, "C");
    PlaylistFixture fixture = fixture_new();
    check_modes(&fixture);
    check_live_settings(&fixture);
    check_directories(&fixture);
    check_policies(&fixture);
    fixture_free(&fixture);
    puts("PASS automatic playlists: stock modes, directories, extension/empty-list persistence, next-open effects and incoming-file policies");
    return 0;
}
