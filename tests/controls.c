#include "../src/controls.h"

#include <assert.h>
#include <stdio.h>

static void panel_hides_after_quarter_second(ControlsVisibility *s, int64_t from)
{
    assert(controls_update(s, false, from + 249999));
    assert(!controls_update(s, false, from + 250000));
}

static void test_panel(void)
{
    ControlsVisibility s = {0};
    int64_t now = INT64_C(10000000);
    assert(CONTROLS_HIDE_DELAY_US == INT64_C(250000));

    /* Playback state and elapsed time never hide a windowed transport. */
    assert(controls_update(&s, false, now));
    now += 60000000;
    assert(controls_update(&s, false, now));
    assert(s.hide_at_us == 0);

    /* Entering fullscreen reveals for exactly a quarter of a second. */
    controls_set_fullscreen(&s, true, now);
    assert(s.visible);
    panel_hides_after_quarter_second(&s, now);
    now += 60000000;
    assert(!controls_update(&s, false, now));

    /* Moving into the bottom zone reveals immediately, including after an
     * arbitrarily long pause, and a stationary pointer keeps it open. */
    assert(controls_update(&s, true, now));
    now += 60000000;
    assert(controls_update(&s, true, now));
    now += 125000;
    assert(controls_update(&s, false, now));
    panel_hides_after_quarter_second(&s, now);

    /* The same hold covers menus, Tab focus, dialogs and seek dragging.
     * Releasing the last hold leaves the full grace interval. */
    now += 10000000;
    assert(controls_update(&s, true, now));
    now += 200000;
    assert(controls_update(&s, true, now));
    now += 700000;
    assert(controls_update(&s, true, now));
    now += 250000;
    assert(controls_update(&s, false, now));
    panel_hides_after_quarter_second(&s, now);

    /* Duplicate state notifications must not extend the initial reveal. */
    now += 10000000;
    controls_set_fullscreen(&s, false, now);
    assert(s.visible && !s.fullscreen && s.hide_at_us == 0);
    controls_set_fullscreen(&s, true, now);
    controls_set_fullscreen(&s, true, now + 125000);
    panel_hides_after_quarter_second(&s, now);

    /* Leaving fullscreen restores the panel even when it was hidden.
     * A later fullscreen entry gets its own independent deadline. */
    now += 2000000;
    controls_set_fullscreen(&s, false, now);
    assert(s.visible && !s.fullscreen && s.hide_at_us == 0);
    now += 60000000;
    assert(controls_update(&s, false, now));
    controls_set_fullscreen(&s, true, now);
    panel_hides_after_quarter_second(&s, now);

    /* A held interaction from the previous mode cannot leak into the
     * next fullscreen entry or delay its initial quarter-second deadline. */
    now += 1000000;
    assert(controls_update(&s, true, now));
    controls_set_fullscreen(&s, false, now);
    assert(!s.holding && s.visible);
    controls_set_fullscreen(&s, true, now);
    panel_hides_after_quarter_second(&s, now);
}

static void cursor_hides_after_three_seconds(CursorVisibility *s, int64_t from)
{
    assert(cursor_update(s, true, false, from + 2999999));
    assert(!cursor_update(s, true, false, from + 3000000));
}

static void test_cursor(void)
{
    CursorVisibility s = {0};
    int64_t now = INT64_C(10000000);
    assert(CURSOR_HIDE_DELAY_US == INT64_C(3000000));

    assert(cursor_update(&s, true, false, now));
    now += 60000000;
    assert(cursor_update(&s, true, false, now));
    assert(s.hide_at_us == 0);

    cursor_set_fullscreen(&s, true, now);
    assert(cursor_update(&s, true, false, now));
    cursor_hides_after_three_seconds(&s, now);

    /* Any motion reveals a hidden cursor immediately, without requiring
     * a panel reveal or a click. Further motion restarts the full delay. */
    now += 4000000;
    cursor_motion(&s, now);
    assert(s.visible);
    now += 2900000;
    cursor_motion(&s, now);
    assert(cursor_update(&s, true, false, now + 100000));
    cursor_hides_after_three_seconds(&s, now);

    /* Menus, dialogs and dragging remain usable; releasing the last hold
     * must not grant a second delay after the activity deadline elapsed. */
    now += 4000000;
    assert(cursor_update(&s, true, true, now));
    now += 60000000;
    assert(cursor_update(&s, true, true, now));
    now += 125000;
    assert(!cursor_update(&s, true, false, now));

    /* Another surface keeps a visible cursor. Returning without genuine
     * pointer activity must not replace the previous fullscreen deadline. */
    now += 4000000;
    assert(cursor_update(&s, false, false, now));
    now += 60000000;
    assert(cursor_update(&s, false, false, now));
    now += 1000000;
    assert(!cursor_update(&s, true, false, now));
    cursor_motion(&s, now);
    cursor_hides_after_three_seconds(&s, now);

    /* Exit restores the cursor, and a later entry starts a fresh timer.
     * Duplicate fullscreen notifications must not prolong that timer. */
    now += 4000000;
    cursor_set_fullscreen(&s, false, now);
    assert(s.visible && !s.holding && s.hide_at_us == 0);
    now += 60000000;
    assert(cursor_update(&s, true, false, now));
    cursor_set_fullscreen(&s, true, now);
    assert(cursor_update(&s, true, false, now));
    cursor_set_fullscreen(&s, true, now + 1000000);
    cursor_hides_after_three_seconds(&s, now);
}

static void test_independent_visibility(void)
{
    ControlsVisibility panel = {0};
    CursorVisibility cursor = {0};
    int64_t now = INT64_C(10000000);
    controls_set_fullscreen(&panel, true, now);
    cursor_set_fullscreen(&cursor, true, now);
    assert(cursor_update(&cursor, true, false, now));

    /* The panel has disappeared long before the cursor's own deadline. */
    assert(!controls_update(&panel, false, now + 250000));
    assert(cursor_update(&cursor, true, false, now + 250000));
    cursor_hides_after_three_seconds(&cursor, now);

    /* Moving over the video restores only the cursor. Conversely, a
     * stationary bottom hover may keep the panel open past cursor hide. */
    now += 4000000;
    cursor_motion(&cursor, now);
    assert(cursor.visible);
    assert(!controls_update(&panel, false, now));
    assert(controls_update(&panel, true, now));
    now += 3000000;
    assert(controls_update(&panel, true, now));
    assert(!cursor_update(&cursor, true, false, now));
}

static void test_cursor_position_events(void)
{
    const int64_t start = INT64_C(10000000);
    CursorVisibility cursor = {0};

    /* Position is already known when fullscreen starts. A layout-generated
     * crossing must not turn that stationary pointer into fresh activity. */
    cursor_position(&cursor, 100.0, 200.0, true, start - 1000000);
    cursor_set_fullscreen(&cursor, true, start);
    assert(cursor_update(&cursor, true, false, start));
    const int64_t deadline = cursor.hide_at_us;
    for (int frame = 1; frame <= 300; frame++) {
        int64_t now = start + INT64_C(1000000) * frame / 60;
        /* A reallocation can change reported coordinates without moving
         * the physical pointer; subsequent events use that new baseline. */
        double x = frame < 60 ? 100.0 : 120.0;
        assert(!cursor_position(&cursor, x, 200.0, true, now));
        assert(cursor_update(&cursor, true, false, now) == (frame < 180));
        assert(cursor.hide_at_us == deadline);
    }

    /* Non-synthetic events at an unchanged position also cannot keep the
     * cursor visible forever, even when supplied at the video frame rate. */
    const int64_t restart = start + 6000000;
    cursor_set_fullscreen(&cursor, false, restart);
    cursor_set_fullscreen(&cursor, true, restart);
    assert(cursor_update(&cursor, true, false, restart));
    for (int frame = 1; frame <= 300; frame++) {
        int64_t now = restart + INT64_C(1000000) * frame / 60;
        assert(!cursor_position(&cursor, 120.0, 200.0, false, now));
        assert(cursor_update(&cursor, true, false, now) == (frame < 180));
        assert(cursor.hide_at_us == restart + CURSOR_HIDE_DELAY_US);
    }

    /* A real coordinate change reveals immediately and starts the full
     * delay. Quiet coordinate changes update the baseline without adding
     * activity, including when followed by a regular same-position event. */
    const int64_t moved = restart + 6000000;
    assert(cursor_position(&cursor, 121.0, 200.0, false, moved));
    assert(cursor.visible);
    assert(cursor.hide_at_us == moved + CURSOR_HIDE_DELAY_US);
    assert(cursor_update(&cursor, true, false, moved));
    assert(!cursor_position(&cursor, 400.0, 500.0, true, moved + 1000000));
    assert(!cursor_position(&cursor, 400.0, 500.0, false, moved + 1500000));
    assert(cursor.hide_at_us == moved + CURSOR_HIDE_DELAY_US);
    cursor_hides_after_three_seconds(&cursor, moved);
    assert(cursor_position(&cursor, 400.0, 501.0, false, moved + 4000000));
    assert(cursor.visible);
    cursor_hides_after_three_seconds(&cursor, moved + 4000000);
}

static void test_cursor_configured_delay(void)
{
    const int64_t delays[] = {100000, 1500000, 5000000};
    const int64_t start = INT64_C(10000000);
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        CursorVisibility cursor = {0};
        const int64_t delay = delays[i];
        cursor_set_delay(&cursor, delay, start);
        cursor_set_fullscreen(&cursor, true, start);
        assert(cursor_update(&cursor, true, false, start));
        assert(cursor_update(&cursor, true, false, start + delay - 1));
        assert(!cursor_update(&cursor, true, false, start + delay));

        /* Motion and later fullscreen entries retain the chosen timeout. */
        const int64_t moved = start + delay + 1000000;
        cursor_motion(&cursor, moved);
        assert(cursor_update(&cursor, true, false, moved + delay - 1));
        assert(!cursor_update(&cursor, true, false, moved + delay));
        const int64_t entered = moved + delay + 1000000;
        cursor_set_fullscreen(&cursor, false, entered);
        cursor_set_fullscreen(&cursor, true, entered);
        assert(cursor_update(&cursor, true, false, entered));
        assert(cursor_update(&cursor, true, false, entered + delay - 1));
        assert(!cursor_update(&cursor, true, false, entered + delay));
    }

    /* A zero value keeps the default for callers with zero-initialized state. */
    CursorVisibility cursor = {0};
    cursor_set_delay(&cursor, 0, start);
    cursor_set_fullscreen(&cursor, true, start);
    assert(cursor_update(&cursor, true, false, start));
    cursor_hides_after_three_seconds(&cursor, start);
}

static void test_cursor_delay_change_uses_activity(void)
{
    CursorVisibility cursor = {0};
    const int64_t start = INT64_C(10000000);
    cursor_set_fullscreen(&cursor, true, start);
    assert(cursor_update(&cursor, true, false, start));

    /* Editing the timeout does not grant a fresh complete interval. One
     * second has already elapsed when the delay becomes two seconds. */
    cursor_set_delay(&cursor, 2000000, start + 1000000);
    assert(cursor_update(&cursor, true, false, start + 1000000));
    assert(cursor_update(&cursor, true, false, start + 1999999));
    assert(!cursor_update(&cursor, true, false, start + 2000000));

    /* Reducing the timeout below the elapsed time takes effect on the
     * next update, instead of postponing hiding until another movement. */
    const int64_t moved = start + 10000000;
    cursor_set_delay(&cursor, 5000000, moved);
    cursor_motion(&cursor, moved);
    assert(cursor_update(&cursor, true, false, moved + 2000000));
    cursor_set_delay(&cursor, 1500000, moved + 2000000);
    assert(!cursor_update(&cursor, true, false, moved + 2000000));
}

static void test_cursor_configured_hold_and_position(void)
{
    CursorVisibility cursor = {0};
    const int64_t start = INT64_C(10000000);
    const int64_t delay = INT64_C(1500000);
    cursor_set_delay(&cursor, delay, start);
    cursor_set_fullscreen(&cursor, true, start);
    assert(cursor_update(&cursor, true, false, start));

    /* An interaction stays usable for arbitrarily long without moving
     * the configured deadline. It can hide immediately when released. */
    assert(cursor_update(&cursor, true, true, start + 1000000));
    assert(cursor_update(&cursor, true, true, start + 60000000));
    const int64_t released = start + 61000000;
    assert(cursor.hide_at_us == start + delay);
    assert(!cursor_update(&cursor, true, false, released));

    /* Quiet coordinate changes followed by stationary native events do
     * not extend a custom deadline, even when delivered every 50 ms. */
    const int64_t entered = released + delay + 1000000;
    cursor_position(&cursor, 100, 200, true, entered);
    cursor_set_fullscreen(&cursor, false, entered);
    cursor_set_fullscreen(&cursor, true, entered);
    assert(cursor_update(&cursor, true, false, entered));
    for (int step = 1; step <= 40; step++) {
        const int64_t now = entered + INT64_C(50000) * step;
        const double x = step < 6 ? 100 : 120;
        assert(!cursor_position(&cursor, x, 200, true, now));
        assert(!cursor_position(&cursor, x, 200, false, now));
        assert(cursor_update(&cursor, true, false, now) == (step < 30));
        assert(cursor.hide_at_us == entered + delay);
    }
}

static void test_cursor_hold_does_not_restart_deadline(void)
{
    CursorVisibility cursor = {0};
    const int64_t start = INT64_C(10000000);
    const int64_t delay = INT64_C(2000000);
    cursor_set_delay(&cursor, delay, start);
    cursor_set_fullscreen(&cursor, true, start);

    /* The first dispatch may arrive later than fullscreen entry. Merely
     * observing pointer presence must not start another two-second wait. */
    assert(cursor_update(&cursor, true, false, start + 1000000));
    assert(cursor.hide_at_us == start + delay);
    assert(cursor.last_activity_us == start);

    /* Holding from one second until four seconds leaves every sampled
     * deadline unchanged. Release hides immediately, not at six seconds. */
    for (int frame = 60; frame <= 240; frame++) {
        const int64_t now = start + INT64_C(1000000) * frame / 60;
        assert(cursor_update(&cursor, true, true, now));
        assert(cursor.hide_at_us == start + delay);
        assert(cursor.last_activity_us == start);
    }
    assert(!cursor_update(&cursor, true, false, start + 4000000));
    assert(cursor.hide_at_us == start + delay);

    /* A real movement during a hold is activity. Releasing it early keeps
     * only the remainder of that movement's interval. */
    const int64_t moved = start + 5000000;
    cursor_motion(&cursor, moved);
    assert(cursor_update(&cursor, true, true, moved + 1000000));
    assert(cursor_update(&cursor, true, false, moved + 1500000));
    assert(cursor.hide_at_us == moved + delay);
    assert(cursor_update(&cursor, true, false, moved + delay - 1));
    assert(!cursor_update(&cursor, true, false, moved + delay));
}

int main(void)
{
    test_panel();
    test_cursor();
    test_independent_visibility();
    test_cursor_position_events();
    test_cursor_configured_delay();
    test_cursor_delay_change_uses_activity();
    test_cursor_configured_hold_and_position();
    test_cursor_hold_does_not_restart_deadline();
    puts("PASS visibility: exact 250 ms panel and configurable cursor delays, activity-anchored changes and holds, real motion, synthetic/stationary events, exit/re-entry");
    return 0;
}
