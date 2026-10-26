# Queue editor

Open the queue with its toolbar button or Ctrl+P. Click an entry or press
Enter to play it. An ordinary click replaces the selection, which then follows
playback changes. Ctrl-click adds individual entries to the selection,
Shift-click selects a range, and Ctrl+A selects the visible entries. This
explicit selection stays in place while playback changes. Delete or the remove
button removes the selected entries from the queue. Files on disk are unchanged.

Drag an entry by its handle to move it before or after another entry. The grab
area extends around the small grip, and neighboring entries animate aside to
open the insertion slot. The insertion line shows its destination. Search
matches titles and paths without changing the underlying queue. Dragging or
removing filtered results still
targets the original entries, including when the same file occurs more than
once.

The sorting menu orders the whole queue by title, path, creation date, or
modification date in either direction. Titles and paths use natural numeric
ordering. Dates are read on demand for local files; entries without the
requested date, including network URLs, stay after dated entries in both
directions. Creation sorting uses the file's actual birth time when available.
Shuffle randomizes the whole queue once.
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
