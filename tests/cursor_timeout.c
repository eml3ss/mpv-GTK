/* Exercise the exact cursor source used by the host against real monotonic
 * time. No GTK initialization, display, frame clock or video playback runs.
 * The callback returns before widget access because controls_revealer is NULL.
 * This verifies scheduling, not compositor presentation of the cursor. */
#define main mpv_gtk_program_main
#include "../src/main.c"
#undef main

typedef struct {
    Player player;
    gint64 started, deadline;
    guint source, pulses;
} CursorClock;

static gboolean quiet_pointer_pulse(gpointer data)
{
    CursorClock *clock = data;
    Player *p = &clock->player;
    if (!p->cursor_source)
        return G_SOURCE_REMOVE;
    gint64 now = g_get_monotonic_time();
    double x = clock->pulses++ % 2 ? 100 : 120;
    g_assert_false(cursor_position(&p->cursor_state, x, 200, true, now));
    g_assert_false(cursor_position(&p->cursor_state, x, 200, false, now));
    cursor_timeout_arm(p, p->cursor_state.hide_at_us);
    /* Stationary/synthetic input may ask to reschedule on every dispatch.
     * It must retain one source and the original absolute target. */
    g_assert_cmpuint(p->cursor_source, ==, clock->source);
    g_assert_cmpint(p->cursor_deadline_us, ==, clock->deadline);
    g_assert_cmpint(p->cursor_state.hide_at_us, ==, clock->deadline);
    return G_SOURCE_CONTINUE;
}

static gboolean scheduler_watchdog(gpointer data)
{
    (void)data;
    g_error("Cursor source failed to dispatch within its wall-clock deadline tolerance");
    return G_SOURCE_REMOVE;
}

static void await_source(Player *p, gint64 deadline)
{
    GSource *watchdog = g_timeout_source_new(0);
    g_source_set_ready_time(watchdog, deadline + 500000);
    g_source_set_callback(watchdog, scheduler_watchdog, NULL, NULL);
    g_source_attach(watchdog, NULL);
    while (p->cursor_source)
        g_main_context_iteration(NULL, TRUE);
    g_source_destroy(watchdog);
    g_source_unref(watchdog);
    gint64 dispatched = g_get_monotonic_time();
    g_assert_cmpint(dispatched, >=, deadline);
    g_assert_cmpint(dispatched - deadline, <=, 500000);
    g_assert_cmpint(p->cursor_deadline_us, ==, 0);
    g_assert_cmpuint(p->ui_ticks, ==, 0);
}

static void test_wall_clock(gint64 delay)
{
    CursorClock clock = {0};
    Player *p = &clock.player;
    clock.started = g_get_monotonic_time();
    clock.deadline = clock.started + delay;
    cursor_set_delay(&p->cursor_state, delay, clock.started);
    cursor_set_fullscreen(&p->cursor_state, true, clock.started);
    cursor_timeout_arm(p, clock.deadline);
    clock.source = p->cursor_source;
    g_assert_cmpuint(clock.source, !=, 0);
    guint pulse = g_timeout_add(10, quiet_pointer_pulse, &clock);
    await_source(p, clock.deadline);
    if (g_main_context_find_source_by_id(NULL, pulse))
        g_source_remove(pulse);
    g_assert_cmpuint(clock.pulses, >, 0);
    gint64 elapsed = g_get_monotonic_time() - clock.started;
    g_print("PASS cursor GLib scheduler: requested %.1f s, wall-clock %.3f s, "
            "%u stationary/synthetic pulses, no GTK frame ticks\n",
            delay / 1000000.0, elapsed / 1000000.0, clock.pulses);
}

static void test_cancel_and_change(void)
{
    Player p = {0};
    gint64 now = g_get_monotonic_time();
    cursor_timeout_arm(&p, now + 200000);
    guint source = p.cursor_source;
    g_assert_nonnull(g_main_context_find_source_by_id(NULL, source));
    cursor_timeout_cancel(&p);
    g_assert_cmpuint(p.cursor_source, ==, 0);
    g_assert_cmpint(p.cursor_deadline_us, ==, 0);
    g_assert_null(g_main_context_find_source_by_id(NULL, source));
    cursor_timeout_cancel(&p); /* Cancelling an idle timer is harmless. */

    now = g_get_monotonic_time();
    cursor_timeout_arm(&p, now + 300000);
    gint64 replacement = now + 100000;
    cursor_timeout_arm(&p, replacement);
    g_assert_cmpint(p.cursor_deadline_us, ==, replacement);
    await_source(&p, replacement);
    g_print("PASS cursor GLib scheduler: cancellation and changed absolute deadline\n");
}

int main(void)
{
    test_wall_clock(100000);
    test_wall_clock(2000000);
    test_cancel_and_change();
    return 0;
}
