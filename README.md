# mpv GTK

A GTK4 media player for native Wayland, powered by [mpv](https://mpv.io/).
Video and controls share one window, and the interface stays responsive
independently of the video's frame rate.

- Open local files and URLs, or drag files into the player.
- Edit the [queue](docs/queue.md) with search, drag reordering, sorting, shuffle, and repeat.
- Select chapters, attach [external audio and subtitles](docs/external-tracks.md), and adjust playback speed.
- Configure playback and mpv keybindings from the settings window.
- Switch to [Trim Mode](docs/trim-mode.md) for lossless video cuts.
- Preview local video frames on [timeline hover](docs/timeline-previews.md).
- View playback statistics and resume playback with mpv's watch-later support.
- Control playback from desktop media widgets and [playerctl through MPRIS](docs/mpris.md).
- English and Russian interface, with colors from your GTK theme.

See the [roadmap](ROADMAP.md) for planned features and implementation progress.

## Build and run

Requires Linux with Wayland, GTK 4.22+, mpv/libmpv, libepoxy, Clang (or another
C11 compiler), Make, pkg-config, Python 3, and `glib-compile-resources`.
Trim Mode additionally requires FFmpeg (`ffmpeg` and `ffprobe`).

```sh
git clone https://github.com/eml3ss/mpv-GTK.git
cd mpv-GTK
make -j
./build/mpv-gtk /path/to/video.mkv
```

The build uses your installed mpv and libmpv. Video is embedded through
libmpv's OpenGL/OpenGL ES API. See [backend details](docs/mpv-backend.md).
Clang is the default compiler; use `make CC=cc` to select another compiler.

Install for the current user:

```sh
make install
mpv-gtk /path/to/video.mkv
```

Use `make uninstall` to remove the application while keeping your settings.

## Configuration

Configuration files and scripts live in `~/.config/mpv-gtk/`
(`$XDG_CONFIG_HOME/mpv-gtk` when set), including `mpv.conf`, `input.conf`,
and `interface.conf`. Saved playback state and installed resources remain
in `~/.local/share/mpv-gtk/`.
The first launch moves legacy configs and scripts without overwriting existing
configuration files. The Misc settings section opens `mpv.conf` and `input.conf`.
`MPV_GTK_CONFIG_FILE` selects an alternative config file; related settings and
saved state then use its parent directory for isolated runs.

## Credits

Thanks to [KocTu4eK](https://github.com/KocTu4eK) for the idea behind Trim Mode.

## License

The application's original code is licensed under
[GPL-3.0-or-later](LICENSE). See [COPYRIGHT](COPYRIGHT) for the license grant
and third-party notices, including [mpv](tools/stock-mpv-licenses/Copyright)
and [Papirus icons](data/icons/NOTICE.txt), which retain their own licenses.
