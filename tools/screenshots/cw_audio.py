#!/usr/bin/env python3
"""Keyed CW as the radio's RX audio, for host_shots.py --rx-audio: raw s16le, 44.1 kHz, mono.

  python3 tools/screenshots/cw_audio.py <out.raw> [--hz 700] [--wpm 22] [--text "CQ CQ DE ON6URE K"]

The tone at --hz (the KEY Tone, 700 Hz by default, is zero beat: the CW Tuner's middle bar; 20 Hz higher lights the bar
two to the left, in CW on the X6100), keyed at --wpm (PARIS: a dot is 1200 / WPM ms) with 4 ms edges, over a little
noise, the text then a word's pause, once: host_shots.py plays it round and round.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import argparse
import math
import random
import struct

RATE = 44100
MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....", "I": "..",
    "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
    "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..",
    "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-", "5": ".....", "6": "-....", "7": "--...",
    "8": "---..", "9": "----.", "/": "-..-.", "?": "..--..", "=": "-...-",
}


def keying(text, dit):
    """(key down, seconds) for each element and gap of text"""
    out = []
    for word in text.upper().split():
        for ch in word:
            for el in MORSE.get(ch, ""):
                out += [(True, dit if el == "." else 3 * dit), (False, dit)]
            out.append((False, 2 * dit))     # 3 dits after a character
        out.append((False, 4 * dit))         # 7 after a word
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--hz", type=float, default=700.0)
    ap.add_argument("--wpm", type=float, default=22.0)
    ap.add_argument("--text", default="CQ CQ DE ON6URE ON6URE K")
    a = ap.parse_args()

    random.seed(1)      # the same file each run
    step, edge, phase = 2 * math.pi * a.hz / RATE, int(0.004 * RATE), 0.0
    buf = bytearray()
    for down, sec in keying(a.text, 1.2 / a.wpm) + [(False, 1.0)]:
        n = int(sec * RATE)
        for i in range(n):
            amp = 0.35 * min(1.0, i / edge, (n - i) / edge) if down else 0.0
            v = amp * math.sin(phase) + 0.01 * random.gauss(0, 1)
            phase += step
            buf += struct.pack("<h", max(-32767, min(32767, int(v * 32767))))
    with open(a.out, "wb") as f:
        f.write(buf)


if __name__ == "__main__":
    main()
