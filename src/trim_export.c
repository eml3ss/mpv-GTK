/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "trim_export.h"

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
typedef enum { PROCESS_EXPORT, PROCESS_PROBE, PROCESS_FRAME } ProcessStage;

typedef struct ExportJob {
    gint references;
    GCancellable *cancel;
    char *input, *source, *directory, *workspace, *temp, *backup, *target, *output;
    char *ffmpeg, *ffprobe, *phase, *failure, *open_path, *idle, *force_window;
    FileIdentity original, verified, replacement;
    gboolean replace, open_result, committed, backup_created, sealed;
    gboolean saved, pause, user_changed, expected_load, seek_issued, restored;
    double position, open_position;
    gint64 index, entry_id, start_ms, end_ms;
    guint timeout;
    uint64_t process_id;
    ProcessStage process_stage;
    gboolean worker_pending;
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
    const char *extension = dot && dot != name ? dot : "";
    g_autofree char *stem = *extension ? g_strndup(name, extension - name) : g_strdup(name);
    g_autofree char *first = filename_time(j->start_ms), *last = filename_time(j->end_ms);
    g_autofree char *target_name = g_strdup_printf("%s_%s-%s%s", stem, first, last, extension);
    j->target = g_build_filename(j->directory, target_name, NULL);
    j->workspace = g_build_filename(j->directory, ".mpv-trim-XXXXXX", NULL);
    if (!g_mkdtemp_full(j->workspace, 0700)) return file_error(error, "Cannot create export workspace", j->directory);
    g_autofree char *temp_name = g_strconcat("partial", extension, NULL);
    g_autofree char *backup_name = g_strconcat("original", extension, NULL);
    j->temp = g_build_filename(j->workspace, temp_name, NULL);
    j->backup = g_build_filename(j->workspace, backup_name, NULL);
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
    struct stat status;
    if (lstat(j->source, &status) == 0) {
        if (!S_ISREG(status.st_mode) || !read_identity(j->source, &current, error)) {
            if (!error || !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                "The replacement path changed; the original backup was retained.");
            return FALSE;
        }
        if (!same_identity(&current, &j->replacement)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "The replacement changed after export; the backup was retained instead of overwriting it.");
            return FALSE;
        }
    } else if (errno != ENOENT) {
        return file_error(error, "Cannot inspect replacement for recovery", j->source);
    }
    if (cancelled(j, error)) return FALSE;
    if (rename(j->backup, j->source) != 0) return file_error(error, "Cannot restore", j->source);
    j->committed = FALSE; j->backup_created = FALSE; j->restored = TRUE;
    return TRUE;
}

static gboolean clean_files(ExportJob *j, gboolean success, GError **error)
{
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
    const char *keys[] = {"busy", "success", "path", "error", "backup"};
    g_autofree char *valid_message = g_utf8_make_valid(message ? message : "Export failed.", -1);
    mpv_node values[] = {
        {.format = MPV_FORMAT_FLAG, .u.flag = 0},
        {.format = MPV_FORMAT_FLAG, .u.flag = success},
        {.format = MPV_FORMAT_STRING, .u.string = j->output ? j->output : ""},
        {.format = MPV_FORMAT_STRING, .u.string = success ? "" : valid_message},
        {.format = MPV_FORMAT_STRING, .u.string = j->backup_created && j->backup ? j->backup : ""}
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
    if (!j->replace && !restoring) {
        g_autofree char *next = g_strdup_printf("%" G_GINT64_FORMAT, selected + 1);
        const char *insert[] = {"loadfile", j->output, "insert-at", next, NULL};
        const char *select[] = {"playlist-play-index", next, NULL};
        mpv_command_async(e->mpv, 0, insert);
        mpv_command_async(e->mpv, 0, select);
    } else {
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
    case OP_SEAL:
        ok = !cancelled(j, &error) && read_identity(j->temp, &j->verified, &error);
        if (ok && j->verified.size <= 0) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "The export is empty."); ok = FALSE;
        }
        j->sealed = ok;
        break;
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
    case OP_PREPARE: begin_process(e, PROCESS_EXPORT); break;
    case OP_SEAL: begin_process(e, PROCESS_PROBE); break;
    case OP_COMMIT:
        if (!j->replace && j->open_result && same_source(e)) save_playback(e);
        if (j->saved && !j->user_changed && j->open_result) open_video(e, FALSE);
        else { publish(e, "cleanup"); file_operation(e, OP_CLEAN_SUCCESS); }
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

static void begin_process(TrimExport *e, ProcessStage stage)
{
    ExportJob *j = e->job;
    if (!j || e->stopped) return;
    j->process_stage = stage;
    if (stage == PROCESS_EXPORT) {
        publish(e, "exporting");
        char start[G_ASCII_DTOSTR_BUF_SIZE], length[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(start, sizeof(start), "%.3f", j->start_ms / 1000.0);
        g_ascii_formatd(length, sizeof(length), "%.3f", (j->end_ms - j->start_ms) / 1000.0);
        g_autofree char *subtitles = g_strdup_printf("setts=duration='max(0,min(DURATION,%s/TB-PTS))'", length);
        const char *args[] = {j->ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-n",
            "-ss", start, "-i", j->source, "-t", length, "-map", "0", "-map_metadata", "0",
            "-map_chapters", "0", "-c", "copy", "-bsf:s", subtitles,
            "-avoid_negative_ts", "make_zero", j->temp, NULL};
        run_process(e, args);
    } else if (stage == PROCESS_PROBE) {
        publish(e, "verifying");
        const char *args[] = {j->ffprobe, "-v", "error", "-show_entries",
            "stream=index,codec_type,width,height:stream_disposition=attached_pic:format=duration",
            "-of", "flat", j->temp, NULL};
        run_process(e, args);
    }
}

static int verified_video_index(const char *text)
{
    /* ffprobe's flat output avoids adding a JSON runtime dependency. */
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    typedef struct { int index, width, height, attached; gboolean video, have_index; } Stream;
    GHashTable *streams = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    for (guint i = 0; lines[i]; ++i) {
        unsigned slot;
        char key[128];
        int consumed = 0;
        if (sscanf(lines[i], "streams.stream.%u.%127[^=]=%n", &slot, key, &consumed) < 2 || consumed <= 0) continue;
        Stream *stream = g_hash_table_lookup(streams, GUINT_TO_POINTER(slot + 1));
        if (!stream) { stream = g_new0(Stream, 1); g_hash_table_insert(streams, GUINT_TO_POINTER(slot + 1), stream); }
        const char *value = lines[i] + consumed;
        if (g_str_equal(key, "codec_type")) stream->video = g_str_equal(value, "\"video\"");
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
        if (stream->video && stream->have_index && stream->width > 0 && stream->height > 0 && !stream->attached &&
            (result < 0 || stream->index < result)) result = stream->index;
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
        "-i", j->temp, "-map", stream, "-frames:v", "1", "-an", "-sn", "-f", "framehash", "pipe:1", NULL};
    run_process(e, args);
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
    g_autofree char *resolved = resolved_path(e->mpv, path);
    if (!resolved) {
        mpv_free(path);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only local video files can be exported.");
        return FALSE;
    }
    ExportJob *j = g_new0(ExportJob, 1);
    j->references = 1; j->cancel = g_cancellable_new();
    j->input = g_strdup(path); mpv_free(path);
    j->source = g_steal_pointer(&resolved);
    j->replace = request->replace; j->open_result = request->open_result;
    j->start_ms = request->start_ms; j->end_ms = request->end_ms;
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
        if (event->error < 0 || map_integer(result, "status", -1) != 0) {
            fail_job(e, *stderr_text ? stderr_text : *process_error ? process_error
                : event->error < 0 ? mpv_error_string(event->error) : "The export subprocess failed.");
            return TRUE;
        }
        if (j->process_stage == PROCESS_EXPORT) file_operation(e, OP_SEAL);
        else if (j->process_stage == PROCESS_PROBE) {
            int stream = verified_video_index(stdout_text);
            if (stream < 0) fail_job(e, "The export has no usable video stream.");
            else decode_probe_frame(e, stream);
        } else {
            if (!g_regex_match_simple("(?m)^\\d+,\\s*-?\\d+,", stdout_text, 0, 0))
                fail_job(e, "The selected interval has no decodable video frame.");
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
