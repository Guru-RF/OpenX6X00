"""Issue #101, the boot picture: the web configurator's production routes (the Display page's card) and its Backup and
Restore, with temporary files for the DATA partition and the system's picture. With bootanim programs as arguments
(bootanim/bootanim.c built with bootanim-host.c, one for each radio): the owner's picture on the screen at the start,
and ours when it can't be read."""
import ast
import binascii
import io
import json
import os
import random
import re
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import threading
import zlib
from array import array
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

APPS = Path(__file__).resolve().parents[3] / "webserver/src/x6100_webserver/apps.py"
BOOT_PICTURE_ROUTES = ("get_boot_picture", "boot_picture_png", "set_boot_picture", "delete_boot_picture")
BOOT_PICTURE_NAMES = BOOT_PICTURE_ROUTES + ("boot_picture_error", "boot_picture_state", "api_error", "replace_file",
                                            "fmt_size")
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


def production(names, consts=()):
    """apps.py's own functions, classes and constants of these names, run with the stand-ins in the namespace
    returned"""
    tree = ast.parse(APPS.read_text())
    nodes = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.ClassDef)) and node.name in names:
            node.decorator_list = []
            nodes.append(node)
        elif isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in consts for t in node.targets):
            nodes.append(node)
    assert {n.name for n in nodes if not isinstance(n, ast.Assign)} == set(names), "apps.py changed its names"
    ns = {"bottle": FakeBottle, "os": os, "datetime": datetime, "binascii": binascii, "settings": SimpleNamespace(),
          "tempfile": tempfile, "re": re, "json": json, "io": io, "shutil": shutil, "sqlite3": sqlite3,
          "tarfile": tarfile, "zlib": zlib, "threading": threading, "secrets": secrets}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "apps-boot-picture", "exec"), ns)
    return ns


class FakeBottle:
    request = SimpleNamespace(files={}, content_length=-1)
    response = SimpleNamespace(status=200, set_header=lambda k, v: None)

    @staticmethod
    def static_file(name, root, mimetype=None):
        path = Path(root) / name
        return SimpleNamespace(served=str(path) if path.is_file() else "404", mimetype=mimetype, headers={},
                               set_header=lambda k, v: None)


def post(ns, data, length=None):
    """The production upload route with a multipart field "file" of data (None: no field)"""
    FakeBottle.response.status = 200
    FakeBottle.request.files = {} if data is None else {"file": SimpleNamespace(file=io.BytesIO(data))}
    FakeBottle.request.content_length = len(data or b"") + 200 if length is None else length
    r = ns["set_boot_picture"]()
    return f"{FakeBottle.response.status} {r.get('msg') or r.get('status')}"


def png_of(head, *chunks):
    """A PNG with this IHDR (w, h, depth, colour type, interlace) and these chunks after it"""
    w, h, depth, colour, lace = head
    ihdr = w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes([depth, colour, 0, 0, lace])
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + b"".join(chunks) + chunk(b"IEND", b"")


def palette_png():
    """A PNG of 16 colours in a palette, as picture editors save a logo: pixel (x, y) is colour x % 16, (3i, 3i+1, 3i+2)"""
    return png_of((W, H, 8, 3, 0), chunk(b"PLTE", bytes(range(48))),
                  chunk(b"IDAT", zlib.compress(b"".join(b"\0" + bytes(x % 16 for x in range(W)) for _ in range(H)))))


def web_checks():
    ns = production(BOOT_PICTURE_NAMES, ("BOOT_PICTURE_MAX", "BOOT_PICTURE_SIZE", "PNG_DEPTHS"))
    own = Picture(lambda x, y: ((x + 40) & 255, (y + 20) & 255, (x ^ y) & 255)).png()
    palette = palette_png()
    with tempfile.TemporaryDirectory(prefix="x6bootpic-") as td:
        td = Path(td)
        ns["settings"].BOOT_PICTURE = str(td / "mnt" / "bootlogo.png")
        ns["settings"].BOOT_PICTURE_BUILTIN = str(td / "usr" / "bootlogo.png")
        (td / "mnt").mkdir()
        (td / "usr").mkdir()
        (td / "usr" / "bootlogo.png").write_bytes(b"ours")
        kept = Path(ns["settings"].BOOT_PICTURE)
        state = lambda: json.dumps(ns["get_boot_picture"](), sort_keys=True)
        shown = lambda: ns["boot_picture_png"]().served.replace(str(td), "")
        check("none of the owner's: the state", state(), '{"own": false}')
        check("none of the owner's: the picture shown on the page", shown(), "/usr/bootlogo.png")
        check("the owner's 800x480 picture uploaded", post(ns, own), "200 OK")
        check("  kept as it came", kept.read_bytes() == own, "True")
        st = ns["get_boot_picture"]()
        check("  the state", f"{st['own']} {st['size']} {st['error']!r}", f"True {len(own) // 1024} kB ''")
        check("  the picture shown on the page", shown(), "/mnt/bootlogo.png")
        check("a palette PNG (colour type 3)", post(ns, palette), "200 OK")
        check("a grey one of 16 bits", post(ns, png_of((W, H, 16, 0, 0), chunk(b"IDAT", zlib.compress(bytes(H * (1 + 2 * W)))))),
              "200 OK")
        post(ns, own)
        refused = [
            ("1024x600", Picture(lambda x, y: (x & 255, 0, 0), 1024, 600).png(),
             "400 The picture is 1024x600 pixels: the radio needs 800x480, the screen's size"),
            ("480x800 (the panel's own way)", png_of((480, 800, 8, 2, 0), chunk(b"IDAT", b"x")),
             "400 The picture is 480x800 pixels: the radio needs 800x480, the screen's size"),
            ("a JPEG", b"\xff\xd8\xff\xe0\0\x10JFIF\0" + bytes(4000), "400 Not a PNG picture"),
            ("an empty file", b"", "400 Not a PNG picture"),
            ("cut short", own[:len(own) // 2], "400 The file is cut short: upload it again"),
            ("cut short in its last chunk", own[:-1], "400 The file is cut short: upload it again"),
            ("damaged", own[:len(own) // 2] + bytes([own[len(own) // 2] ^ 0x55]) + own[len(own) // 2 + 1:],
             "400 The file is damaged: a checksum in it is wrong"),
            ("a chunk before IHDR", own[:8] + chunk(b"tEXt", b"a\0b") + own[8:], "400 Not a PNG picture: its header is missing"),
            ("RGB of 4 bits, no such PNG", png_of((W, H, 4, 2, 0), chunk(b"IDAT", b"x")),
             "400 Not a PNG picture the radio can read: an unknown kind of PNG"),
            ("an unknown interlace", png_of((W, H, 8, 2, 2), chunk(b"IDAT", b"x")),
             "400 Not a PNG picture the radio can read: an unknown kind of PNG"),
            ("no picture data", png_of((W, H, 8, 2, 0)), "400 Not a PNG picture: it has no picture data"),
            ("2 MB and a byte", png_of((W, H, 8, 2, 0), chunk(b"IDAT", b"x"), chunk(b"zTXt", bytes(2 * 1024 * 1024 - 64))),
             "413 Larger than 2 MB"),
            ("no file in the form", None, "400 No file"),
        ]
        for what, data, want in refused:
            check(f"refused: {what}", post(ns, data), want)
        check("  the picture kept as it was", kept.read_bytes() == own, "True")
        check("a body declared larger than 2 MB: refused before it is read", post(ns, own, 3 * 1024 * 1024),
              "413 Larger than 2 MB")
        check("nothing left beside it (each written to a name of its own, then put in place)",
              " ".join(sorted(p.name for p in (td / "mnt").iterdir())), "bootlogo.png")
        # one copied there by hand, as before this page (SFTP): the page says why the radio won't show it
        kept.write_bytes(Picture(lambda x, y: (x & 255, 0, 0), 640, 480).png())
        st = ns["get_boot_picture"]()
        check("one of 640x480 put there by hand: the state", f"{st['own']} {st['error']}",
              "True The picture is 640x480 pixels: the radio needs 800x480, the screen's size")
        FakeBottle.response.status = 200
        check("Default", f"{ns['delete_boot_picture']()['own']} {kept.exists()}", "False False")
        check("Default with none", f"{FakeBottle.response.status} {ns['delete_boot_picture']()['status']}", "200 OK")
        check("  ours shown on the page again", shown(), "/usr/bootlogo.png")
    # the changes are a POST and a DELETE, not one of POST_READS: refuse_other_sites takes them as changes (the
    # radio's own names, the page's own site and port), as every other change; the page and the picture are reads
    routes = {}
    for node in ast.parse(APPS.read_text()).body:
        if isinstance(node, ast.FunctionDef) and node.name in BOOT_PICTURE_ROUTES:
            for d in node.decorator_list:
                if isinstance(d, ast.Call) and isinstance(d.func, ast.Attribute):
                    routes[node.name] = f"{d.func.attr.upper()} {d.args[0].value}"
    check("the routes", ", ".join(routes[n] for n in sorted(routes)),
          "GET /api/boot_picture/png, DELETE /api/boot_picture, GET /api/boot_picture, POST /api/boot_picture")


BACKUP_NAMES = ("backup_files", "backup_member", "backup_current", "plural", "fmt_size", "read_text", "memory_rows",
                "describe_backup", "check_sqlite", "check_member", "unpack_backup", "backup_error", "download_backup",
                "backup_stage", "stage_token_path", "stage_token_new", "stage_token", "stage_clear", "stage_files",
                "install_file", "save_safety_copy", "apply_memory_slots", "restore_backup", "BackupError",
                "boot_picture_error")


def backup_of(members):
    """A backup file as the Backup page makes one: manifest.json and these members (name -> bytes)"""
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz") as tf:
        for name, data in [("manifest.json", json.dumps({"version": 1, "date": "2026-10-31 12:00"}).encode()),
                           *members.items()]:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return out.getvalue()


def backup_checks():
    ns = production(BACKUP_NAMES, ("BACKUP_MAX_BYTES", "BACKUP_MAX_MEMBERS", "BACKUP_TEXT_MAX", "BACKUP_NAME",
                                   "BACKUP_DIRS", "BACKUP_GUI_PARTS", "TAR_ERRORS", "MEM_SLOTS", "backup_lock",
                                   "BOOT_PICTURE_MAX", "BOOT_PICTURE_SIZE", "PNG_DEPTHS"))
    gui = []
    ns.update(read_callsign=lambda: "ON6URE", gui_service=lambda action: gui.append(action) or 0,
              gui_stop_for_restore=lambda: gui.append("stop") or None)
    own = Picture(lambda x, y: ((x + 40) & 255, (y + 20) & 255, (x ^ y) & 255)).png()
    rnd = random.Random(101)   # a picture that does not compress: over BACKUP_TEXT_MAX, under 2 MB
    noise = png_of((W, H, 8, 2, 0), chunk(b"IDAT", zlib.compress(b"".join(b"\0" + rnd.randbytes(3 * W) for _ in range(H)))))
    with tempfile.TemporaryDirectory(prefix="x6bootpicbackup-") as td:
        td = Path(td)
        mnt = td / "mnt"
        mnt.mkdir()
        s = ns["settings"]
        for name in ("CHANNELS_FILE", "CLUSTER_CONF", "TCI_CONF", "QSO_LOG_DB", "CW_TRAINER_FILE", "DB_PATH"):
            setattr(s, name, str(mnt / name.lower()))
        s.VOICE_DIR, s.WIFI_DIR = str(mnt / "msg"), str(td / "wifi")
        s.BACKUP_WORK, s.BACKUP_SAFETY = str(mnt / "backup_work"), str(mnt / "backup_before_restore")
        s.BOOT_PICTURE = str(mnt / "bootlogo.png")
        kept = Path(s.BOOT_PICTURE)
        parts = lambda: json.dumps(ns["describe_backup"](ns["backup_current"]()))
        check("Backup, none of the owner's: the parts on the radio", parts(), "{}")
        kept.write_bytes(own)
        check("Backup, the owner's: the parts on the radio", parts(), '{"bootpic": "your own, %d kB"}' % (len(own) // 1024))
        FakeBottle.request.query = {"parts": "bootpic"}
        with ns["download_backup"]() as f, tarfile.open(fileobj=f) as tf:
            names = tf.getnames()
            same = tf.extractfile("bootlogo.png").read() == own
            manifest = json.load(tf.extractfile("manifest.json"))
        check("the Backup file of Boot picture: its members", " ".join(names), "manifest.json bootlogo.png")
        check("  the picture as on the radio, the part in the manifest", f"{same} {manifest['parts']}",
              "True {'bootpic': 'your own, %d kB'}" % (len(own) // 1024))

        def restore(data, chosen=("bootpic",)):
            """The Backup page's Restore of a file: its upload (unpacked, checked) and the parts chosen"""
            archive = td / "upload.tar"
            archive.write_bytes(data)
            ns["stage_clear"]()
            os.makedirs(ns["backup_stage"](), mode=0o700)
            try:
                ns["unpack_backup"](str(archive), ns["backup_stage"]())
            except ns["BackupError"] as e:
                return f"refused: {e}"
            FakeBottle.request.json = {"parts": list(chosen), "token": ns["stage_token_new"]()}
            r = ns["restore_backup"]()
            return f"restored {r['restored']}, failed {r['failed']}"

        backup = backup_of({"bootlogo.png": own, "cluster.conf": b"host=dxfun.com\nport=8000\n"})
        kept.write_bytes(palette_png())
        check("Restore of Boot picture", restore(backup), "restored ['bootpic'], failed {}")
        check("  the picture back, the one it replaced kept aside, the radio's screen left running",
              f"{kept.read_bytes() == own} {(mnt / 'backup_before_restore' / 'bootlogo.png').read_bytes() == palette_png()} {gui}",
              "True True []")
        kept.write_bytes(palette_png())
        check("Restore of the cluster alone: the picture left", f"{restore(backup, ('cluster',))} {kept.read_bytes() == palette_png()}",
              "restored ['cluster'], failed {} True")
        check("Restore of one over 1 MB, the other parts' cap",
              f"{restore(backup_of({'bootlogo.png': noise}))} {kept.read_bytes() == noise}",
              "restored ['bootpic'], failed {} True")
        small = Picture(lambda x, y: (x & 255, 0, 0), 640, 480).png()
        refused = [
            ("640x480", backup_of({"bootlogo.png": small}),
             "refused: The boot picture in the file: The picture is 640x480 pixels: the radio needs 800x480, the screen's size"),
            ("cut short", backup_of({"bootlogo.png": own[:-20]}),
             "refused: The boot picture in the file: The file is cut short: upload it again"),
            ("over 2 MB", backup_of({"bootlogo.png": own + bytes(2 * 1024 * 1024)}), "refused: The file is too large"),
            ("a cluster.conf over 1 MB, as before", backup_of({"cluster.conf": b"x" * (1024 * 1024 + 1)}),
             "refused: The file is too large")]
        for what, data, want in refused:
            check(f"Restore of a file with a boot picture {what}" if "cluster" not in what else f"Restore of {what}",
                  restore(data), want)
        check("  the picture on the radio left as it was", kept.read_bytes() == noise, "True")


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
                 "small": small.png(), "jpeg": b"\xff\xd8\xff\xe0\0\x10JFIF\0" + bytes(4000), "palette": palette_png()}
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
        want_palette = Picture(lambda x, y: tuple(range(3 * (x % 16), 3 * (x % 16) + 3))).shown()

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
                    want_palette.tobytes(): "own, its palette's colours",
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
                    ("the owner's with a transparent half", "rgba", "own, the right half black"),
                    ("the owner's in a palette (the web check's, taken there)", "palette", "own, its palette's colours")):
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


web_checks()
backup_checks()
bootanim_checks(sys.argv[1:])
bad = [c for c in cases if c[1] != c[2]]
for what, got, want in cases:
    print(("ok   " if got == want else "FAIL ") + what + ": " + got + ("" if got == want else "  (want: " + want + ")"))
print("The boot picture (#101): %d of %d cases as they should be" % (len(cases) - len(bad), len(cases)))
assert not bad
