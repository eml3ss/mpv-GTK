#!/usr/bin/env bash
# Compatibility entry point: the player now uses the installed stock engine.
set -Eeuo pipefail
stock_tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec bash "$stock_tools_dir/stage-stock-mpv.sh" "$@"
