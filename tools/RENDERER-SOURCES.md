The player uses the installed distribution build of libmpv and its standard
OpenGL Render API. Video is rendered into GtkGLArea inside the same native
Wayland window as the interface, using standard libmpv widget integration.
The embedding fixes `vo=libmpv`; it does not open a native mpv video window.
No custom Render API, renderer patch, Vulkan backend, or CPU frame transport is
used. The ordinary embedded API supports OpenGL/OpenGL ES; native gpu/gpu-next
and Vulkan are not presented as supported video outputs inside this widget.

Stage the engine beside the player with:

```sh
bash tools/stage-stock-mpv.sh
```

This copies the installed `libmpv.so.2` found through pkg-config and the installed
`mpv` executable into `build/libmpv.so.2` and `build/mpv-engine`. The copies are
byte-for-byte identical to their resolved system files. The script records
their paths, SHA256 digests, package version and client API version in
`build/stock-mpv.json`, and the full version output in
`build/stock-mpv-version.txt`. They continue to use the installed runtime
dependencies. No system files, player configuration or running processes are
changed. `tools/build-private-mpv.sh` is a compatibility wrapper for this staging
command; it no longer builds or applies patches.

The included mpv copyright and GPL/LGPL notices are in
`tools/stock-mpv-licenses/` and are staged into `build/stock-mpv-licenses/`.
Upstream sources are available at [mpv-player/mpv](https://github.com/mpv-player/mpv);
the installed distribution package may also contain its normal packaging
patches. The application does not add any patches to that engine.

For optional source inspection or development only:

```sh
bash tools/fetch-renderer-sources.sh
```

That command downloads the official mpv 0.41.0 source tag, pinned to commit
`41f6a645068483470267271e1d09966ca3b9f413`, and an uninstalled Meson 1.9.1 Python
bootstrap. Source downloads and Meson are unnecessary for the normal application
build. The vendor checkout has been restored to the unmodified official release.

The retired 0.10 custom Render API patch, changed source files, and original
artifacts were preserved in a read-only snapshot under `build/rollback-0.10/`.
They are outside the active build and are not applied by the staging command.
