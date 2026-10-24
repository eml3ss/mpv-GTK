#!/usr/bin/env bash
# Stage byte-for-byte copies of the installed stock engine. No mpv patch/build.
set -Eeuo pipefail
if (( $# )); then
    printf 'Usage: tools/stage-stock-mpv.sh\n' >&2
    exit 2
fi
stock_project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
python3 - "$stock_project_dir" <<'PY'
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

project = Path(sys.argv[1])
output = project / "build"
libdir = Path(subprocess.check_output(
    [os.environ.get("PKG_CONFIG", "pkg-config"), "--variable=libdir", "mpv"],
    text=True).strip())
library_source = (libdir / "libmpv.so.2").resolve(strict=True)
binary_path = shutil.which("mpv")
if not binary_path:
    raise SystemExit("The installed stock mpv executable was not found in PATH")
binary_source = Path(binary_path).resolve(strict=True)
for source in (library_source, binary_source):
    with source.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise SystemExit(f"Expected an installed ELF engine, received: {source}")

library = ctypes.CDLL(str(library_source))
if hasattr(library, "mpv_gtk_vulkan_api_version"):
    raise SystemExit("Refusing to stage the retired custom Render API library")
library.mpv_client_api_version.argtypes = []
library.mpv_client_api_version.restype = ctypes.c_ulong
client_api = library.mpv_client_api_version()
version = subprocess.check_output([str(binary_source), "--version"], text=True)
output.mkdir(parents=True, exist_ok=True)

def write_atomic(destination, contents, mode):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file() and not destination.is_symlink() and destination.read_bytes() == contents:
        return
    descriptor, temporary = tempfile.mkstemp(prefix="." + destination.name + ".", dir=destination.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(contents)
        os.chmod(temporary, mode)
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

def stage(source, name):
    contents = source.read_bytes()
    write_atomic(output / name, contents, 0o755)
    return {
        "source": str(source),
        "file": name,
        "sha256": hashlib.sha256(contents).hexdigest(),
        "size": len(contents),
    }

provenance = {
    "origin": "Unmodified files from the installed distribution mpv package",
    "upstream": "https://mpv.io/",
    "upstream_source": "https://github.com/mpv-player/mpv",
    "version": version.splitlines()[0],
    "client_api": f"{client_api >> 16}.{client_api & 0xffff}",
    "license": "GPL-2.0-or-later AND LGPL-2.1-or-later",
    "library": stage(library_source, "libmpv.so.2"),
    "command_line_engine": stage(binary_source, "mpv-engine"),
}
if shutil.which("pacman"):
    package = subprocess.run(["pacman", "-Q", "mpv"], text=True, capture_output=True)
    if package.returncode == 0:
        provenance["distribution_package"] = package.stdout.strip()
write_atomic(output / "stock-mpv.json", (json.dumps(provenance, indent=2) + "\n").encode(), 0o644)
write_atomic(output / "stock-mpv-version.txt", version.encode(), 0o644)
notices = project / "tools/stock-mpv-licenses"
for name in ("Copyright", "LICENSE.GPL", "LICENSE.LGPL"):
    write_atomic(output / "stock-mpv-licenses" / name, (notices / name).read_bytes(), 0o644)
print(f"Staged {provenance['version']} as build/libmpv.so.2 and build/mpv-engine")
PY
