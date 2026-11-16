The player uses the installed distribution build of libmpv and its standard
OpenGL Render API. Video is rendered into GtkGLArea inside the same native
Wayland window as the interface, using standard libmpv widget integration.
The embedding fixes `vo=libmpv`; it does not open a native mpv video window.
No custom Render API, renderer patch, Vulkan backend, or CPU frame transport is
used. The ordinary embedded API supports OpenGL/OpenGL ES; native gpu/gpu-next
and Vulkan are not presented as supported video outputs inside this widget.

The player links directly against the installed `libmpv.so.2` found through
pkg-config. Nothing is copied or pinned, so mpv, FFmpeg and libplacebo updates
from the distribution apply without rebuilding the player.

The included mpv copyright and GPL/LGPL notices are in
`tools/stock-mpv-licenses/` and are installed with the application.
Upstream sources are available at [mpv-player/mpv](https://github.com/mpv-player/mpv);
the installed distribution package may also contain its normal packaging
patches. The application does not add any patches to that engine.

For optional source inspection or development only:

```sh
bash tools/fetch-renderer-sources.sh
```

That command downloads the official mpv 0.41.0 source tag, pinned to commit
`41f6a645068483470267271e1d09966ca3b9f413`, into `vendor/mpv` and records it in
`vendor/renderer-sources.json`. It requires Git and does not download a build
system. These optional upstream sources are unnecessary for the normal
application build.

The retired 0.10 custom Render API patch, changed source files, and original
artifacts were preserved in a read-only snapshot under `build/rollback-0.10/`.
They are outside the active build and are not applied.
