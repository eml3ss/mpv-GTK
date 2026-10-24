# Stock libmpv in a single GTK window

The player uses the official C API of an unmodified `libmpv.so.2`.
Copies of the installed library and mpv executable are staged beside the
application. Their versions, source paths, and SHA-256 hashes are recorded in
`stock-mpv.json`. The `$ORIGIN` RUNPATH selects the staged library. The system
installation of mpv is unchanged.

## Video integration

The embedded output uses `vo=libmpv`. The standard OpenGL Render API draws video
into a `GtkGLArea` framebuffer, with GTK providing the interface. Video and
controls belong to a single native Wayland window. This integration uses the
stock renderer without mpv patches, CPU frame transfers, or a separate video
window.

The application enforces the embedded VO after configuration loading,
including includes and profiles. The standalone `gpu` and `gpu-next` outputs
are outside this Render API integration. Values in the configuration file are
preserved; selecting another VO there cannot create a second video window.

## Startup and configuration

Initial media loading starts before GTK initialization. mpv reads the file and
discovers tracks while the interface is being prepared. The `on_preloaded` hook
holds video output initialization until GTK has created the shared GL context
and Render API. The application releases the hook and presents the window once
the media has loaded.

Because `config=no` disables mpv's automatic configuration-directory lookup,
the application explicitly supplies paths for watch-later state and the GPU
shader cache. User-specified paths in `mpv.conf` take precedence.

Example configuration beside the executable:

```ini
vo=libmpv
gpu-api=opengl
opengl-es=auto
hwdec=auto-safe
```

The settings offer Auto, OpenGL, and OpenGL ES. Vulkan is disabled because the
standard mpv 0.41 Render API used here does not expose it. The GPU API and
OpenGL ES options are saved atomically to `mpv.conf` and take effect after a
restart. Settings that support live changes apply immediately; restart-only
settings are identified in the interface.

Legacy renderer options from version 0.10 are migrated from `interface.conf`.
Explicit mpv options take precedence, and mpv handles includes and profiles.
Language preferences, cursor delay, custom keybindings, and other settings are
preserved.

## Controls and timing

In windowed mode, the controls sit below the video. In fullscreen, they appear
as an overlay and hide automatically. A left click on the information button
shows playback statistics over the video; a right click opens a separate
statistics window. GTK manages fullscreen state and cursor visibility.

The UI frame clock runs independently of video playback. Rendering uses
`MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME=0` so it does not wait for a video's
target presentation time. `video-timing-offset=0` avoids extra video lead time
before GTK presents the frame.

## Validation

The test suite covers real mpv options, includes, profiles, events, keybindings,
settings, and rendered GL/GLES pixels. GUI checks are skipped when a Wayland
display is unavailable. A separate headless check requires exactly one
`xdg_toplevel`, actual video pixels, and a PNG capture. It returns exit code 77
if the environment cannot run its compositor.

See the [mpv libmpv documentation](https://mpv.io/manual/stable/#embedding-into-other-programs-libmpv).
