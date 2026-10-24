#include "../src/timeline.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>

static void near(double actual, double expected)
{
    assert(isfinite(actual));
    assert(fabs(actual - expected) < 1e-9);
}

static Timeline playing(void)
{
    return (Timeline){
        .position = 10,
        .duration = 100,
        .speed = 1,
        .sampled_at = 1000000,
        .loaded = true,
    };
}

int main(void)
{
    Timeline t = playing();
    near(timeline_position(&t, 1100000), 10.1);
    near(timeline_position(&t, 9000000), 10.25); /* stale observations stay bounded */
    near(timeline_position(&t, INT64_MAX), 10.25);
    near(timeline_position(&t, 1000000), 10);
    near(timeline_position(&t, 999999), 10); /* reversed monotonic sample */
    near(timeline_position(&t, -1), 10);
    t.sampled_at = 0;
    near(timeline_position(&t, 1100000), 10);
    t.sampled_at = INT64_MIN;
    near(timeline_position(&t, 1100000), 10);

    t = playing();
    t.paused = true;
    near(timeline_position(&t, 1200000), 10);
    t.paused = false;
    t.buffering = true;
    near(timeline_position(&t, 1200000), 10);
    t.buffering = false;
    t.seeking = true;
    near(timeline_position(&t, 1200000), 10);
    t.seeking = false;
    t.loaded = false;
    near(timeline_position(&t, 1200000), 10);

    t = playing();
    timeline_rebase(&t, 1100000);
    near(t.position, 10.1);
    assert(t.sampled_at == 1100000);
    t.speed = 2;
    near(timeline_position(&t, 1100000), 10.1); /* changing rate cannot jump */
    near(timeline_position(&t, 1150000), 10.2);
    timeline_rebase(&t, 1150000);
    t.paused = true;
    near(timeline_position(&t, 9000000), 10.2);
    timeline_rebase(&t, 9000000);
    t.paused = false;
    t.speed = 0.5;
    near(timeline_position(&t, 9100000), 10.25); /* no paused time leaks on resume */

    t = playing();
    t.duration = 10.05;
    near(timeline_position(&t, 1200000), 10.05);
    t.position = 12;
    t.paused = true;
    near(timeline_position(&t, 1200000), 10.05);
    const double no_duration[] = {0, -1, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(no_duration) / sizeof(no_duration[0]); ++i) {
        t.duration = no_duration[i];
        near(timeline_position(&t, 1200000), 12);
    }
    t.position = -12;
    near(timeline_position(&t, 1200000), 0);
    const double bad_positions[] = {NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(bad_positions) / sizeof(bad_positions[0]); ++i) {
        t.position = bad_positions[i];
        near(timeline_position(&t, 1200000), 0);
        timeline_rebase(&t, 1200000);
        near(t.position, 0);
        assert(t.sampled_at == 1200000);
    }
    t.position = DBL_MAX;
    t.duration = 15;
    near(timeline_position(&t, 1200000), 15);

    puts("PASS timeline: pause/buffer/seek freezes, speed continuity, bounds, invalid samples");
    return 0;
}
