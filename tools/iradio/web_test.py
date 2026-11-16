#!/usr/bin/env python3
"""The web configurator's Internet radio page on a PC: its favourites API (/api/iradio) through bottle's WSGI app,
with the web server's own code, the favourites file in a new temporary directory. See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/iradio/web_test.py [-v]

Without BOTTLE (no buildroot tree next to this repository) it says so and passes: the other two tests of the folder
need nothing. The file's lines are the ones src/iradio_list.c reads: name<TAB>address<TAB>codec<TAB>kbps<TAB>
country<TAB>uuid.
"""
import io
import json
import os
import shutil
import sys
import tempfile
import types
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
try:
    import cgi  # noqa: F401
except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for forms only
    cgi = types.ModuleType("cgi")

    class FieldStorage:
        def __init__(self, **kw):
            raise NotImplementedError("no cgi module on this Python")
    cgi.FieldStorage = FieldStorage
    sys.modules["cgi"] = cgi
try:
    import bottle  # noqa: F401
except ImportError:
    print("web_test.py: skipped, bottle not found (BOTTLE=<a folder with bottle.py>)")
    sys.exit(0)
from x6100_webserver import apps, settings  # noqa: E402

UUID = "96062a7b-0601-11e8-ae97-52543be04c81"


class Favourites(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-iradio-web.")
        self.addCleanup(shutil.rmtree, self.root)
        for name in ("IRADIO_FAV_FILE", "FFMPEG"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        settings.IRADIO_FAV_FILE = os.path.join(self.root, "iradio", "favorites.tsv")    # its folder not there yet
        settings.FFMPEG = os.path.join(self.root, "no-ffmpeg")

    def req(self, method, path, body=None):
        data = b"" if body is None else json.dumps(body).encode()
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "HTTP_HOST": "192.168.1.10",
               "CONTENT_TYPE": "application/json", "CONTENT_LENGTH": str(len(data)), "wsgi.input": io.BytesIO(data)}
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])

        out = b"".join(apps.app(env, start))
        try:
            return got["status"], json.loads(out)
        except ValueError:
            return got["status"], out

    def ok(self, method, path, body=None):
        code, r = self.req(method, path, body)
        self.assertEqual(code, 200, r)
        return r

    def refused(self, method, path, body, words):
        code, r = self.req(method, path, body)
        self.assertEqual(code, 400, r)
        self.assertIn(words, r["msg"])

    def lines(self):
        with open(settings.IRADIO_FAV_FILE, encoding="utf-8") as f:
            return f.read().splitlines()

    def write(self, text):
        os.makedirs(os.path.dirname(settings.IRADIO_FAV_FILE), exist_ok=True)
        with open(settings.IRADIO_FAV_FILE, "w", encoding="utf-8") as f:
            f.write(text)

    def test_empty(self):
        self.assertEqual(self.ok("GET", "/api/iradio"), {"stations": [], "max": 150, "every": False})

    def test_add_edit_move_delete(self):
        r = self.ok("POST", "/api/iradio", {"name": "  Radio  One ", "url": " http://one.example.org/s.mp3 ",
                                            "codec": "mp3", "kbps": "128", "cc": "be"})
        self.assertEqual(r["stations"], [{"id": 0, "name": "Radio One", "url": "http://one.example.org/s.mp3",
                                          "codec": "MP3", "kbps": "128", "cc": "BE", "uuid": ""}])
        self.ok("POST", "/api/iradio", {"name": "Ràdio Dos", "url": "https://two.example.org/live"})
        self.assertEqual(self.lines(), ["Radio One\thttp://one.example.org/s.mp3\tMP3\t128\tBE\t",
                                        "Ràdio Dos\thttps://two.example.org/live\t\t\t\t"])
        self.ok("POST", "/api/iradio/1/up")
        self.assertEqual([s["name"] for s in self.ok("GET", "/api/iradio")["stations"]], ["Ràdio Dos", "Radio One"])
        r = self.ok("PUT", "/api/iradio/1", {"name": "Radio 1", "url": "http://one.example.org/s.mp3", "codec": "AAC+"})
        self.assertEqual(r["stations"][1]["codec"], "AAC+")
        self.ok("DELETE", "/api/iradio/0")
        self.assertEqual(self.lines(), ["Radio 1\thttp://one.example.org/s.mp3\tAAC+\t\t\t"])
        self.refused("DELETE", "/api/iradio/5", None, "reload the page")
        self.assertFalse([n for n in os.listdir(os.path.dirname(settings.IRADIO_FAV_FILE)) if n.endswith(".tmp")])

    def test_uuid_kept_while_the_address_stays(self):
        self.write("From the radio\thttp://x.example.org/\tMP3\t128\tUY\t%s\n" % UUID)
        r = self.ok("PUT", "/api/iradio/0", {"name": "Renamed", "url": "http://x.example.org/", "codec": "MP3"})
        self.assertEqual(r["stations"][0]["uuid"], UUID)
        r = self.ok("PUT", "/api/iradio/0", {"name": "Renamed", "url": "http://y.example.org/"})
        self.assertEqual(r["stations"][0]["uuid"], "")

    def test_refusals(self):
        self.ok("POST", "/api/iradio", {"name": "One", "url": "http://one.example.org/"})
        for body, words in (({"name": "", "url": "http://a/"}, "Name"),
                            ({"name": "é" * 32, "url": "http://a/"}, "Name"),
                            ({"name": "A", "url": "ftp://a/"}, "Address"),
                            ({"name": "A", "url": "http://a/b c"}, "Address"),
                            ({"name": "A", "url": "http://a/\"x"}, "Address"),
                            ({"name": "A", "url": "http://a/" + "x" * 250}, "Address"),
                            ({"name": "A", "url": "http://a/", "codec": "MP3 128"}, "Format"),
                            ({"name": "A", "url": "http://a/", "kbps": "fast"}, "kbps"),
                            ({"name": "A", "url": "http://a/", "cc": "BEL"}, "Country"),
                            ({"name": "Again", "url": "http://one.example.org/"}, "in the list already"),
                            ("not an object", "Invalid request")):
            self.refused("POST", "/api/iradio", body, words)
        self.assertEqual(len(self.lines()), 1)

    def test_full(self):
        self.write("".join("S%d\thttp://s%d.example.org/\n" % (i, i) for i in range(150)))
        self.refused("POST", "/api/iradio", {"name": "One more", "url": "http://more.example.org/"}, "150 places")

    def test_reads_as_the_gui(self):
        """Lines the GUI skips are not shown and go when the list is written again; a long name is cut as the GUI
        cuts it"""
        self.write("Good\thttp://good.example.org/\taac\t64\tbe\tnot-a-uuid\r\n"
                   "no address\n\tftp://x/\n\nNo tab http://x/\n"
                   + "é" * 40 + "\thttps://long.example.org/\n")
        st = self.ok("GET", "/api/iradio")["stations"]
        self.assertEqual([(s["name"][:4], s["codec"], s["cc"], s["uuid"]) for s in st],
                         [("Good", "AAC", "BE", ""), ("é" * 4, "", "", "")])
        self.assertEqual(len(st[1]["name"].encode()), 62)

    def test_formats_flag(self):
        with open(settings.FFMPEG, "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(settings.FFMPEG, 0o755)
        self.assertTrue(self.ok("GET", "/api/iradio")["every"])

    def test_page(self):
        code, out = self.req("GET", "/iradio")
        self.assertEqual(code, 200)
        self.assertIn(b"/api/iradio", out)
        self.assertIn(b"radio-browser.info", out)


if __name__ == "__main__":
    unittest.main()
