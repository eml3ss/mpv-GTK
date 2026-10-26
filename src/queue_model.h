#ifndef MPV_GTK_QUEUE_MODEL_H
#define MPV_GTK_QUEUE_MODEL_H

#include <glib.h>
#include <mpv/client.h>
#include <stdint.h>

typedef struct QueueModel QueueModel;
typedef struct {
    int64_t id;
    char *title, *filename;
    gboolean current;
} QueueEntry;

typedef enum {
    QUEUE_SORT_TITLE_ASC, QUEUE_SORT_TITLE_DESC,
    QUEUE_SORT_PATH_ASC, QUEUE_SORT_PATH_DESC,
    QUEUE_SORT_CREATED_ASC, QUEUE_SORT_CREATED_DESC,
    QUEUE_SORT_MODIFIED_ASC, QUEUE_SORT_MODIFIED_DESC
} QueueSort;

/* All calls and callbacks belong on the GTK/main context. Playlist snapshots
 * are copied; entry pointers remain valid until the next model update. */
QueueModel *queue_model_new(mpv_handle *mpv, void (*changed)(gpointer),
                            void (*error)(const char *, gpointer), gpointer data);
const GPtrArray *queue_model_entries(const QueueModel *model);
gboolean queue_model_busy(const QueueModel *model);
const char *queue_model_repeat(const QueueModel *model);
void queue_model_update(QueueModel *model, const mpv_node *playlist);
void queue_model_set_position(QueueModel *model, int64_t position);
void queue_model_update_repeat(QueueModel *model, const char *value);
gboolean queue_model_handle_event(QueueModel *model, const mpv_event *event);

/* Mutations resolve stable IDs against fresh native snapshots. Reordering
 * never reloads media or changes the current playing entry. */
void queue_model_play(QueueModel *model, int64_t id);
void queue_model_remove(QueueModel *model, const int64_t *ids, gsize count);
/* before_id == -1 moves the entry to the end. */
void queue_model_move(QueueModel *model, int64_t id, int64_t before_id);
/* Clear the entire queue and stop playback; source files are never deleted. */
void queue_model_clear(QueueModel *model);
/* Date sorts inspect local files on a cancellable worker only when requested.
 * Creation means birth time, with no ctime/mtime fallback. Unknown dates sort
 * last in both directions; ties retain the current native queue order. */
void queue_model_sort(QueueModel *model, QueueSort sort);
void queue_model_shuffle(QueueModel *model);
void queue_model_set_repeat(QueueModel *model, gboolean enabled);
void queue_model_free(QueueModel *model);

#endif
