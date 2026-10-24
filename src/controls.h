#ifndef MPV_GTK_CONTROLS_H
#define MPV_GTK_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

#define CONTROLS_HIDE_DELAY_US INT64_C(250000)
#define CURSOR_HIDE_DELAY_US INT64_C(3000000)

typedef struct {
    bool fullscreen;
    bool visible;
    bool holding;
    int64_t hide_at_us;
} ControlsVisibility;

static inline int64_t controls_hide_deadline(int64_t now)
{
    return now > INT64_MAX - CONTROLS_HIDE_DELAY_US
         ? INT64_MAX : now + CONTROLS_HIDE_DELAY_US;
}

/* Only a fullscreen transition starts a fresh reveal. Repeated compositor
 * notifications must not keep postponing the hide deadline. */
static inline void controls_set_fullscreen(ControlsVisibility *s, bool fullscreen,
                                           int64_t now)
{
    if (s->fullscreen == fullscreen && fullscreen)
        return;
    s->fullscreen = fullscreen;
    s->visible = true;
    s->holding = false;
    s->hide_at_us = fullscreen ? controls_hide_deadline(now) : 0;
}

/* The caller combines pointer, keyboard focus, menus, dialogs and dragging
 * into hold. No video frame or playback state participates in this policy. */
static inline bool controls_update(ControlsVisibility *s, bool hold, int64_t now)
{
    if (!s->fullscreen) {
        s->visible = true;
        s->holding = false;
        s->hide_at_us = 0;
    } else if (hold) {
        s->visible = true;
        s->holding = true;
        s->hide_at_us = controls_hide_deadline(now);
    } else if (s->holding) {
        /* Start the full delay when the last interaction ends, rather
         * than from the preceding frame-clock sample. */
        s->holding = false;
        s->hide_at_us = controls_hide_deadline(now);
    } else if (s->visible && now >= s->hide_at_us) {
        s->visible = false;
    }
    return s->visible;
}

typedef struct {
    bool fullscreen;
    bool visible;
    bool holding;
    bool pointer_present;
    bool position_known;
    double pointer_x, pointer_y;
    int64_t hide_at_us, delay_us, last_activity_us;
} CursorVisibility;

static inline int64_t cursor_hide_deadline(const CursorVisibility *s, int64_t now)
{
    int64_t delay = s->delay_us > 0 ? s->delay_us : CURSOR_HIDE_DELAY_US;
    return now > INT64_MAX - delay ? INT64_MAX : now + delay;
}

static inline void cursor_set_delay(CursorVisibility *s, int64_t delay_us,
                                     int64_t now)
{
    s->delay_us = delay_us > 0 ? delay_us : CURSOR_HIDE_DELAY_US;
    if (s->fullscreen) {
        /* Changing the preference changes the interval, not the activity
         * timestamp. It must not silently start another complete delay. */
        if (!s->last_activity_us)
            s->last_activity_us = now;
        s->hide_at_us = cursor_hide_deadline(s, s->last_activity_us);
    }
}

static inline void cursor_set_fullscreen(CursorVisibility *s, bool fullscreen,
                                         int64_t now)
{
    if (s->fullscreen == fullscreen && fullscreen)
        return;
    s->fullscreen = fullscreen;
    s->visible = true;
    s->holding = false;
    s->pointer_present = false;
    s->last_activity_us = now;
    s->hide_at_us = fullscreen ? cursor_hide_deadline(s, now) : 0;
}

/* Motion anywhere over the video restores the cursor without changing
 * the transport panel. A stationary pointer may then hide independently. */
static inline void cursor_motion(CursorVisibility *s, int64_t now)
{
    s->visible = true;
    s->pointer_present = true;
    s->last_activity_us = now;
    s->hide_at_us = s->fullscreen ? cursor_hide_deadline(s, now) : 0;
}

/* Like mpv's set_mouse_pos(), equal surface coordinates are not activity.
 * GTK also sends quiet, synthetic motion when layout changes: remember its
 * coordinates without letting it reveal the cursor or extend the deadline. */
static inline bool cursor_position(CursorVisibility *s, double x, double y,
                                    bool quiet, int64_t now)
{
    bool changed = !s->position_known || s->pointer_x != x || s->pointer_y != y;
    s->position_known = true;
    s->pointer_x = x;
    s->pointer_y = y;
    if (!changed || quiet)
        return false;
    cursor_motion(s, now);
    return true;
}

/* Only active interactions hold the cursor: dragging, menus and dialogs.
 * Holding suspends hiding without inventing pointer activity. Releasing a
 * hold evaluates the same deadline, which may already have elapsed. */
static inline bool cursor_update(CursorVisibility *s, bool pointer_present,
                                  bool hold, int64_t now)
{
    if (!s->fullscreen) {
        s->visible = true;
        s->holding = false;
        s->pointer_present = pointer_present;
        s->hide_at_us = 0;
    } else {
        s->pointer_present = pointer_present;
        s->holding = pointer_present && hold;
        if (!pointer_present || hold)
            s->visible = true;
        else if (s->visible && now >= s->hide_at_us)
            s->visible = false;
    }
    return s->visible;
}

#endif
