#define _GNU_SOURCE
#include "trim_export.h"
#include "mp4_titles.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define EXPORT_COMMAND_BASE (UINT64_C(1) << 46)
#define EXPORT_COMMAND_LIMIT (EXPORT_COMMAND_BASE + (UINT64_C(1) << 30))
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

typedef struct {
    dev_t device;
    ino_t inode;
    off_t size;
    struct timespec modified;
    mode_t mode;
} FileIdentity;

typedef enum { OP_PREPARE, OP_SEAL, OP_COMMIT, OP_ROLLBACK,
               OP_CLEAN_SUCCESS, OP_CLEAN_FAILURE } FileOperation;
typedef enum { PROCESS_EXPORT, PROCESS_PROBE, PROCESS_FRAME,
               PROCESS_ANALYZE, PROCESS_PACKETS, PROCESS_HEAD, PROCESS_TAIL, PROCESS_ENDING,
               PROCESS_SPLICE, PROCESS_SPLICE_END } ProcessStage;

typedef struct ExportJob {
    gint references;
    GCancellable *cancel;
    char *input, *source, *directory, *workspace, *temp, *backup, *target, *output;
    char *ffmpeg, *ffprobe, *phase, *failure, *open_path, *idle, *force_window;
    char *extension, *renamed; /* Audio-only container; replacement saved under a new name. */
    /* Frame-accurate cuts ("smart cut"): a re-encoded head up to the first
     * keyframe, a stream-copied middle ("tail") up to the last keyframe, a
     * re-encoded ending, and the remaining source streams. */
    TrimPreciseMode precise;
    gint64 splice_ms, end_splice_ms, video_index, tail_packets;
    double start_time, ending_start;
    gboolean smart;
    char *codec, *pix_fmt, *profile, *rotation, *head, *tail, *ending, *list;
    const char *encoders[3];
    guint encoder_count, encoder_index;
    const char *precise_result; /* "", "cpu", "gpu" or "fallback". */
    FileIdentity original, verified, replacement;
    gboolean replace, open_result, committed, backup_created, sealed, exclude_video;
    gboolean saved, pause, user_changed, expected_load, seek_issued, restored;
    double position, open_position;
    gint64 index, entry_id, start_ms, end_ms;
    GArray *excluded_audio;
    guint timeout;
    uint64_t process_id;
    ProcessStage process_stage;
    gboolean worker_pending, inserted;
} ExportJob;

struct TrimExport {
    gint references;
    gboolean stopped;
    mpv_handle *mpv;
    ExportJob *job;
    uint64_t serial;
    char *deferred_idle, *deferred_force;
};

typedef struct {
    TrimExport *owner;
    ExportJob *job;
    FileOperation operation;
} FileTask;

static void finish(TrimExport *e, gboolean success, const char *message);
static void fail_job(TrimExport *e, const char *message);
static void file_operation(TrimExport *e, FileOperation operation);
static void begin_process(TrimExport *e, ProcessStage stage);
static void open_video(TrimExport *e, gboolean restoring);
static void recover_open(TrimExport *e, const char *message);

static const mpv_node *map_field(const mpv_node *map, const char *key)
{
    if (!map || map->format != MPV_FORMAT_NODE_MAP || !map->u.list) return NULL;
    for (int i = 0; i < map->u.list->num; ++i)
        if (g_strcmp0(map->u.list->keys[i], key) == 0) return &map->u.list->values[i];
    return NULL;
}

static char *captured_text(const mpv_node *map, const char *key)
{
    const mpv_node *value = map_field(map, key);
    if (value && value->format == MPV_FORMAT_STRING && value->u.string) return g_strdup(value->u.string);
    if (value && value->format == MPV_FORMAT_BYTE_ARRAY && value->u.ba && value->u.ba->data && value->u.ba->size)
        return g_strndup(value->u.ba->data, MIN(value->u.ba->size, (size_t)(1024 * 1024)));
    return g_strdup("");
}

static gint64 map_integer(const mpv_node *map, const char *key, gint64 fallback)
{
    const mpv_node *value = map_field(map, key);
    return value && value->format == MPV_FORMAT_INT64 ? value->u.int64 : fallback;
}

static void publish_map(TrimExport *e, const char **keys, mpv_node *values, int count)
{
    if (e->stopped) return;
    mpv_node_list list = {.num = count, .keys = (char **)keys, .values = values};
    mpv_node node = {.format = MPV_FORMAT_NODE_MAP, .u.list = &list};
    mpv_set_property(e->mpv, "user-data/trim-export", MPV_FORMAT_NODE, &node);
}

static void publish(TrimExport *e, const char *phase)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    if (phase) { g_free(j->phase); j->phase = g_strdup(phase); }
    const char *keys[] = {"busy", "phase", "mode", "source", "start_ms", "end_ms", "path"};
    mpv_node values[] = {
        {.format = MPV_FORMAT_FLAG, .u.flag = 1},
        {.format = MPV_FORMAT_STRING, .u.string = j->phase ? j->phase : "preparing"},
        {.format = MPV_FORMAT_STRING, .u.string = j->replace ? "replace" : "keep"},
        {.format = MPV_FORMAT_STRING, .u.string = j->source ? j->source : j->input},
        {.format = MPV_FORMAT_INT64, .u.int64 = j->start_ms},
        {.format = MPV_FORMAT_INT64, .u.int64 = j->end_ms},
        {.format = MPV_FORMAT_STRING, .u.string = j->target ? j->target : ""}
    };
    publish_map(e, keys, values, G_N_ELEMENTS(keys));
}

static ExportJob *job_ref(ExportJob *j) { g_atomic_int_inc(&j->references); return j; }

static void job_unref(ExportJob *j)
{
    if (!j || !g_atomic_int_dec_and_test(&j->references)) return;
    if (j->timeout) g_source_remove(j->timeout);
    g_clear_object(&j->cancel);
    g_free(j->input); g_free(j->source); g_free(j->directory); g_free(j->workspace);
    g_free(j->temp); g_free(j->backup); g_free(j->target); g_free(j->output);
    g_free(j->ffmpeg); g_free(j->ffprobe); g_free(j->phase); g_free(j->failure);
    g_free(j->open_path); g_free(j->idle); g_free(j->force_window);
    g_free(j->extension); g_free(j->renamed);
    g_free(j->codec); g_free(j->pix_fmt); g_free(j->profile); g_free(j->rotation);
    g_free(j->head); g_free(j->tail); g_free(j->ending); g_free(j->list);
    g_clear_pointer(&j->excluded_audio, g_array_unref);
    g_free(j);
}

static TrimExport *export_ref(TrimExport *e) { g_atomic_int_inc(&e->references); return e; }

static void export_unref(TrimExport *e)
{
    if (!g_atomic_int_dec_and_test(&e->references)) return;
    job_unref(e->job);
    g_free(e->deferred_idle); g_free(e->deferred_force);
    g_free(e);
}

static void file_task_free(gpointer data)
{
    FileTask *task = data;
    job_unref(task->job); export_unref(task->owner); g_free(task);
}

static gboolean file_error(GError **error, const char *operation, const char *path)
{
    int code = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "%s %s: %s",
                operation, path ? path : "", g_strerror(code));
    return FALSE;
}

static void identity_from_stat(FileIdentity *identity, const struct stat *status)
{
    *identity = (FileIdentity){.device = status->st_dev, .inode = status->st_ino,
        .size = status->st_size, .modified = status->st_mtim, .mode = status->st_mode & 0777};
}

static gboolean read_identity(const char *path, FileIdentity *identity, GError **error)
{
    struct stat status;
    if (stat(path, &status) != 0) return file_error(error, "Cannot inspect", path);
    if (!S_ISREG(status.st_mode)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                            "Only local regular video files can be exported.");
        return FALSE;
    }
    identity_from_stat(identity, &status);
    return TRUE;
}

static gboolean same_identity(const FileIdentity *a, const FileIdentity *b)
{
    /* ctime intentionally excluded: creating the backup hard link changes it. */
    return a->device == b->device && a->inode == b->inode && a->size == b->size &&
        a->modified.tv_sec == b->modified.tv_sec && a->modified.tv_nsec == b->modified.tv_nsec;
}

static gboolean check_original(ExportJob *j, GError **error)
{
    FileIdentity current;
    if (!read_identity(j->source, &current, error)) return FALSE;
    if (!same_identity(&current, &j->original)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "The original changed during export; it was not replaced.");
        return FALSE;
    }
    return TRUE;
}

static char *resolved_path(mpv_handle *mpv, const char *path)
{
    if (!path || !*path) return NULL;
    g_autofree char *decoded = g_str_has_prefix(path, "file://") ? g_filename_from_uri(path, NULL, NULL) : NULL;
    if (strstr(path, "://") && !decoded) return NULL;
    char *working = mpv_get_property_string(mpv, "working-directory");
    g_autofree char *absolute = g_canonicalize_filename(decoded ? decoded : path,
                                                       working && *working ? working : NULL);
    mpv_free(working);
    return realpath(absolute, NULL);
}

static gboolean same_source(TrimExport *e)
{
    char *path = mpv_get_property_string(e->mpv, "path");
    g_autofree char *resolved = resolved_path(e->mpv, path);
    mpv_free(path);
    return e->job && resolved && g_str_equal(resolved, e->job->source);
}

static gboolean cancelled(ExportJob *j, GError **error)
{
    return g_cancellable_set_error_if_cancelled(j->cancel, error);
}

static gboolean copy_exclusive(const char *source, const char *target, ExportJob *j,
                                gboolean preserve_times, const FileIdentity *expected,
                                GError **error)
{
    int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return file_error(error, "Cannot open", source);
    struct stat source_status;
    gboolean ok = fstat(input, &source_status) == 0;
    if (!ok) file_error(error, "Cannot inspect", source);
    FileIdentity identity;
    if (ok) {
        identity_from_stat(&identity, &source_status);
        if (!S_ISREG(source_status.st_mode) || (expected && !same_identity(&identity, expected))) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "The export source changed before copying.");
            ok = FALSE;
        }
    }
    int output = ok ? open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    if (ok && output < 0) ok = file_error(error, "Cannot create", target);
    struct stat output_status;
    gboolean owned = output >= 0 && fstat(output, &output_status) == 0;
    char buffer[128 * 1024];
    while (ok) {
        if (cancelled(j, error)) { ok = FALSE; break; }
        ssize_t got;
        do { got = read(input, buffer, sizeof(buffer)); } while (got < 0 && errno == EINTR);
        if (!got) break;
        if (got < 0) { ok = file_error(error, "Cannot read", source); break; }
        ssize_t written = 0;
        while (written < got) {
            ssize_t count;
            do { count = write(output, buffer + written, (size_t)(got - written)); } while (count < 0 && errno == EINTR);
            if (count <= 0) { ok = file_error(error, "Cannot write", target); break; }
            written += count;
        }
    }
    if (ok && fchmod(output, source_status.st_mode & 0777) != 0) ok = file_error(error, "Cannot set permissions on", target);
    if (ok && preserve_times) {
        struct timespec times[] = {source_status.st_atim, source_status.st_mtim};
        if (futimens(output, times) != 0) ok = file_error(error, "Cannot preserve timestamps on", target);
    }
    if (ok && fsync(output) != 0) ok = file_error(error, "Cannot synchronize", target);
    if (ok) {
        struct stat current;
        if (!owned || lstat(target, &current) != 0 || current.st_dev != output_status.st_dev || current.st_ino != output_status.st_ino) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "The export destination changed while copying.");
            ok = FALSE;
        }
    }
    if (ok && fstat(input, &source_status) != 0) ok = file_error(error, "Cannot inspect", source);
    if (ok) {
        FileIdentity after;
        identity_from_stat(&after, &source_status);
        if (!same_identity(&identity, &after)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "The export source changed while copying.");
            ok = FALSE;
        }
    }
    if (output >= 0) close(output);
    close(input);
    if (!ok && owned) {
        struct stat current;
        if (lstat(target, &current) == 0 && current.st_dev == output_status.st_dev && current.st_ino == output_status.st_ino)
            unlink(target);
    }
    return ok;
}

static char *filename_time(gint64 ms)
{
    return g_strdup_printf("%02" G_GINT64_FORMAT ".%02" G_GINT64_FORMAT ".%02" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT,
        ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}

static gboolean prepare_files(ExportJob *j, GError **error)
{
    if (cancelled(j, error) || !read_identity(j->source, &j->original, error)) return FALSE;
    j->directory = g_path_get_dirname(j->source);
    g_autofree char *name = g_path_get_basename(j->source);
    char *dot = strrchr(name, '.');
    const char *source_extension = dot && dot != name ? dot : "";
    g_autofree char *stem = *source_extension ? g_strndup(name, source_extension - name) : g_strdup(name);
    const char *extension = j->extension ? j->extension : source_extension;
    g_autofree char *first = filename_time(j->start_ms), *last = filename_time(j->end_ms);
    g_autofree char *target_name = g_strdup_printf("%s_%s-%s%s", stem, first, last, extension);
    j->target = g_build_filename(j->directory, target_name, NULL);
    if (j->replace && g_strcmp0(extension, source_extension) != 0) {
        g_autofree char *renamed = g_strconcat(stem, extension, NULL);
        j->renamed = g_build_filename(j->directory, renamed, NULL);
    }
    j->workspace = g_build_filename(j->directory, ".mpv-trim-XXXXXX", NULL);
    if (!g_mkdtemp_full(j->workspace, 0700)) return file_error(error, "Cannot create export workspace", j->directory);
    g_autofree char *temp_name = g_strconcat("partial", extension, NULL);
    g_autofree char *backup_name = g_strconcat("original", extension, NULL);
    j->temp = g_build_filename(j->workspace, temp_name, NULL);
    j->backup = g_build_filename(j->workspace, backup_name, NULL);
    j->head = g_build_filename(j->workspace, "head.ts", NULL);
    j->tail = g_build_filename(j->workspace, "tail.ts", NULL);
    j->ending = g_build_filename(j->workspace, "ending.ts", NULL);
    j->list = g_build_filename(j->workspace, "parts.ffconcat", NULL);
    return TRUE;
}

static gboolean commit_copy(ExportJob *j, GError **error)
{
    g_autofree char *name = g_path_get_basename(j->target);
    char *dot = strrchr(name, '.');
    const char *extension = dot && dot != name ? dot : "";
    g_autofree char *stem = *extension ? g_strndup(name, extension - name) : g_strdup(name);
    for (guint suffix = 1; suffix <= 10000; ++suffix) {
        if (cancelled(j, error)) return FALSE;
        g_autofree char *next_name = suffix == 1 ? g_strdup(name) : g_strdup_printf("%s_%u%s", stem, suffix, extension);
        g_autofree char *target = g_build_filename(j->directory, next_name, NULL);
        int result = -1;
#ifdef SYS_renameat2
        result = (int)syscall(SYS_renameat2, AT_FDCWD, j->temp, AT_FDCWD, target, RENAME_NOREPLACE);
        if (result == 0) { j->output = g_steal_pointer(&target); return TRUE; }
        if (errno == EEXIST || errno == ENOTEMPTY) continue;
        if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return file_error(error, "Cannot save", target);
#endif
        result = link(j->temp, target);
        if (result == 0) { unlink(j->temp); j->output = g_steal_pointer(&target); return TRUE; }
        if (errno == EEXIST) continue;
        if (copy_exclusive(j->temp, target, j, FALSE, &j->verified, error)) {
            unlink(j->temp); j->output = g_steal_pointer(&target); return TRUE;
        }
        if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS)) { g_clear_error(error); continue; }
        return FALSE;
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Too many exports already use this filename.");
    return FALSE;
}

static gboolean rename_exclusive(const char *source, const char *target, GError **error)
{
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD, target, RENAME_NOREPLACE) == 0) return TRUE;
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
        if (errno == EEXIST || errno == ENOTEMPTY) goto exists;
        return file_error(error, "Cannot save", target);
    }
#endif
    if (link(source, target) == 0) { unlink(source); return TRUE; }
    if (errno != EEXIST) return file_error(error, "Cannot save", target);
#ifdef SYS_renameat2
exists:
#endif
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "%s already exists; the original was not replaced.", target);
    return FALSE;
}

static gboolean commit_files(ExportJob *j, GError **error)
{
    FileIdentity current;
    if (cancelled(j, error) || !check_original(j, error) || !read_identity(j->temp, &current, error)) return FALSE;
    if (!j->sealed || !same_identity(&current, &j->verified) || current.size <= 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "The verified export changed before it could be saved.");
        return FALSE;
    }
    if (!j->replace) return commit_copy(j, error);
    if (link(j->source, j->backup) == 0) j->backup_created = TRUE;
    else {
        if (!copy_exclusive(j->source, j->backup, j, TRUE, &j->original, error)) return FALSE;
        j->backup_created = TRUE;
    }
    if (cancelled(j, error) || !check_original(j, error)) return FALSE;
    FileIdentity fresh;
    if (!read_identity(j->source, &fresh, error)) return FALSE;
    if (!same_identity(&fresh, &j->original)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "The original changed during export; it was not replaced.");
        return FALSE;
    }
    if (chmod(j->temp, fresh.mode) != 0) return file_error(error, "Cannot set permissions on", j->temp);
    if (j->renamed) {
        /* The original stays in place until the new file exists and is
         * recorded, so rollback can always restore it from the backup. */
        if (!rename_exclusive(j->temp, j->renamed, error)) return FALSE;
        j->committed = TRUE;
        j->output = g_strdup(j->renamed);
        if (!read_identity(j->renamed, &j->replacement, error)) return FALSE;
        if (unlink(j->source) != 0) return file_error(error, "Cannot remove", j->source);
        return TRUE;
    }
    if (rename(j->temp, j->source) != 0) return file_error(error, "Cannot replace", j->source);
    j->committed = TRUE;
    j->output = g_strdup(j->source);
    if (!read_identity(j->source, &j->replacement, error)) return FALSE;
    return TRUE;
}

static gboolean rollback_files(ExportJob *j, GError **error)
{
    if (cancelled(j, error)) return FALSE;
    FileIdentity current;
    if (!j->backup_created) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "The original backup is missing.");
        return FALSE;
    }
    const char *replaced = j->renamed ? j->renamed : j->source;
    struct stat status;
    if (lstat(replaced, &status) == 0) {
        if (!S_ISREG(status.st_mode) || !read_identity(replaced, &current, error)) {
            if (!error || !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                "The replacement path changed; the original backup was retained.");
            return FALSE;
        }
        if (!same_identity(&current, &j->replacement)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "The replacement changed after export; the backup was retained instead of overwriting it.");
            return FALSE;
        }
        if (j->renamed && unlink(replaced) != 0) return file_error(error, "Cannot remove", replaced);
    } else if (errno != ENOENT) {
        return file_error(error, "Cannot inspect replacement for recovery", replaced);
    }
    if (cancelled(j, error)) return FALSE;
    if (rename(j->backup, j->source) != 0) return file_error(error, "Cannot restore", j->source);
    j->committed = FALSE; j->backup_created = FALSE; j->restored = TRUE;
    return TRUE;
}

static void remove_parts(ExportJob *j)
{
    const char *parts[] = {j->head, j->tail, j->ending, j->list};
    for (gsize i = 0; i < G_N_ELEMENTS(parts); ++i)
        if (parts[i]) unlink(parts[i]);
}

static gboolean clean_files(ExportJob *j, gboolean success, GError **error)
{
    remove_parts(j);
    if (j->temp && unlink(j->temp) != 0 && errno != ENOENT) return file_error(error, "Cannot remove temporary export", j->temp);
    if (success && j->backup_created) {
        if (cancelled(j, error)) return FALSE;
        if (unlink(j->backup) != 0 && errno != ENOENT) return file_error(error, "Cannot remove original backup", j->backup);
        j->backup_created = FALSE;
    }
    if (j->workspace && rmdir(j->workspace) != 0 && errno != ENOENT && errno != ENOTEMPTY)
        return file_error(error, "Cannot remove export workspace", j->workspace);
    return TRUE;
}

static gboolean property_flag(mpv_handle *mpv, const char *name)
{
    int value = 0;
    return mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &value) >= 0 && value;
}

static void restore_options(TrimExport *e, const char *idle, const char *force)
{
    if (e->stopped) return;
    if (idle) mpv_set_property_string(e->mpv, "idle", idle);
    if (force) mpv_set_property_string(e->mpv, "force-window", force);
}

static void finish(TrimExport *e, gboolean success, const char *message)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    if (j->timeout) { g_source_remove(j->timeout); j->timeout = 0; }
    if (j->saved) {
        if (property_flag(e->mpv, "idle-active")) {
            g_free(e->deferred_idle); g_free(e->deferred_force);
            e->deferred_idle = g_strdup(j->idle); e->deferred_force = g_strdup(j->force_window);
            int pause = j->pause;
            mpv_set_property(e->mpv, "pause", MPV_FORMAT_FLAG, &pause);
        } else restore_options(e, j->idle, j->force_window);
    }
    const char *keys[] = {"busy", "success", "path", "error", "backup", "precise"};
    g_autofree char *valid_message = g_utf8_make_valid(message ? message : "Export failed.", -1);
    mpv_node values[] = {
        {.format = MPV_FORMAT_FLAG, .u.flag = 0},
        {.format = MPV_FORMAT_FLAG, .u.flag = success},
        {.format = MPV_FORMAT_STRING, .u.string = j->output ? j->output : ""},
        {.format = MPV_FORMAT_STRING, .u.string = success ? "" : valid_message},
        {.format = MPV_FORMAT_STRING, .u.string = j->backup_created && j->backup ? j->backup : ""},
        {.format = MPV_FORMAT_STRING, .u.string = (char *)(j->precise_result ? j->precise_result : "")}
    };
    publish_map(e, keys, values, G_N_ELEMENTS(keys));
    e->job = NULL;
    job_unref(j);
}

static void fail_job(TrimExport *e, const char *message)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    g_autofree char *detail = g_strdup(message ? message : "Unexpected error.");
    g_free(j->failure);
    j->failure = g_str_has_prefix(detail, "Export failed:") ? g_strdup(detail) : g_strdup_printf("Export failed: %s", detail);
    if (j->process_id) { mpv_abort_async_command(e->mpv, j->process_id); j->process_id = 0; }
    if (j->temp) file_operation(e, OP_CLEAN_FAILURE);
    else finish(e, FALSE, j->failure);
}

static void save_playback(TrimExport *e)
{
    ExportJob *j = e->job;
    if (!j || j->saved || e->stopped) return;
    j->saved = TRUE;
    char *value = mpv_get_property_string(e->mpv, "idle");
    j->idle = g_strdup(value ? value : "yes"); mpv_free(value);
    value = mpv_get_property_string(e->mpv, "force-window");
    j->force_window = g_strdup(value ? value : "no"); mpv_free(value);
    j->pause = property_flag(e->mpv, "pause");
    mpv_get_property(e->mpv, "time-pos", MPV_FORMAT_DOUBLE, &j->position);
    mpv_get_property(e->mpv, "playlist-pos", MPV_FORMAT_INT64, &j->index);
    g_autofree char *entry_property = g_strdup_printf("playlist/%" G_GINT64_FORMAT "/id", j->index);
    j->entry_id = -1;
    mpv_get_property(e->mpv, entry_property, MPV_FORMAT_INT64, &j->entry_id);
    mpv_set_property_string(e->mpv, "idle", "yes");
    mpv_set_property_string(e->mpv, "force-window", "yes");
    int pause = 1;
    mpv_set_property(e->mpv, "pause", MPV_FORMAT_FLAG, &pause);
}

static gboolean open_timeout(gpointer data)
{
    TrimExport *e = data;
    if (!e->job || e->stopped) return G_SOURCE_REMOVE;
    e->job->timeout = 0;
    recover_open(e, "Could not open the exported video.");
    return G_SOURCE_REMOVE;
}

static gint64 saved_entry_index(TrimExport *e)
{
    ExportJob *j = e->job;
    if (j->entry_id < 0) return j->index;
    mpv_node playlist = {0};
    if (mpv_get_property(e->mpv, "playlist", MPV_FORMAT_NODE, &playlist) < 0) return -1;
    gint64 index = -1;
    if (playlist.format == MPV_FORMAT_NODE_ARRAY && playlist.u.list) {
        for (int i = 0; i < playlist.u.list->num; ++i)
            if (map_integer(&playlist.u.list->values[i], "id", -1) == j->entry_id) { index = i; break; }
    }
    mpv_free_node_contents(&playlist);
    return index;
}

static gint64 playlist_index_of(TrimExport *e, const char *path)
{
    mpv_node playlist = {0};
    if (!path || mpv_get_property(e->mpv, "playlist", MPV_FORMAT_NODE, &playlist) < 0) return -1;
    gint64 index = -1;
    if (playlist.format == MPV_FORMAT_NODE_ARRAY && playlist.u.list) {
        for (int i = 0; i < playlist.u.list->num; ++i) {
            const mpv_node *filename = map_field(&playlist.u.list->values[i], "filename");
            if (filename && filename->format == MPV_FORMAT_STRING &&
                g_strcmp0(filename->u.string, path) == 0) { index = i; break; }
        }
    }
    mpv_free_node_contents(&playlist);
    return index;
}

static void remove_playlist_entry(TrimExport *e, gint64 index)
{
    if (index < 0) return;
    g_autofree char *text = g_strdup_printf("%" G_GINT64_FORMAT, index);
    const char *remove[] = {"playlist-remove", text, NULL};
    mpv_command_async(e->mpv, 0, remove);
}

/* A renamed replacement leaves the original path missing: point its queue
 * entry at the new file without starting playback. */
static void swap_renamed_entry(TrimExport *e)
{
    ExportJob *j = e->job;
    if (!j || !j->renamed || !j->committed || e->stopped) return;
    gint64 index = j->saved ? saved_entry_index(e) : playlist_index_of(e, j->input);
    if (index < 0) return;
    g_autofree char *next = g_strdup_printf("%" G_GINT64_FORMAT, index + 1);
    const char *insert[] = {"loadfile", j->output, "insert-at", next, NULL};
    mpv_command_async(e->mpv, 0, insert);
    remove_playlist_entry(e, index);
}

static void open_video(TrimExport *e, gboolean restoring)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    if (j->timeout) { g_source_remove(j->timeout); j->timeout = 0; }
    gint64 selected = saved_entry_index(e);
    if (selected < 0) {
        j->user_changed = TRUE;
        if (j->committed || (!j->replace && j->output && !j->failure)) file_operation(e, OP_CLEAN_SUCCESS);
        else fail_job(e, "The original playlist entry was removed; automatic reopening was cancelled.");
        return;
    }
    publish(e, restoring ? "restoring" : "opening");
    g_free(j->open_path); j->open_path = g_strdup(restoring ? j->source : j->output);
    j->open_position = restoring ? j->position : 0;
    j->expected_load = TRUE; j->seek_issued = FALSE;
    j->timeout = g_timeout_add_seconds(15, open_timeout, e);
    g_autofree char *index = g_strdup_printf("%" G_GINT64_FORMAT, selected);
    if ((!j->replace || j->renamed) && !restoring) {
        g_autofree char *next = g_strdup_printf("%" G_GINT64_FORMAT, selected + 1);
        const char *insert[] = {"loadfile", j->output, "insert-at", next, NULL};
        const char *select[] = {"playlist-play-index", next, NULL};
        mpv_command_async(e->mpv, 0, insert);
        mpv_command_async(e->mpv, 0, select);
        j->inserted = j->renamed != NULL;
    } else {
        if (j->inserted) {
            /* The renamed replacement was rolled back; drop its queue entry. */
            remove_playlist_entry(e, playlist_index_of(e, j->renamed));
            j->inserted = FALSE;
            selected = saved_entry_index(e);
            g_free(index); index = g_strdup_printf("%" G_GINT64_FORMAT, selected);
        }
        const char *select[] = {"playlist-play-index", index, NULL};
        mpv_command_async(e->mpv, 0, select);
    }
}

static gboolean rollback_after_stop(gpointer data)
{
    TrimExport *e = data;
    if (!e->job || e->stopped) return G_SOURCE_REMOVE;
    e->job->timeout = 0;
    file_operation(e, OP_ROLLBACK);
    return G_SOURCE_REMOVE;
}

static void begin_rollback(TrimExport *e, const char *message)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    if (j->timeout) { g_source_remove(j->timeout); j->timeout = 0; }
    g_free(j->failure); j->failure = g_strdup_printf("Export failed: %s", message);
    publish(e, "rollback");
    const char *stop[] = {"stop", "keep-playlist", NULL};
    mpv_command_async(e->mpv, 0, stop);
    j->timeout = g_timeout_add(50, rollback_after_stop, e);
}

static void recover_open(TrimExport *e, const char *message)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    if (g_strcmp0(j->phase, "opening") == 0 || (j->committed && g_strcmp0(j->phase, "committing") == 0)) {
        if (j->committed) begin_rollback(e, message);
        else {
            g_free(j->failure); j->failure = g_strdup_printf("Export failed: %s Saved copy: %s", message, j->output ? j->output : "");
            open_video(e, TRUE);
        }
    } else {
        g_autofree char *detail = g_strdup_printf("%s Original %s: %s", j->failure ? j->failure : message,
            j->backup_created ? "backup" : "file", j->backup_created ? j->backup : j->source);
        finish(e, FALSE, detail);
    }
}

static gboolean closing_timeout(gpointer data)
{
    TrimExport *e = data;
    if (!e->job || e->stopped) return G_SOURCE_REMOVE;
    e->job->timeout = 0;
    g_free(e->job->failure);
    e->job->failure = g_strdup("Export failed: Could not close the original for replacement.");
    open_video(e, TRUE);
    return G_SOURCE_REMOVE;
}

static void validated(TrimExport *e)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    if (!j->replace || !same_source(e)) {
        publish(e, "committing"); file_operation(e, OP_COMMIT); return;
    }
    save_playback(e);
    publish(e, "closing");
    j->timeout = g_timeout_add_seconds(15, closing_timeout, e);
    const char *stop[] = {"stop", "keep-playlist", NULL};
    mpv_command_async(e->mpv, 0, stop);
}

static void file_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    FileTask *work = data;
    ExportJob *j = work->job;
    g_autoptr(GError) error = NULL;
    gboolean ok = FALSE;
    switch (work->operation) {
    case OP_PREPARE: ok = prepare_files(j, &error); break;
    case OP_SEAL: {
        char metadata_error[256];
        ok = !cancelled(j, &error);
        /* MOV/MP4 remuxing turns readable title tags back into name atoms.
         * Normalize the remaining audio tracks before verification seals
         * this temporary file; stream exclusions need no index remapping. */
        if (ok && mp4_fix_audio_titles(j->temp, metadata_error, sizeof(metadata_error)) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Cannot preserve exported audio titles: %s", metadata_error);
            ok = FALSE;
        }
        if (ok) ok = !cancelled(j, &error) && read_identity(j->temp, &j->verified, &error);
        if (ok && j->verified.size <= 0) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "The export is empty."); ok = FALSE;
        }
        j->sealed = ok;
        break;
    }
    case OP_COMMIT: ok = commit_files(j, &error); break;
    case OP_ROLLBACK: ok = rollback_files(j, &error); break;
    case OP_CLEAN_SUCCESS: ok = clean_files(j, TRUE, &error); break;
    case OP_CLEAN_FAILURE: ok = clean_files(j, FALSE, &error); break;
    }
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error ? g_steal_pointer(&error)
        : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "File operation failed."));
}

static void file_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)data;
    GTask *task = G_TASK(result);
    FileTask *work = g_task_get_task_data(task);
    TrimExport *e = work->owner;
    ExportJob *j = work->job;
    g_autoptr(GError) error = NULL;
    gboolean ok = g_task_propagate_boolean(task, &error);
    j->worker_pending = FALSE;
    if (e->stopped || e->job != j) return;
    if (!ok) {
        const char *detail = error ? error->message : "File operation failed.";
        if (work->operation == OP_CLEAN_SUCCESS) {
            /* A saved result remains valid if deleting its backup failed. */
            finish(e, TRUE, NULL);
        } else if (work->operation == OP_CLEAN_FAILURE) {
            g_autofree char *message = g_strdup_printf("%s Cleanup: %s", j->failure ? j->failure : "Export failed.", detail);
            finish(e, FALSE, message);
        } else if (work->operation == OP_ROLLBACK) {
            g_autofree char *message = g_strdup_printf("%s Recovery: %s", j->failure ? j->failure : "Export failed.", detail);
            finish(e, FALSE, message);
        } else if (work->operation == OP_COMMIT && j->committed) recover_open(e, detail);
        else if (work->operation == OP_COMMIT && j->saved && !j->user_changed) {
            g_free(j->failure); j->failure = g_strdup_printf("Export failed: %s", detail);
            open_video(e, TRUE);
        } else fail_job(e, detail);
        return;
    }
    switch (work->operation) {
    case OP_PREPARE: begin_process(e, j->smart ? PROCESS_ANALYZE : PROCESS_EXPORT); break;
    case OP_SEAL: begin_process(e, PROCESS_PROBE); break;
    case OP_COMMIT:
        if (!j->replace && j->open_result && same_source(e)) save_playback(e);
        if (j->saved && !j->user_changed && j->open_result) open_video(e, FALSE);
        else { swap_renamed_entry(e); publish(e, "cleanup"); file_operation(e, OP_CLEAN_SUCCESS); }
        break;
    case OP_ROLLBACK: open_video(e, TRUE); break;
    case OP_CLEAN_SUCCESS: finish(e, TRUE, NULL); break;
    case OP_CLEAN_FAILURE: finish(e, FALSE, j->failure ? j->failure : "Export failed."); break;
    }
}

static void file_operation(TrimExport *e, FileOperation operation)
{
    ExportJob *j = e->job;
    if (!j || e->stopped || j->worker_pending) return;
    j->worker_pending = TRUE;
    FileTask *work = g_new0(FileTask, 1);
    *work = (FileTask){.owner = export_ref(e), .job = job_ref(j), .operation = operation};
    GTask *task = g_task_new(NULL, j->cancel, file_done, NULL);
    g_task_set_task_data(task, work, file_task_free);
    g_task_set_return_on_cancel(task, FALSE);
    g_task_run_in_thread(task, file_worker);
    g_object_unref(task);
}

static gboolean run_process(TrimExport *e, const char *const *arguments)
{
    ExportJob *j = e->job;
    int count = 0;
    while (arguments[count]) ++count;
    mpv_node *args = g_new0(mpv_node, count);
    for (int i = 0; i < count; ++i) args[i] = (mpv_node){.format = MPV_FORMAT_STRING, .u.string = (char *)arguments[i]};
    mpv_node_list arg_list = {.num = count, .values = args};
    char *keys[] = {"name", "args", "playback_only", "capture_stdout", "capture_stderr", "capture_size"};
    mpv_node values[] = {
        {.format = MPV_FORMAT_STRING, .u.string = "subprocess"},
        {.format = MPV_FORMAT_NODE_ARRAY, .u.list = &arg_list},
        {.format = MPV_FORMAT_FLAG, .u.flag = 0},
        {.format = MPV_FORMAT_FLAG, .u.flag = 1},
        {.format = MPV_FORMAT_FLAG, .u.flag = 1},
        {.format = MPV_FORMAT_INT64, .u.int64 = 1024 * 1024}
    };
    mpv_node_list map = {.num = G_N_ELEMENTS(keys), .keys = keys, .values = values};
    mpv_node command = {.format = MPV_FORMAT_NODE_MAP, .u.list = &map};
    j->process_id = EXPORT_COMMAND_BASE + ++e->serial;
    int status = mpv_command_node_async(e->mpv, j->process_id, &command);
    g_free(args);
    if (status < 0) {
        j->process_id = 0;
        fail_job(e, mpv_error_string(status));
        return FALSE;
    }
    return TRUE;
}

static char *seconds_text(double seconds)
{
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(text, sizeof(text), "%.6f", MAX(0.0, seconds));
    return g_strdup(text);
}

static char *video_selector(const ExportJob *j, int input)
{
    return j->video_index >= 0 ? g_strdup_printf("%d:%" G_GINT64_FORMAT, input, j->video_index)
                               : g_strdup_printf("%d:v:0", input);
}

static void add_args(GPtrArray *args, ...)
{
    va_list list;
    va_start(list, args);
    for (const char *arg; (arg = va_arg(list, const char *));)
        g_ptr_array_add(args, g_strdup(arg));
    va_end(list);
}

static char *render_node(void)
{
    g_autoptr(GDir) directory = g_dir_open("/dev/dri", 0, NULL);
    const char *name;
    char *best = NULL;
    while (directory && (name = g_dir_read_name(directory)))
        if (g_str_has_prefix(name, "renderD") && (!best || strcmp(name, best + 9) < 0)) {
            g_free(best);
            best = g_build_filename("/dev/dri", name, NULL);
        }
    return best;
}

static gboolean ten_bit(const char *pix_fmt)
{
    return pix_fmt && (strstr(pix_fmt, "10") || g_str_equal(pix_fmt, "p010le"));
}

/* Encoder options chosen to match the copied tail: same codec, chroma
 * layout and bit depth, at a quality where the re-encoded frames are
 * visually indistinguishable. Color properties follow the decoded frames. */
static gboolean add_encoder(ExportJob *j, GPtrArray *args, const char *encoder)
{
    const char *profile = NULL;
    if (j->profile && g_str_equal(j->codec, "h264")) {
        if (g_str_has_suffix(j->profile, "Baseline")) profile = "baseline";
        else if (g_str_equal(j->profile, "Main")) profile = "main";
        else if (g_str_equal(j->profile, "High")) profile = "high";
    }
    if (g_str_equal(encoder, "libx264")) {
        add_args(args, "-c:v", "libx264", "-preset", "medium", "-crf", "15",
                 "-pix_fmt", j->pix_fmt, NULL);
        if (profile) add_args(args, "-profile:v", profile, NULL);
    } else if (g_str_equal(encoder, "libx265")) {
        add_args(args, "-c:v", "libx265", "-preset", "fast", "-crf", "16",
                 "-pix_fmt", j->pix_fmt, "-x265-params", "log-level=error", NULL);
    } else if (g_str_has_suffix(encoder, "_nvenc")) {
        add_args(args, "-c:v", encoder, "-preset", "p7", "-tune", "hq", "-rc", "vbr",
                 "-cq", "16", "-b:v", "0", "-pix_fmt", ten_bit(j->pix_fmt) ? "p010le" : "yuv420p", NULL);
        if (profile) add_args(args, "-profile:v", profile, NULL);
    } else if (g_str_has_suffix(encoder, "_vaapi")) {
        add_args(args, "-vf", ten_bit(j->pix_fmt) ? "format=p010le,hwupload" : "format=nv12,hwupload",
                 "-c:v", encoder, "-rc_mode", "CQP", "-qp", "18", NULL);
    } else {
        return FALSE;
    }
    return TRUE;
}

static void choose_encoders(ExportJob *j)
{
    j->encoder_count = j->encoder_index = 0;
    gboolean h264 = g_strcmp0(j->codec, "h264") == 0, hevc = g_strcmp0(j->codec, "hevc") == 0;
    if ((!h264 && !hevc) || !j->pix_fmt) return;
    /* Hardware encoders take 4:2:0 input: 8-bit, or 10-bit for HEVC. */
    gboolean hardware = j->precise == TRIM_PRECISE_GPU &&
        (g_str_equal(j->pix_fmt, "yuv420p") || g_str_equal(j->pix_fmt, "yuvj420p") ||
         g_str_equal(j->pix_fmt, "nv12") ||
         (hevc && (g_str_equal(j->pix_fmt, "yuv420p10le") || g_str_equal(j->pix_fmt, "p010le"))));
    if (hardware) {
        j->encoders[j->encoder_count++] = h264 ? "h264_nvenc" : "hevc_nvenc";
        j->encoders[j->encoder_count++] = h264 ? "h264_vaapi" : "hevc_vaapi";
    }
    j->encoders[j->encoder_count++] = h264 ? "libx264" : "libx265";
}

static void fall_back_to_keyframe(TrimExport *e)
{
    ExportJob *j = e->job;
    j->smart = FALSE;
    j->precise_result = "fallback";
    j->sealed = FALSE;
    remove_parts(j);
    if (j->temp) unlink(j->temp);
    begin_process(e, PROCESS_EXPORT);
}

/* Parts: head [start, splice) re-encoded; tail [splice, end_splice) copied,
 * or to the end of the file when end_splice is -1; ending from the last
 * copied frame to the end, re-encoded. splice == end re-encodes it all. */
static gboolean has_head(const ExportJob *j) { return j->splice_ms > j->start_ms; }
static gboolean has_tail(const ExportJob *j) { return j->splice_ms < j->end_ms; }
static gboolean has_ending(const ExportJob *j)
{
    return has_tail(j) && j->end_splice_ms >= 0 && j->ending_start < j->end_ms / 1000.0 - 0.001;
}

static gboolean smart_stage_needed(const ExportJob *j, ProcessStage stage)
{
    switch (stage) {
    case PROCESS_PACKETS: return has_tail(j) && j->end_splice_ms >= 0;
    case PROCESS_HEAD: return has_head(j);
    case PROCESS_TAIL: return has_tail(j);
    case PROCESS_ENDING: return has_ending(j);
    case PROCESS_EXPORT: return TRUE;
    case PROCESS_SPLICE: return has_head(j);
    case PROCESS_SPLICE_END: return has_ending(j);
    default: return FALSE;
    }
}

/* Run the first needed stage at or after `stage` in the given sequence. */
static void continue_smart(TrimExport *e, const ProcessStage *order, gsize count, ProcessStage stage)
{
    ExportJob *j = e->job;
    gsize i = 0;
    while (i < count && order[i] != stage) ++i;
    for (; i < count; ++i) {
        if (!smart_stage_needed(j, order[i])) continue;
        if (order[i] == PROCESS_EXPORT) {
            g_autofree char *list = g_strdup_printf("ffconcat version 1.0\n%s%s%s",
                has_head(j) ? "file 'head.ts'\n" : "", has_tail(j) ? "file 'tail.ts'\n" : "",
                has_ending(j) ? "file 'ending.ts'\n" : "");
            if (!g_file_set_contents(j->list, list, -1, NULL)) { fall_back_to_keyframe(e); return; }
        }
        begin_process(e, order[i]);
        return;
    }
    validated(e);
}

static const ProcessStage parts_order[] = {
    PROCESS_PACKETS, PROCESS_HEAD, PROCESS_TAIL, PROCESS_ENDING, PROCESS_EXPORT
};
static const ProcessStage checks_order[] = { PROCESS_SPLICE, PROCESS_SPLICE_END };

/* An encoder that is missing or fails (no NVIDIA GPU, no VA-API device,
 * unsupported input) hands over to the next candidate, ending with the
 * software encoder and finally the keyframe export. */
static void try_next_encoder(TrimExport *e)
{
    ExportJob *j = e->job;
    if (++j->encoder_index < j->encoder_count)
        continue_smart(e, parts_order, G_N_ELEMENTS(parts_order), PROCESS_HEAD);
    else
        fall_back_to_keyframe(e);
}

static gboolean add_encode(ExportJob *j, GPtrArray *args, double from, double length, const char *output)
{
    const char *encoder = j->encoders[j->encoder_index];
    g_autofree char *seek = seconds_text(from);
    g_autofree char *duration = seconds_text(length);
    g_autofree char *video = video_selector(j, 0);
    add_args(args, j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y", NULL);
    if (g_str_has_suffix(encoder, "_vaapi")) {
        g_autofree char *node = render_node();
        if (!node) return FALSE;
        g_autofree char *device = g_strdup_printf("vaapi=va:%s", node);
        add_args(args, "-init_hw_device", device, "-filter_hw_device", "va", NULL);
    }
    add_args(args, "-ss", seek, "-i", j->source, "-t", duration, "-map", video,
             "-an", "-sn", "-dn", NULL);
    add_encoder(j, args, encoder);
    add_args(args, "-f", "mpegts", output, NULL);
    return TRUE;
}

static void begin_smart_stage(TrimExport *e, ProcessStage stage)
{
    ExportJob *j = e->job;
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *video = video_selector(j, 0);
    double start = j->start_ms / 1000.0, splice = j->splice_ms / 1000.0, end = j->end_ms / 1000.0;
    double end_splice = j->end_splice_ms / 1000.0;
    if (stage == PROCESS_ANALYZE) {
        publish(e, "encoding");
        g_autofree char *select = j->video_index >= 0
            ? g_strdup_printf("%" G_GINT64_FORMAT, j->video_index) : g_strdup("v:0");
        add_args(args, j->ffprobe, "-v", "error", "-select_streams", select, "-show_entries",
                 "stream=codec_name,profile,pix_fmt:stream_side_data=rotation:format=start_time",
                 "-of", "flat", j->source, NULL);
    } else if (stage == PROCESS_PACKETS) {
        publish(e, "encoding");
        /* Packet order from the first to the last keyframe decides how many
         * packets the copied middle keeps and which frame it shows last. */
        g_autofree char *select = j->video_index >= 0
            ? g_strdup_printf("%" G_GINT64_FORMAT, j->video_index) : g_strdup("v:0");
        char from[G_ASCII_DTOSTR_BUF_SIZE], to[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(from, sizeof(from), "%.6f", j->start_time + MAX(0.0, splice - 1.0));
        g_ascii_formatd(to, sizeof(to), "%.6f", j->start_time + end_splice + 1.0);
        g_autofree char *interval = g_strdup_printf("%s%%%s", from, to);
        add_args(args, j->ffprobe, "-v", "error", "-select_streams", select, "-read_intervals",
                 interval, "-show_entries", "packet=pts_time,flags", "-of", "csv=p=0",
                 j->source, NULL);
    } else if (stage == PROCESS_HEAD) {
        publish(e, "encoding");
        /* Stop half a millisecond early so the splice keyframe itself is
         * taken from the copied middle, never duplicated. */
        if (!add_encode(j, args, start, splice - start - 0.0005, j->head)) { try_next_encoder(e); return; }
    } else if (stage == PROCESS_TAIL) {
        publish(e, "exporting");
        /* Seeking just past the keyframe starts the copy exactly on it.
         * Leading pictures (open-GOP RASL/B-frames) display before the
         * splice and reference the replaced frames; the head already
         * contains them, so they are dropped. A middle that ends before the
         * selection's end stops right before the last keyframe in decode
         * order; the frames it cannot show come from the ending. */
        g_autofree char *seek = seconds_text(splice + 0.0005);
        g_autofree char *drop = j->end_splice_ms >= 0
            ? g_strdup_printf("noise=drop=lt(pts\\,startpts)+gte(n\\,%" G_GINT64_FORMAT ")", j->tail_packets)
            : g_strdup("noise=drop=lt(pts\\,startpts)");
        add_args(args, j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                 "-ss", seek, "-i", j->source, NULL);
        if (j->end_splice_ms >= 0) {
            g_autofree char *length = seconds_text(end_splice - splice + 2.0);
            add_args(args, "-t", length, NULL);
        }
        add_args(args, "-map", video, "-c", "copy", "-bsf:v", drop, "-f", "mpegts", j->tail, NULL);
    } else if (stage == PROCESS_ENDING) {
        publish(e, "encoding");
        if (!add_encode(j, args, j->ending_start, end - j->ending_start - 0.0005, j->ending)) {
            try_next_encoder(e);
            return;
        }
    } else if (stage == PROCESS_EXPORT) {
        publish(e, "exporting");
        g_autofree char *seek = seconds_text(start);
        g_autofree char *length = seconds_text(end - start);
        g_autofree char *subtitles = g_strdup_printf(
            "setts=pts='max(0,PTS)':dts='max(0,DTS)':duration='max(0,min(DURATION+min(0,PTS),%s/TB-max(0,PTS)))'",
            length);
        add_args(args, j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-n", NULL);
        if (j->rotation) add_args(args, "-display_rotation:v:0", j->rotation, NULL);
        add_args(args, "-f", "concat", "-safe", "0", "-i", j->list,
                 "-ss", seek, "-t", length, "-i", j->source,
                 "-map", "0:v:0", "-map", "1", "-map", "-1:V", "-map", "-1:d", NULL);
        for (guint i = 0; i < j->excluded_audio->len; ++i) {
            g_ptr_array_add(args, g_strdup("-map"));
            g_ptr_array_add(args, g_strdup_printf("-1:%" G_GINT64_FORMAT,
                g_array_index(j->excluded_audio, gint64, i)));
        }
        /* Copied audio starts at the keyframe before the cut; keep only
         * the part after the exact start so it stays in sync. */
        add_args(args, "-map_metadata", "1", "-map_chapters", "1", "-c", "copy",
                 "-bsf:a", "noise=drop=lt(pts\\,0)", "-bsf:s", subtitles,
                 "-avoid_negative_ts", "make_zero", j->temp, NULL);
    } else if (stage == PROCESS_SPLICE) {
        publish(e, "verifying");
        g_autofree char *length = seconds_text(splice - start + 1.0);
        add_args(args, j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-xerror",
                 "-i", j->temp, "-map", "0:v:0", "-t", length, "-f", "null", "-", NULL);
    } else if (stage == PROCESS_SPLICE_END) {
        publish(e, "verifying");
        g_autofree char *seek = seconds_text(j->ending_start - start - 1.0);
        add_args(args, j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-xerror",
                 "-ss", seek, "-i", j->temp, "-map", "0:v:0", "-f", "null", "-", NULL);
    }
    g_ptr_array_add(args, NULL);
    run_process(e, (const char *const *)args->pdata);
}

static void begin_process(TrimExport *e, ProcessStage stage)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    j->process_stage = stage;
    if (j->smart && stage != PROCESS_PROBE && stage != PROCESS_FRAME) {
        begin_smart_stage(e, stage);
        return;
    }
    if (stage == PROCESS_EXPORT) {
        publish(e, "exporting");
        char start[G_ASCII_DTOSTR_BUF_SIZE], length[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(start, sizeof(start), "%.3f", j->start_ms / 1000.0);
        g_ascii_formatd(length, sizeof(length), "%.3f", (j->end_ms - j->start_ms) / 1000.0);
        g_autofree char *subtitles = g_strdup_printf("setts=duration='max(0,min(DURATION,%s/TB-PTS))'", length);
        const char *prefix[] = {j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-n",
            "-ss", start, "-i", j->source, "-t", length, "-map", j->exclude_video ? "0:a" : "0", NULL};
        const char *suffix[] = {"-map_metadata", "0",
            "-map_chapters", "0", "-c", "copy", "-bsf:s", subtitles,
            "-avoid_negative_ts", "make_zero", j->temp, NULL};
        g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; prefix[i]; ++i) g_ptr_array_add(args, g_strdup(prefix[i]));
        /* Data streams (timecode, telemetry) cannot be stream-copied into
         * MP4/MOV or Matroska; MP4/MOV rebuild the timecode track from the
         * copied timecode tag. A negative map without matches is harmless. */
        g_ptr_array_add(args, g_strdup("-map"));
        g_ptr_array_add(args, g_strdup("-0:d"));
        for (guint i = 0; i < j->excluded_audio->len; ++i) {
            g_ptr_array_add(args, g_strdup("-map"));
            g_ptr_array_add(args, g_strdup_printf("-0:%" G_GINT64_FORMAT,
                g_array_index(j->excluded_audio, gint64, i)));
        }
        for (guint i = 0; suffix[i]; ++i) g_ptr_array_add(args, g_strdup(suffix[i]));
        g_ptr_array_add(args, NULL);
        run_process(e, (const char *const *)args->pdata);
    } else if (stage == PROCESS_PROBE) {
        publish(e, "verifying");
        const char *args[] = {j->ffprobe, "-v", "error", "-show_entries",
            "stream=index,codec_type,width,height:stream_disposition=attached_pic:format=duration",
            "-of", "flat", j->temp, NULL};
        run_process(e, args);
    }
}

static int verified_stream_index(const char *text, gboolean video)
{
    /* ffprobe's flat output avoids adding a JSON runtime dependency. */
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    typedef struct { int index, width, height, attached; gboolean video, audio, have_index; } Stream;
    GHashTable *streams = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    for (guint i = 0; lines[i]; ++i) {
        unsigned slot;
        char key[128];
        int consumed = 0;
        if (sscanf(lines[i], "streams.stream.%u.%127[^=]=%n", &slot, key, &consumed) < 2 || consumed <= 0) continue;
        Stream *stream = g_hash_table_lookup(streams, GUINT_TO_POINTER(slot + 1));
        if (!stream) { stream = g_new0(Stream, 1); g_hash_table_insert(streams, GUINT_TO_POINTER(slot + 1), stream); }
        const char *value = lines[i] + consumed;
        if (g_str_equal(key, "codec_type")) {
            stream->video = g_str_equal(value, "\"video\"");
            stream->audio = g_str_equal(value, "\"audio\"");
        }
        else if (g_str_equal(key, "index")) { stream->index = (int)g_ascii_strtoll(value, NULL, 10); stream->have_index = TRUE; }
        else if (g_str_equal(key, "width")) stream->width = (int)g_ascii_strtoll(value, NULL, 10);
        else if (g_str_equal(key, "height")) stream->height = (int)g_ascii_strtoll(value, NULL, 10);
        else if (g_str_equal(key, "disposition.attached_pic")) stream->attached = (int)g_ascii_strtoll(value, NULL, 10);
    }
    GHashTableIter iter; gpointer value;
    int result = -1;
    g_hash_table_iter_init(&iter, streams);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        Stream *stream = value;
        gboolean usable = video ? stream->video && stream->width > 0 && stream->height > 0 && !stream->attached
            : stream->audio;
        if (usable && stream->have_index && (result < 0 || stream->index < result)) result = stream->index;
    }
    g_hash_table_unref(streams);
    return result;
}

static void decode_probe_frame(TrimExport *e, int index)
{
    ExportJob *j = e->job;
    g_autofree char *stream = g_strdup_printf("0:%d", index);
    j->process_stage = PROCESS_FRAME;
    const char *args[] = {j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-xerror",
        "-i", j->temp, "-map", stream, j->exclude_video ? "-frames:a" : "-frames:v", "1",
        j->exclude_video ? "-vn" : "-an", "-sn", "-f", "framehash", "pipe:1", NULL};
    run_process(e, args);
}

static void analyze_source(ExportJob *j, const char *text)
{
    g_auto(GStrv) lines = g_strsplit(text ? text : "", "\n", -1);
    for (guint i = 0; lines[i]; ++i) {
        char *equals = strchr(lines[i], '=');
        if (equals && g_str_has_prefix(lines[i], "format.start_time=")) {
            const char *value = equals + 1 + (equals[1] == '"');
            double time = g_ascii_strtod(value, NULL);
            j->start_time = isfinite(time) ? time : 0;
            continue;
        }
        if (!equals || !g_str_has_prefix(lines[i], "streams.stream.0.")) continue;
        *equals = '\0';
        const char *key = lines[i] + strlen("streams.stream.0.");
        g_autofree char *value = g_strdup(equals + 1);
        if (value[0] == '"') {
            size_t length = strlen(value);
            if (length >= 2 && value[length - 1] == '"') value[length - 1] = '\0';
            memmove(value, value + 1, strlen(value));
        }
        char **slot = g_str_equal(key, "codec_name") ? &j->codec
            : g_str_equal(key, "profile") ? &j->profile
            : g_str_equal(key, "pix_fmt") ? &j->pix_fmt
            : g_str_has_suffix(key, ".rotation") ? &j->rotation : NULL;
        if (slot && *value && !*slot) *slot = g_steal_pointer(&value);
    }
    /* Concatenation drops the source's display matrix; restore it. */
    if (j->rotation && g_ascii_strtod(j->rotation, NULL) == 0) g_clear_pointer(&j->rotation, g_free);
}

/* From ffprobe's "pts,flags" packet list in decode order: the packets from
 * the first keyframe up to the last one, and the last frame they show. */
static gboolean measure_middle(ExportJob *j, const char *text)
{
    g_auto(GStrv) lines = g_strsplit(text ? text : "", "\n", -1);
    double first = j->splice_ms / 1000.0, last = j->end_splice_ms / 1000.0, shown = -1;
    gint64 index = -1, count = 0;
    for (guint i = 0; lines[i]; ++i) {
        g_strstrip(lines[i]);
        char *comma = strchr(lines[i], ',');
        if (!comma) continue;
        *comma = '\0';
        char *end = NULL;
        double pts = g_ascii_strtod(lines[i], &end);
        gboolean known = end && end != lines[i] && isfinite(pts);
        gboolean key = strchr(comma + 1, 'K') != NULL;
        if (known) pts -= j->start_time;
        if (index < 0) {
            if (key && known && fabs(pts - first) < 0.001) { index = 0; count = 0; }
            else continue;
        } else if (key && known && fabs(pts - last) < 0.001) {
            j->tail_packets = count;
            j->ending_start = shown + 0.0005;
            return shown >= first;
        }
        ++count;
        if (known && pts >= first - 0.0001) shown = MAX(shown, pts);
    }
    return FALSE;
}

static void smart_stage_finished(TrimExport *e, gboolean failed, const char *output)
{
    ExportJob *j = e->job;
    ProcessStage stage = j->process_stage;
    if (failed) {
        if (stage == PROCESS_HEAD || stage == PROCESS_ENDING) try_next_encoder(e);
        else fall_back_to_keyframe(e);
        return;
    }
    if (stage == PROCESS_HEAD || stage == PROCESS_ENDING) {
        const char *encoder = j->encoders[j->encoder_index];
        j->precise_result = g_str_has_prefix(encoder, "lib") ? "cpu" : "gpu";
    }
    switch (stage) {
    case PROCESS_ANALYZE:
        analyze_source(j, output);
        choose_encoders(j);
        if (j->encoder_count) continue_smart(e, parts_order, G_N_ELEMENTS(parts_order), PROCESS_PACKETS);
        else fall_back_to_keyframe(e);
        break;
    case PROCESS_PACKETS:
        if (measure_middle(j, output))
            continue_smart(e, parts_order, G_N_ELEMENTS(parts_order), PROCESS_HEAD);
        else
            fall_back_to_keyframe(e);
        break;
    case PROCESS_HEAD: continue_smart(e, parts_order, G_N_ELEMENTS(parts_order), PROCESS_TAIL); break;
    case PROCESS_TAIL: continue_smart(e, parts_order, G_N_ELEMENTS(parts_order), PROCESS_ENDING); break;
    case PROCESS_ENDING: continue_smart(e, parts_order, G_N_ELEMENTS(parts_order), PROCESS_EXPORT); break;
    case PROCESS_SPLICE: continue_smart(e, checks_order, G_N_ELEMENTS(checks_order), PROCESS_SPLICE_END); break;
    case PROCESS_SPLICE_END: validated(e); break;
    default: break;
    }
}

TrimExport *trim_export_new(mpv_handle *mpv)
{
    g_return_val_if_fail(mpv != NULL, NULL);
    TrimExport *e = g_new0(TrimExport, 1);
    e->references = 1; e->mpv = mpv;
    const char *keys[] = {"busy"};
    mpv_node value = {.format = MPV_FORMAT_FLAG, .u.flag = 0};
    publish_map(e, keys, &value, 1);
    return e;
}

static gboolean validate_audio_selection(TrimExport *e, const TrimExportRequest *request,
                                          GError **error)
{
    if (!request->excluded_audio_count) return TRUE;
    mpv_node tracks = {0};
    gboolean valid = request->excluded_audio && request->excluded_audio_count <= G_MAXUINT &&
        mpv_get_property(e->mpv, "track-list", MPV_FORMAT_NODE, &tracks) >= 0 &&
        tracks.format == MPV_FORMAT_NODE_ARRAY && tracks.u.list &&
        request->excluded_audio_count <= (gsize)tracks.u.list->num;
    for (gsize i = 0; valid && i < request->excluded_audio_count; ++i) {
        gint64 index = request->excluded_audio[i];
        gboolean found = FALSE;
        if (index < 0) { valid = FALSE; break; }
        for (gsize previous = 0; previous < i; ++previous)
            if (request->excluded_audio[previous] == index) { valid = FALSE; break; }
        for (int n = 0; valid && n < tracks.u.list->num; ++n) {
            const mpv_node *track = &tracks.u.list->values[n];
            const mpv_node *type = map_field(track, "type");
            const mpv_node *external = map_field(track, "external");
            const mpv_node *filename = map_field(track, "external-filename");
            if (map_integer(track, "ff-index", -1) == index && type &&
                type->format == MPV_FORMAT_STRING && g_strcmp0(type->u.string, "audio") == 0 &&
                !(external && external->format == MPV_FORMAT_FLAG && external->u.flag) &&
                !(filename && filename->format == MPV_FORMAT_STRING && filename->u.string && *filename->u.string)) {
                found = TRUE;
                break;
            }
        }
        valid &= found;
    }
    mpv_free_node_contents(&tracks);
    if (!valid) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "The export audio selection no longer matches the source file.");
    return valid;
}

static gboolean valid_extension(const char *extension)
{
    if (!extension || extension[0] != '.' || !extension[1] || strlen(extension) > 8) return FALSE;
    for (const char *p = extension + 1; *p; ++p) if (!g_ascii_isalnum(*p)) return FALSE;
    return TRUE;
}

gboolean trim_export_start(TrimExport *e, const TrimExportRequest *request, GError **error)
{
    if (!e || e->stopped || e->job) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "An export is already running or the player is closing.");
        return FALSE;
    }
    if (!request || request->start_ms < 0 || request->end_ms <= request->start_ms ||
        !request->ffmpeg_path || !*request->ffmpeg_path || !request->ffprobe_path || !*request->ffprobe_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "The export interval or tool path is invalid.");
        return FALSE;
    }
    char *path = mpv_get_property_string(e->mpv, "path");
    if (request->expected_input && g_strcmp0(path, request->expected_input) != 0) {
        mpv_free(path);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
            "The source file changed before the export could start.");
        return FALSE;
    }
    g_autofree char *resolved = resolved_path(e->mpv, path);
    if (!resolved) {
        mpv_free(path);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only local video files can be exported.");
        return FALSE;
    }
    if (!validate_audio_selection(e, request, error)) { mpv_free(path); return FALSE; }
    if (request->exclude_video && !valid_extension(request->audio_extension)) {
        mpv_free(path);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "The audio export format is invalid.");
        return FALSE;
    }
    char *current = mpv_get_property_string(e->mpv, "path");
    gboolean same_input = g_strcmp0(path, current) == 0;
    mpv_free(current);
    if (!same_input) {
        mpv_free(path);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
            "The source file changed before the export could start.");
        return FALSE;
    }
    ExportJob *j = g_new0(ExportJob, 1);
    j->references = 1; j->cancel = g_cancellable_new();
    j->input = g_strdup(path); mpv_free(path);
    j->source = g_steal_pointer(&resolved);
    j->replace = request->replace; j->open_result = request->open_result;
    j->exclude_video = request->exclude_video;
    j->extension = j->exclude_video ? g_strdup(request->audio_extension) : NULL;
    j->start_ms = request->start_ms; j->end_ms = request->end_ms;
    j->precise = request->precise;
    j->splice_ms = request->splice_ms;
    j->end_splice_ms = request->end_splice_ms;
    j->video_index = request->video_index;
    j->precise_result = "";
    if (j->precise != TRIM_PRECISE_OFF && !j->exclude_video) {
        if (j->splice_ms >= j->start_ms && j->splice_ms <= j->end_ms &&
            (j->splice_ms > j->start_ms || j->end_splice_ms >= 0) &&
            (j->end_splice_ms < 0 || (j->end_splice_ms > j->splice_ms && j->end_splice_ms <= j->end_ms)))
            j->smart = TRUE;
        else if (j->splice_ms < 0) j->precise_result = "fallback";
    }
    j->excluded_audio = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)request->excluded_audio_count);
    if (request->excluded_audio_count)
        g_array_append_vals(j->excluded_audio, request->excluded_audio, (guint)request->excluded_audio_count);
    j->ffmpeg = g_strdup(request->ffmpeg_path); j->ffprobe = g_strdup(request->ffprobe_path);
    e->job = j;
    publish(e, "preparing");
    file_operation(e, OP_PREPARE);
    return TRUE;
}

gboolean trim_export_busy(const TrimExport *e) { return e && e->job != NULL; }

gboolean trim_export_handle_event(TrimExport *e, const mpv_event *event)
{
    if (!e || e->stopped) return FALSE;
    ExportJob *j = e->job;
    if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->reply_userdata >= EXPORT_COMMAND_BASE &&
        event->reply_userdata < EXPORT_COMMAND_LIMIT) {
        if (!j || j->process_id != event->reply_userdata) return TRUE;
        j->process_id = 0;
        const mpv_event_command *command = event->data;
        const mpv_node *result = command ? &command->result : NULL;
        g_autofree char *stderr_text = captured_text(result, "stderr");
        g_autofree char *stdout_text = captured_text(result, "stdout");
        g_autofree char *process_error = captured_text(result, "error_string");
        gboolean failed = event->error < 0 || map_integer(result, "status", -1) != 0;
        if (j->smart && (failed || (j->process_stage != PROCESS_EXPORT &&
                                    j->process_stage != PROCESS_PROBE &&
                                    j->process_stage != PROCESS_FRAME))) {
            smart_stage_finished(e, failed, stdout_text);
            return TRUE;
        }
        if (failed) {
            fail_job(e, *stderr_text ? stderr_text : *process_error ? process_error
                : event->error < 0 ? mpv_error_string(event->error) : "The export subprocess failed.");
            return TRUE;
        }
        if (j->process_stage == PROCESS_EXPORT) file_operation(e, OP_SEAL);
        else if (j->process_stage == PROCESS_PROBE) {
            int stream = verified_stream_index(stdout_text, !j->exclude_video);
            if (stream < 0) fail_job(e, j->exclude_video ? "The export has no usable audio stream."
                : "The export has no usable video stream.");
            else decode_probe_frame(e, stream);
        } else {
            if (!g_regex_match_simple("(?m)^\\d+,\\s*-?\\d+,", stdout_text, 0, 0) && j->smart)
                fall_back_to_keyframe(e);
            else if (!g_regex_match_simple("(?m)^\\d+,\\s*-?\\d+,", stdout_text, 0, 0))
                fail_job(e, j->exclude_video ? "The selected interval has no decodable audio frame."
                    : "The selected interval has no decodable video frame.");
            else if (j->smart) continue_smart(e, checks_order, G_N_ELEMENTS(checks_order), PROCESS_SPLICE);
            else validated(e);
        }
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_FILE_LOADED && e->deferred_idle) {
        restore_options(e, e->deferred_idle, e->deferred_force);
        g_clear_pointer(&e->deferred_idle, g_free); g_clear_pointer(&e->deferred_force, g_free);
    }
    if (!j) return FALSE;
    if (event->event_id == MPV_EVENT_START_FILE) {
        if (j->saved && !j->expected_load) j->user_changed = TRUE;
    } else if (event->event_id == MPV_EVENT_END_FILE) {
        const mpv_event_end_file *end = event->data;
        if (g_strcmp0(j->phase, "closing") == 0) {
            if (j->timeout) { g_source_remove(j->timeout); j->timeout = 0; }
            publish(e, "committing"); file_operation(e, OP_COMMIT);
        } else if ((g_strcmp0(j->phase, "opening") == 0 || g_strcmp0(j->phase, "restoring") == 0) &&
                   end && end->reason == MPV_END_FILE_REASON_ERROR) recover_open(e, "mpv could not open the exported video.");
    } else if (event->event_id == MPV_EVENT_FILE_LOADED && j->expected_load &&
        (g_strcmp0(j->phase, "opening") == 0 || g_strcmp0(j->phase, "restoring") == 0)) {
        char *path = mpv_get_property_string(e->mpv, "path");
        g_autofree char *resolved = resolved_path(e->mpv, path); mpv_free(path);
        if (g_strcmp0(resolved, j->open_path) != 0) {
            j->expected_load = FALSE; j->user_changed = TRUE;
            if (j->committed || (!j->replace && j->output && !j->failure)) file_operation(e, OP_CLEAN_SUCCESS);
            else fail_job(e, j->failure ? j->failure : "Could not reopen the original.");
        } else {
            j->expected_load = FALSE; j->seek_issued = TRUE;
            char position[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_dtostr(position, sizeof(position), j->open_position);
            const char *seek[] = {"seek", position, "absolute+exact", NULL};
            mpv_command_async(e->mpv, 0, seek);
        }
    } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && j->seek_issued && !property_flag(e->mpv, "seeking")) {
        j->seek_issued = FALSE;
        if (j->timeout) { g_source_remove(j->timeout); j->timeout = 0; }
        if (!j->user_changed) {
            int pause = j->pause; mpv_set_property(e->mpv, "pause", MPV_FORMAT_FLAG, &pause);
        }
        if (g_strcmp0(j->phase, "restoring") == 0) file_operation(e, OP_CLEAN_FAILURE);
        else if (g_strcmp0(j->phase, "opening") == 0) {
            /* The renamed replacement now plays; its missing original goes. */
            if (j->inserted) { remove_playlist_entry(e, saved_entry_index(e)); j->inserted = FALSE; }
            publish(e, "cleanup"); file_operation(e, OP_CLEAN_SUCCESS);
        }
    }
    return FALSE;
}

void trim_export_shutdown(TrimExport *e)
{
    if (!e || e->stopped) return;
    e->stopped = TRUE;
    ExportJob *j = e->job;
    if (!j) return;
    if (j->timeout) { g_source_remove(j->timeout); j->timeout = 0; }
    if (j->process_id) { mpv_abort_async_command(e->mpv, j->process_id); j->process_id = 0; }
    g_cancellable_cancel(j->cancel);
    /* Workers retain the job, and cancellation never deletes its backup. */
    e->job = NULL; job_unref(j);
}

void trim_export_free(TrimExport *e)
{
    if (!e) return;
    trim_export_shutdown(e);
    export_unref(e);
}
