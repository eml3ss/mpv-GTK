#include "audio_scrub.h"

#include <math.h>
#include <stdint.h>
#include <unistd.h>

#define SCRUB_BASE (UINT64_C(1) << 54)
#define SCRUB_COMMAND_BASE (SCRUB_BASE + UINT64_C(1024))
#define SCRUB_QUERY_BASE (SCRUB_BASE + (UINT64_C(1) << 32))
#define SCRUB_LIMIT (SCRUB_BASE + (UINT64_C(1) << 33))
#define SILENT_GAIN (-150.0)
#define RECOVERY_MS 250

typedef struct { const char *name; mpv_format format; } ScrubProperty;
static const ScrubProperty properties[] = {
    {"volume-gain", MPV_FORMAT_DOUBLE}, {"pause", MPV_FORMAT_FLAG},
    {"speed", MPV_FORMAT_DOUBLE}, {"container-fps", MPV_FORMAT_DOUBLE}
};

struct AudioScrub {
    mpv_handle *mpv;
    char *saved_property, *saved_expansion, *pause_property, *pause_expansion;
    gboolean held, releasing, stopped, acknowledged, seek_seen;
    gboolean paused, pause_hold_acknowledged, position_ready, seeking_ready, seeking;
    guint64 serial, final_request, position_request, seeking_request, pause_hold_request;
    double target, position, speed, fps;
    gint64 issued;
    guint recovery;
};

static guint64 next_command(AudioScrub *scrub)
{
    return SCRUB_COMMAND_BASE + ++scrub->serial;
}

static void command(AudioScrub *scrub, const char *const *arguments)
{
    mpv_command_async(scrub->mpv, next_command(scrub), (const char **)arguments);
}

static void suppress_gain(AudioScrub *scrub)
{
    double gain = SILENT_GAIN;
    mpv_set_property_async(scrub->mpv, next_command(scrub), "volume-gain", MPV_FORMAT_DOUBLE, &gain);
}

static void hold_pause(AudioScrub *scrub)
{
    int paused = TRUE;
    scrub->pause_hold_acknowledged = FALSE;
    scrub->pause_hold_request = next_command(scrub);
    if (mpv_set_property_async(scrub->mpv, scrub->pause_hold_request, "pause", MPV_FORMAT_FLAG, &paused) < 0)
        audio_scrub_cancel(scrub);
}

static void clear_final_seek(AudioScrub *scrub)
{
    if (scrub->recovery) { g_source_remove(scrub->recovery); scrub->recovery = 0; }
    scrub->releasing = scrub->acknowledged = scrub->seek_seen = FALSE;
    scrub->final_request = scrub->position_request = scrub->seeking_request = 0;
    scrub->position_ready = scrub->seeking_ready = FALSE;
}

void audio_scrub_cancel(AudioScrub *scrub)
{
    if (!scrub || scrub->stopped) return;
    clear_final_seek(scrub);
    if (!scrub->held) return;
    scrub->held = FALSE;
    scrub->pause_hold_request = 0;
    scrub->pause_hold_acknowledged = FALSE;
    /* The snapshot lives in the core, so cancel/free can restore even before
     * any client property reply was dispatched. These commands share its FIFO
     * with begin's capture and suppression; no render-thread read is needed. */
    const char *restore[] = {"expand-properties", "set", "volume-gain", scrub->saved_expansion, NULL};
    command(scrub, restore);
    /* Restore gain before resuming playback. A held frame remains paused
     * through every drag seek, including the final exact seek. */
    const char *resume[] = {"expand-properties", "set", "pause", scrub->pause_expansion, NULL};
    command(scrub, resume);
}

static void maybe_restore(AudioScrub *scrub)
{
    if (!scrub->held || !scrub->releasing || !scrub->acknowledged ||
        !scrub->position_ready || !scrub->seeking_ready || scrub->seeking || !isfinite(scrub->position)) return;
    double allowance = MAX(.25, 1 / MAX(1, scrub->fps));
    if (!scrub->paused)
        allowance += MAX((gint64)0, g_get_monotonic_time() - scrub->issued) / 1000000.0 * scrub->speed;
    if (fabs(scrub->position - scrub->target) <= allowance) audio_scrub_cancel(scrub);
}

static void probe_final_seek(AudioScrub *scrub)
{
    if (!scrub->held || !scrub->releasing || !scrub->acknowledged ||
        scrub->position_request || scrub->seeking_request) return;
    scrub->position_ready = scrub->seeking_ready = FALSE;
    scrub->position = NAN; scrub->seeking = TRUE;
    scrub->position_request = SCRUB_QUERY_BASE + ++scrub->serial;
    scrub->seeking_request = SCRUB_QUERY_BASE + ++scrub->serial;
    if (mpv_get_property_async(scrub->mpv, scrub->position_request, "time-pos", MPV_FORMAT_DOUBLE) < 0) {
        scrub->position_request = 0; scrub->position_ready = TRUE;
    }
    if (mpv_get_property_async(scrub->mpv, scrub->seeking_request, "seeking", MPV_FORMAT_FLAG) < 0) {
        scrub->seeking_request = 0; scrub->seeking_ready = TRUE;
    }
    maybe_restore(scrub);
}

static gboolean recover_release(gpointer data)
{
    AudioScrub *scrub = data;
    if (!scrub->held || !scrub->releasing || scrub->stopped) {
        scrub->recovery = 0;
        return G_SOURCE_REMOVE;
    }
    if (g_get_monotonic_time() - scrub->issued >= 2 * G_TIME_SPAN_SECOND) {
        scrub->recovery = 0;
        audio_scrub_cancel(scrub);
        return G_SOURCE_REMOVE;
    }
    probe_final_seek(scrub);
    if (!scrub->held) { scrub->recovery = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

AudioScrub *audio_scrub_new(mpv_handle *mpv)
{
    g_return_val_if_fail(mpv != NULL, NULL);
    static gint instance;
    AudioScrub *scrub = g_new0(AudioScrub, 1);
    scrub->mpv = mpv; scrub->speed = 1; scrub->fps = 25;
    guint token = (guint)g_atomic_int_add(&instance, 1);
    scrub->saved_property = g_strdup_printf("user-data/mpv-gtk-scrub-gain-%ld-%u", (long)getpid(), token);
    scrub->pause_property = g_strdup_printf("user-data/mpv-gtk-scrub-pause-%ld-%u", (long)getpid(), token);
    /* Restore only while our attenuation remains effective, so a user's or
     * new file's unobserved gain edit wins too. volume-gain's print format is
     * one decimal; raw user-data would JSON-quote the captured numeric string. */
    scrub->saved_expansion = g_strdup_printf(
        "${?volume-gain==-150.0:${%s}}${!volume-gain==-150.0:${=volume-gain}}", scrub->saved_property);
    scrub->pause_expansion = g_strdup_printf(
        "${?pause==yes:${%s}}${!pause==yes:${=pause}}", scrub->pause_property);
    for (guint i = 0; i < G_N_ELEMENTS(properties); ++i)
        mpv_observe_property(mpv, SCRUB_BASE + i, properties[i].name, properties[i].format);
    return scrub;
}

void audio_scrub_begin(AudioScrub *scrub)
{
    if (!scrub || scrub->stopped) return;
    clear_final_seek(scrub);
    if (scrub->held) return;
    scrub->held = TRUE;
    const char *capture[] = {"expand-properties", "set", scrub->saved_property, "${=volume-gain}", NULL};
    command(scrub, capture);
    const char *capture_pause[] = {"expand-properties", "set", scrub->pause_property, "${=pause}", NULL};
    command(scrub, capture_pause);
    suppress_gain(scrub);
    hold_pause(scrub);
}

void audio_scrub_end(AudioScrub *scrub, double position)
{
    if (!scrub || scrub->stopped || !scrub->held) return;
    clear_final_seek(scrub);
    if (!isfinite(position)) { audio_scrub_cancel(scrub); return; }
    scrub->releasing = TRUE;
    scrub->target = MAX(0, position);
    scrub->issued = g_get_monotonic_time();
    char seconds[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(seconds, sizeof(seconds), scrub->target);
    const char *seek[] = {"seek", seconds, "absolute+exact", NULL};
    scrub->final_request = next_command(scrub);
    if (mpv_command_async(scrub->mpv, scrub->final_request, seek) < 0) {
        audio_scrub_cancel(scrub);
        return;
    }
    scrub->recovery = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, RECOVERY_MS, recover_release, scrub, NULL);
}

gboolean audio_scrub_active(const AudioScrub *scrub)
{
    return scrub && !scrub->stopped && scrub->held;
}

gboolean audio_scrub_handle_event(AudioScrub *scrub, const mpv_event *event)
{
    if (!scrub || scrub->stopped || !event) return FALSE;
    if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && event->reply_userdata >= SCRUB_BASE &&
        event->reply_userdata < SCRUB_BASE + G_N_ELEMENTS(properties)) {
        const mpv_event_property *property = event->data;
        if (!property || !property->data) return TRUE;
        guint index = (guint)(event->reply_userdata - SCRUB_BASE);
        if (index == 0 && property->format == MPV_FORMAT_DOUBLE) {
            double gain = *(double *)property->data;
            if (scrub->held && isfinite(gain) && fabs(gain - SILENT_GAIN) > .000001) {
                /* Keep a user's observed gain edit, then silence this gesture
                 * again. Mute, volume, track selection and filters are intact. */
                mpv_set_property_async(scrub->mpv, next_command(scrub), scrub->saved_property,
                                        MPV_FORMAT_DOUBLE, &gain);
                suppress_gain(scrub);
            }
        } else if (index == 1 && property->format == MPV_FORMAT_FLAG) {
            scrub->paused = *(int *)property->data;
            if (scrub->held && scrub->pause_hold_acknowledged && !scrub->paused) {
                /* A Play request during dragging takes effect on release;
                 * our own forced pause notifications never replace intent. */
                const char *remember_play[] = {"set", scrub->pause_property, "no", NULL};
                command(scrub, remember_play);
                hold_pause(scrub);
            }
        }
        else if (property->format == MPV_FORMAT_DOUBLE) {
            double value = *(double *)property->data;
            if (isfinite(value) && value > 0) {
                if (index == 2) scrub->speed = value;
                else if (index == 3) scrub->fps = value;
            }
        }
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->reply_userdata >= SCRUB_COMMAND_BASE &&
        event->reply_userdata < SCRUB_QUERY_BASE) {
        if (event->reply_userdata == scrub->final_request) {
            if (event->error < 0) audio_scrub_cancel(scrub);
            else scrub->acknowledged = TRUE;
        }
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY && event->reply_userdata >= SCRUB_COMMAND_BASE &&
        event->reply_userdata < SCRUB_QUERY_BASE) {
        if (event->reply_userdata == scrub->pause_hold_request) {
            if (event->error < 0) audio_scrub_cancel(scrub);
            else scrub->pause_hold_acknowledged = TRUE;
        }
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY && event->reply_userdata >= SCRUB_QUERY_BASE &&
        event->reply_userdata < SCRUB_LIMIT) {
        const mpv_event_property *property = event->data;
        gboolean valid = event->error >= 0 && property && property->data;
        if (event->reply_userdata == scrub->position_request) {
            scrub->position_request = 0; scrub->position_ready = TRUE;
            scrub->position = valid && property->format == MPV_FORMAT_DOUBLE ? *(double *)property->data : NAN;
        } else if (event->reply_userdata == scrub->seeking_request) {
            scrub->seeking_request = 0; scrub->seeking_ready = TRUE;
            scrub->seeking = !valid || property->format != MPV_FORMAT_FLAG || *(int *)property->data;
        }
        maybe_restore(scrub);
        return TRUE;
    }
    if (event->event_id == MPV_EVENT_START_FILE || event->event_id == MPV_EVENT_END_FILE ||
        event->event_id == MPV_EVENT_SHUTDOWN) audio_scrub_cancel(scrub);
    else if (scrub->held && scrub->releasing && scrub->acknowledged) {
        if (event->event_id == MPV_EVENT_SEEK) scrub->seek_seen = TRUE;
        else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && scrub->seek_seen) {
            probe_final_seek(scrub);
        }
    }
    return FALSE;
}

void audio_scrub_free(AudioScrub *scrub)
{
    if (!scrub) return;
    audio_scrub_cancel(scrub);
    scrub->stopped = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(properties); ++i) mpv_unobserve_property(scrub->mpv, SCRUB_BASE + i);
    g_free(scrub->saved_property); g_free(scrub->saved_expansion);
    g_free(scrub->pause_property); g_free(scrub->pause_expansion); g_free(scrub);
}
