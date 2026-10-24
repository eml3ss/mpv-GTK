# mpv GTK

A GTK4 media player for native Wayland, powered by [mpv](https://mpv.io/).
Video and controls share one window, and the interface stays responsive
independently of the video's frame rate.

- Open local files and URLs, or drag files into the player.
- Manage playlists, chapters, audio tracks, subtitles, and playback speed.
- Configure playback and mpv keybindings from the settings window.
- Switch to [Trim Mode](docs/trim-mode.md) for lossless video cuts.
- View playback statistics and resume playback with mpv's watch-later support.
- English and Russian interface, with colors from your GTK theme.

See the [roadmap](ROADMAP.md) for planned features and implementation progress.

## Build and run

Requires Linux with Wayland, GTK 4.22+, mpv/libmpv, libepoxy, a C11 compiler,
Make, pkg-config, Python 3, and `glib-compile-resources`.
Trim Mode additionally requires FFmpeg (`ffmpeg` and `ffprobe`).

```sh
git clone https://github.com/eml3ss/mpv-GTK.git
cd mpv-GTK
make -j
./build/mpv-gtk /path/to/video.mkv
```

The build uses your installed mpv and libmpv. Video is embedded through
libmpv's OpenGL/OpenGL ES API. See [backend details](docs/mpv-backend.md).

Install for the current user:

```sh
make install
mpv-gtk /path/to/video.mkv
```

Use `make uninstall` to remove the application while keeping your settings.

## Configuration

`mpv.conf`, `input.conf`, and `interface.conf` live beside the executable:
in `build/` when running locally, or `~/.local/share/mpv-gtk/` after installation.
Updates preserve existing settings. The settings window includes buttons to
open `mpv.conf` and `input.conf`.

## License

The application's original code is licensed under
[GPL-3.0-or-later](LICENSE). See [COPYRIGHT](COPYRIGHT) for the license grant
and third-party notices, including [mpv](tools/stock-mpv-licenses/Copyright)
and [Papirus icons](data/icons/NOTICE.txt), which retain their own licenses.
