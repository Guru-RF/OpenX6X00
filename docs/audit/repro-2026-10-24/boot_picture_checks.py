"""Issue #101, the boot picture. With bootanim programs as arguments (bootanim/bootanim.c built with bootanim-host.c,
one for each radio): the owner's picture on the screen at the start, and ours when it can't be read."""
import os
import subprocess
import sys
import tempfile
import zlib
from array import array
from pathlib import Path

W, H = 800, 480
cases = []


def check(what, got, want):
    cases.append((what, str(got), str(want)))


class Picture:
    """A picture made of f(x, y) -> (r, g, b) or (r, g, b, a): its PNG, and the 480x800 panel's words once shown"""
    def __init__(self, f, w=W, h=H):
        self.w, self.h = w, h
        self.rows = [[f(x, y) for x in range(w)] for y in range(h)]

    def png(self):
        alpha = len(self.rows[0][0]) == 4
        raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in self.rows)
        head = self.w.to_bytes(4, "big") + self.h.to_bytes(4, "big") + bytes([8, 6 if alpha else 2, 0, 0, 0])
        return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")

    def shown(self, f=lambda p: p):
        """The framebuffer's words: landscape (x, y) at panel (y, 799 - x), red at bit 16; f(p) the colour shown"""
        words = array("I", bytes(4 * W * H))
        for y, row in enumerate(self.rows):
            for x, p in enumerate(row):
                r, g, b = f(p)[:3]
                words[(W - 1 - x) * H + y] = r << 16 | g << 8 | b
        return words


def chunk(kind, data):
    return len(data).to_bytes(4, "big") + kind + data + zlib.crc32(kind + data).to_bytes(4, "big")


def bootanim_checks(binaries):
    # ours: dots on its background colour (the top-left pixel's) all over the screen, so every part of its steps
    # (the ring, the signal (blue over red), the name, the rule, the subtitle, the foot) has some; the owner's: a
    # picture of all colours
    ours = Picture(lambda x, y: ((x * 3) & 255, (y * 5) & 255, ((x + y) * 7) & 255) if (x // 16 + y // 16) % 5 == 2
                   else (8, 9, 12))
    own_f = lambda x, y: ((x + 40) & 255, (y + 20) & 255, (x ^ y) & 255)
    own = Picture(own_f)
    rgba = Picture(lambda x, y: own_f(x, y) + ((255,) if x < W // 2 else (0,)))   # the right half transparent
    by_hand = Picture(lambda x, y: (x & 255, x & 255, y & 255))
    small = Picture(own_f, 640, 480)
    with tempfile.TemporaryDirectory(prefix="x6bootanim-", dir=os.environ.get("AUDIT_TMP")) as td:
        td = Path(td)
        files = {"own": own.png(), "ours": ours.png(), "rgba": rgba.png(), "by_hand": by_hand.png(),
                 "small": small.png(), "jpeg": b"\xff\xd8\xff\xe0\0\x10JFIF\0" + bytes(4000)}
        files["cut"] = files["own"][:len(files["own"]) // 2]
        damaged = bytearray(files["own"])
        damaged[len(damaged) // 2] ^= 0x55   # inside IDAT: its checksum no longer matches
        files["damaged"] = bytes(damaged)
        for name, data in files.items():
            (td / (name + ".png")).write_bytes(data)
        fb = td / "fb"
        marker = array("I", [0x5A5A5A5A]) * (W * H)
        want_own, want_ours = own.shown(), ours.shown()
        bg = own.rows[0][0]   # the top-left pixel: the colour the picture fades in from
        half = own.shown(lambda p: tuple(b + ((c - b) * 128 >> 8) for c, b in zip(p, bg)))
        want_rgba = rgba.shown(lambda p: p if p[3] else (0, 0, 0))

        def run(binary, own_file, ours_file="ours", *args, gui_up=False, snap=0):
            fb.write_bytes(marker.tobytes())
            env = {"PATH": os.environ.get("PATH", ""), "FB": str(fb), "OURS": str(td / (ours_file + ".png")),
                   "OWN": str(td / (own_file + ".png"))}
            if gui_up:
                env["GUI_UP"] = "1"
            if snap:
                env["SNAP"] = str(snap)
            r = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True, timeout=60)
            shot = array("I")
            shot.frombytes(fb.read_bytes())
            snapped = array("I")
            if snap:
                snapped.frombytes((td / "fb.snap").read_bytes())
            read = [line[5:] for line in r.stdout.splitlines() if line.startswith("read ")]
            touched = [line for line in r.stdout.splitlines() if line.startswith("touched")]
            return r.returncode, ", ".join(read) + (" (touched: %s)" % touched if touched else ""), shot, snapped

        def shows(shot):
            return {want_own.tobytes(): "own", want_ours.tobytes(): "ours", want_rgba.tobytes(): "own, the right half black",
                    marker.tobytes(): "nothing (untouched)"}.get(shot.tobytes(), "something else")

        for binary in binaries:
            radio = Path(binary).name
            for what, own_file, want in (
                    ("the owner's picture", "own", "own"),
                    ("none of the owner's", "none", "ours"),
                    ("the owner's 640x480", "small", "ours"),
                    ("the owner's cut short", "cut", "ours"),
                    ("the owner's damaged (a checksum in its picture data)", "damaged", "ours"),
                    ("the owner's a JPEG named .png", "jpeg", "ours"),
                    ("the owner's with a transparent half", "rgba", "own, the right half black")):
                rc, read, shot, _ = run(binary, own_file)
                both = "/mnt/bootlogo.png" if want != "ours" else "/mnt/bootlogo.png, /usr/share/x6100/bootlogo.png"
                check(f"{radio}, {what}: exit, read, shown", f"{rc}; {read}; {shows(shot)}", f"0; {both}; {want}")
            rc, read, shot, _ = run(binary, "none", "none")
            check(f"{radio}, neither picture readable: exit, read, shown", f"{rc}; {read}; {shows(shot)}",
                  "1; /mnt/bootlogo.png, /usr/share/x6100/bootlogo.png; nothing (untouched)")
            rc, read, shot, _ = run(binary, "own", gui_up=True)
            check(f"{radio}, the GUI up already: exit, read, shown", f"{rc}; {read}; {shows(shot)}",
                  "0; ; nothing (untouched)")
            rc, read, shot, _ = run(binary, "own", "ours", str(td / "by_hand.png"))
            check(f"{radio}, a picture named by hand: read the one named, shown",
                  f"{read == str(td / 'by_hand.png')}; {shot == by_hand.shown()}", "True; True")
            # halfway through its 2.8 s, after the frame of 1400 ms, every pixel of the owner's picture is halfway
            # from the top-left pixel's colour; ours at that moment is still drawing itself, partly done
            rc, read, shot, snapped = run(binary, "own", snap=36)
            check(f"{radio}, the owner's picture at 1.4 s: all of it halfway", str(snapped == half), "True")
            rc, read, shot, snapped = run(binary, "none", snap=36)
            bg_word = 8 << 16 | 9 << 8 | 12
            done = sum(1 for s, w in zip(snapped, want_ours) if w != bg_word and s == w)
            to_come = sum(1 for s, w in zip(snapped, want_ours) if w != bg_word and s == bg_word)
            check(f"{radio}, ours at 1.4 s: some parts done, some still to come", str(done > 1000 and to_come > 1000),
                  "True")
            check(f"{radio}, ours at the end: whole", shows(shot), "ours")


bootanim_checks(sys.argv[1:])
bad = [c for c in cases if c[1] != c[2]]
for what, got, want in cases:
    print(("ok   " if got == want else "FAIL ") + what + ": " + got + ("" if got == want else "  (want: " + want + ")"))
print("The boot picture (#101): %d of %d cases as they should be" % (len(cases) - len(bad), len(cases)))
assert not bad
