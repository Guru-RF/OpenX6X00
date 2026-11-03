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

    # A change to a channel as the page sends it: the channel as the page shows it, "was" (and the one at the
    # place it moves to, "past"); shown: the list the page shows, by default the radio's now

    def put(self, i, new, shown=None):
        shown = shown or self.channels()
        return self.req("PUT", f"/api/channels/{i}", {**new, "was": shown[i]})

    def delete(self, i, shown=None):
        shown = shown or self.channels()
        return self.req("DELETE", f"/api/channels/{i}", {"was": shown[i]})

    def move(self, i, to, shown=None):
        shown = shown or self.channels()
        return self.req("POST", f"/api/channels/{i}/move", {"to": to, "was": shown[i], "past": shown[to]})

    def done(self, answer):
        code, r = answer
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
            r = self.done(self.put(i, new))
            self.assertEqual(r["channels"][i], new)
            before[places[i]] = json.dumps(new)
            self.assertEqual(meaning(self.read()), before)

    def test_delete_keeps_the_rest(self):
        _, places = self.mixed()
        shown = self.channels()
        for i in (len(places) - 1, 7, 0):
            before = meaning(self.read())
            r = self.done(self.delete(i))
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
            r = self.done(self.move(i, to))
            shown.insert(to, shown.pop(i))
            self.assertEqual(r["channels"], shown)
            now = meaning(self.read())
            self.assertEqual([now[p] for p in range(len(now)) if p not in places],
                             [before[p] for p in range(len(before)) if p not in places])
            order = [before[p] for p in places]
            order.insert(to, order.pop(i))
            self.assertEqual([now[p] for p in places], order)

    def test_undo_of_a_delete(self):
        """What the page does for Undo after a delete: what the delete answered with, handed back as it is. The
        entry comes back as it was at its place, also one whose name or frequency a request can't have, whose
        name is not UTF-8, or that has keys of its own (until now: added again through the request rules, so
        refused for "M\u00e9t\u00e9o", or with its name cut to 31 bytes and its fraction of a hertz gone)"""
        _, places = self.mixed()
        before, shown = meaning(self.read()), self.channels()
        for i in range(len(places)):
            r = self.done(self.delete(i))
            self.assertEqual(len(r["channels"]), len(places) - 1)
            r = self.ok("POST", "/api/channels/restore", r["deleted"])
            self.assertEqual(r["channels"], shown)
            self.assertEqual(meaning(self.read()), before)
        self.assertIn(b'"TOW\xc9R"', self.read())
        self.write([ch("A", 7_000_000), {**ch("NOTE", 7_100_000), "note": "keep me", "n": [1.0, 2]}])
        before = meaning(self.read())
        r = self.ok("POST", "/api/channels/restore", self.done(self.delete(1))["deleted"])
        self.assertEqual(meaning(self.read()), before)

    def test_undo_refused(self):
        """/restore takes back an entry the radio keeps, as there is room for it, and nothing else"""
        self.write([ch(f"C{i}", 7_000_000 + i) for i in range(99)])
        gone = self.done(self.delete(5))["deleted"]
        self.ok("POST", "/api/channels", ch("ONE MORE", 7_074_000))
        self.ok("POST", "/api/channels", ch("100", 7_074_000))
        before = self.read()
        code, r = self.req("POST", "/api/channels/restore", gone)
        self.assertEqual((code, r["msg"]), (400, "At most 100 channels"))
        self.done(self.delete(99))
        before = self.read()
        for bad in ({**gone, "entry": json.dumps(ch("ZERO", 0))}, {**gone, "entry": "{"}, {**gone, "entry": 5},
                    {**gone, "place": "5"}, {"entry": gone["entry"]}, "not an object"):
            code, r = self.req("POST", "/api/channels/restore", bad)
            self.assertEqual((code, r["msg"]), (400, "Invalid request"), bad)
            self.assertEqual(self.read(), before)
        self.ok("POST", "/api/channels/restore", {**gone, "place": 1000})       # at the end, as the list is now
        self.assertEqual(self.channels()[-1]["name"], "C5")

    def test_save_keeps_what_it_leaves(self):
        """A Save changes in the entry what was changed on the page: a name or a frequency left as shown stays as
        the file has it, one the page would not take too, and so do the entry's other keys (until now the whole
        entry from the request: a mode change of "M\u00e9t\u00e9o" refused, a name cut, a fraction of a hertz
        and a key of its own gone)"""
        self.model(X6100)
        entries = [x for x in OUT_OF_RANGE if x["name"] in ("", "DCF77", "M\u00e9t\u00e9o", "HALF", "TAB\tIN IT")]
        entries += [{**ch("NOTE", 7_100_000), "note": "keep me"},
                    ch("A NAME OF FORTY CHARACTERS, NINE MORE...", 3_573_000)]
        self.write(entries)
        for i, x in enumerate(entries):
            new = {**self.channels()[i], "mode": "CW", "att": True}
            self.done(self.put(i, new))
            want = {**x, "mode": "CW", "att": True}
            self.assertEqual(json.loads(self.read())["channels"][i], want)
        # A change has to fit: the name or the frequency changed, it is checked
        before = self.read()
        code, r = self.put(1, {**self.channels()[1], "frequency": 77_600})
        self.assertEqual((code, r["msg"]), (400, "Frequency must be 0.1-55 or 70-500 MHz"))
        code, r = self.put(2, {**self.channels()[2], "name": "M\u00e9t\u00e9o 2"})
        self.assertEqual((code, r["msg"]), (400, "Invalid name: up to 31 printable ASCII characters"))
        code, r = self.put(3, {**self.channels()[3], "frequency": 14_074_000.0})     # not whole hertz: changed
        self.assertEqual(code, 400)
        self.assertEqual(self.read(), before)
        self.done(self.put(2, {**self.channels()[2], "name": "METEO"}))
        self.assertEqual(json.loads(self.read())["channels"][2]["name"], "METEO")

    def stale(self, answer, before):
        """A change the page made on a list that changed meanwhile: 409, the radio's list with it, the file as it
        was"""
        code, r = answer
        self.assertEqual((code, r.get("msg")), (409, apps.CHANNELS_CHANGED))
        self.assertEqual(r["channels"], self.channels())
        self.assertEqual(self.read(), before)

    def test_stale_page_changes_nothing(self):
        """The page shows A B C D; on the radio APP > Channels deletes A, or moves D to the top. A Save, a Delete
        or a move of the page then changes nothing (v0.3.8: the channel now at that place, C(CW) over D, B deleted
        for A)"""
        abcd = [ch(n, 7_000_000 + k * 100_000) for k, n in enumerate("ABCD")]
        for radio in (abcd[1:], abcd[3:] + abcd[:3], [{"left": "out"}] + abcd[1:]):
            self.write(abcd)
            shown = self.channels()
            before = self.write(radio)
            self.stale(self.put(2, {**abcd[2], "mode": "CW"}, shown), before)
            self.stale(self.delete(0, shown), before)
            self.stale(self.delete(1, shown), before)
            self.stale(self.move(3, 2, shown), before)
            self.stale(self.move(0, 1, shown), before)
        # B changed on the radio: a change of B, or a move past it, changes nothing; one of C still goes
        self.write(abcd)
        shown = self.channels()
        before = self.write([abcd[0], {**abcd[1], "agc": "FAST"}] + abcd[2:])
        self.stale(self.put(1, {**abcd[1], "mode": "CW"}, shown), before)
        self.stale(self.delete(1, shown), before)
        self.stale(self.move(2, 1, shown), before)
        self.done(self.put(2, {**abcd[2], "mode": "CW"}, shown))
        before = self.read()
        self.stale(self.put(9, abcd[0], [abcd[0]] * 10), before)              # no channel at that place now
        self.stale(self.req("DELETE", "/api/channels/0", {}), before)        # no "was": a page from before this
        code, _ = self.req("DELETE", "/api/channels/0")
        self.assertEqual(code, 400)
        self.assertEqual(self.read(), before)

    def test_own_changes_in_a_row(self):
        """The page's own changes go one at a time, each made on the list it showed when clicked: a Save of C
        queued behind a move of D above it changes nothing (v0.3.8: C(CW) put over D); twice Up on D moves it once"""
        self.write([ch(n, 7_000_000 + k * 1000) for k, n in enumerate("ABCDEF")])
        shown = self.channels()
        self.done(self.move(3, 2, shown))
        before = self.read()
        self.stale(self.put(2, {**shown[2], "mode": "CW"}, shown), before)
        self.stale(self.move(3, 2, shown), before)
        self.stale(self.delete(3, shown), before)
        self.assertEqual([c["name"] for c in self.channels()], list("ABDCEF"))
        # Saves of other rows (Save all) don't move anything: each still finds its channel
        for i in (0, 1, 4, 5):
            self.done(self.put(i, {**shown[i], "agc": "OFF"}, shown))
        self.assertEqual([(c["name"], c["agc"]) for c in self.channels()],
                         [("A", "OFF"), ("B", "OFF"), ("D", "AUTO"), ("C", "AUTO"), ("E", "OFF"), ("F", "OFF")])

    def gui_saves_meanwhile(self, *lists):
        """The GUI saves these lists, one each time the page is about to put its file in place"""
        real, lists = apps.write_channels, list(lists)

        def write(doc, ready):
            if lists:
                self.write(lists.pop(0))
            return real(doc, ready)
        apps.write_channels = write
        self.addCleanup(setattr, apps, "write_channels", real)

    def test_gui_save_meanwhile(self):
        """A save of the GUI's that lands while the page writes is kept: the page's change is made again on its
        list (until now the list the page had read went over it, the GUI's change gone)"""
        abcd = [ch(n, 7_000_000 + k * 100_000) for k, n in enumerate("ABCD")]
        a2 = {**abcd[0], "name": "A2"}
        self.write(abcd)
        shown = self.channels()
        self.gui_saves_meanwhile([a2] + abcd[1:])               # APP > Channels renames A
        r = self.done(self.put(2, {**abcd[2], "mode": "CW"}, shown))
        self.assertEqual(json.loads(self.read())["channels"], [a2, abcd[1], {**abcd[2], "mode": "CW"}, abcd[3]])
        self.assertEqual(r["channels"], json.loads(self.read())["channels"])
        self.write(abcd)
        self.gui_saves_meanwhile(abcd[1:])                     # ... deletes A
        self.ok("POST", "/api/channels", ch("NEW", 7_074_000))
        self.assertEqual(json.loads(self.read())["channels"], abcd[1:] + [ch("NEW", 7_074_000)])
        self.write(abcd)
        self.gui_saves_meanwhile(abcd[1:])                     # ... deletes A: C is no longer channel 2
        self.stale(self.put(2, {**abcd[2], "mode": "CW"}, shown), self.gui_file(abcd[1:]))
        self.write(abcd)
        self.gui_saves_meanwhile(abcd[1:], abcd[2:])           # twice: 409, the GUI's list as it is
        code, r = self.req("POST", "/api/channels", ch("NEW", 7_074_000))
        self.stale((code, r), self.gui_file(abcd[2:]))
        self.assertEqual(sorted(os.listdir(self.root)), ["channels.json", "compatible", "params.db"])

    def test_file_time_later(self):
        """The GUI reloads the file when its time differs from the one it read, which FAT keeps to 2 s: the page's
        file is later than the one it replaced (until now a second change within 2 s did not reach the radio)"""
        self.write([ch("A", 7_000_000)])
        st = os.stat(settings.CHANNELS_FILE)
        os.utime(settings.CHANNELS_FILE, ns=(st.st_atime_ns, st.st_mtime_ns + 10_000_000_000))   # as if in one 2 s
        old = os.stat(settings.CHANNELS_FILE).st_mtime_ns
        self.ok("POST", "/api/channels", ch("B", 7_100_000))
        self.assertEqual(os.stat(settings.CHANNELS_FILE).st_mtime_ns, old + 2_000_000_000)
        self.done(self.delete(0))
        self.assertEqual(os.stat(settings.CHANNELS_FILE).st_mtime_ns, old + 4_000_000_000)
        os.utime(settings.CHANNELS_FILE, ns=(st.st_atime_ns, st.st_mtime_ns - 10_000_000_000))  # older: its own time
        self.ok("POST", "/api/channels", ch("C", 7_200_000))
        self.assertGreater(os.stat(settings.CHANNELS_FILE).st_mtime_ns, st.st_mtime_ns - 1_000_000_000)

    def test_on_the_card(self):
        """The new file and then its folder, the rename, are on the card before the answer, as the GUI's own
        save does (until now the folder not: a power cut could leave the old list)"""
        synced, real = [], os.fsync

        def fsync(fd):
            synced.append(os.readlink(f"/proc/self/fd/{fd}"))
            return real(fd)
        os.fsync = fsync
        self.addCleanup(setattr, os, "fsync", real)
        self.write([ch("A", 7_000_000)])
        self.ok("POST", "/api/channels", ch("B", 7_100_000))
        self.assertEqual(len(synced), 2, synced)
        self.assertTrue(os.path.basename(synced[0]).startswith("channels.json."), synced)
        self.assertEqual(synced[1], os.path.realpath(self.root))

    @staticmethod
    def gui_file(entries):
        return json.dumps({"version": 1, "channels": entries}, indent="\t").encode()

    def test_unreadable_left_alone(self):
        """A file the GUI can't read either: the page says so and changes nothing (v0.3.8: Add wrote over it)"""
        for raw in (b'{"version": 1, "channels": [', b"", b'[1, 2]', b'{"channels": {"a": 1}}',
                    b'{"channels": [{"name": "N", "frequency": NaN, "mode": "AM", "agc": "OFF", "pre": false, '
                    b'"att": true}]}'):
            self.write(raw=raw)
            code, r = self.req("GET", "/api/channels")
            self.assertEqual((code, r["channels"]), (200, []))
            self.assertIn("can't be read", r.get("error", ""), raw)
            was = ch("N", 7_074_000)
            for method, path, body in (("POST", "/api/channels", ch("NEW", 7_074_000)),
                                       ("PUT", "/api/channels/0", {**ch("NEW", 7_074_000), "was": was}),
                                       ("DELETE", "/api/channels/0", {"was": was}),
                                       ("POST", "/api/channels/0/move", {"to": 0, "was": was, "past": was})):
                code, r = self.req(method, path, body)
                self.assertGreaterEqual(code, 400, (raw, method, path))
                self.assertEqual(self.read(), raw)

    def test_as_cjson_reads_more(self):
        r"""cJSON reads the text up to a NUL byte, skips bytes 1-32 before the value, refuses a \u escape of half a
        UTF-16 pair and more than 1000 lists and objects within each other; the GUI's strings end at a NUL. The
        page reads the file so too: one the radio can't read is left as it is, one it reads is listed (until now a
        NUL byte in a string or a lone \udc80 read, and an Add turned them into a file the radio reads: \u0000,
        a raw 0x80 byte)"""
        a = json.dumps(ch("A", 7_000_000)).encode()
        for raw in (b'{"version": 1, "channels": [{"name": "A\0B", "frequency": 7000000, "mode": "USB", '
                    b'"agc": "AUTO", "pre": false, "att": false}]}',
                    b'\0' + b'{"version": 1, "channels": [%s]}' % a,
                    b'{"version": 1, "channels": [%s, {"name": "X\\udc80"}]}' % a,
                    b'{"version": 1, "channels": [%s, {"name": "X\\ud800"}]}' % a,
                    b'{"version": 1, "channels": [%s, {"name": "X\\uD800\\u0041"}]}' % a,
                    b'{"version": 1, "channels": [%s, {"name": "X\\u0041\\uDFFF"}]}' % a,
                    b'{"version": 1, "x": %s, "channels": [%s]}' % (b"[" * 1000 + b"]" * 1000, a),
                    b'{"version": 1, "x": %s, "channels": [%s]}' % (b"[" * 200000 + b"]" * 200000, a)):
            self.write(raw=raw)
            code, r = self.req("GET", "/api/channels")
            self.assertEqual((code, r["channels"]), (200, []), raw[:80])
            self.assertIn("can't be read", r.get("error", ""), raw[:80])
            code, r = self.req("POST", "/api/channels", ch("NEW", 7_074_000))
            self.assertEqual(code, 500, raw[:80])
            self.assertEqual(self.read(), raw)
        for raw, names in ((b'{"version": 1, "channels": [%s]}\0{"left": "over"' % a, ["A"]),
                           (b'\x0c\x0b\x01 {"version": 1, "channels": [%s]}' % a, ["A"]),
                           (b'{"version": 1, "channels": [%s, %s, %s]}' % (
                               a, json.dumps(ch("X\\udc80", 7_100_000)).encode(),
                               json.dumps(ch("PAIR", 7_200_000)).encode().replace(b"PAIR", b"\\ud83d\\ude00 \\\\udc80")),
                            ["A", "X\\udc80", "\U0001f600 \\udc80"]),
                           (b'{"version": 1, "channels": [{"name": "M\\u0000ODE", "frequency": 7000000, '
                            b'"mode": "USB\\u0000x", "agc": "AUTO\\u0000", "pre": false, "att": false}]}', ["M"]),
                           (b'{"version": 1, "x": %s, "channels": [%s]}' % (b"[" * 100 + b"]" * 100, a), ["A"])):
            self.write(raw=raw)
            self.assertEqual([c["name"] for c in self.channels()], names, raw[:80])
            self.ok("POST", "/api/channels", ch("NEW", 7_074_000))
            self.assertEqual([c["name"] for c in self.channels()], names + ["NEW"], raw[:80])
        self.assertEqual(self.channels()[0]["mode"], "USB")

    def test_too_deep_to_write(self):
        """A file the page reads but can't write back, nested too deep for its Python (999 lists in each other, the
        radio's limit 1000): the reason, and the file as it was (until now a 500 page of bottle's)"""
        self.write(raw=b'{"version": 1, "x": %s, "channels": [%s]}' % (b"[" * 999 + b"]" * 999,
                                                                     json.dumps(ch("A", 7_000_000)).encode()))
        self.assertEqual(len(self.channels()), 1)
        before = self.read()
        code, r = self.req("POST", "/api/channels", ch("NEW", 7_074_000))
        if code == 500:      # as deep as this Python goes
            self.assertEqual(r["msg"], "Can't save: channels.json is nested too deep for the page")
            self.assertEqual(self.read(), before)
        self.write([ch("A", 7_000_000)])
        before, real = self.read(), json.dump

        def dump(*a, **kw):
            raise RecursionError("maximum recursion depth exceeded")
        json.dump = dump
        self.addCleanup(setattr, json, "dump", real)
        code, r = self.req("POST", "/api/channels", ch("NEW", 7_074_000))
        self.assertEqual((code, r["msg"]), (500, "Can't save: channels.json is nested too deep for the page"))
        self.assertEqual(self.read(), before)
        self.assertEqual(sorted(os.listdir(self.root)), ["channels.json", "compatible", "params.db"])

    def test_too_big_a_number(self):
        """A number too big for a double anywhere in the file (1e400: cJSON's infinity), which can't be written
        back as JSON: the page says why and changes nothing (until now listed, and every change a 500 "Out of range
        float values are not JSON compliant")"""
        good = json.dumps(ch("A", 7_000_000)).encode()
        for raw in (b'{"version": 1, "channels": [%s, {"name": "BIG", "frequency": 1e400, "mode": "USB", '
                    b'"agc": "AUTO", "pre": false, "att": false}]}' % good,
                    b'{"version": 1e999, "channels": [%s]}' % good,
                    b'{"version": 1, "x": [-1E+400], "channels": [%s]}' % good):
            self.write(raw=raw)
            code, r = self.req("GET", "/api/channels")
            self.assertEqual((code, r["channels"]), (200, []))
            self.assertIn("too big a number", r.get("error", ""), raw)
            code, r = self.req("POST", "/api/channels", ch("NEW", 7_074_000))
            self.assertEqual(code, 500)
            self.assertIn("too big a number", r["msg"])
            self.assertEqual(self.read(), raw)
        self.write(raw=b'{"version": 1, "x": 1e-400, "big": %d, "channels": [%s]}' % (10 ** 30, good))
        self.ok("POST", "/api/channels", ch("NEW", 7_074_000))            # a tiny one is 0, a long integer as it is
        self.assertIn(b'"big": %d' % 10 ** 30, self.read())

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
                self.done(self.put(0, ch("OK", f)))
            for f in bad:
                code, r = self.req("POST", "/api/channels", ch("NO", f))
                self.assertEqual((code, r["msg"]), (400, msg), f)
                code, r = self.put(0, ch("NO", f))
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
