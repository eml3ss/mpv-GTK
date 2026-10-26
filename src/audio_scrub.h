#ifndef MPV_GTK_AUDIO_SCRUB_H
#define MPV_GTK_AUDIO_SCRUB_H

#include <glib.h>
#include <mpv/client.h>

typedef struct AudioScrub AudioScrub;

/* Use from the creating main context. Begin queues gain capture/suppression
 * before subsequent seek commands. End submits the final exact seek and
 * restores gain after it settles; cancellation restores without seeking. */
AudioScrub *audio_scrub_new(mpv_handle *mpv);
void audio_scrub_begin(AudioScrub *scrub);
void audio_scrub_end(AudioScrub *scrub, double position);
void audio_scrub_cancel(AudioScrub *scrub);
gboolean audio_scrub_handle_event(AudioScrub *scrub, const mpv_event *event);
gboolean audio_scrub_active(const AudioScrub *scrub);
void audio_scrub_free(AudioScrub *scrub);

#endif
