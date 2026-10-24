#!/usr/bin/env python3
"""Real libmpv integration without GTK or a display (null video/audio outputs).

This verifies engine options, decoding, and asynchronous event handling only.
It does not verify GTK rendering or the UI's independent frame clock.
"""

from __future__ import annotations

import ctypes as C
import ctypes.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from smoke import generate_clip


FLAG, INT64, DOUBLE = 3, 4, 5
NONE, SHUTDOWN, LOG, SET_REPLY, COMMAND_REPLY = 0, 1, 2, 4, 5
END_FILE, FILE_LOADED, SEEK, RESTART, PROPERTY = 7, 8, 20, 21, 22


class Event(C.Structure):
    _fields_ = [
        ("event_id", C.c_int), ("error", C.c_int),
        ("reply_userdata", C.c_uint64), ("data", C.c_void_p),
    ]


class Property(C.Structure):
    _fields_ = [("name", C.c_char_p), ("format", C.c_int), ("data", C.c_void_p)]


class Log(C.Structure):
    _fields_ = [
        ("prefix", C.c_char_p), ("level", C.c_char_p),
        ("text", C.c_char_p), ("log_level", C.c_int),
    ]


class EndFile(C.Structure):
    _fields_ = [("reason", C.c_int), ("error", C.c_int)]


def api():
    library = ctypes.util.find_library("mpv")
    if not library:
        raise RuntimeError("libmpv was not found")
    lib = C.CDLL(library)
    signatures = {
        "mpv_create": (C.c_void_p, []),
        "mpv_set_option_string": (C.c_int, [C.c_void_p, C.c_char_p, C.c_char_p]),
        "mpv_initialize": (C.c_int, [C.c_void_p]),
        "mpv_observe_property": (C.c_int, [C.c_void_p, C.c_uint64, C.c_char_p, C.c_int]),
        "mpv_request_log_messages": (C.c_int, [C.c_void_p, C.c_char_p]),
        "mpv_command_async": (C.c_int, [C.c_void_p, C.c_uint64, C.POINTER(C.c_char_p)]),
        "mpv_set_property_async": (C.c_int, [C.c_void_p, C.c_uint64, C.c_char_p, C.c_int, C.c_void_p]),
        "mpv_wait_event": (C.POINTER(Event), [C.c_void_p, C.c_double]),
        "mpv_error_string": (C.c_char_p, [C.c_int]),
        "mpv_terminate_destroy": (None, [C.c_void_p]),
    }
    for name, (result, args) in signatures.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = result, args
    return lib


def frontend_options() -> list[tuple[str, str]]:
    source = (Path(__file__).resolve().parents[1] / "src" / "main.c").read_text()
    table = re.search(r"const char \*options\[\]\[2\]\s*=\s*\{(.*?)\n\s*\};", source, re.S)
    if not table:
        raise RuntimeError("could not locate frontend libmpv options table")
    options = re.findall(r'\{"([^"\\]+)",\s*"([^"\\]*)"\}', table[1])
    if not options:
        raise RuntimeError("frontend options table is empty")
    return [("config", "no"), *options]


class Engine:
    def __init__(self):
        self.lib = api()
        self.handle = self.lib.mpv_create()
        if not self.handle:
            raise RuntimeError("mpv_create failed")
        self.serial = 0
        self.properties = {}
        self.replies = set()
        self.loaded = self.restarts = self.seeks = 0
        self.file_errors = []
        self.logs = []
        try:
            for name, value in frontend_options():
                self.check(self.lib.mpv_set_option_string(self.handle, name.encode(), value.encode()), f"option {name}={value}")
            # Exercise the application's options first, then isolate this test
            # from display, GPU, and speaker availability.
            for name, value in (("vo", "null"), ("ao", "null"), ("hwdec", "no")):
                self.check(self.lib.mpv_set_option_string(self.handle, name.encode(), value.encode()), f"test option {name}")
            self.check(self.lib.mpv_initialize(self.handle), "mpv_initialize")
            self.check(self.lib.mpv_request_log_messages(self.handle, b"error"), "request logs")
            observed = {
                "time-pos": DOUBLE, "duration": DOUBLE, "pause": FLAG,
                "speed": DOUBLE, "volume": DOUBLE, "seeking": FLAG,
                "video-params/w": INT64, "audio-params/samplerate": INT64,
            }
            for serial, (name, fmt) in enumerate(observed.items(), 1):
                self.check(self.lib.mpv_observe_property(self.handle, serial, name.encode(), fmt), f"observe {name}")
        except Exception:
            self.close()
            raise

    def check(self, code: int, operation: str) -> None:
        if code < 0:
            raise RuntimeError(f"{operation}: {self.lib.mpv_error_string(code).decode()}")

    def close(self) -> None:
        if self.handle:
            self.lib.mpv_terminate_destroy(self.handle)
            self.handle = None

    def command(self, *args: str) -> int:
        self.serial += 1
        array = (C.c_char_p * (len(args) + 1))(*(arg.encode() for arg in args), None)
        self.check(self.lib.mpv_command_async(self.handle, self.serial, array), f"command {args}")
        return self.serial

    def set(self, name: str, value, fmt: int) -> int:
        self.serial += 1
        data = C.c_int(value) if fmt == FLAG else C.c_double(value)
        self.check(self.lib.mpv_set_property_async(self.handle, self.serial, name.encode(), fmt, C.byref(data)), f"set {name}")
        return self.serial

    def poll(self, timeout: float = 0.05) -> None:
        event = self.lib.mpv_wait_event(self.handle, timeout).contents
        if event.event_id in (SET_REPLY, COMMAND_REPLY):
            self.check(event.error, f"async request {event.reply_userdata}")
            self.replies.add(event.reply_userdata)
        elif event.event_id == PROPERTY:
            prop = C.cast(event.data, C.POINTER(Property)).contents
            if prop.data and prop.format in (FLAG, INT64, DOUBLE):
                kind = {FLAG: C.c_int, INT64: C.c_int64, DOUBLE: C.c_double}[prop.format]
                self.properties[prop.name.decode()] = C.cast(prop.data, C.POINTER(kind)).contents.value
        elif event.event_id == FILE_LOADED:
            self.loaded += 1
        elif event.event_id == RESTART:
            self.restarts += 1
        elif event.event_id == SEEK:
            self.seeks += 1
        elif event.event_id == END_FILE:
            end = C.cast(event.data, C.POINTER(EndFile)).contents
            if end.error < 0:
                self.file_errors.append(end.error)
        elif event.event_id == LOG:
            log = C.cast(event.data, C.POINTER(Log)).contents
            self.logs.append(f"{log.prefix.decode()}: {log.text.decode().strip()}")
        elif event.event_id == SHUTDOWN:
            raise RuntimeError("unexpected engine shutdown")

    def wait(self, predicate, description: str, timeout: float = 6) -> None:
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise RuntimeError(f"timeout: {description}; properties={self.properties}; logs={self.logs}")
            self.poll()

    def pump(self, duration: float) -> None:
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.poll(min(0.05, max(0, deadline - time.monotonic())))


def run_case(clip: Path, fps: int) -> None:
    engine = Engine()
    try:
        # GFile.get_uri() encodes spaces and Cyrillic names in local file URIs.
        request = engine.command("loadfile", clip.as_uri(), "replace")
        engine.wait(lambda: request in engine.replies and engine.loaded == 1 and engine.properties.get("time-pos", 0) > 0.05, "media starts")
        engine.wait(lambda: engine.properties.get("video-params/w") == 320 and engine.properties.get("audio-params/samplerate") == 48000, "audio and video decoded")
        assert abs(engine.properties["duration"] - 12) < 0.1, engine.properties

        request = engine.set("pause", 1, FLAG)
        engine.wait(lambda: request in engine.replies and engine.properties.get("pause") == 1, "async pause")
        engine.pump(0.15)
        paused_position = engine.properties["time-pos"]
        engine.pump(0.35)
        assert abs(engine.properties["time-pos"] - paused_position) < 0.05, "position advances while paused"

        volume = engine.set("volume", 25.0, DOUBLE)
        speed = engine.set("speed", 1.5, DOUBLE)
        engine.wait(lambda: {volume, speed} <= engine.replies and engine.properties.get("volume") == 25 and engine.properties.get("speed") == 1.5, "async volume and speed")

        previous_restarts, previous_seeks = engine.restarts, engine.seeks
        request = engine.command("seek", "3", "absolute+exact")
        engine.wait(lambda: request in engine.replies and engine.restarts > previous_restarts and engine.seeks > previous_seeks and abs(engine.properties.get("time-pos", -1) - 3) < 0.1, "observed seek while paused")
        assert engine.properties["pause"] == 1
        engine.pump(0.3)
        assert abs(engine.properties["time-pos"] - 3) < 0.1, "seek resumed paused media"

        request = engine.set("pause", 0, FLAG)
        engine.wait(lambda: request in engine.replies and engine.properties.get("pause") == 0 and engine.properties.get("time-pos", 0) > 3.3, "async resume advances position")
        if engine.logs or engine.file_errors:
            raise RuntimeError(f"unexpected playback errors: {engine.logs}, {engine.file_errors}")

        # A failed load must produce an observable terminal file error rather
        # than hang the event loop; command submission itself may succeed.
        request = engine.command("loadfile", str(clip.with_name("missing-media.mp4")), "replace")
        engine.wait(lambda: request in engine.replies and bool(engine.file_errors), "missing media error")
        assert all(code < 0 for code in engine.file_errors)
        print(f"PASS engine only: {fps} fps audio/video via escaped file URI, async pause/seek/resume/volume/speed, failed-load error", flush=True)
    finally:
        engine.close()


def main() -> int:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("FAIL engine only: ffmpeg is required", file=sys.stderr)
        return 1
    try:
        with tempfile.TemporaryDirectory(prefix="mpv-gtk-engine-") as temporary:
            for fps in (1, 24):
                clip = Path(temporary) / f"sample {fps}fps Unicode Ω.mp4"
                generate_clip(ffmpeg, clip, fps)
                run_case(clip, fps)
    except (AssertionError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL engine only: {error}", file=sys.stderr)
        return 1
    print("GTK rendering and UI frame-clock independence were not tested.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
