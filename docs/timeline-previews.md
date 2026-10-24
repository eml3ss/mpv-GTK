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
A separate, headless libmpv decoder supplies frames. Network streams or failed
extraction fall back to the timestamp. Audio files and cover images do not show previews.
The card displays the source video rather than the player's video filters.

Images fit inside 256 × 144 pixels while preserving their display aspect ratio.
A short hover delay avoids unnecessary work when the pointer crosses the bar;
the card shows a spinner while a frame is being generated.

## Resource use

The implementation is native C and GTK. The card and decoder initialize on
first hover, without a startup scan. A dedicated worker owns a paused libmpv
instance that keeps the current video open and reuses its demuxer and decoder.
It uses null video/audio outputs, no scripts, and no extra video window.

Hover targets are coalesced every 50 ms. A quick keyframe seek produces an
initial image, followed by an exact seek after the pointer settles for about
150 ms. Frames arrive as raw BGRA pixels, without launching FFmpeg or encoding
and decoding PNG for each request. Main playback is independent of the worker.

The in-memory LRU cache holds at most 48 thumbnails, about 7 MiB of decoded
image data. Exact results replace provisional keyframe previews. There is no
disk thumbnail cache. The decoder stays warm briefly after leaving the timeline
and is released after 30 seconds of inactivity; disabling previews releases it
asynchronously and clears the cache. Source changes and newer requests reject
stale results. The card remains passive and does not take keyboard focus or
hold fullscreen controls open.

The focused check `make test-seek-preview` uses generated red/blue video to
verify decoded frames, timeline mapping, cache reuse, zero per-hover FFmpeg launches, cancellation, and the
persisted switch without changing user configuration or playback position.
