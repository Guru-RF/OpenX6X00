#!/usr/bin/env python3
"""Runs ON THE RADIO (drive.py copies it there): presses the front keys, turns the knobs and
dumps the screen, from commands on stdin, one a line. The keys and knobs are Linux input
devices; the events are written into their nodes as if pressed.

  key NAME [ms]     press and release NAME (APP, GEN, KEY, MSG, DFN, DFL, F1..F5, LOCK, PTT, BAND_DOWN,
                    BAND_UP, AM, CW, SSB, AB, PRE, ATU, VM, AGC, FST, VOL, MFK), held ms (default 80)
  long NAME         held 1.3 s: the long press
  mfk N | main N | vol N   turn the knob N detents, clockwise when positive
  sleep S           wait S seconds (floats allowed)
  shot NAME         the screen into /tmp/shots/NAME.raw (SIGUSR1 to the GUI)

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import os
import struct
import subprocess
import sys
import time

EV_SYN, EV_KEY, EV_REL = 0, 1, 2
KEY_POWER = 116
HAPPY = 0x2c0      # BTN_TRIGGER_HAPPY1
KEYS = {"POWER": KEY_POWER, "GEN": HAPPY, "APP": HAPPY + 6, "KEY": HAPPY + 1, "MSG": HAPPY + 7, "DFN": HAPPY + 2,
        "DFL": HAPPY + 8, "F1": HAPPY + 12, "F2": HAPPY + 13, "F3": HAPPY + 14, "F4": HAPPY + 18, "F5": HAPPY + 19,
        "LOCK": HAPPY + 24, "PTT": HAPPY + 3, "BAND_DOWN": HAPPY + 4, "BAND_UP": HAPPY + 5, "AM": HAPPY + 9,
        "CW": HAPPY + 10, "SSB": HAPPY + 11, "AB": HAPPY + 15, "PRE": HAPPY + 16, "ATU": HAPPY + 17,
        "VM": HAPPY + 21, "AGC": HAPPY + 22, "FST": HAPPY + 23, "VOL": HAPPY + 20, "MFK": HAPPY + 26}
KEYPAD = "/dev/input/event0"
# each knob reports its own axis (REL_X, REL_Y, REL_Z); an event on another axis is dropped by the kernel
KNOBS = {"main": ("/dev/input/event1", 0, -1), "vol": ("/dev/input/event2", 1, -1), "mfk": ("/dev/input/event3", 2, -1)}
SHOTS = "/tmp/shots"


def emit(path, events):
    with open(path, "wb") as f:
        for t, c, v in events:
            f.write(struct.pack("llHHi", 0, 0, t, c, v))
            f.write(struct.pack("llHHi", 0, 0, EV_SYN, 0, 0))


def key(name, ms):
    code = KEYS[name.upper()]
    emit(KEYPAD, [(EV_KEY, code, 1)])
    time.sleep(ms / 1000)
    emit(KEYPAD, [(EV_KEY, code, 0)])


def turn(knob, n):
    path, axis, sign = KNOBS[knob]
    for _ in range(abs(n)):
        emit(path, [(EV_REL, axis, sign if n > 0 else -sign)])
        time.sleep(0.03)


def shot(name):
    os.makedirs(SHOTS, exist_ok=True)
    if os.path.exists("/tmp/screen.raw"):
        os.remove("/tmp/screen.raw")
    pid = subprocess.run(["pidof", "x6100_gui"], capture_output=True, text=True).stdout.split()
    os.kill(int(pid[0]), 10)   # SIGUSR1
    for _ in range(50):
        if os.path.exists("/tmp/screen.raw"):
            break
        time.sleep(0.1)
    os.rename("/tmp/screen.raw", os.path.join(SHOTS, name + ".raw"))


def main():
    for line in sys.stdin:
        w = line.split("#")[0].split()
        if not w:
            continue
        cmd = w[0].lower()
        if cmd == "key":
            key(w[1], int(w[2]) if len(w) > 2 else 80)
        elif cmd == "long":
            key(w[1], 1300)
        elif cmd in KNOBS:
            turn(cmd, int(w[1]))
        elif cmd == "sleep":
            time.sleep(float(w[1]))
        elif cmd == "shot":
            shot(w[1])
        else:
            sys.exit(f"unknown command: {line.strip()}")
        time.sleep(0.35)   # the GUI redraws
    print("done")


if __name__ == "__main__":
    main()
