#!/usr/bin/env bash
# Fetch build inputs only. This script never builds or installs the player/mpv.
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: tools/fetch-renderer-sources.sh [--fetch-only]

Fetch the pinned official mpv 0.41.0 sources and a private Meson 1.9.1
bootstrap into this project's vendor/ directory. No system packages,
installed player, configuration files, or running processes are changed.
The default operation is --fetch-only; no compilation or installation runs.
This is optional source inspection/development; the player uses stock mpv
and its normal build does not require source downloads or Meson.
EOF
}

case "${1:---fetch-only}" in
    --help|-h) usage; exit 0 ;;
    --fetch-only) ;;
    *) usage >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    usage >&2
    exit 2
fi

for renderer_tool in git curl python3 mktemp; do
    if ! command -v "$renderer_tool" >/dev/null 2>&1; then
        printf 'Required command is missing: %s\n' "$renderer_tool" >&2
        exit 1
    fi
done

renderer_tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
renderer_project_dir=$(cd -- "$renderer_tools_dir/.." && pwd -P)
renderer_vendor_dir="$renderer_project_dir/vendor"
renderer_mpv_dir="$renderer_vendor_dir/mpv"
renderer_meson_dir="$renderer_vendor_dir/meson"

# Pinned to the full commit linked by the official v0.41.0 release page:
# https://github.com/mpv-player/mpv/releases/tag/v0.41.0
renderer_mpv_commit=41f6a645068483470267271e1d09966ca3b9f413
renderer_mpv_origin=https://github.com/mpv-player/mpv.git
renderer_mpv_tag=v0.41.0

# Digest and wheel URL from https://pypi.org/pypi/meson/1.9.1/json
renderer_meson_version=1.9.1
renderer_meson_sha256=f824ab770c041a202f532f69e114c971918ed2daff7ea56583d80642564598d0
renderer_meson_url=https://files.pythonhosted.org/packages/9c/07/b48592d325cb86682829f05216e4efb2dc881762b8f1bafb48b57442307a/meson-1.9.1-py3-none-any.whl

mkdir -p -- "$renderer_vendor_dir"
renderer_stage=$(mktemp -d "$renderer_vendor_dir/.fetch-renderer-sources.XXXXXXXX")
trap 'rm -rf -- "$renderer_stage"' EXIT

if [[ -e "$renderer_mpv_dir" || -L "$renderer_mpv_dir" ]]; then
    if [[ -L "$renderer_mpv_dir" || ! -d "$renderer_mpv_dir/.git" ]]; then
        printf 'Refusing to replace an existing source path: %s\n' "$renderer_mpv_dir" >&2
        exit 1
    fi
    renderer_actual_commit=$(git -C "$renderer_mpv_dir" rev-parse HEAD)
    renderer_actual_origin=$(git -C "$renderer_mpv_dir" remote get-url origin)
    if [[ "$renderer_actual_commit" != "$renderer_mpv_commit" ||
          "$renderer_actual_origin" != "$renderer_mpv_origin" ]]; then
        printf 'Existing mpv checkout does not match the pinned source; left untouched: %s\n' \
            "$renderer_mpv_dir" >&2
        exit 1
    fi
    printf 'Using pinned mpv checkout: %s\n' "$renderer_mpv_commit"
else
    printf 'Fetching official mpv %s sources…\n' "$renderer_mpv_tag"
    git init --quiet "$renderer_stage/mpv"
    git -C "$renderer_stage/mpv" remote add origin "$renderer_mpv_origin"
    if ! git -C "$renderer_stage/mpv" -c protocol.version=2 fetch --depth=1 --no-tags \
        origin "refs/tags/$renderer_mpv_tag:refs/tags/$renderer_mpv_tag"; then
        printf 'Source download failed. Run this same command in a terminal with network access.\n' >&2
        exit 1
    fi
    renderer_actual_commit=$(git -C "$renderer_stage/mpv" rev-parse "refs/tags/$renderer_mpv_tag^{commit}")
    if [[ "$renderer_actual_commit" != "$renderer_mpv_commit" ]]; then
        printf 'mpv tag verification failed: expected %s, received %s\n' \
            "$renderer_mpv_commit" "$renderer_actual_commit" >&2
        exit 1
    fi
    git -C "$renderer_stage/mpv" -c advice.detachedHead=false checkout --quiet --detach "$renderer_mpv_commit"
    mv -- "$renderer_stage/mpv" "$renderer_mpv_dir"
fi

if [[ -e "$renderer_meson_dir" || -L "$renderer_meson_dir" ]]; then
    if [[ -L "$renderer_meson_dir" || ! -f "$renderer_meson_dir/.source-sha256" ||
          ! -f "$renderer_meson_dir/mesonbuild/mesonmain.py" ||
          "$(cat -- "$renderer_meson_dir/.source-sha256")" != "$renderer_meson_sha256" ]]; then
        printf 'Refusing to replace an unrecognized Meson bootstrap: %s\n' "$renderer_meson_dir" >&2
        exit 1
    fi
    printf 'Using private Meson bootstrap: %s\n' "$renderer_meson_version"
else
    printf 'Fetching official Meson %s bootstrap…\n' "$renderer_meson_version"
    if ! curl --proto '=https' --tlsv1.2 --location --fail --show-error \
        --connect-timeout 20 --max-time 180 --retry 2 \
        "$renderer_meson_url" --output "$renderer_stage/meson.whl"; then
        printf 'Meson download failed. Run this same command again with network access.\n' >&2
        exit 1
    fi
    python3 - "$renderer_stage/meson.whl" "$renderer_stage/meson" "$renderer_meson_sha256" <<'PY'
import hashlib
from pathlib import Path, PurePosixPath
import stat
import sys
import zipfile

archive, destination, expected = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
actual = hashlib.sha256(archive.read_bytes()).hexdigest()
if actual != expected:
    raise SystemExit(f"Meson SHA256 verification failed: expected {expected}, received {actual}")
with zipfile.ZipFile(archive) as wheel:
    for member in wheel.infolist():
        path = PurePosixPath(member.filename)
        mode = member.external_attr >> 16
        if path.is_absolute() or ".." in path.parts or stat.S_ISLNK(mode):
            raise SystemExit(f"Unexpected path in Meson archive: {member.filename}")
    wheel.extractall(destination)
(destination / ".source-sha256").write_text(expected + "\n", encoding="ascii")
PY
    mv -- "$renderer_stage/meson" "$renderer_meson_dir"
fi

python3 - "$renderer_vendor_dir" "$renderer_mpv_commit" "$renderer_meson_version" "$renderer_meson_sha256" <<'PY'
import json
from pathlib import Path
import sys

vendor = Path(sys.argv[1])
manifest = {
    "mpv": {
        "version": "0.41.0",
        "commit": sys.argv[2],
        "origin": "https://github.com/mpv-player/mpv.git",
        "path": "mpv",
    },
    "meson": {
        "version": sys.argv[3],
        "sha256": sys.argv[4],
        "source": "https://pypi.org/pypi/meson/1.9.1/json",
        "path": "meson",
    },
}
staged = vendor / ".renderer-sources.json.tmp"
staged.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
staged.replace(vendor / "renderer-sources.json")
PY

printf '\nSources ready:\n  mpv:   %s\n  Meson: %s\n' "$renderer_mpv_dir" "$renderer_meson_dir"
printf 'Only sources were fetched; the installed player and its configuration are unchanged.\n'
