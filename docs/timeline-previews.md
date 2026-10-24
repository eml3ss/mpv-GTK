# Timeline hover previews

Hover over the playback timeline to see a small GTK card with a video frame
and the time under the pointer. The same card works on the Trim Mode timeline.
Previewing does not seek, pause, or otherwise change the playing video.

Toggle **Timeline hover previews** in **Settings → Interface**. The preference
is enabled by default, applies immediately, and is saved as `timeline-previews`
in `interface.conf`. Turning it off hides the card, cancels pending work, and
releases cached thumbnails.

## Prototype scope

Frames are available for local video files using the selected video stream.
FFmpeg must be installed. Network streams, missing tools, or failed extraction
fall back to the timestamp. Audio files and cover images do not show previews.
The card displays the source video rather than the player's video filters.

Images fit inside 256 × 144 pixels while preserving their display aspect ratio.
A short hover delay avoids unnecessary work when the pointer crosses the bar;
the card shows a spinner while a frame is being generated.

## Resource use

The implementation is native C and GTK. It creates the card and reads source
metadata on demand, and runs FFmpeg asynchronously through libmpv. Samples use
half-second buckets with a 120 ms debounce. Only one extraction can run at a
time; stale requests are cancelled and late replies cannot replace the current
preview. Leaving the timeline, hiding it, changing media, or closing the player
cancels pending work.

An in-memory LRU cache holds at most 48 thumbnails, about 7 MiB of decoded image
data. There is no disk thumbnail cache or startup scan. The preview remains
passive and does not take keyboard focus or hold fullscreen controls open.

The focused check `make test-seek-preview` uses generated red/blue video to
verify decoded frames, timeline mapping, cache reuse, cancellation, and the
persisted switch without changing user configuration or playback position.
