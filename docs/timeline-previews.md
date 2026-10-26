# Timeline hover previews

Hover over the playback timeline to see a small GTK card with a video frame
and the time under the pointer. The same card works on the Trim Mode timeline.
Previewing does not seek, pause, or otherwise change the playing video.
An invisible hover area extends 16 pixels above and below the timeline; the
visible bar keeps its size. The card's arrow points to the hovered position.

Toggle **Timeline hover previews** in **Settings → Interface**. The preference
is enabled by default, applies immediately, and is saved as `timeline-previews`
in `interface.conf`. Turning it off cancels frame decoding and releases cached
thumbnails. A compact timestamp bubble with the same arrow remains available.

## Prototype scope

Frames are available for local video files using the selected video stream.
A separate, headless libmpv decoder supplies frames. Network streams or failed
extraction fall back to the timestamp. Audio files and cover images show only
the timestamp.
The card displays the source video rather than the player's video filters.

Thumbnail pixels fit inside 160 × 90 while preserving their display aspect
ratio. GTK scales them into the existing 256 × 144 image area.
A short hover delay avoids unnecessary work when the pointer crosses the bar;
the card shows a spinner while a frame is being generated.

## Resource use

The implementation is native C and GTK. The card and decoder initialize on
first hover, without a startup scan. A dedicated worker owns a paused libmpv
instance that keeps the current video open and reuses its demuxer and decoder.
It uses null video/audio outputs, no scripts, and no extra video window.
When the playing video uses NVDEC, previews use `nvdec-copy` with automatic
software fallback. Otherwise they use software decoding without probing GPU
backends. Unsupported codecs still require decoding the source at its encoded
resolution before scaling; the small output alone cannot reduce that work.

Hover targets are coalesced every 16 ms. A quick keyframe seek produces an
initial image, followed by an exact seek after the pointer settles for about
150 ms. Frames arrive as raw BGRA pixels, without launching FFmpeg or encoding
and decoding PNG for each request. Main playback is independent of the worker.
New pointer targets reuse an in-flight load or fast seek instead of repeatedly
restarting it, then seek to the newest target when that work finishes.

The in-memory LRU cache holds at most 48 thumbnails, about 2.7 MiB of decoded
image data. Exact results replace provisional keyframe previews. There is no
disk thumbnail cache. The decoder stays warm briefly after leaving the timeline
and is released after 30 seconds of inactivity; disabling previews releases it
asynchronously and clears the cache. Source changes and newer requests reject
stale results. The card remains passive and does not take keyboard focus or
hold fullscreen controls open. Its native Wayland input region is empty,
including after resizes, so moving through the card cannot steal the pointer
from the timeline or trigger repeated popup/hide cycles. Leaving the timeline
hover area hides the card and rejects late frames. Nearby buttons, entries,
and other sliders keep their normal pointer behavior.

The focused check `make test-seek-preview` uses generated red/blue video to
verify decoded frames, timeline mapping, cache reuse, zero per-hover FFmpeg launches, cancellation, and the
persisted switch without changing user configuration or playback position.
