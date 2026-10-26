# Roadmap

Feature IDs follow the agreed feature list. Checked tasks are implemented;
unchecked tasks remain planned. Existing mpv commands are noted separately
from unfinished interface work. Item 16 is tentative.

## 1. Playlist editor

- [x] Display the queue, select entries, and navigate between files.
- [x] Add files through opening or drag and drop; configure automatic playlists.
- [ ] Reorder entries by dragging, remove selected entries, and clear the queue.
- [ ] Add search, sorting, shuffle, and repeat controls for the whole queue.

## 2. Saved playlists and sessions

- [x] Open existing playlist files.
- [ ] Export the current queue to M3U.
- [ ] Restore the queue and selected file after restarting the player.

## 3. Playback history

- [x] Save and resume file positions using mpv's watch-later support.
- [ ] Browse and search recently opened files and URLs, with a continue-watching list.
- [ ] Remove individual history entries or clear the history.

## 4. External audio and subtitle tracks

- [x] Select and cycle audio and subtitle tracks already loaded by mpv.
- [ ] Attach external subtitle and audio files through the interface.
- [ ] Recognize dropped subtitle files as additions to the current video.

## 5. Desktop integration through MPRIS

- [x] Expose playback state, metadata, and controls through MPRIS.
- [x] Support desktop media controls, media keys routed by the desktop, and playerctl.
- [x] Supply video thumbnails and audio cover art up to 341 × 341, with a player-icon fallback, to desktop media widgets.

## 6. Prevent screen blanking during playback

- [ ] Inhibit automatic screen blanking and suspend while video is playing.
- [ ] Release inhibition when playback is paused or stopped, and when the player closes.

## 7. Timeline previews and precise seeking

- [x] Seek immediately while dragging the timeline; display chapter markers.
- [x] Show a frame thumbnail and timestamp on hover (local-video prototype).
- [x] Reuse a persistent native decoder with low-resolution keyframe previews and idle-time exact refinement.
- [x] Enable or disable previews in Interface settings, with immediate application and persistence.
- [ ] Add a dialog for seeking to a specified hh:mm:ss position.

## 8. Bookmarks

- [ ] Save named positions within a media file.
- [ ] Keep bookmarks between sessions and provide quick navigation and deletion.

## 9. A–B repeat controls

Existing engine support: mpv's A–B repeat command is available through keybindings.

- [ ] Set and clear the A and B points through the interface.
- [ ] Display the selected range and repeat state on the timeline.

## 10. Advanced subtitle settings

- [x] Adjust subtitle scale and delay from settings.
- [ ] Configure font, color, outline, position, and encoding.
- [ ] Configure preferred subtitle languages.
- [ ] Select a second subtitle track for simultaneous display.

## 13. Screenshot controls and destination

Existing engine support: screenshot commands and presets are available in the
keybinding editor.

- [ ] Capture screenshots through a menu action, with or without subtitles.
- [ ] Add a setting to choose the screenshot destination: clipboard or folder.
- [ ] Allow choosing the output folder and image format for saved screenshots.
- [ ] Provide actions to open the saved image or its folder.

## 14. Network playback settings

- [x] Open URLs and enable mpv's ytdl integration when its helper is available.
- [ ] Select online video quality and format through the interface.
- [ ] Provide buffering settings and clear recovery controls after connection failures.

## 16. Remember interface state — tentative

Under consideration; this feature may not be implemented. It is not a committed
implementation task. Queue restoration remains a separate task under item 2.

- [ ] Remember the window size.
- [ ] Remember the last opened directory.
- [ ] Restore queue-panel visibility and other selected display preferences.

## 17. Trim Mode

- [x] Switch between playback and native GTK trim controls with the scissors button or Ctrl+T.
- [x] Show the selected range, keyframe markers, millisecond time, and current frame number.
- [x] Animate both selection edges with the original 200 ms easing and smooth retargeting.
- [x] Keep the standard toolbar buttons aligned in both trim layouts.
- [x] Hide Trim Mode for audio files, including tracks with embedded cover art.
- [x] Edit start/end times and set either boundary from the playback position.
- [x] Navigate previous/next keyframes while playing or paused and preserve mode-specific shortcuts.
- [x] Export a separate copy or replace the original using the native lossless backend.
- [x] Choose which source audio tracks to include in copy and replacement exports, including silent exports.
- [x] Preserve export verification, backup/rollback, result opening, and playback state.
- [x] Show native GTK export progress; initialize the backend and trim controls on demand.
- [x] Implement all Trim Mode logic in C, including keyframes, validation, export, and recovery.
