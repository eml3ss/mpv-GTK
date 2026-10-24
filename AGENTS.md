# Project instructions

## Communication and documentation

- Follow the user's language in conversation.
- Write documentation, code comments, diagnostics, and commit messages in
  English. Preserve the player's localization and its test expectations.
- Keep README.md short: purpose, main features, build/install instructions,
  configuration paths, and license. Put detailed technical information in docs/.

## Git workflow

- The user authorizes committing completed changes and pushing them to this
  repository. Use the configured remote and the current branch's upstream.
- Review the working tree and staged diff before committing. Include only the
  current task's changes; preserve unrelated user changes.
- Use concise English commit messages that describe the resulting change.
- Preserve existing history. Do not force-push, amend published commits, or
  change branches without a task-specific reason.
- If pushing fails, keep the local commit and report the actual failure.
  Never claim that a local commit was published.

## Application architecture

- Use native Wayland. Never recommend X11 or XWayland, including as a fallback
  or diagnostic step.
- Keep the video and interface inside one GTK window using the standard
  libmpv OpenGL/OpenGL ES Render API. Preserve the embedded vo=libmpv output.
- Keep UI updates independent of video frame rate and playback state.
- When launching with a file, show the window with loaded media. Preserve
  this behavior when optimizing startup.
- Preserve GTK theme colors and accents. Use the bundled icons and GTK's
  built-in resources; avoid indexing system icon themes during startup.
- Keep settings pages, keybinding catalogs, and unused translations lazy.

## Configuration and installation

- Keep mpv.conf, input.conf, and interface.conf beside the real executable.
  The default installed directory is ~/.local/share/mpv-gtk/.
- Preserve existing configs, custom bindings, comments, profiles, and file
  permissions during changes and installation. Never replace live configs
  with repository defaults or an older backup.
- For application changes, build and install the finished version with make
  and make install. Documentation-only changes do not require a rebuild.
- The user prefers to test playback manually. Avoid lengthy test runs and
  requests for user-run tests or diagnostic logs unless explicitly requested.
  Report only validation actually performed.
- Do not stop or restart a running player without the user's instruction.

## Licensing and repository contents

- Original project code is GPL-3.0-or-later; see LICENSE and COPYRIGHT.
- Preserve third-party licenses and attribution, especially mpv and Papirus.
  Do not apply the project's license grant to third-party files.
- Keep generated build artifacts, personal configs, media, and backups out
  of commits.
