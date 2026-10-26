# Queue editor

Open the queue with its toolbar button or Ctrl+P. Click an entry to select it;
double-click it or press Enter to play it. Ctrl-click adds individual entries
to the selection, Shift-click selects a range, and Ctrl+A selects the visible
entries. Delete or the remove button removes the selected entries from the
queue. Files on disk are unchanged.

Drag an entry by its handle to move it before or after another entry. The
insertion line shows its destination. Search matches titles and paths without
changing the underlying queue. Dragging or removing filtered results still
targets the original entries, including when the same file occurs more than
once.

The sorting menu orders the whole queue by title or path in either direction,
with natural numeric ordering. Shuffle randomizes the whole queue once.
Repeat loops the whole queue using mpv's playlist repeat setting. Reordering,
sorting, and shuffling keep the current file and playback position.

Clear stops playback and empties the entire queue. Removing the currently
playing entry lets mpv advance to the next one. Queue editing controls are
temporarily disabled while an operation finishes.

The editor and its playlist subscription are created when the panel opens;
closing the panel releases its widgets. Run `make test-queue-model` for the
native queue regression checks with generated media and null outputs.
`make test-queue-view` checks search, selection, keyboard editing, and drag/drop
through GTK without presenting a window.
