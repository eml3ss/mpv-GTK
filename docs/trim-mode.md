# Trim Mode

Click the scissors button or press Ctrl+T to switch the player into Trim Mode.
Click or press it again to restore the playback controls. Video stays in the
same GTK/Wayland window. The interface uses the current GTK theme and accent.

Trim Mode is implemented entirely in C and does not load a Lua script.
Its button is hidden for audio files, including files with embedded cover art.
Audio playback keeps the regular controls even if Trim Mode was active for
the preceding video; returning to video restores that mode.

## Controls

The trim timeline highlights the selected interval and marks video keyframes.
Click or drag it to seek. The playback counter shows milliseconds and mpv's
estimated frame number.
During a timeline drag, video follows the selected position and stays on that
frame while the pointer is held still. Audio stays silent. Releasing the
pointer restores the previous playing/paused state at the final position;
mute, volume, and the selected audio track are preserved.
Both selection edges animate over 200 ms with cubic ease-out, matching the
original script. Repeated edits continue smoothly from the current drawing.

| Action | Control or shortcut |
| --- | --- |
| Toggle Trim Mode | Scissors button / Ctrl+T |
| Set start at current position | Left boundary button / I |
| Set end at current position | Right boundary button / O |
| Previous / next keyframe | Key buttons / `[` and `]` |
| Play / pause | Center playback button / Space |
| Choose exported audio tracks | Audio selection button beside the export buttons |
| Export a copy, keeping the original | Copy export button / Ctrl+E |
| Export and replace the original | Replace export button / Ctrl+Shift+E |

Repeated keyframe clicks continue from the last selected keyframe while
playback advances within that interval. Seeking elsewhere or playing past
the next keyframe makes navigation follow the current position again.
Rapid clicks update the pending target while the current seek finishes,
instead of adding separate decoder seeks for every click.

Edit the start/end fields as HH:MM:SS.mmm, MM:SS.mmm, or decimal seconds.
Enter commits a valid value and clears focus. Leaving the field also commits;
invalid values are discarded on blur. Escape cancels the edit. The start must
precede the end, and both boundaries must remain within the file duration.
User keybindings retain priority over the trim defaults. Trim shortcuts are
inactive while a time field is being edited or playback mode is selected.
Existing Trim Mode message and binding names are handled by a native mpv
client for compatibility.

In narrower windows, the boundary controls occupy their own centered row.
The remaining toolbar controls keep their normal alignment.
Fullscreen panel and cursor autohide remain available; editing or dragging
holds the controls while the interaction is active.

## Export

FFmpeg and ffprobe are required. Exports support local video files and use
FFmpeg stream copy, subject to the original codec/keyframe cutting behavior.
They keep the original extension, metadata, chapter mapping, and source streams
except audio tracks explicitly excluded from export.
MP4 audio track names remain readable by mpv after export, including when
only a subset of the original tracks is kept. The completed temporary file's
audio title atoms are normalized before verification; source files are not
patched during copy exports.

The audio selection button opens a compact menu above it, with checkboxes for
the source file's audio tracks, with titles, language, and codec when available.
All tracks are included initially. Select any subset, use **All** to restore
them, or choose
**No audio** for a silent result. The button uses the theme accent when the
selection differs from all tracks. This does not change the audio being played.
The selection applies to both export buttons and their shortcuts, is frozen
when export starts, and resets to all tracks when a different file loads.
External audio files attached to playback are not part of the source export.

Copy export saves beside the source with the selected times in its name;
existing files are never overwritten. Replacement export writes and checks a
temporary result before replacing the source, with original-file identity
checks and backup/rollback handling. Opening the result preserves the prior
pause state. A native GTK progress card displays the current export phase.

## Configuration and loading

The native backend initializes when Trim Mode is first used; its widgets and
keyframe scan are also deferred. Keyframe scanning is asynchronous and caches recent files.
Markers are drawn from cached geometry, independently of video frame updates.

Options live in `~/.config/mpv-gtk/script-opts/trim-mode.conf`
(`$XDG_CONFIG_HOME/mpv-gtk/script-opts/trim-mode.conf` when set).
Updates preserve this file. Available options are `remember`, `open-copy`,
`open-replace`, `ffmpeg_path`, and `ffprobe_path`. The defaults open exported
results and do not remember the mode. Enabling `remember` restores the saved
mode and initializes its backend at startup. Runtime state stays in
`~/.local/share/mpv-gtk/trim-mode-state.json`
(`$XDG_DATA_HOME/mpv-gtk/trim-mode-state.json` when set). An explicit
`MPV_GTK_CONFIG_FILE` keeps both options and state under that config file's
parent directory for isolated runs.
The system mpv plugin and configuration are unchanged.

The focused integration check, `make test-trim`, uses generated media and an
isolated configuration to exercise activation, keyframes, boundary validation,
selected/all/silent audio exports, reopening, and shutdown with all Lua scripts disabled.
It also checks filename collisions, tool failures, concurrent source changes,
and cancellation during export. Its native check deliberately fails opening a
replacement and verifies that rollback restores the original bytes, permissions,
playback position, and pause state. Run `make test-trim-native` to exercise that
recovery path without a graphical display.
`make test-trim-titles` checks MP4 audio names through FFmpeg and libmpv for
all-track, selected-track, silent, and replacement exports without a display.

`make test-trim-keyframes` checks repeated keyframe navigation during timed
playback, rapid clicks, and seek/source resets without a graphical display.
It includes large input bursts and verifies that the main context remains
responsive. `make test-trim-keyframe-render` additionally checks overlapping
seeks against a real libmpv software render context, without opening a window.
