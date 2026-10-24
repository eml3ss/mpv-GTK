# Trim Mode

Click the scissors button or press Ctrl+T to switch the player into Trim Mode.
Click or press it again to restore the playback controls. Video stays in the
same GTK/Wayland window. The interface uses the current GTK theme and accent.

The GTK frontend uses the adapted backend from the supplied ModernZ Trim Mode
plugin. Its boundary rules, keyframe navigation, lossless export, verification,
and file-recovery logic are retained. Source provenance and the backend's
LGPL-2.1-or-later license are in [data/scripts](../data/scripts/NOTICE.txt).

## Controls

The trim timeline highlights the selected interval and marks video keyframes.
Click or drag it to seek. The playback counter shows milliseconds and mpv's
estimated frame number.

| Action | Control or shortcut |
| --- | --- |
| Toggle Trim Mode | Scissors button / Ctrl+T |
| Set start at current position | Left boundary button / I |
| Set end at current position | Right boundary button / O |
| Previous / next keyframe | Key buttons / `[` and `]` |
| Play / pause | Center playback button / Space |
| Export a copy, keeping the original | Copy export button / Ctrl+E |
| Export and replace the original | Replace export button / Ctrl+Shift+E |

Edit the start/end fields as HH:MM:SS.mmm, MM:SS.mmm, or decimal seconds.
Enter commits a valid value and clears focus. Leaving the field also commits;
invalid values are discarded on blur. Escape cancels the edit. The start must
precede the end, and both boundaries must remain within the file duration.
User keybindings retain priority over the trim defaults. Trim shortcuts are
inactive while a time field is being edited or playback mode is selected.

In narrower windows, the boundary controls occupy their own centered row.
Fullscreen panel and cursor autohide remain available; editing or dragging
holds the controls while the interaction is active.

## Export

FFmpeg and ffprobe are required. Exports support local video files and use
FFmpeg stream copy, subject to the original codec/keyframe cutting behavior.
They keep the original extension, streams, metadata, and chapter mapping.

Copy export saves beside the source with the selected times in its name;
existing files are never overwritten. Replacement export writes and checks a
temporary result before replacing the source, with original-file identity
checks and backup/rollback handling. Opening the result preserves the prior
pause state. A native GTK progress card displays the current export phase.

## Configuration and loading

The backend loads when Trim Mode is first used; its widgets and keyframe scan
are also deferred. Keyframe scanning is asynchronous and caches recent files.
Markers are drawn from cached geometry, independently of video frame updates.

Options live in `script-opts/trim-mode.conf` beside the real executable.
Updates preserve this file. Available options are `remember`, `open-copy`,
`open-replace`, `ffmpeg_path`, and `ffprobe_path`. The defaults open exported
results and do not remember the mode. Enabling `remember` restores the saved
mode and loads its backend at startup. State stays in the player directory.
The system mpv plugin and configuration are unchanged.

The focused integration check, `make test-trim`, uses generated media and an
isolated configuration to exercise activation, keyframes, boundary validation,
copy/replacement export, reopening, and shutdown.
