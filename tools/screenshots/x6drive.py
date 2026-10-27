#!/usr/bin/env python3
"""Runs ON THE RADIO (drive.py copies it there): presses the front keys, turns the knobs and
dumps the screen, from commands on stdin, one a line. The keys and knobs are Linux input
devices; the events are written into their nodes as if pressed.

  key NAME [ms]     press and release NAME (APP, GEN, KEY, MSG, DFN, DFL, F1..F5, LOCK, PTT, BAND_DOWN,
                    BAND_UP, AM, CW, SSB, AB, PRE, ATU, VM, AGC, FST, VOL, MFK), held ms (default 80)
  long NAME         held 1.3 s: the long press
  mfk N | main N | vol N   turn the knob N detents, clockwise when positive
  inner N           the X6200's inner MFK ring, N detents
  sleep S           wait S seconds (floats allowed)
  shot NAME         the screen into /tmp/shots/NAME.raw (SIGUSR1 to the GUI)

The radio's model comes from the device tree: the X6200's keypad has other codes for APP, KEY,
MSG, DFN, ATU and FST, no LOCK, PRE, VM or AGC key, and no POWER key device (its base reports
the POWER key); an X6200 has the inner ring.

The PC build (host_shots.py imports this): X6DRIVE_DEV_DIR is the folder of its key and knob
FIFOs (keypad, main, vol, mfk, power: the X6100's codes), X6DRIVE_TMP the folder its /tmp is,
X6DRIVE_PID the GUI's pid.

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

def model():
    try:
        with open("/proc/device-tree/compatible", "rb") as f:
            return "x6200" if b"xiegu,x6200" in f.read() else "x6100"
    except OSError:
        return "x6100"


MODEL = model()
# The front keys as src/front_keys.c maps them; the two keypads share most codes
KEYS = {"GEN": HAPPY, "F1": HAPPY + 12, "F2": HAPPY + 13, "F3": HAPPY + 14, "F4": HAPPY + 18, "F5": HAPPY + 19,
        "PTT": HAPPY + 3, "BAND_DOWN": HAPPY + 4, "BAND_UP": HAPPY + 5, "AM": HAPPY + 9, "CW": HAPPY + 10,
        "SSB": HAPPY + 11, "AB": HAPPY + 15, "VOL": HAPPY + 20, "MFK": HAPPY + 26}
if MODEL == "x6200":
    KEYS.update({"APP": HAPPY + 1, "KEY": HAPPY + 2, "MSG": HAPPY + 6, "DFN": HAPPY + 7, "DFL": HAPPY + 8,
                 "ATU": HAPPY + 16, "FST": HAPPY + 17})
else:
    KEYS.update({"POWER": KEY_POWER, "APP": HAPPY + 6, "KEY": HAPPY + 1, "MSG": HAPPY + 7, "DFN": HAPPY + 2,
                 "DFL": HAPPY + 8, "LOCK": HAPPY + 24, "PRE": HAPPY + 16, "ATU": HAPPY + 17, "VM": HAPPY + 21,
                 "AGC": HAPPY + 22, "FST": HAPPY + 23})
KEYPAD = "/dev/input/event0"
# each knob reports its own axis (REL_X, REL_Y, REL_Z, REL_RX); an event on another axis is dropped by the kernel
KNOBS = {"main": ("/dev/input/event1", 0, -1), "vol": ("/dev/input/event2", 1, -1), "mfk": ("/dev/input/event3", 2, -1)}
if MODEL == "x6200":
    KNOBS["inner"] = ("/dev/input/event4", 3, -1)
SHOTS = "/tmp/shots"


def emit(path, events):
    with open(path, "wb") as f:
        for t, c, v in events:
            f.write(struct.pack("llHHi", 0, 0, t, c, v))
            f.write(struct.pack("llHHi", 0, 0, EV_SYN, 0, 0))


POWER_DEV = "/dev/input/event4"   # the PMIC's key (axp20x-pek): the matrix keypad drops KEY_POWER
TMP = os.environ.get("X6DRIVE_TMP", "/tmp")
DEV_DIR = os.environ.get("X6DRIVE_DEV_DIR")
if DEV_DIR:   # the PC build: src/host/host_panel.c's FIFOs, one a device, the X6100's codes
    KEYPAD, POWER_DEV = f"{DEV_DIR}/keypad", f"{DEV_DIR}/power"
    KNOBS = {k: (f"{DEV_DIR}/{k}", axis, sign) for k, (_, axis, sign) in KNOBS.items() if k != "inner"}


def key(name, ms):
    if name.upper() not in KEYS:
        sys.exit(f"no {name.upper()} key on the {MODEL.upper()}")
    code = KEYS[name.upper()]
    dev = POWER_DEV if code == KEY_POWER else KEYPAD
    emit(dev, [(EV_KEY, code, 1)])
    time.sleep(ms / 1000)
    emit(dev, [(EV_KEY, code, 0)])


def turn(knob, n):
    path, axis, sign = KNOBS[knob]
    for _ in range(abs(n)):
        emit(path, [(EV_REL, axis, sign if n > 0 else -sign)])
        time.sleep(0.03)


def dump():
    """SIGUSR1 to the GUI, then the path of the screen it wrote (/tmp/screen.raw)."""
    raw = os.path.join(TMP, "screen.raw")
    if os.path.exists(raw):
        os.remove(raw)
    pid = os.environ.get("X6DRIVE_PID") or \
        subprocess.run(["pidof", "x6100_gui"], capture_output=True, text=True).stdout.split()[0]
    os.kill(int(pid), 10)   # SIGUSR1
    for _ in range(50):
        if os.path.exists(raw):
            break
        time.sleep(0.1)
    return raw


def shot(name):
    os.makedirs(SHOTS, exist_ok=True)
    os.rename(dump(), os.path.join(SHOTS, name + ".raw"))


def run(lines, take=shot):
    """The commands of a script; take(NAME) for each shot."""
    for line in lines:
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
            take(w[1])
        else:
            sys.exit(f"unknown command: {line.strip()}")
        time.sleep(0.35)   # the GUI redraws


def main():
    run(sys.stdin)
    print("done")


if __name__ == "__main__":
    main()
