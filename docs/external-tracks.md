# External audio and subtitles

Open the subtitle menu and choose **Add subtitles…**, or open the audio-track
menu with the middle mouse button on the volume button and choose
**Add audio track…**. Both actions accept multiple files. Added tracks become
available in the normal track menu; the last successfully added track is selected.
Adding the same file again selects the existing track instead of duplicating it.

Dropping subtitle files onto playing media attaches them to that file, regardless
of the **Allow replace** and **Add video to queue** settings. The ordinary file
chooser also has a subtitle filter. Open media first when attaching subtitles
without a video or audio file in the same selection.

When media and subtitles are opened or dropped together, the subtitles belong to
the first media file in that selection. This also works when that file is appended
to the queue. Other queue entries retain their own subtitles. Audio files dropped
normally remain media entries; use **Add audio track…** to attach them instead.

The implementation uses stock mpv `sub-add` and `audio-add` with `cached`,
asynchronously and one at a time. Source changes cancel pending attachments;
file-picker results are rejected if their original media has changed.
Mixed media/subtitle selections use native per-file `loadfile` options, so mpv
keeps the subtitle paths with the correct queue entry and restores its settings
when that entry ends. No files are copied or modified.

## Precise seeking

Playback settings expose mpv's `hr-seek` modes: **Default**, **Disabled**,
**Absolute only**, and **Always**. Changes apply immediately and are saved in
`mpv.conf`. This sets mpv's default seeking precision. Commands that explicitly
request exact or keyframe seeking, including timeline scrubbing and Trim Mode
keyframe navigation, keep their requested behavior.
