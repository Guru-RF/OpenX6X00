#!/usr/bin/env python3
"""Host test of the apps' pages of the web configurator (webserver/src/x6100_webserver/apps.py, views/wefax.html).
See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/apps_web/apps_web_test.py [-v]

1. WeFax: the page's stations are the app's (src/wefax_window.c stations[], read from the source), a channel's
   name as the app lists it (channel_name there), the channels named WEFAX after the stations as the app's Freq.
   list has them (none within 50 Hz of a station, none past INT32_MAX).
2. Through bottle's WSGI app (skipped without BOTTLE): the page, in the menu and on General, with the list in it as
   JavaScript that a channel's name cannot break out of.
Its files go in build/ (the Makefile empties it first).
"""
import io
import json
import os
import re
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
    from x6100_webserver import apps, settings  # noqa: E402
except ImportError:
    bottle = apps = settings = None

BUILD = os.path.join(HERE, "build")
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

    def test_general_has_the_tile(self):
        code, page, _ = self.req("/")
        self.assertEqual(code, 200)
        self.assertIn(b'class="tile" href="/wefax"', page)


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
