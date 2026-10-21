#!/usr/bin/env python3
"""Takes a screenshot of the radio from here, over SSH: the GUI dumps what the panel shows on
SIGUSR1 (/tmp/screen.raw, XRGB8888, the portrait panel), which is turned into a landscape PNG.

  python3 tools/screenshots/grab.py 192.168.1.20 [name.png]

Needs ssh access as root (a key in /root/.ssh/authorized_keys on the radio, or the password
asked by ssh) and Pillow. Without a name the file is shot_<time>.png in the current directory.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import os
import subprocess
import sys
import tempfile
import time

from PIL import Image

PANEL = (480, 800)      # the panel is portrait; the GUI draws landscape 800x480 turned on it


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    host = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else time.strftime("shot_%Y%m%d_%H%M%S.png")
    ssh = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"]
    subprocess.run(ssh + [f"root@{host}", "rm -f /tmp/screen.raw; kill -USR1 $(pidof x6100_gui); "
                          "for i in 1 2 3 4 5 6 7 8 9 10; do [ -f /tmp/screen.raw ] && break; sleep 0.2; done"], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "screen.raw")
        subprocess.run(["scp", "-q", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                        "-o", "LogLevel=ERROR", f"root@{host}:/tmp/screen.raw", raw], check=True)
        data = open(raw, "rb").read()
    if len(data) != PANEL[0] * PANEL[1] * 4:
        sys.exit(f"unexpected dump size {len(data)}")
    im = Image.frombuffer("RGBA", PANEL, data, "raw", "BGRA", 0, 1).convert("RGB")
    im.rotate(-90, expand=True).save(out)
    print(out)


if __name__ == "__main__":
    main()
