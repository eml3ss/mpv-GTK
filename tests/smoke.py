#!/usr/bin/env python3
"""Exercise real GTK/libmpv playback; exit 77 when no display is accessible.

Usage: python3 tests/smoke.py [path/to/mpv-gtk]
Requires ffmpeg, GTK 4, and a running Wayland session. Test clips are
generated in a temporary directory and removed after the run.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


SECONDS = 8
MIN_UI_HZ = 20
MAX_TICK_GAP_MS = 500


def accessible_display() -> bool:
    """Use GTK's own backend selection, including any GDK_BACKEND override."""
    library = ctypes.util.find_library("gtk-4")
    if not library:
        raise RuntimeError("GTK 4 shared library was not found")
    gtk = ctypes.CDLL(library)
    gtk.gdk_set_allowed_backends.argtypes = [ctypes.c_char_p]
    gtk.gdk_set_allowed_backends.restype = None
    gtk.gdk_set_allowed_backends(b"wayland")
    gtk.gtk_init_check.argtypes = []
    gtk.gtk_init_check.restype = ctypes.c_int
    return bool(gtk.gtk_init_check())


def generate_clip(ffmpeg: str, target: Path, fps: int) -> None:
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", f"testsrc2=size=320x180:rate={fps}:duration=12",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=12",
        "-c:v", "mpeg4", "-q:v", "4", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "64k", "-shortest", str(target),
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=45)
    if result.returncode:
        raise RuntimeError(f"ffmpeg could not generate {fps} fps clip:\n{result.stderr}")


def number(metrics: dict, key: str, *, positive: bool = False) -> float:
    value = metrics.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise RuntimeError(f"missing or nonnumeric metric {key!r}: {value!r}")
    if not math.isfinite(value) or value < 0 or (positive and value <= 0):
        raise RuntimeError(f"invalid metric {key!r}: {value!r}")
    return float(value)


def check_metrics(metrics: dict, fps: int) -> dict:
    if metrics.get("loaded") is not True:
        raise RuntimeError("test clip did not load")
    if metrics.get("seek_ok") is not True:
        raise RuntimeError("asynchronous seek did not complete while paused")
    if number(metrics, "error_count") != 0:
        raise RuntimeError(f"player reported {metrics['error_count']} errors")

    play_seconds = number(metrics, "play_seconds", positive=True)
    paused_seconds = number(metrics, "paused_seconds", positive=True)
    if play_seconds < SECONDS * 0.35 or paused_seconds < SECONDS * 0.25:
        raise RuntimeError("self-test did not exercise enough playback and pause time")

    play_ticks = number(metrics, "play_ticks", positive=True)
    paused_ticks = number(metrics, "paused_ticks", positive=True)
    ui_ticks = number(metrics, "ui_ticks", positive=True)
    if ui_ticks < play_ticks + paused_ticks:
        raise RuntimeError("UI tick total is smaller than its phase counts")
    play_hz = play_ticks / play_seconds
    pause_hz = paused_ticks / paused_seconds
    if min(play_hz, pause_hz) < MIN_UI_HZ:
        raise RuntimeError(
            f"GTK updates too slowly: playing {play_hz:.1f} Hz, paused {pause_hz:.1f} Hz "
            f"(minimum {MIN_UI_HZ} Hz)"
        )
    max_gap = number(metrics, "max_tick_gap_ms", positive=True)
    if max_gap >= MAX_TICK_GAP_MS:
        raise RuntimeError(f"GTK main loop stalled for {max_gap:.1f} ms")
    number(metrics, "max_render_ms")
    native = metrics.get("native_window") is True
    number(metrics, "render_calls", positive=not native)
    frame_updates = number(metrics, "frame_updates", positive=not native)
    if native:
        if not metrics.get("vo") or metrics["vo"] == "libmpv":
            raise RuntimeError("native playback did not activate a stock video output")
    elif fps == 1 and play_ticks <= frame_updates * 3:
        raise RuntimeError("1 fps clip did not demonstrate independent GTK updates")

    return {
        "video_fps": fps,
        "vo": metrics.get("vo"),
        "native_window": native,
        "playing_ui_hz": round(play_hz, 1),
        "paused_ui_hz": round(pause_hz, 1),
        "max_tick_gap_ms": round(max_gap, 1),
        "render_calls": metrics["render_calls"],
        "frame_updates": metrics["frame_updates"],
    }


def run_case(binary: Path, clip: Path, fps: int) -> dict:
    command = [str(binary), f"--self-test={SECONDS}", str(clip)]
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=SECONDS + 15
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"{fps} fps self-test did not exit after {SECONDS + 15}s") from error
    if result.returncode:
        raise RuntimeError(
            f"{fps} fps self-test exited {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    objects = []
    for line in result.stdout.splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            objects.append(value)
    if len(objects) != 1:
        raise RuntimeError(
            f"expected one JSON metrics object, found {len(objects)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    try:
        return check_metrics(objects[0], fps)
    except RuntimeError as error:
        raise RuntimeError(
            f"{fps} fps: {error}\nmetrics: {json.dumps(objects[0], sort_keys=True)}\n"
            f"stderr:\n{result.stderr}"
        ) from error


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "binary", nargs="?",
        default=os.environ.get("MPV_GTK_BINARY", str(root / "build" / "mpv-gtk")),
    )
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        parser.error(f"player binary does not exist: {binary}; build it first")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        parser.error("ffmpeg is required to generate test clips")

    try:
        if not accessible_display():
            print(
                "SKIP: GTK cannot open a Wayland display; "
                "UI playback was not tested.",
                file=sys.stderr,
            )
            return 77
        with tempfile.TemporaryDirectory(prefix="mpv-gtk-smoke-") as temporary:
            for fps in (1, 24):
                clip = Path(temporary) / f"sample-{fps}fps.mp4"
                generate_clip(ffmpeg, clip, fps)
                summary = run_case(binary, clip, fps)
                print(f"PASS {json.dumps(summary, sort_keys=True)}", flush=True)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
