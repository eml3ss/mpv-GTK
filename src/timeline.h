#ifndef MPV_GTK_TIMELINE_H
#define MPV_GTK_TIMELINE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double position;
    double duration;
    double speed;
    int64_t sampled_at;
    bool paused;
    bool buffering;
    bool seeking;
    bool loaded;
} Timeline;

/* A UI clock, not a request to decode an extra video frame. Keep the
 * extrapolation short if a stream stops producing position observations. */
static inline double timeline_position(const Timeline *t, int64_t now)
{
    double p = isfinite(t->position) ? t->position : 0;
    if (t->loaded && !t->paused && !t->buffering && !t->seeking &&
        t->sampled_at > 0 && now > t->sampled_at) {
        double elapsed = (now - t->sampled_at) / 1000000.0;
        p += fmin(elapsed, 0.25) * t->speed;
    }
    if (t->duration > 0 && isfinite(t->duration))
        p = fmin(p, t->duration);
    return fmax(0, p);
}

static inline void timeline_rebase(Timeline *t, int64_t now)
{
    t->position = timeline_position(t, now);
    t->sampled_at = now;
}

#endif
