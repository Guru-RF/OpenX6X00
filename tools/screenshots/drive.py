#!/usr/bin/env python3
"""Drives the radio from here and collects screenshots: the commands of a script file (see
x6drive.py for them) run on the radio over SSH, then every screen dumped lands as a landscape
PNG in the output folder.

  python3 tools/screenshots/drive.py 192.168.1.20 script.txt [out_dir]

Needs ssh access as root (a key on the radio) and Pillow. The output folder defaults to
docs/screenshots. DRIVE_SSH_OPTS adds ssh options, for example a shared connection
("-o ControlPath=/tmp/x62-ctl") on a network that blocks repeated logins.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

PANEL = (480, 800)
SSH = ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"] \
    + os.environ.get("DRIVE_SSH_OPTS", "").split()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    host, script = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.dirname(here)), "docs", "screenshots")
    os.makedirs(out, exist_ok=True)
    subprocess.run(["scp", "-q"] + SSH + [os.path.join(here, "x6drive.py"), f"root@{host}:/tmp/x6drive.py"], check=True)
    with open(script) as f:
        r = subprocess.run(["ssh"] + SSH + [f"root@{host}", "rm -rf /tmp/shots; python3 /tmp/x6drive.py"],
                           stdin=f, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"radio: {r.stderr.strip() or r.stdout.strip()}")
    if "shot " not in open(script).read():
        return      # a script that only drives the radio: nothing to fetch
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["scp", "-q", "-r"] + SSH + [f"root@{host}:/tmp/shots", tmp], check=True)
        for name in sorted(os.listdir(os.path.join(tmp, "shots"))):
            data = open(os.path.join(tmp, "shots", name), "rb").read()
            if len(data) != PANEL[0] * PANEL[1] * 4:
                print(f"{name}: unexpected size {len(data)}", file=sys.stderr)
                continue
            im = Image.frombuffer("RGBA", PANEL, data, "raw", "BGRA", 0, 1).convert("RGB")
            dest = os.path.join(out, name[:-4] + ".png")
            # a name ending in _p is a portrait screen (Walkie): the panel's own orientation, turned 180
            im.rotate(180 if name.endswith("_p.raw") else -90, expand=True).save(dest)
            print(dest)


if __name__ == "__main__":
    main()
