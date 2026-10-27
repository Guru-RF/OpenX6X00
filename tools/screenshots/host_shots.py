#!/usr/bin/env python3
"""Screenshots of the PC build without a desktop: the GUI runs headless (SDL's dummy video) in a
sandbox, driven by a script of x6drive.py's commands; every "shot NAME" lands as NAME.png.

  python3 tools/screenshots/host_shots.py <build dir> <script> <out dir> [--theme NAME]
                                          [--set name=value ...] [--keep]

<build dir> is a host build (host/build.sh, cmake -DHOST_BUILD=ON). <script> is a file, or the
commands inline with ";" between them ("key APP; sleep 1; shot apps"). Each run starts from the
default settings (the build's params.db, or sql/params.sql) with the theme and the --set values in
params; the GUI gets 10 s after its FIFOs appear before the script starts.

The sandbox (bwrap): the whole system read-only, the GUI's data folder and /tmp private, no
network (no cluster spots: the shots are the same each run), no D-Bus (NetworkManager and BlueZ on
this PC are left alone), and a PipeWire of its own with a null sink (the GUI needs PulseAudio,
and the stub radio's noise stays off this PC's speakers). Needs bwrap, pipewire, pipewire-pulse,
pactl, sqlite3 and Pillow.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

INSIDE = """#!/bin/sh
# inside bwrap: a private PipeWire with a null sink, then the GUI headless
export XDG_RUNTIME_DIR=/tmp/xdg DBUS_SESSION_BUS_ADDRESS= DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent
mkdir -p $XDG_RUNTIME_DIR && chmod 700 $XDG_RUNTIME_DIR
pipewire > /tmp/pw.log 2>&1 &
sleep 1
pipewire-pulse > /tmp/pwp.log 2>&1 &
sleep 1
pactl load-module module-null-sink sink_name=shots > /tmp/pactl.log 2>&1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy exec "$@" > /tmp/gui.log 2>&1
"""


def cache_value(build, key):
    with open(os.path.join(build, "CMakeCache.txt")) as f:
        for line in f:
            if line.startswith(key + ":"):
                return line.split("=", 1)[1].strip()
    sys.exit(f"{build}: no {key} in CMakeCache.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("build")
    ap.add_argument("script")
    ap.add_argument("out")
    ap.add_argument("--theme")
    ap.add_argument("--set", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--keep", action="store_true", help="keep the run folder (its gui.log)")
    a = ap.parse_args()

    build = os.path.abspath(a.build)
    gui = os.path.join(build, "src", "x6100_gui")
    data_dir = cache_value(build, "HOST_DATA_DIR")
    source = cache_value(build, "CMAKE_HOME_DIRECTORY")
    if os.path.isfile(a.script):
        lines = open(a.script).read().splitlines()
    else:
        lines = a.script.split(";")
    os.makedirs(a.out, exist_ok=True)

    run = tempfile.mkdtemp(prefix="host_shots-")
    data, tmp = os.path.join(run, "data"), os.path.join(run, "tmp")
    os.makedirs(data)
    os.makedirs(tmp)
    db = os.path.join(data, "params.db")
    if os.path.exists(os.path.join(build, "params.db")):
        shutil.copy(os.path.join(build, "params.db"), db)
    else:
        with open(os.path.join(source, "sql", "params.sql")) as f:
            subprocess.run(["sqlite3", db], stdin=f, check=True)
    sets = ([f"theme_name={a.theme}"] if a.theme else []) + a.set
    for s in sets:
        name, _, value = s.partition("=")
        v = value if value.lstrip("-").isdigit() else "'" + value.replace("'", "''") + "'"
        subprocess.run(["sqlite3", db, f"INSERT INTO params VALUES('{name}', {v})"], check=True)
    with open(os.path.join(tmp, "inside.sh"), "w") as f:
        f.write(INSIDE)
    os.chmod(os.path.join(tmp, "inside.sh"), 0o755)
    empty = os.path.join(run, "empty")
    open(empty, "w").close()
    os.makedirs(data_dir, exist_ok=True)   # the mount point

    # /tmp first: the build may lie under it, and is bound again over the private one
    info_r, info_w = os.pipe()
    cmd = ["bwrap", "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--bind", tmp, "/tmp",
           "--ro-bind", build, build, "--bind", data, data_dir, "--tmpfs", "/run/dbus"]
    notice = os.path.join(build, "share", "image-required")
    if os.path.exists(notice):
        cmd += ["--ro-bind", empty, notice]   # no "new card image" note in the shots
    cmd += ["--unshare-net", "--unshare-ipc", "--die-with-parent",
            "--info-fd", str(info_w), "/tmp/inside.sh", gui]
    proc = subprocess.Popen(cmd, pass_fds=(info_w,))
    os.close(info_w)
    info = b""
    while not info.rstrip().endswith(b"}"):
        chunk = os.read(info_r, 4096)
        if not chunk:
            break
        info += chunk
    os.close(info_r)
    pid = int(info.split(b'"child-pid":')[1].split(b"}")[0].split(b",")[0])
    status = 0
    try:
        for _ in range(300):
            if os.path.exists(os.path.join(data, "run", "keypad")) or proc.poll() is not None:
                break
            time.sleep(0.1)
        time.sleep(10)
        if proc.poll() is not None:
            sys.exit(f"the GUI ended (see {tmp}/gui.log)")

        os.environ.update({"X6DRIVE_DEV_DIR": os.path.join(data, "run"), "X6DRIVE_TMP": tmp, "X6DRIVE_PID": str(pid)})
        import x6drive
        from raw2png import raw_to_png

        def take(name):
            nonlocal status
            raw = x6drive.dump()
            dest = os.path.join(a.out, name + ".png")
            if os.path.exists(raw) and raw_to_png(raw, dest):
                print(dest)
            else:
                print(f"{name}: no screen", file=sys.stderr)
                status = 1

        x6drive.run(lines, take)
    finally:
        try:
            os.kill(pid, signal.SIGKILL)   # it does not end on SIGTERM
        except ProcessLookupError:
            pass
        proc.wait()
        if a.keep:
            print(f"run folder: {run}")
        else:
            shutil.rmtree(run, ignore_errors=True)
    sys.exit(status)


if __name__ == "__main__":
    main()
