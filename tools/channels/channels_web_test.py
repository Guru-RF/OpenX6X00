#!/usr/bin/env python3
"""Tests of the web configurator's Memories page on a PC (#158): the named channel list (/api/channels) and the MEM
slots (/api/memories), the web server's own code through bottle's WSGI app, channels.json, params.db and the device
tree's model in a new temporary directory. See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/channels/channels_web_test.py [-v]

WEBSERVER=<a webserver/src> tests another web server (make v038: v0.3.8's, which has to fail).

The GUI's rules for an entry of channels.json (parse_entry in src/channel_list.c) are written out here once more,
in gui_keeps(), as the radio reads the file: the page has to show what the radio shows, and leave in the file what
it does not show. The presets are the GUI's own, read from src/channel_list.c.
"""
import inspect
import io
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import types
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + \
    [os.environ.get("WEBSERVER") or os.path.join(TOP, "webserver", "src")]
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
    sys.exit("bottle not found: give BOTTLE=<a folder with bottle.py>, to make or in the environment")
from x6100_webserver import apps, settings  # noqa: E402

X6100 = b"xiegu,x6100\0allwinner,sun8i-r16\0"
X6200 = b"xiegu,x6200\0allwinner,sun50i-h616\0"
# The GUI's mode and AGC names (mode_names, agc_names in src/channel_list.c), by the radio_mode_ of the presets
MODES = {"usb": "USB", "lsb": "LSB", "usb_dig": "USB-D", "lsb_dig": "LSB-D", "cw": "CW", "cwr": "CWR", "am": "AM",
         "nfm": "NFM", "sam": "SAM", "wfm": "WFM"}
AGCS = ("AUTO", "OFF", "SLOW", "FAST")


def gui_presets():
    """The channels a new radio starts with, from src/channel_list.c"""
    with open(os.path.join(TOP, "src", "channel_list.c")) as f:
        block = f.read().split("presets[] = {", 1)[1].split("};", 1)[0]
    found = re.findall(r'\{\s*"([^"]*)",\s*(\d+),\s*radio_mode_(\w+),\s*radio_agc_(\w+)\s*\}', block)
    assert len(found) >= 10, "the presets of src/channel_list.c not found"
    return [ch(n, int(f), MODES[m], a.upper()) for n, f, m, a in found]


def ch(name, freq, mode="USB", agc="AUTO", pre=False, att=False):
    return {"name": name, "frequency": freq, "mode": mode, "agc": agc, "pre": pre, "att": att}


def first_wins(pairs):
    """A JSON object as cJSON looks a key up in it: the first value of a key given twice"""
    obj = {}
    for k, v in pairs:
        obj.setdefault(k, v)
    return obj


def meaning(data):
    """channels.json as the GUI reads it, each value in a form that keeps its type (1 is not 1.0 nor true) and the
    bytes of each string (UTF-8 or not)"""
    doc = json.loads(data.decode("utf-8", "surrogateescape").lstrip("\ufeff"), object_pairs_hook=first_wins,
                     strict=False)
    return [json.dumps(x, ensure_ascii=False) for x in doc["channels"]]


def gui_keeps(x):
    """The channel the GUI makes of an entry, None for one it leaves out (parse_entry, src/channel_list.c): a name,
    a number for the frequency of more than 0 and at most 4294967295 (cut to whole hertz), a mode and an AGC of its
    tables, PRE and ATT booleans; the name cut at 31 bytes (CHANNEL_NAME_MAX)"""
    if not isinstance(x, dict):
        return None
    f = x.get("frequency")
    if not isinstance(x.get("name"), str) or isinstance(f, bool) or not isinstance(f, (int, float)) \
            or not isinstance(x.get("pre"), bool) or not isinstance(x.get("att"), bool):
        return None
    if not 0 < f <= 0xFFFFFFFF or x.get("mode") not in MODES.values() or x.get("agc") not in AGCS:
        return None
    name = x["name"].encode("utf-8", "surrogateescape")[:31].decode("utf-8", "surrogateescape")
    return ch(name, int(f), x["mode"], x["agc"], x["pre"], x["att"])


# Entries the GUI keeps that v0.3.8's page could not show: above 56 MHz, below 0.1 MHz, names outside its 31
# printable ASCII characters, a fraction of a hertz
OUT_OF_RANGE = [
    ch("", 98_500_000, "WFM"),                    # FM broadcast, added on an X6200 (#158)
    ch("TOWER", 118_100_000, "AM"),               # the airband
    ch("2M", 144_300_000, "USB"),                 # a transverter
    ch("WR12 70CM", 433_500_000, "NFM"),          # the WR12
    ch("DCF77", 77_500, "CW"),                    # under 0.1 MHz
    ch("1 HZ", 1),
    ch("TOP", 0xFFFFFFFF),
    ch("M\u00e9t\u00e9o", 7_074_000),             # UTF-8
    ch("A NAME OF FORTY CHARACTERS, NINE MORE...", 3_573_000),
    ch("TAB\tIN IT", 10_136_000),
    {"name": "HALF", "frequency": 14_074_000.5, "mode": "USB-D", "agc": "AUTO", "pre": False, "att": False},
]

# Entries the GUI leaves out, one by one
LEFT_OUT = [
    "not an object", 7_074_000, None, [ch("IN A LIST", 7_074_000)],
    {"frequency": 7_074_000, "mode": "USB", "agc": "AUTO", "pre": False, "att": False},           # no name
    {**ch("", 7_074_000), "name": 5},
    {**ch("STR", 0), "frequency": "7074000"},
    {**ch("BOOL", 0), "frequency": True},
    ch("ZERO", 0), ch("NEGATIVE", -7_074_000), ch("2^32", 0x100000000),
    ch("FM", 145_500_000, "FM"), ch("MED", 7_074_000, "USB", "MEDIUM"), ch("LOWER", 7_074_000, "usb"),
    {**ch("PRE", 7_074_000), "pre": 0}, {k: v for k, v in ch("NO ATT", 7_074_000).items() if k != "att"},
]


class DbPlugin:
    """params.db for the routes that take dbcon, committed after the request (bottle-sqlite in __main__)"""
    name, api = "test_db", 2

    def __init__(self):
        self.path = None

    def apply(self, callback, route):
        try:
            takes = "dbcon" in inspect.signature(route.callback).parameters
        except (TypeError, ValueError):
            takes = False
        if not takes:
            return callback

        def wrapper(*a, **kw):
            con = sqlite3.connect(self.path)
            try:
                out = callback(*a, dbcon=con, **kw)
                con.commit()
                return out
            finally:
                con.close()
        return wrapper


DB = DbPlugin()
apps.app.install(DB)


class Memories(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-channels-web.")
        self.addCleanup(shutil.rmtree, self.root)
        for name in ("CHANNELS_FILE", "DB_PATH"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        self.addCleanup(setattr, settings, "DEVICE_TREE", getattr(settings, "DEVICE_TREE", None))
        settings.CHANNELS_FILE = os.path.join(self.root, "channels.json")
        settings.DEVICE_TREE = os.path.join(self.root, "compatible")
        settings.DB_PATH = DB.path = os.path.join(self.root, "params.db")
        con = sqlite3.connect(DB.path)
        con.execute("CREATE TABLE memory (id INTEGER, name TEXT, val INTEGER, UNIQUE (id, name) ON CONFLICT REPLACE)")
        con.commit()
        con.close()
        self.model(X6200)

    def model(self, compatible):
        with open(settings.DEVICE_TREE, "wb") as f:
            f.write(compatible)

    def write(self, entries=None, raw=None):
        """channels.json with these entries, or these bytes; the bytes written"""
        data = raw if raw is not None else json.dumps({"version": 1, "channels": entries}, indent="\t").encode()
        with open(settings.CHANNELS_FILE, "wb") as f:
            f.write(data)
        return data

    def read(self):
        with open(settings.CHANNELS_FILE, "rb") as f:
            return f.read()

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

    def channels(self):
        code, r = self.req("GET", "/api/channels")
        self.assertEqual(code, 200)
        return r["channels"]

    def ok(self, method, path, body=None):
        code, r = self.req(method, path, body)
        self.assertEqual(code, 200, r)
        return r

    # Reading

    def test_presets(self):
        self.write(gui_presets())
        self.assertEqual(self.channels(), gui_presets())

    def test_out_of_range_read_whole(self):
        """Every channel the GUI keeps is on the page, the presets and the others (v0.3.8: none at all)"""
        entries = gui_presets() + OUT_OF_RANGE
        self.write(entries)
        self.assertEqual(self.channels(), [gui_keeps(x) for x in entries])
        self.assertEqual(self.channels()[-3]["name"], "A NAME OF FORTY CHARACTERS, NIN")     # the GUI's 31 bytes
        self.assertEqual(self.channels()[-1]["frequency"], 14_074_000)

    def test_left_out_alone(self):
        """An entry the GUI leaves out is left out alone, the ones around it shown"""
        entries = []
        for i, bad in enumerate(LEFT_OUT):
            entries += [ch(f"GOOD {i}", 3_500_000 + i * 1000), bad]
        self.write(entries + OUT_OF_RANGE)
        for bad in LEFT_OUT:
            self.assertIsNone(gui_keeps(bad), bad)
        self.assertEqual(self.channels(), [gui_keeps(x) for x in entries + OUT_OF_RANGE if gui_keeps(x)])

    def test_first_100(self):
        """The GUI keeps the first 100 it can read; the page shows those, and takes no more"""
        entries = [x for i in range(110) for x in (ch(f"C{i}", 7_000_000 + i), {"bad": i})]
        self.write(entries)
        self.assertEqual([c["name"] for c in self.channels()], [f"C{i}" for i in range(100)])
        before = self.read()
        code, r = self.req("POST", "/api/channels", ch("ONE MORE", 7_074_000))
        self.assertEqual((code, r["msg"]), (400, "At most 100 channels"))
        self.assertEqual(self.read(), before)

    def test_as_cjson_reads(self):
        """A UTF-8 BOM, text after the value, a name not in UTF-8, a key given twice (the first counts), a control
        character in a string: the GUI reads all of these"""
        self.write(raw=b'\xef\xbb\xbf{"version": 1, "channels": [{"name": "L\xe9a\x01", "frequency": 7074000, '
                       b'"frequency": "x", "mode": "USB", "agc": "AUTO", "pre": false, "att": false}]}\nleft over')
        self.assertEqual(self.channels(), [ch("L\udce9a\x01", 7_074_000)])

    def test_no_file(self):
        self.assertEqual(self.channels(), [])
        self.ok("POST", "/api/channels", ch("FIRST", 7_074_000))
        self.assertEqual(meaning(self.read()), meaning(json.dumps({"channels": [ch("FIRST", 7_074_000)]}).encode()))

    # Changing: nothing the user did not touch changes

    def mixed(self):
        """A file of presets, channels v0.3.8 could not show and entries the GUI leaves out, mixed, with a name not
        in UTF-8 and a key given twice; its bytes and the places of the channels the page shows"""
        entries = gui_presets()[:6] + LEFT_OUT[:4] + OUT_OF_RANGE + LEFT_OUT[4:] + gui_presets()[6:]
        data = json.dumps({"version": 1, "note": "mine", "channels": entries}, indent="\t").encode()
        data = data.replace(b'"name": "TOWER"', b'"name": "TOW\xc9R"', 1)
        data = data.replace(b'"name": "2M",', b'"name": "2M", "frequency": 145000000,', 1)
        self.write(raw=data)
        places = [i for i, x in enumerate(entries) if gui_keeps(x)]
        self.assertEqual(len(self.channels()), len(places))
        return data, places

    def test_add_keeps_the_rest(self):
        data, _ = self.mixed()
        new = ch("NEW", 7_074_000, "USB-D")
        r = self.ok("POST", "/api/channels", new)
        self.assertEqual(r["channels"][-1], new)
        self.assertEqual(meaning(self.read()), meaning(data) + [json.dumps(new)])
        self.assertIn(b'"TOW\xc9R"', self.read())                    # the byte as it was
        self.assertEqual(json.loads(self.read().decode("utf-8", "surrogateescape"))["note"], "mine")

    def test_edit_keeps_the_rest(self):
        _, places = self.mixed()
        for i in (0, 7, len(places) - 1):
            before = meaning(self.read())
            new = ch(f"EDITED {i}", 10_000_000 + i, "AM", "SLOW", True, True)
            r = self.ok("PUT", f"/api/channels/{i}", new)
            self.assertEqual(r["channels"][i], new)
            before[places[i]] = json.dumps(new)
            self.assertEqual(meaning(self.read()), before)

    def test_delete_keeps_the_rest(self):
        _, places = self.mixed()
        shown = self.channels()
        for i in (len(places) - 1, 7, 0):
            before = meaning(self.read())
            r = self.ok("DELETE", f"/api/channels/{i}")
            del shown[i]
            self.assertEqual(r["channels"], shown)
            del before[places[i]]
            self.assertEqual(meaning(self.read()), before)
            places = [p - (p > places[i]) for p in places[:i] + places[i + 1:]]

    def test_move_keeps_the_rest(self):
        """The page's channels change places among themselves; the entries the GUI leaves out stay where they are"""
        _, places = self.mixed()
        for i, to in ((0, 1), (5, 6), (6, 5), (len(places) - 1, 0), (2, len(places) - 1)):
            before, shown = meaning(self.read()), self.channels()
            r = self.ok("POST", f"/api/channels/{i}/move", {"to": to})
            shown.insert(to, shown.pop(i))
            self.assertEqual(r["channels"], shown)
            now = meaning(self.read())
            self.assertEqual([now[p] for p in range(len(now)) if p not in places],
                             [before[p] for p in range(len(before)) if p not in places])
            order = [before[p] for p in places]
            order.insert(to, order.pop(i))
            self.assertEqual([now[p] for p in places], order)

    def test_undo_of_a_delete(self):
        """What the page does for Undo after a delete: the channel added again at the end, moved back"""
        self.mixed()
        before, shown = meaning(self.read()), self.channels()
        gone = shown[3]
        self.ok("DELETE", "/api/channels/3")
        r = self.ok("POST", "/api/channels", gone)
        self.ok("POST", f"/api/channels/{len(r['channels']) - 1}/move", {"to": 3})
        self.assertEqual(self.channels(), shown)
        now = meaning(self.read())
        self.assertEqual(sorted(now), sorted(before))

    def test_unreadable_left_alone(self):
        """A file the GUI can't read either: the page says so and changes nothing (v0.3.8: Add wrote over it)"""
        for raw in (b'{"version": 1, "channels": [', b"", b'[1, 2]', b'{"channels": {"a": 1}}',
                    b'{"channels": [{"name": "N", "frequency": NaN, "mode": "AM", "agc": "OFF", "pre": false, '
                    b'"att": true}]}'):
            self.write(raw=raw)
            code, r = self.req("GET", "/api/channels")
            self.assertEqual((code, r["channels"]), (200, []))
            self.assertIn("can't be read", r.get("error", ""), raw)
            for method, path, body in (("POST", "/api/channels", ch("NEW", 7_074_000)),
                                       ("PUT", "/api/channels/0", ch("NEW", 7_074_000)),
                                       ("DELETE", "/api/channels/0", None),
                                       ("POST", "/api/channels/0/move", {"to": 0})):
                code, r = self.req(method, path, body)
                self.assertGreaterEqual(code, 400, (raw, method, path))
                self.assertEqual(self.read(), raw)

    # What a request takes: where the radio tunes, on each model

    def test_channel_ranges(self):
        for model, good, bad, msg in (
                (X6100, (100_000, 55_000_000, 70_000_000, 145_500_000, 500_000_000),
                 (99_999, 55_000_001, 60_000_000, 69_999_999, 500_000_001, 1_000_000_000),
                 "Frequency must be 0.1-55 or 70-500 MHz"),
                (X6200, (100_000, 55_500_000, 98_500_000, 118_100_000, 1_000_000_000),
                 (99_999, 1_000_000_001, 0xFFFFFFFF), "Frequency must be 0.1-1000 MHz")):
            self.model(model)
            self.write(gui_presets()[:2])
            for f in good:
                self.ok("POST", "/api/channels", ch("OK", f))
                self.ok("PUT", "/api/channels/0", ch("OK", f))
            for f in bad:
                code, r = self.req("POST", "/api/channels", ch("NO", f))
                self.assertEqual((code, r["msg"]), (400, msg), f)
                code, r = self.req("PUT", "/api/channels/0", ch("NO", f))
                self.assertEqual((code, r["msg"]), (400, msg), f)
            self.assertEqual(len(self.channels()), 2 + len(good))

    def test_request_rules(self):
        """A name of 31 printable ASCII characters at most, whole hertz, a known mode and AGC, PRE and ATT booleans"""
        self.write([])
        for bad in (ch("X" * 32, 7_074_000), ch("M\u00e9t\u00e9o", 7_074_000), {**ch("F", 0), "frequency": 7e6},
                    {**ch("B", 0), "frequency": True}, ch("M", 7_074_000, "FM"), ch("A", 7_074_000, "USB", "MED"),
                    {**ch("P", 7_074_000), "pre": 1}, "not an object"):
            code, _ = self.req("POST", "/api/channels", bad)
            self.assertEqual(code, 400, bad)
        self.assertEqual(self.channels(), [])

    def test_page_input(self):
        """The Freq input of the page goes from 0.1 MHz to the model's top"""
        for model, top in ((X6100, b"500.0"), (X6200, b"1000.0")):
            self.model(model)
            code, page = self.req("GET", "/memories")
            self.assertEqual(code, 200)
            self.assertIn(b"const TUNE_MHZ = [0.1, " + top + b"];", page)
            self.assertNotIn(b"max='56'", page)

    def test_mem_slots(self):
        """A MEM slot the radio saved on FM broadcast or the airband can be saved on the page too (X6200); the
        same ranges as a channel"""
        self.model(X6200)
        for slot, f, mode in ((1, 98_500_000, "WFM"), (2, 118_100_000, "AM"), (3, 100_000, "AM")):
            r = self.ok("PUT", f"/api/memories/{slot}", {"frequency": f, "mode": mode, "agc": "AUTO", "pre": False,
                                                         "att": False})
            self.assertEqual(r["slots"][slot - 1]["frequency"], f)
        self.model(X6100)
        code, r = self.req("PUT", "/api/memories/4", {"frequency": 60_000_000, "mode": "USB", "agc": "AUTO",
                                                      "pre": False, "att": False})
        self.assertEqual((code, r["msg"]), (400, "Frequency must be 0.1-55 or 70-500 MHz"))
        self.ok("PUT", "/api/memories/4", {"frequency": 144_300_000, "mode": "USB", "agc": "AUTO", "pre": False,
                                           "att": False})
        code, r = self.req("GET", "/api/memories")
        self.assertEqual([s["frequency"] for s in r["slots"][:5]], [98_500_000, 118_100_000, 100_000, 144_300_000,
                                                                     None])


if __name__ == "__main__":
    unittest.main()
