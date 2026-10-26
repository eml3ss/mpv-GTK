#!/usr/bin/env bash
set -Eeuo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$project_dir"
if ! pkg-config --exists wlroots-0.20 cairo wayland-server pixman-1; then
    printf 'SKIP: wlroots 0.20 development files are unavailable.\n'
    exit 77
fi
read -r -a test_flags <<< "$(pkg-config --cflags --libs wlroots-0.20 cairo wayland-server pixman-1)"
mkdir -p build/headless-wayland-protocols
wayland-scanner server-header \
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
    build/headless-wayland-protocols/xdg-shell-protocol.h
"${CC:-clang}" -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
    -Ibuild/headless-wayland-protocols tests/headless_wayland.c \
    -o build/test-headless-wayland "${test_flags[@]}"
test_directory=$(mktemp -d "$project_dir/build/.headless-wayland.XXXXXXXX")
trap 'rm -rf -- "$test_directory"' EXIT
ffmpeg -nostdin -v error -f lavfi -i 'testsrc2=size=320x180:rate=24:duration=12' \
    -c:v mpeg4 -q:v 3 -y "$test_directory/video.mp4"
printf 'hwdec=no\nvo=libmpv\nosc=no\n' > "$test_directory/mpv.conf"
MPV_GTK_CONFIG_FILE="$test_directory/mpv.conf" \
    ./build/test-headless-wayland "${MPV_GTK_BINARY:-$project_dir/build/mpv-gtk}" \
    "$test_directory/video.mp4" "$project_dir/build/headless-wayland.png"
