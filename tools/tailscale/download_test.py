#!/usr/bin/env python3
"""The download of Tailscale (rootfs/usr/sbin/x6100-tailscale, download()): its own code against a web server on
127.0.0.1 that serves a small tarball made like Tailscale's and cuts the link where a case says (#262). See the
Makefile:

    make          (python3 -I -B download_test.py [-v])

1. A link cut mid-way: the try says where it broke off and keeps what came; the next one asks for the rest only
   (an HTTP range), and tailscaled comes out of the whole.
2. Cut twice, then whole: three tries, each going on from the last.
3. A server that ignores the range and sends it all again: taken from the start, not added to the part.
4. A part that is wrong (bytes that changed on the card): the whole file fails its check and is deleted, the next
   try starts again at 0.
5. A part of another version is deleted, not taken further.
6. Turned off during the download: nothing left on the card.
"""
import hashlib
import http.server
import io
import os
import random
import runpy
import shutil
import tarfile
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-tailscale")
VERSION = "9.9.9"
PROGRAM = random.Random(262).randbytes(300_000)        # stands in for tailscaled: not packed smaller by gzip


def tarball():
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        info = tarfile.TarInfo(f"tailscale_{VERSION}_arm/tailscaled")
        info.size = len(PROGRAM)
        t.addfile(info, io.BytesIO(PROGRAM))
        info = tarfile.TarInfo(f"tailscale_{VERSION}_arm/README")
        info.size = 5
        t.addfile(info, io.BytesIO(b"hello"))
    return buf.getvalue()


TGZ = tarball()


class Server(http.server.BaseHTTPRequestHandler):
    cuts = []                  # bytes of the body each answer sends before it closes the link; None: all of it
    ignore_range = False
    ranges = []                # the Range header of each request

    def do_GET(self):
        rng = self.headers.get("Range")
        Server.ranges.append(rng)
        start = 0
        if rng and not Server.ignore_range:
            start = int(rng.split("=")[1].split("-")[0])
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(TGZ) - 1}/{len(TGZ)}")
        else:
            self.send_response(200)
        body = TGZ[start:]
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        cut = Server.cuts.pop(0) if Server.cuts else None
        self.wfile.write(body if cut is None else body[:cut])
        self.wfile.flush()
        self.close_connection = True

    def log_message(self, *args):
        pass


class Download(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Server)
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.g = runpy.run_path(SCRIPT)["download"].__globals__
        self.g.update(VERSION=VERSION, DIR=self.dir, BIN=os.path.join(self.dir, "tailscaled"),
                      TGZ_URL=f"http://127.0.0.1:{self.httpd.server_port}/tailscale_{VERSION}_arm.tgz",
                      TGZ_SHA256=hashlib.sha256(TGZ).hexdigest(), TGZ_SIZE=len(TGZ),
                      BIN_SHA256=hashlib.sha256(PROGRAM).hexdigest())
        self.said = []
        self.g["say"] = lambda state, text, **kw: self.said.append(text)
        self.off = False
        self.g["stopping"] = lambda: self.off
        Server.cuts, Server.ignore_range, Server.ranges = [], False, []
        self.part = os.path.join(self.dir, f"download-{VERSION}.part")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def download(self):
        self.g["download"]()

    def fails(self, text):
        with self.assertRaises(self.g["Fail"]) as e:
            self.download()
        self.assertIn(text, str(e.exception))

    def program_out(self):
        with open(self.g["BIN"], "rb") as f:
            self.assertEqual(f.read(), PROGRAM)
        self.assertEqual([n for n in os.listdir(self.dir) if n.endswith(".part")], [])

    def test_1_cut_goes_on(self):
        Server.cuts = [len(TGZ) // 3]
        self.fails("broke off")
        self.assertEqual(os.path.getsize(self.part), len(TGZ) // 3)
        self.download()
        self.assertEqual(Server.ranges, [None, f"bytes={len(TGZ) // 3}-"])
        self.program_out()

    def test_2_cut_twice(self):
        Server.cuts = [1000, 50_000]
        self.fails("broke off")
        self.fails("broke off")
        self.assertEqual(os.path.getsize(self.part), 51_000)
        self.download()
        self.assertEqual(Server.ranges, [None, "bytes=1000-", "bytes=51000-"])
        self.program_out()

    def test_3_range_ignored(self):
        Server.cuts = [len(TGZ) // 2]
        self.fails("broke off")
        Server.ignore_range = True
        self.download()
        self.program_out()

    def test_4_wrong_part(self):
        with open(self.part, "wb") as f:
            f.write(b"\0" * 1000)                  # not what the first 1000 bytes are
        self.fails("does not match its checksum")
        self.assertFalse(os.path.exists(self.part))
        self.download()
        self.assertEqual(Server.ranges, ["bytes=1000-", None])
        self.program_out()

    def test_5_other_version(self):
        old = os.path.join(self.dir, "download-1.0.0.part")
        older = os.path.join(self.dir, "download.part")          # what v0.3.12 and before named it
        for p in (old, older):
            with open(p, "wb") as f:
                f.write(TGZ[:1000])
        self.download()
        self.assertEqual(Server.ranges, [None])
        self.assertFalse(os.path.exists(old) or os.path.exists(older))
        self.program_out()

    def test_6_turned_off(self):
        Server.cuts = [len(TGZ) // 3]
        self.fails("broke off")
        self.off = True
        with self.assertRaises(self.g["Stopped"]):
            self.download()
        self.assertFalse(os.path.exists(self.part))


if __name__ == "__main__":
    unittest.main()
