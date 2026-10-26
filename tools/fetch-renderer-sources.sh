#!/usr/bin/env bash
# Fetch optional upstream sources only; never build or install the player/mpv.
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: tools/fetch-renderer-sources.sh [--fetch-only]

Fetch the pinned official mpv 0.41.0 sources into this project's vendor/
directory. No system packages, installed player, configuration files, or
running processes are changed.
The default operation is --fetch-only; no compilation or installation runs.
This is optional source inspection/development; the player uses stock mpv
and its normal build does not require source downloads.
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

for renderer_tool in git mktemp; do
    if ! command -v "$renderer_tool" >/dev/null 2>&1; then
        printf 'Required command is missing: %s\n' "$renderer_tool" >&2
        exit 1
    fi
done

renderer_tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
renderer_project_dir=$(cd -- "$renderer_tools_dir/.." && pwd -P)
renderer_vendor_dir="$renderer_project_dir/vendor"
renderer_mpv_dir="$renderer_vendor_dir/mpv"

# Pinned to the full commit linked by the official v0.41.0 release page:
# https://github.com/mpv-player/mpv/releases/tag/v0.41.0
renderer_mpv_commit=41f6a645068483470267271e1d09966ca3b9f413
renderer_mpv_origin=https://github.com/mpv-player/mpv.git
renderer_mpv_tag=v0.41.0

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

cat > "$renderer_stage/renderer-sources.json" <<EOF
{
  "mpv": {
    "version": "0.41.0",
    "commit": "$renderer_mpv_commit",
    "origin": "$renderer_mpv_origin",
    "path": "mpv"
  }
}
EOF
mv -T -- "$renderer_stage/renderer-sources.json" "$renderer_vendor_dir/renderer-sources.json"

printf '\nSources ready:\n  mpv: %s\n' "$renderer_mpv_dir"
printf 'Only sources were fetched; the installed player and its configuration are unchanged.\n'
