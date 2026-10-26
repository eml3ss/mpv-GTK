#include "queue_model.h"

#include <gio/gio.h>
#include <string.h>

#define QUEUE_REQUEST_BASE (UINT64_C(1) << 57)
#define QUEUE_REQUEST_LIMIT (QUEUE_REQUEST_BASE + (UINT64_C(1) << 56))

typedef enum { ACTION_NONE, ACTION_PLAY, ACTION_REMOVE, ACTION_MOVE,
               ACTION_CLEAR, ACTION_SORT, ACTION_SHUFFLE, ACTION_REPEAT } Action;
typedef enum { REQUEST_NONE, REQUEST_PLAYLIST, REQUEST_COMMAND, REQUEST_REPEAT,
               REQUEST_DIRECTORY } Request;

typedef struct {
    int64_t id;
    guint ordinal;
    char *filename;
    gint64 seconds;
    guint32 nanoseconds;
    gboolean known;
} FileDate;

typedef struct {
    QueueModel *owner;
    GPtrArray *files;
    char *directory;
    gboolean created, descending;
} DateScan;

struct QueueModel {
    gint references;
    mpv_handle *mpv;
    GPtrArray *entries;
    GPtrArray *date_entries;
    GArray *ids, *order;
    GCancellable *date_cancel;
    char *repeat;
    void (*changed)(gpointer);
    void (*error)(const char *, gpointer);
    gpointer data;
    Action action;
    Request request;
    uint64_t request_id;
    int64_t entry_id, before_id, pending_play;
    guint remove_cursor, commands, command_limit;
    QueueSort sort;
    int sort_mode;
    gboolean started, resume_sent, stopped;
};

/* Models may be destroyed and recreated while old replies remain queued. */
static uint64_t next_request;

static const mpv_node *field(const mpv_node *node, const char *key)
{
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list) return NULL;
    for (int i = 0; i < node->u.list->num; ++i)
        if (g_str_equal(node->u.list->keys[i], key)) return &node->u.list->values[i];
    return NULL;
}

static const char *text_field(const mpv_node *node, const char *key)
{
    const mpv_node *value = field(node, key);
    return value && value->format == MPV_FORMAT_STRING && value->u.string ? value->u.string : "";
}

static gboolean flag_field(const mpv_node *node, const char *key)
{
    const mpv_node *value = field(node, key);
    return value && value->format == MPV_FORMAT_FLAG && value->u.flag;
}

static void entry_free(gpointer data)
{
    QueueEntry *entry = data;
    g_free(entry->title);
    g_free(entry->filename);
    g_free(entry);
}

static void file_date_free(gpointer data)
{
    FileDate *date = data;
    g_free(date->filename);
    g_free(date);
}

static QueueModel *model_ref(QueueModel *m)
{
    g_atomic_int_inc(&m->references);
    return m;
}

static void model_unref(QueueModel *m)
{
    if (!g_atomic_int_dec_and_test(&m->references)) return;
    g_ptr_array_unref(m->entries);
    g_clear_pointer(&m->date_entries, g_ptr_array_unref);
    g_clear_pointer(&m->ids, g_array_unref);
    g_clear_pointer(&m->order, g_array_unref);
    g_clear_object(&m->date_cancel);
    g_free(m->repeat);
    g_free(m);
}

static void changed(QueueModel *m)
{
    if (!m->stopped && m->changed) m->changed(m->data);
}

static void finish(QueueModel *m, const char *error)
{
    int64_t pending_play = m->pending_play;
    m->pending_play = -1;
    if (!error && m->action == ACTION_SORT && m->entries->len)
        m->sort_mode = m->sort;
    else if (!m->entries->len || (!error && (m->action == ACTION_SHUFFLE ||
             (m->action == ACTION_MOVE && m->started))))
        m->sort_mode = -1;
    m->action = ACTION_NONE;
    m->request = REQUEST_NONE;
    m->request_id = 0;
    g_clear_pointer(&m->ids, g_array_unref);
    g_clear_pointer(&m->order, g_array_unref);
    g_clear_pointer(&m->date_entries, g_ptr_array_unref);
    if (m->date_cancel) g_cancellable_cancel(m->date_cancel);
    g_clear_object(&m->date_cancel);
    if (pending_play >= 0 && !m->stopped)
        queue_model_play(m, pending_play);
    else
        changed(m);
    if (error && !m->stopped && m->error) m->error(error, m->data);
}

QueueModel *queue_model_new(mpv_handle *mpv, void (*changed_cb)(gpointer),
                            void (*error_cb)(const char *, gpointer), gpointer data)
{
    g_return_val_if_fail(mpv != NULL, NULL);
    QueueModel *m = g_new0(QueueModel, 1);
    m->references = 1;
    m->mpv = mpv;
    m->entries = g_ptr_array_new_with_free_func(entry_free);
    m->repeat = g_strdup("no");
    m->sort_mode = -1;
    m->pending_play = -1;
    m->changed = changed_cb;
    m->error = error_cb;
    m->data = data;
    return m;
}

const GPtrArray *queue_model_entries(const QueueModel *m) { return m ? m->entries : NULL; }
gboolean queue_model_busy(const QueueModel *m) { return m && m->action != ACTION_NONE; }
const char *queue_model_repeat(const QueueModel *m) { return m ? m->repeat : "no"; }
int queue_model_sort_mode(const QueueModel *m) { return m ? m->sort_mode : -1; }

void queue_model_update(QueueModel *m, const mpv_node *playlist)
{
    if (!m || m->stopped) return;
    GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);
    if (playlist && playlist->format == MPV_FORMAT_NODE_ARRAY && playlist->u.list) {
        for (int i = 0; i < playlist->u.list->num; ++i) {
            const mpv_node *item = &playlist->u.list->values[i];
            const mpv_node *id = field(item, "id");
            if (!id || id->format != MPV_FORMAT_INT64 || id->u.int64 < 0) continue;
            QueueEntry *entry = g_new0(QueueEntry, 1);
            entry->id = id->u.int64;
            entry->filename = g_strdup(text_field(item, "filename"));
            const char *title = text_field(item, "title");
            g_autofree char *fallback = NULL;
            if (!*title) {
                g_autoptr(GFile) file = g_file_new_for_commandline_arg(entry->filename);
                fallback = g_file_get_basename(file);
                title = fallback ? fallback : entry->filename;
            }
            entry->title = g_utf8_make_valid(title, -1);
            entry->current = flag_field(item, "playing") || flag_field(item, "current");
            g_ptr_array_add(entries, entry);
        }
    }
    gboolean same_order = entries->len == m->entries->len;
    for (guint i = 0; same_order && i < entries->len; ++i) {
        const QueueEntry *a = entries->pdata[i], *b = m->entries->pdata[i];
        same_order = a->id == b->id;
    }
    gboolean equal = same_order;
    for (guint i = 0; equal && i < entries->len; ++i) {
        const QueueEntry *a = entries->pdata[i], *b = m->entries->pdata[i];
        equal = a->id == b->id && a->current == b->current &&
            g_strcmp0(a->title, b->title) == 0 && g_strcmp0(a->filename, b->filename) == 0;
    }
    if (equal) {
        g_ptr_array_unref(entries);
        return;
    }
    if (!same_order || !entries->len)
        m->sort_mode = -1;
    g_ptr_array_unref(m->entries);
    m->entries = entries;
    changed(m);
}

void queue_model_set_position(QueueModel *m, int64_t position)
{
    if (!m || m->stopped) return;
    gboolean different = FALSE;
    for (guint i = 0; i < m->entries->len; ++i) {
        QueueEntry *entry = m->entries->pdata[i];
        gboolean current = (int64_t)i == position;
        different |= entry->current != current;
        entry->current = current;
    }
    if (different) changed(m);
}

void queue_model_update_repeat(QueueModel *m, const char *value)
{
    if (!m || m->stopped) return;
    if (!value || !*value) value = "no";
    if (g_strcmp0(value, m->repeat) == 0) return;
    g_free(m->repeat);
    m->repeat = g_strdup(value);
    changed(m);
}

static uint64_t request_id(QueueModel *m, Request type)
{
    m->request = type;
    m->request_id = QUEUE_REQUEST_BASE + ++next_request;
    return m->request_id;
}

static gboolean run(QueueModel *m, const char **args)
{
    int status = mpv_command_async(m->mpv, request_id(m, REQUEST_COMMAND), args);
    if (status < 0) {
        finish(m, mpv_error_string(status));
        return FALSE;
    }
    ++m->commands;
    return TRUE;
}

static gboolean snapshot(QueueModel *m)
{
    int status = mpv_get_property_async(m->mpv, request_id(m, REQUEST_PLAYLIST),
                                       "playlist", MPV_FORMAT_NODE);
    if (status < 0) {
        finish(m, mpv_error_string(status));
        return FALSE;
    }
    return TRUE;
}

static gboolean begin(QueueModel *m, Action action)
{
    if (!m || m->stopped || m->action != ACTION_NONE) return FALSE;
    m->action = action;
    m->started = FALSE;
    m->resume_sent = FALSE;
    m->remove_cursor = m->commands = 0;
    m->command_limit = G_MAXUINT;
    changed(m);
    return TRUE;
}

static int index_of(const QueueModel *m, int64_t id)
{
    for (guint i = 0; i < m->entries->len && i <= G_MAXINT; ++i) {
        const QueueEntry *entry = m->entries->pdata[i];
        if (entry->id == id) return (int)i;
    }
    return -1;
}

static void move_indices(QueueModel *m, int from, int before)
{
    char source[32], target[32];
    g_snprintf(source, sizeof(source), "%d", from);
    g_snprintf(target, sizeof(target), "%d", before);
    const char *args[] = {"playlist-move", source, target, NULL};
    run(m, args);
}

typedef struct { int64_t id; guint ordinal; char *key; } SortEntry;

static void sort_entry_free(gpointer data)
{
    SortEntry *entry = data;
    g_free(entry->key);
    g_free(entry);
}

static gint sort_entries(gconstpointer a, gconstpointer b, gpointer data)
{
    const SortEntry *left = *(SortEntry *const *)a, *right = *(SortEntry *const *)b;
    int result = strcmp(left->key, right->key);
    gboolean descending = GPOINTER_TO_INT(data);
    if (result) return descending ? -result : result;
    return (left->ordinal > right->ordinal) - (left->ordinal < right->ordinal);
}

static void prepare_sort(QueueModel *m)
{
    g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(sort_entry_free);
    for (guint i = 0; i < m->entries->len; ++i) {
        const QueueEntry *source = m->entries->pdata[i];
        const char *text = m->sort == QUEUE_SORT_PATH_ASC || m->sort == QUEUE_SORT_PATH_DESC
                         ? source->filename : source->title;
        g_autofree char *valid = g_utf8_make_valid(text, -1);
        g_autofree char *folded = g_utf8_casefold(valid, -1);
        SortEntry *entry = g_new0(SortEntry, 1);
        entry->id = source->id;
        entry->ordinal = i;
        entry->key = g_utf8_collate_key_for_filename(folded, -1);
        g_ptr_array_add(entries, entry);
    }
    gboolean descending = m->sort == QUEUE_SORT_TITLE_DESC || m->sort == QUEUE_SORT_PATH_DESC;
    g_ptr_array_sort_with_data(entries, sort_entries, GINT_TO_POINTER(descending));
    m->order = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), entries->len);
    for (guint i = 0; i < entries->len; ++i) {
        SortEntry *entry = entries->pdata[i];
        g_array_append_val(m->order, entry->id);
    }
    m->command_limit = MIN((guint64)entries->len * 2 + 16, (guint64)G_MAXUINT);
}

static gboolean date_sort(QueueSort sort)
{
    return sort >= QUEUE_SORT_CREATED_ASC && sort <= QUEUE_SORT_MODIFIED_DESC;
}

static void date_scan_free(gpointer data)
{
    DateScan *scan = data;
    g_ptr_array_unref(scan->files);
    g_free(scan->directory);
    model_unref(scan->owner);
    g_free(scan);
}

static GFile *date_file(const char *filename, const char *directory)
{
    if (!filename || !*filename) return NULL;
    if (g_path_is_absolute(filename)) return g_file_new_for_path(filename);
    g_autofree char *scheme = g_uri_parse_scheme(filename);
    if (scheme) {
        if (g_ascii_strcasecmp(scheme, "file") != 0) return NULL;
        g_autofree char *hostname = NULL;
        g_autofree char *path = g_filename_from_uri(filename, &hostname, NULL);
        if (!path || (hostname && *hostname && g_ascii_strcasecmp(hostname, "localhost") != 0))
            return NULL;
        return g_file_new_for_path(path);
    }
    /* Resolve against the engine's directory, not the frontend process's
     * current directory or the directory containing another playlist item. */
    if (!directory || !g_path_is_absolute(directory)) return NULL;
    g_autofree char *path = g_build_filename(directory, filename, NULL);
    return g_file_new_for_path(path);
}

static gint compare_dates(gconstpointer a, gconstpointer b, gpointer data)
{
    const FileDate *left = *(FileDate *const *)a, *right = *(FileDate *const *)b;
    /* Unknown dates always stay after real dates, including newest-first. */
    if (left->known != right->known) return left->known ? -1 : 1;
    if (left->known) {
        int order = (left->seconds > right->seconds) - (left->seconds < right->seconds);
        if (!order)
            order = (left->nanoseconds > right->nanoseconds) -
                    (left->nanoseconds < right->nanoseconds);
        if (order) return GPOINTER_TO_INT(data) ? -order : order;
    }
    return (left->ordinal > right->ordinal) - (left->ordinal < right->ordinal);
}

static void scan_dates(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source;
    DateScan *scan = data;
    const char *seconds = scan->created ? G_FILE_ATTRIBUTE_TIME_CREATED : G_FILE_ATTRIBUTE_TIME_MODIFIED;
    const char *nsec = scan->created ? G_FILE_ATTRIBUTE_TIME_CREATED_NSEC : G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC;
    const char *usec = scan->created ? G_FILE_ATTRIBUTE_TIME_CREATED_USEC : G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC;
    g_autofree char *attributes = g_strdup_printf("%s,%s,%s", seconds, nsec, usec);
    for (guint i = 0; i < scan->files->len; ++i) {
        if (g_task_return_error_if_cancelled(task)) return;
        FileDate *date = scan->files->pdata[i];
        g_autoptr(GFile) file = date_file(date->filename, scan->directory);
        if (!file) continue;
        /* Local GIO exposes true birth time when the filesystem provides it;
         * absent creation metadata stays unknown, with no ctime fallback. */
        g_autoptr(GFileInfo) info = g_file_query_info(file, attributes,
            G_FILE_QUERY_INFO_NONE, cancel, NULL);
        if (!info || !g_file_info_has_attribute(info, seconds)) continue;
        date->seconds = (gint64)g_file_info_get_attribute_uint64(info, seconds);
        if (g_file_info_has_attribute(info, nsec))
            date->nanoseconds = g_file_info_get_attribute_uint32(info, nsec);
        else if (g_file_info_has_attribute(info, usec))
            date->nanoseconds = g_file_info_get_attribute_uint32(info, usec) * 1000;
        date->known = date->nanoseconds < 1000000000;
    }
    if (g_task_return_error_if_cancelled(task)) return;
    g_ptr_array_sort_with_data(scan->files, compare_dates, GINT_TO_POINTER(scan->descending));
    GArray *order = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), scan->files->len);
    for (guint i = 0; i < scan->files->len; ++i) {
        const FileDate *date = scan->files->pdata[i];
        g_array_append_val(order, date->id);
    }
    g_task_return_pointer(task, order, (GDestroyNotify)g_array_unref);
}

static void dates_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)data;
    GTask *task = G_TASK(result);
    DateScan *scan = g_task_get_task_data(task);
    QueueModel *m = scan->owner;
    g_autoptr(GError) error = NULL;
    GArray *order = g_task_propagate_pointer(task, &error);
    g_clear_object(&m->date_cancel);
    if (m->stopped) {
        g_clear_pointer(&order, g_array_unref);
        return;
    }
    if (!order) {
        finish(m, error ? error->message : "Cannot inspect queue file dates.");
        return;
    }
    m->order = order;
    m->command_limit = MIN((guint64)order->len * 2 + 16, (guint64)G_MAXUINT);
    /* The core may have changed while filesystem metadata was read. Check
     * membership in a fresh native snapshot before issuing any move. */
    snapshot(m);
}

static void start_date_worker(QueueModel *m, const char *directory)
{
    DateScan *scan = g_new0(DateScan, 1);
    scan->owner = model_ref(m);
    scan->files = g_steal_pointer(&m->date_entries);
    scan->directory = g_strdup(directory);
    scan->created = m->sort == QUEUE_SORT_CREATED_ASC || m->sort == QUEUE_SORT_CREATED_DESC;
    scan->descending = m->sort == QUEUE_SORT_CREATED_DESC || m->sort == QUEUE_SORT_MODIFIED_DESC;
    m->date_cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, m->date_cancel, dates_ready, NULL);
    g_task_set_task_data(task, scan, date_scan_free);
    g_task_set_return_on_cancel(task, FALSE);
    g_task_run_in_thread(task, scan_dates);
    g_object_unref(task);
}

static void prepare_date_sort(QueueModel *m)
{
    m->date_entries = g_ptr_array_new_with_free_func(file_date_free);
    for (guint i = 0; i < m->entries->len; ++i) {
        const QueueEntry *entry = m->entries->pdata[i];
        FileDate *date = g_new0(FileDate, 1);
        date->id = entry->id;
        date->ordinal = i;
        date->filename = g_strdup(entry->filename);
        g_ptr_array_add(m->date_entries, date);
    }
    int status = mpv_get_property_async(m->mpv, request_id(m, REQUEST_DIRECTORY),
                                       "working-directory", MPV_FORMAT_STRING);
    if (status < 0) finish(m, mpv_error_string(status));
}

static gboolean same_membership(QueueModel *m)
{
    if (!m->order || m->entries->len != m->order->len) return FALSE;
    g_autoptr(GHashTable) ids = g_hash_table_new(g_int64_hash, g_int64_equal);
    for (guint i = 0; i < m->entries->len; ++i) {
        QueueEntry *entry = m->entries->pdata[i];
        g_hash_table_add(ids, &entry->id);
    }
    for (guint i = 0; i < m->order->len; ++i)
        if (!g_hash_table_contains(ids, &g_array_index(m->order, int64_t, i))) return FALSE;
    return TRUE;
}

static void process_snapshot(QueueModel *m)
{
    if (m->started && m->action != ACTION_REMOVE && m->action != ACTION_SORT) {
        finish(m, NULL);
        return;
    }
    if (m->action == ACTION_SORT) {
        if (!m->order) {
            if (date_sort(m->sort)) {
                prepare_date_sort(m);
                return;
            }
            prepare_sort(m);
        }
        else if (!same_membership(m)) {
            finish(m, "The queue changed while sorting. Please try again.");
            return;
        }
        for (guint i = 0; i < m->order->len; ++i) {
            int64_t wanted = g_array_index(m->order, int64_t, i);
            const QueueEntry *entry = m->entries->pdata[i];
            if (entry->id == wanted) continue;
            if (m->commands >= m->command_limit) {
                finish(m, "The queue kept changing while sorting. Please try again.");
                return;
            }
            int from = index_of(m, wanted);
            if (from < 0) { finish(m, "A queue entry disappeared while sorting."); return; }
            move_indices(m, from, (int)i);
            return;
        }
        finish(m, NULL);
    } else if (m->action == ACTION_REMOVE) {
        while (m->remove_cursor < m->ids->len) {
            int index = index_of(m, g_array_index(m->ids, int64_t, m->remove_cursor++));
            if (index < 0) continue;
            char text[32];
            g_snprintf(text, sizeof(text), "%d", index);
            const char *args[] = {"playlist-remove", text, NULL};
            run(m, args);
            return;
        }
        finish(m, NULL);
    } else if (m->action == ACTION_MOVE) {
        int from = index_of(m, m->entry_id);
        int before = m->before_id < 0 ? (int)m->entries->len : index_of(m, m->before_id);
        if (from < 0 || before < 0 || from == before || from + 1 == before) {
            finish(m, NULL);
            return;
        }
        m->started = TRUE;
        move_indices(m, from, before);
    } else if (m->action == ACTION_PLAY) {
        int index = index_of(m, m->entry_id);
        if (index < 0) { finish(m, NULL); return; }
        char text[32];
        g_snprintf(text, sizeof(text), "%d", index);
        /* Match the original queue activation: selecting the current entry
         * resumes it without reopening the file or resetting its position. */
        const char *args[] = {"set", "playlist-pos", text, NULL};
        m->started = TRUE;
        run(m, args);
    } else if (m->action == ACTION_CLEAR) {
        const char *args[] = {"stop", NULL};
        m->started = TRUE;
        run(m, args);
    } else if (m->action == ACTION_SHUFFLE) {
        const char *args[] = {"playlist-shuffle", NULL};
        m->started = TRUE;
        run(m, args);
    }
}

gboolean queue_model_handle_event(QueueModel *m, const mpv_event *event)
{
    if (!m || m->stopped || !event ||
        (event->event_id != MPV_EVENT_GET_PROPERTY_REPLY && event->event_id != MPV_EVENT_COMMAND_REPLY) ||
        event->reply_userdata < QUEUE_REQUEST_BASE || event->reply_userdata >= QUEUE_REQUEST_LIMIT)
        return FALSE;
    if (!m->request_id || event->reply_userdata != m->request_id) return TRUE;
    Request request = m->request;
    m->request = REQUEST_NONE;
    m->request_id = 0;
    if (event->error < 0) { finish(m, mpv_error_string(event->error)); return TRUE; }
    if (request == REQUEST_PLAYLIST) {
        const mpv_event_property *property = event->data;
        const mpv_node *node = property && property->format == MPV_FORMAT_NODE ? property->data : NULL;
        if (!node || node->format != MPV_FORMAT_NODE_ARRAY) {
            finish(m, "Cannot read the current queue.");
            return TRUE;
        }
        queue_model_update(m, node);
        process_snapshot(m);
    } else if (request == REQUEST_DIRECTORY) {
        const mpv_event_property *property = event->data;
        if (!property || property->format != MPV_FORMAT_STRING || !property->data ||
            !*(const char *const *)property->data) {
            finish(m, "Cannot read the player's working directory.");
            return TRUE;
        }
        start_date_worker(m, *(const char *const *)property->data);
    } else if (request == REQUEST_REPEAT) {
        const mpv_event_property *property = event->data;
        if (property && property->format == MPV_FORMAT_STRING && property->data)
            queue_model_update_repeat(m, *(const char *const *)property->data);
        finish(m, NULL);
    } else if (request == REQUEST_COMMAND) {
        if (m->action == ACTION_PLAY) {
            if (m->pending_play >= 0 || m->resume_sent) {
                /* Native playlist observations publish the resulting state;
                 * another full snapshot would only delay the next click. */
                finish(m, NULL);
            } else {
                m->resume_sent = TRUE;
                const char *args[] = {"set", "pause", "no", NULL};
                run(m, args);
            }
        } else if (m->action == ACTION_REPEAT) {
            int status = mpv_get_property_async(m->mpv, request_id(m, REQUEST_REPEAT),
                                               "loop-playlist", MPV_FORMAT_STRING);
            if (status < 0) finish(m, mpv_error_string(status));
        } else snapshot(m);
    }
    return TRUE;
}

void queue_model_play(QueueModel *m, int64_t id)
{
    if (!m || m->stopped || id < 0) return;
    if (m->action != ACTION_NONE) {
        if (m->action == ACTION_PLAY && !m->started) {
            /* The requested snapshot has not yet resolved an index. Use it
             * to validate the latest clicked ID instead of loading old ones. */
            m->entry_id = id;
            m->pending_play = -1;
        } else {
            m->pending_play = m->action == ACTION_PLAY && m->entry_id == id ? -1 : id;
        }
        return;
    }
    if (!begin(m, ACTION_PLAY)) return;
    m->entry_id = id;
    snapshot(m);
}

void queue_model_remove(QueueModel *m, const int64_t *ids, gsize count)
{
    if (!ids || !count || count > G_MAXUINT || !begin(m, ACTION_REMOVE)) return;
    m->ids = g_array_sized_new(FALSE, FALSE, sizeof(int64_t), (guint)count);
    g_array_append_vals(m->ids, ids, (guint)count);
    snapshot(m);
}

void queue_model_move(QueueModel *m, int64_t id, int64_t before_id)
{
    if (id < 0 || before_id < -1 || !begin(m, ACTION_MOVE)) return;
    m->entry_id = id;
    m->before_id = before_id;
    snapshot(m);
}

void queue_model_clear(QueueModel *m)
{
    if (begin(m, ACTION_CLEAR)) snapshot(m);
}

void queue_model_sort(QueueModel *m, QueueSort sort)
{
    if (sort < QUEUE_SORT_TITLE_ASC || sort > QUEUE_SORT_MODIFIED_DESC || !begin(m, ACTION_SORT)) return;
    m->sort = sort;
    snapshot(m);
}

void queue_model_shuffle(QueueModel *m)
{
    if (begin(m, ACTION_SHUFFLE)) snapshot(m);
}

void queue_model_set_repeat(QueueModel *m, gboolean enabled)
{
    if (!begin(m, ACTION_REPEAT)) return;
    const char *args[] = {"set", "loop-playlist", enabled ? "inf" : "no", NULL};
    run(m, args);
}

void queue_model_free(QueueModel *m)
{
    if (!m || m->stopped) return;
    m->stopped = TRUE;
    if (m->request_id) mpv_abort_async_command(m->mpv, m->request_id);
    if (m->date_cancel) g_cancellable_cancel(m->date_cancel);
    model_unref(m);
}
