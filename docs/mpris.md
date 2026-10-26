# Desktop media controls

mpv GTK exposes [MPRIS 2](https://specifications.freedesktop.org/mpris/latest/)
on the user's D-Bus session. Desktop media widgets and tools such as playerctl
can control the player without focusing its window.

Supported controls include play/pause, stop, previous/next file, seeking,
volume, speed, repeat, opening a URI, raising the window, and fullscreen.
Playback status, position, duration, title, artist, album, and the current
file's URI follow the playing media. Controls reflect whether media is loaded,
seeking is available, and adjacent queue entries exist.
Desktop widgets also receive a thumbnail for local videos or the embedded
cover for audio files. The player icon appears while artwork is loading or
when an image is unavailable.

For example, with playerctl installed:

```sh
playerctl --list-all
playerctl --player=mpv_gtk play-pause
playerctl --player=mpv_gtk next
playerctl --player=mpv_gtk position 30
playerctl --player=mpv_gtk volume 0.5
playerctl --player=mpv_gtk metadata
```

Each window runs in its own process and publishes a separate name:
`org.mpris.MediaPlayer2.mpv_gtk.instance<PID>`. To select a particular window,
use its full player name from `playerctl --list-all`.

Desktop-wide media keys use the desktop's MPRIS routing. When a media key is
delivered directly to the focused player, Play/Pause, Stop, Previous, and Next
also work through the regular keybinding system. Custom bindings retain
priority. The player does not install global keyboard grabs.

## Behavior and resource use

D-Bus connection and registration start asynchronously after the initial
window is presented with loaded media. MPRIS does not delay video startup or
create another window. State changes use mpv property notifications; position
queries do not cause a stream of D-Bus position-change signals. Artwork is
prepared asynchronously after presentation, using a small native decode of
the current local file; it does not scan the library or fetch network images.

Stop keeps the queue so Play can restart the stopped item from its beginning.
MPRIS OpenUri is an explicit playback request and starts the supplied URI;
ordinary file opening retains its configured append/replace behavior.
Closing the player releases its MPRIS name without waiting for D-Bus replies.
If no session bus is available, normal playback continues.

The standard root and player interfaces live at `/org/mpris/MediaPlayer2`.
The optional MPRIS TrackList and Playlists interfaces are not exposed.

## Validation

`make test-mpris` exercises the service on a private D-Bus session with generated
media and null video/audio outputs. It verifies remote commands, metadata,
property changes, seeking, stable track IDs, stop/restart behavior, and cleanup
without controlling other desktop players. FFmpeg and dbus-daemon are needed
for this check. `make test-mpris-art` additionally verifies exported thumbnail
and cover files. The application uses its existing GLib/GIO and GdkPixbuf
dependencies.
