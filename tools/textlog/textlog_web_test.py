#!/usr/bin/env python3
"""Tests of the web configurator's side of the keyboard modes' text log (#55: /api/textlog, /textlog/<day>) on a
PC, through bottle's WSGI app, the folder a new temporary directory. Needs bottle (0.13.4 here):

    PYTHONPATH=<a directory with bottle.py> python3 -B tools/textlog/textlog_web_test.py [-v]
"""
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "webserver", "src"))
sys.dont_write_bytecode = True
try:
    import bottle  # noqa: F401
except ImportError:
    sys.exit("bottle is not installed: PYTHONPATH=<a directory with bottle.py> python3 -B " + sys.argv[0])
from x6100_webserver import apps, settings  # noqa: E402

DAY1 = "2026-10-30 21:00:00Z 14.071000 PSK31 RX Q87% CQ CQ DE PA3XYZ K\n"
DAY2 = "2026-10-31 14:05:12Z 14.090850 JTTY RX -12dB <script>alert(1)</script>\n"


class Textlog(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-textlog-web.")
        self.addCleanup(shutil.rmtree, self.root)
        self.addCleanup(setattr, settings, "TEXTLOG_DIR", settings.TEXTLOG_DIR)
        settings.TEXTLOG_DIR = os.path.join(self.root, "digi_text")
        os.mkdir(settings.TEXTLOG_DIR)
        for name, text in (("2026-10-30.txt", DAY1), ("2026-10-31.txt", DAY2), ("notes.txt", "mine\n")):
            with open(os.path.join(settings.TEXTLOG_DIR, name), "w") as f:
                f.write(text)

    def req(self, method, path, query="", headers=None):
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "QUERY_STRING": query, "HTTP_HOST": "192.168.1.10"}
        env.update(headers or {})
        env["wsgi.input"] = io.BytesIO(b"")
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])
            got["headers"] = {k.lower(): v for k, v in hdrs}

        it = apps.app(env, start)
        out = b"".join(it)
        if hasattr(it, "close"):    # a file's answer: its file closed, as a WSGI server does
            it.close()
        try:
            return got["status"], got["headers"], json.loads(out)
        except ValueError:
            return got["status"], got["headers"], out

    def test_list(self):
        code, _, r = self.req("GET", "/api/textlog")
        self.assertEqual(code, 200)
        self.assertEqual([f["name"] for f in r["files"]], ["2026-10-31.txt", "2026-10-30.txt"])   # newest first
        self.assertEqual(r["files"][0]["day"], "2026-10-31")
        self.assertEqual(r["files"][1]["bytes"], len(DAY1))
        self.assertEqual(r["total"], "1 kB")

    def test_no_folder(self):
        shutil.rmtree(settings.TEXTLOG_DIR)
        code, _, r = self.req("GET", "/api/textlog")
        self.assertEqual((code, r["files"]), (200, []))

    def test_read(self):
        code, h, body = self.req("GET", "/textlog/2026-10-31.txt")
        self.assertEqual(code, 200)
        self.assertEqual(body.decode(), DAY2)
        self.assertEqual(h["content-type"], "text/plain; charset=UTF-8")    # what came over the air is never a page
        self.assertEqual(h["x-content-type-options"], "nosniff")
        self.assertNotIn("content-disposition", h)

    def test_download(self):
        code, h, body = self.req("GET", "/textlog/2026-10-30.txt", "dl=1")
        self.assertEqual(code, 200)
        self.assertEqual(body.decode(), DAY1)
        self.assertIn('attachment; filename="2026-10-30.txt"', h["content-disposition"])

    def test_not_a_day(self):
        for name in ("notes.txt", "..%2Fparams.db", "2026-10-31.txt.bak", "2026-1-31.txt"):
            code, _, _ = self.req("GET", "/textlog/" + name)
            self.assertEqual(code, 404, name)
        code, _, r = self.req("DELETE", "/api/textlog/notes.txt")
        self.assertEqual(code, 400)
        self.assertTrue(os.path.exists(os.path.join(settings.TEXTLOG_DIR, "notes.txt")))

    def test_delete(self):
        code, _, r = self.req("DELETE", "/api/textlog/2026-10-30.txt")
        self.assertEqual(code, 200)
        self.assertEqual([f["name"] for f in r["files"]], ["2026-10-31.txt"])
        self.assertFalse(os.path.exists(os.path.join(settings.TEXTLOG_DIR, "2026-10-30.txt")))
        code, _, r = self.req("DELETE", "/api/textlog/2026-10-30.txt")
        self.assertEqual(code, 404)

    def test_delete_from_another_site(self):
        code, _, _ = self.req("DELETE", "/api/textlog/2026-10-30.txt", headers={"HTTP_ORIGIN": "http://evil.example"})
        self.assertEqual(code, 403)
        code, _, _ = self.req("DELETE", "/api/textlog/2026-10-30.txt", headers={"HTTP_HOST": "evil.example"})
        self.assertEqual(code, 403)
        self.assertTrue(os.path.exists(os.path.join(settings.TEXTLOG_DIR, "2026-10-30.txt")))

    def test_page(self):
        code, _, body = self.req("GET", "/logs")
        self.assertEqual(code, 200)
        self.assertIn(b'id="textlog"', body)
        self.assertIn(b"loadTextlog()", body)


if __name__ == "__main__":
    unittest.main()
