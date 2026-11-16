#!/usr/bin/env python3
"""Host test of the apps' pages of the web configurator (webserver/src/x6100_webserver/apps.py, views/wefax.html).
See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/apps_web/apps_web_test.py [-v]

1. WeFax: the page's stations are the app's (src/wefax_window.c stations[], read from the source), a channel's
   name as the app lists it (channel_name there), the channels named WEFAX after the stations as the app's Freq.
   list has them (none within 50 Hz of a station, none past INT32_MAX).
2. Through bottle's WSGI app (skipped without BOTTLE): each app's page, in the menu and on General, what it puts
   into its script as JavaScript that a name cannot break out of, and every file it loads from /static there.
3. src/remote_server.c: a page's "watching" words ("wefax rows 1", "ft8 wf 1", "js8 wf 1") are taken before the
   app's own branch, which wants operate and control: after it, a viewer's "ft8 wf 1" was refused as an unknown
   command. "js8 send TEXT" is taken before the words are split (its text has spaces), a text message of 500
   characters fits TEXT_MAX, and the JS8 page's ring and lines are as the page reads them.
Its files go in build/ (the Makefile empties it first).
"""
import io
import json
import os
import re
import sqlite3
import sys
import types
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
SRC = os.path.join(TOP, "src")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
try:
    import cgi  # noqa: F401
except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for forms only
    cgi = types.ModuleType("cgi")
    cgi.FieldStorage = object
    sys.modules["cgi"] = cgi
try:
    import bottle  # noqa: F401
    from x6100_webserver import apps, config_pages, settings  # noqa: E402
except ImportError:
    bottle = apps = config_pages = settings = None

BUILD = os.path.join(HERE, "build")
APPS = ("ft8", "js8", "wefax")       # the Apps menu's pages
runs = 0


def fresh_dir():
    """A folder of its own for the next case, under build/"""
    global runs
    runs += 1
    path = os.path.join(BUILD, f"case-{os.getpid()}-{runs}")
    os.makedirs(path, exist_ok=True)
    return path


def source(name):
    with open(os.path.join(SRC, name), encoding="utf-8") as f:
        return f.read()


def c_stations():
    """stations[] of src/wefax_window.c as [(group, [(name, Hz, note)])]"""
    body = re.search(r"static const freq_list_item_t stations\[\] = \{(.*?)\n\};", source("wefax_window.c"), re.S)
    groups = []
    for line in body.group(1).splitlines():
        g = re.search(r'\{\.group = "([^"]+)"\}', line)
        s = re.search(r'\{\.freq_hz = (\d+), \.name = "([^"]+)"(?:, \.note = "([^"]*)")?\}', line)
        if g:
            groups.append((g.group(1), []))
        elif s:
            groups[-1][1].append((s.group(2), int(s.group(1)), s.group(3) or ""))
    return groups


def write_channels(path, channels):
    with open(path, "w") as f:
        json.dump({"version": 1, "channels": [dict({"mode": "USB", "agc": "SLOW", "pre": False, "att": False}, **c)
                                              for c in channels]}, f)


@unittest.skipIf(apps is None, "no bottle: give BOTTLE=<a folder with bottle.py>")
class WefaxStations(unittest.TestCase):
    def setUp(self):
        self.addCleanup(setattr, settings, "CHANNELS_FILE", settings.CHANNELS_FILE)
        settings.CHANNELS_FILE = os.path.join(fresh_dir(), "channels.json")

    def test_the_apps_list(self):
        theirs = c_stations()
        self.assertEqual(len(theirs), 8)                  # DWD, Northwood, Athens and NOAA's five
        self.assertEqual([(g, [tuple(s) for s in st]) for g, st in apps.WEFAX_STATIONS], theirs)
        self.assertIn("#define WEFAX_CENTER_HZ     %d " % apps.WEFAX_CENTER_HZ, source("wefax/decoder.h"))
        self.assertIn("#define SAME_STATION_HZ %d" % apps.WEFAX_SAME_HZ, source("wefax_window.c"))

    def test_channel_name(self):
        """channel_name in src/wefax_window.c: its separators skipped, '#' dropped, the whole name when nothing
        follows (so "WEFAX #" is not listed with an empty name)"""
        for name, shown in (("WEFAX DWD DDK3", "DWD DDK3"), ("WEFAX-Kyodo #1", "Kyodo 1"), ("wefax_:./ JMH", "JMH"),
                            ("WEFAX", "WEFAX"), ("WEFAX #", "WEFAX "), ("WEFAX##", "WEFAX"), ("WEFAXJMH", "JMH")):
            self.assertEqual(apps.wefax_channel_name(name), shown, name)

    def test_with_the_channels(self):
        self.assertEqual(apps.wefax_stations()[-1][0], "Honolulu (US)")      # no channels.json: the stations alone
        write_channels(settings.CHANNELS_FILE, [
            {"name": "WEFAX DWD DDK3", "frequency": 7880000 - 1500 + 30},    # the station itself, 30 Hz off
            {"name": "WEFAX Kyodo", "frequency": 12745500},
            {"name": "wefax JMH #2", "frequency": 7795000},
            {"name": "Club net", "frequency": 3650000},                      # not WEFAX
            {"name": "WEFAX far", "frequency": 0x7FFFFFFF - 1000},           # past INT32_MAX once centred
        ])
        groups = apps.wefax_stations()
        self.assertEqual(groups[-1], ["Channels", [["Kyodo", 12747000, ""], ["JMH 2", 7796500, ""]]])
        self.assertEqual(len(groups), 9)


@unittest.skipIf(apps is None, "no bottle: give BOTTLE=<a folder with bottle.py>")
class Pages(unittest.TestCase):
    def setUp(self):
        d = fresh_dir()
        for name in ("CHANNELS_FILE", "DB_PATH"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        settings.CHANNELS_FILE = os.path.join(d, "channels.json")
        settings.DB_PATH = os.path.join(d, "params.db")

    def req(self, path, headers=None):
        env = {"REQUEST_METHOD": "GET", "PATH_INFO": path, "HTTP_HOST": "192.168.1.10"}
        env.update(headers or {})
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])
            got["headers"] = dict(hdrs)

        out = b"".join(apps.app(env, start))
        return got["status"], out, got["headers"]

    def test_wefax_page(self):
        write_channels(settings.CHANNELS_FILE, [{"name": "WEFAX </script><b>x", "frequency": 9000000}])
        code, page, _ = self.req("/wefax")
        self.assertEqual(code, 200)
        self.assertIn(b'<a href="/wefax"', page)                   # in the menu, under Apps
        self.assertIn(b"Apps", page)
        listed = re.search(rb"const STATIONS = (.*?);   /\*", page).group(1)
        self.assertNotIn(b"</", listed)                             # no way out of the script
        self.assertEqual(json.loads(listed), apps.wefax_stations())
        self.assertEqual(json.loads(listed)[-1], ["Channels", [["</script><b>x", 9001500, ""]]])
        self.assertIn(b'"wefax rows 1"', page)

    def test_ft8_and_js8_pages(self):
        con = sqlite3.connect(settings.DB_PATH)
        con.execute("CREATE TABLE params (name TEXT PRIMARY KEY, val TEXT)")
        con.executemany("INSERT INTO params VALUES (?, ?)", [("callsign", 'ON0X"</script>'), ("qth", "JO20sv")])
        con.commit()
        con.close()
        for name in ("ft8", "js8"):
            code, page, _ = self.req("/" + name)
            self.assertEqual(code, 200)
            self.assertIn(b'<a href="/%s"' % name.encode(), page)
            m = re.search(rb"const MYCALL = (.*?)\.toUpperCase\(\), MYGRID = (.*?)\.toUpperCase\(\);", page)
            self.assertEqual((json.loads(m.group(1)), json.loads(m.group(2))), ('ON0X"</script>', "JO20sv"), name)
            self.assertNotIn(b"</", m.group(0))
            self.assertIn(b'"%s wf 1"' % name.encode(), page)

    def test_js8_page_room(self):
        """The box takes what the radio sends (TX_TEXT_MAX in js8_window.c), and no more"""
        tx_max = int(re.search(r"#define TX_TEXT_MAX +(\d+)", source("js8_window.c")).group(1))
        code, page, _ = self.req("/js8")
        self.assertEqual(set(re.findall(rb'maxlength="(\d+)"', page)), {str(tx_max).encode()})

    def test_general_has_the_tiles(self):
        code, page, _ = self.req("/")
        self.assertEqual(code, 200)
        for name in APPS:
            self.assertIn(b'class="tile" href="/%s"' % name.encode(), page)

    def test_static_files(self):
        """What the apps' pages load from /static is there, served (the map's land and the satellite picture,
        which app_wall.js asks for, too)"""
        for name in APPS:
            code, page, _ = self.req("/" + name)
            loads = set(re.findall(rb'(?:src|href)="/static/([^"?]+)', page)) | {b"img/world.svg", b"img/world_sat.jpg"}
            for f in loads:
                code, body, _ = self.req("/static/" + f.decode())
                self.assertEqual(code, 200, (name, f))
                self.assertTrue(body, (name, f))

    def test_satellite_picture(self):
        """Its cost on the radio's root file system stays small, and it goes out as it is: gzip would gain
        nothing on a JPEG (a script still goes gzipped)"""
        path = os.path.join(apps.PACKAGE_DIR, "static", "img", "world_sat.jpg")
        self.assertLess(os.path.getsize(path), 300_000)
        code, body, hdrs = self.req("/static/img/world_sat.jpg", {"HTTP_ACCEPT_ENCODING": "gzip, deflate"})
        self.assertEqual((code, body[:3], hdrs.get("Content-Encoding")), (200, b"\xff\xd8\xff", None))
        code, body, hdrs = self.req("/static/js/app_wall.js", {"HTTP_ACCEPT_ENCODING": "gzip"})
        self.assertEqual(hdrs.get("Content-Encoding"), "gzip" if config_pages.gzip else None)


class RemoteWords(unittest.TestCase):
    def test_watching_before_the_apps_commands(self):
        src = source("remote_server.c")
        body = src[src.index("static void handle_text(client_t *c, char *text)"):]
        for app, watch in (("wefax", '!strcmp(a[0], "wefax") && n >= 3 && !strcmp(a[1], "rows")'),
                           ("ft8", '!strcmp(a[0], "ft8") || !strcmp(a[0], "js8")) && n >= 3 && !strcmp(a[1], "wf")'),
                           ("js8", '!strcmp(a[0], "ft8") || !strcmp(a[0], "js8")) && n >= 3 && !strcmp(a[1], "wf")')):
            self.assertLess(body.index(watch), body.index('!strcmp(a[0], "%s") && n >= 2)' % app), app)
        self.assertLess(body.index('!strncmp(text, "js8 send ", 9)'), body.index("strtok(text,"))

    def test_text_room(self):
        """"js8 send " and the page's 500 characters (TX_TEXT_MAX in js8_window.c, the page's maxlength) fit a
        text message and the command's queue"""
        src = source("remote_server.c")
        text_max = int(re.search(r"#define TEXT_MAX +(\d+)", src).group(1))
        tx_max = int(re.search(r"#define TX_TEXT_MAX +(\d+)", source("js8_window.c")).group(1))
        acmd_text = int(re.search(r"char +text\[(\d+)\]; +/\* A_LOG's QSO", src).group(1))
        self.assertGreater(text_max, len("js8 send ") + tx_max)
        self.assertGreater(acmd_text, tx_max)


if __name__ == "__main__":
    os.makedirs(BUILD, exist_ok=True)
    out = io.StringIO()
    result = unittest.TextTestRunner(stream=out, verbosity=2 if "-v" in sys.argv else 1).run(
        unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    if not result.wasSuccessful() or "-v" in sys.argv:
        sys.stderr.write(out.getvalue())
    skipped = f", {len(result.skipped)} skipped" if result.skipped else ""
    print(f"apps_web_test: {result.testsRun} tests, {len(result.failures) + len(result.errors)} failed{skipped}")
    sys.exit(0 if result.wasSuccessful() else 1)
