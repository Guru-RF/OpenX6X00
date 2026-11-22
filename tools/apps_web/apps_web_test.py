#!/usr/bin/env python3
"""Host test of the apps' pages of the web configurator (webserver/src/x6100_webserver/apps.py, views/wefax.html).
See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/apps_web/apps_web_test.py [-v]

1. WeFax: the page's stations are the app's (src/wefax_window.c stations[], read from the source), a channel's
   name as the app lists it (channel_name there), the channels named WEFAX after the stations as the app's Freq.
   list has them (none within 50 Hz of a station, none past INT32_MAX).
2. Through bottle's WSGI app (skipped without BOTTLE): each app's page, in the menu and on General, what it puts
   into its script as JavaScript that a name cannot break out of, and every file it loads from /static there.
3. Countries and flags: /api/dxcc reads cty.dat as src/dxcc.c does, and the pages' lookup (app_wall.js, run
   under node when there is one) says what dxcc_lookup says (build/dxcc_calls, the Makefile's) for every prefix and
   whole call of cty.dat and for the hard ones (/P, DL/..., <...>, /MM); flags.json and flags.png are src/flags.h's.
4. src/remote_server.c: a page's "watching" words ("wefax rows 1", "ft8 wf 1", "js8 wf 1") are taken before the
   app's own branch, which wants operate and control: after it, a viewer's "ft8 wf 1" was refused as an unknown
   command. "js8 send TEXT" is taken before the words are split (its text has spaces), a text message of 500
   characters fits TEXT_MAX, and the JS8 page's ring and lines are as the page reads them.
5. Each Apps page opens its app on the radio by itself, once (RadioSession.autoOpen in radio_session.js, from
   OlivLoze's fork): auto_open.js runs it under node on a clock of its own (opened after the wait for the role, never
   over another app, not paired said without a dialog, once only, a timeout said); each page calls it with its own
   command and its app's names, on its app's message and on the radio's state.
Its files go in build/ (the Makefile empties it first).
"""
import io
import json
import os
import gzip
import re
import shutil
import sqlite3
import subprocess
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


def get(path, headers=None):
    """GET through bottle's WSGI app: (status, body, headers)"""
    env = {"REQUEST_METHOD": "GET", "PATH_INFO": path, "HTTP_HOST": "192.168.1.10"}
    env.update(headers or {})
    setup_testing_defaults(env)
    got = {}

    def start(status, hdrs, exc=None):
        got["status"] = int(status.split()[0])
        got["headers"] = dict(hdrs)

    out = b"".join(apps.app(env, start))
    return got["status"], out, got["headers"]


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
        return get(path, headers)

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
        """The box takes what the radio sends (TX_TEXT_MAX in js8_window.c), and no more; the chat's the room left
        after the call"""
        tx_max = int(re.search(r"#define TX_TEXT_MAX +(\d+)", source("js8_window.c")).group(1))
        code, page, _ = self.req("/js8")
        self.assertIn(b'id="msg" maxlength="%d"' % tx_max, page)
        self.assertIn(b"const MAXTX = %d;" % tx_max, page)
        self.assertEqual(apps.JS8_SAVED_MAX, tx_max)

    def test_js8_saved(self):
        """The radio's saved messages as its load_saved reads them: ten lines at most, in capitals, an empty line
        an empty slot, a line cut at TX_TEXT_MAX; none saved yet: its first slot's default"""
        self.addCleanup(setattr, settings, "JS8_SAVED_FILE", settings.JS8_SAVED_FILE)
        settings.JS8_SAVED_FILE = os.path.join(fresh_dir(), "js8_saved.txt")
        self.assertEqual(self.req("/api/js8_saved")[1], b'{"saved": [{"n": 1, "text": "TNX 73 GL"}]}')
        with open(settings.JS8_SAVED_FILE, "wb") as f:
            f.write(b"<call> tnx fer the qso 73\n\nqrz? de <mycall>\r\nwith\ra cr\n" + b"x" * 600 + b"\n"
                    + b"\n" * 5 + b"eleventh\n")
        code, body, _ = self.req("/api/js8_saved")
        saved = json.loads(body)["saved"]
        self.assertEqual([m["n"] for m in saved], [1, 3, 4, 5, 6])
        self.assertEqual(saved[0]["text"], "<CALL> TNX FER THE QSO 73")
        self.assertEqual(saved[1]["text"], "QRZ? DE <MYCALL>")
        self.assertEqual(saved[2]["text"], "WITH")                       # strcspn: up to the first \r
        self.assertEqual(saved[3]["text"], "X" * 500)                    # the first 507 bytes, cut at 500
        self.assertEqual(saved[4]["text"], "X" * 93)                     # the rest of the line: the next slot

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


CTY = os.path.join(TOP, "third-party", "cty", "cty.dat")
STATIC = os.path.join(TOP, "webserver", "src", "x6100_webserver", "static")


@unittest.skipIf(apps is None, "no bottle: give BOTTLE=<a folder with bottle.py>")
class Countries(unittest.TestCase):
    def test_short_names(self):
        """short_name in src/dxcc.c, those that change a name"""
        body = source("dxcc.c")
        body = body[body.index("static const char *short_name("):]
        pairs = re.findall(r'\{ "([^"]+)", +"([^"]+)" \}', body[:body.index("NULL, NULL")])
        self.assertEqual(apps.CTY_SHORT, {a: b for a, b in pairs if a != b})

    def test_flags(self):
        """flags.json is src/flags.h's list, flags.png has a 40 x 30 cell for each of its flags"""
        src = source("flags.h")
        iso = re.findall(r'"([a-z]{2})"', src[src.index("flag_iso"):src.index("flag_of")])
        of = {p: int(i) for p, i in re.findall(r'\{"([^"]+)", (\d+)\}', src[src.index("flag_of"):])}
        with open(os.path.join(STATIC, "img", "flags.json")) as f:
            got = json.load(f)
        self.assertEqual(got["of"], of)
        with open(os.path.join(STATIC, "img", "flags.png"), "rb") as f:
            head = f.read(24)
        n = got["n"]
        self.assertEqual((head[:8], int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")),
                         (b"\x89PNG\r\n\x1a\n", n * 40, (len(iso) + n - 1) // n * 30))

    def test_table(self):
        with open(CTY, encoding="latin-1") as f:
            t = apps.cty_table(f.read())
        e = t["e"][t["p"]["ON"]]
        self.assertEqual(e[:2], ["Belgium", "ON"])
        self.assertAlmostEqual(e[3], 4.0, delta=1)          # east positive
        # a primary prefix not among the entity's own prefixes is not one of its (1S: Spratly; CE9: Antarctica,
        # whose CE9 is another's): as on the radio
        for i in (t["e"].index(e) for e in t["e"] if e[0] in ("Spratly Islands", "Antarctica")):
            self.assertNotEqual(t["p"].get(t["e"][i][1]), i, t["e"][i])
        self.assertEqual(t["e"][t["p"]["DL"]][0], "Germany")

    def test_api(self):
        """The table whole, gzipped where the radio's Python has zlib and the browser takes it, plain otherwise"""
        self.addCleanup(setattr, apps, "CTY_PATHS", apps.CTY_PATHS)
        apps.CTY_PATHS = ("/nonexistent/cty.dat", CTY)
        with open(CTY, encoding="latin-1") as f:
            table = apps.cty_table(f.read())
        code, body, hdrs = get("/api/dxcc")
        self.assertEqual((code, hdrs.get("Content-Encoding"), json.loads(body)), (200, None, table))
        code, body, hdrs = get("/api/dxcc", {"HTTP_ACCEPT_ENCODING": "gzip, deflate"})
        if apps.gzip:
            self.assertEqual(hdrs.get("Content-Encoding"), "gzip")
            body = gzip.decompress(body)
        self.assertEqual(json.loads(body), table)
        apps.CTY_PATHS = ("/nonexistent/cty.dat",)
        self.assertEqual(get("/api/dxcc")[0], 404)

    @unittest.skipUnless(shutil.which("node") and os.path.exists(os.path.join(BUILD, "dxcc_calls")),
                         "no node, or no build/dxcc_calls (make builds it)")
    def test_lookup_as_the_radio(self):
        with open(CTY, encoding="latin-1") as f:
            t = apps.cty_table(f.read())
        table = os.path.join(BUILD, "dxcc.json")
        with open(table, "w") as f:
            json.dump(t, f)
        # whole calls listed under two entities: the radio's binary search finds either
        counts = {}
        with open(CTY, encoding="latin-1") as f:
            for c in re.findall(r"=([^,;(\[<{~\s]+)", f.read()):
                counts[c] = counts.get(c, 0) + 1
        twice = {c for c, n in counts.items() if n > 1}
        calls = ["ON6URE", "EA8/ON6URE", "ON6URE/P", "ON6URE/QRP", "F/ON6URE/P", "W1AW/7", "W1AW/MM", "W1AW/AM",
                 "<...>", "<ON6URE>", "on6ure", "K", "4X/G4ABC", "KH6/W1AW", "W1AW/KH6", "VP2E/W1AW", "R0AA",
                 "UA9ABC", "TA1AA", "IT9ABC", "3D2AG", "BY1AA", "JA1QRZ", "VK2DEF", "PY2AAA", "SP5XX/QRP", "OH2BBB",
                 "DL/SP2KMO", "SP2KMO/DL", "CE9AA", "1S1A", "A/B/C/D", "ON6URE/1/P", "WAYTOOLONGCALLSIGN12345/P", "W1AW/LGT"]
        calls += [p + "9XZ" for p in sorted(t["p"])] + [x for x in sorted(t["x"]) if x not in twice]
        stdin = "\n".join(calls) + "\n"
        radio = subprocess.run([os.path.join(BUILD, "dxcc_calls")], input=stdin, capture_output=True, text=True,
                               encoding="latin-1", check=True).stdout.splitlines()
        page = subprocess.run(["node", os.path.join(HERE, "country_js.js"), os.path.join(STATIC, "js", "app_wall.js"),
                               table, os.path.join(STATIC, "img", "flags.json")], input=stdin, capture_output=True,
                              text=True, check=True).stdout.splitlines()
        self.assertEqual((len(radio), len(page)), (len(calls), len(calls)))
        diff = [(a, b) for a, b in zip(radio, page) if a != b]
        self.assertEqual(diff[:10], [], "%d of %d differ" % (len(diff), len(radio)))


class AutoOpen(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "no node")
    def test_the_rules(self):
        js = os.path.join(STATIC, "js", "radio_session.js")
        r = subprocess.run(["node", os.path.join(HERE, "auto_open.js"), js], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("auto_open: 16 checks, 0 failed", r.stdout)

    def test_each_page_calls_it(self):
        """With its own command and the names the radio gives its app (screen_main.c app_name: the FT8 app is FT8,
        FT4 or FT2), on its app's message and on "state"; the pages load the radio_session.js that has it"""
        views = os.path.join(TOP, "webserver", "src", "x6100_webserver", "views")
        names = {"ft8": "PROTOS", "js8": '["JS8"]', "wefax": '["WeFax"]'}
        for app in APPS:
            with open(os.path.join(views, app + ".html")) as f:
                page = f.read()
            self.assertIn('RadioSession.autoOpen({cmd: "%s", names: %s' % (app, names[app]), page, app)
            msg = "fax" if app == "wefax" else app
            self.assertTrue(re.search(r'head === "%s"\) \{ %s = JSON.parse\(body\);[^\n]*autoOpen\(\); \}' % (app, msg), page),
                            app)
            self.assertTrue(re.search(r'head === "state"\)(?:[^\n]*\n){0,2}[^\n]*autoOpen\(\)', page), app)
            self.assertIn("/static/js/radio_session.js?v=9", page, app)
        self.assertIn('PROTOS = ["FT4", "FT8", "FT2"]', open(os.path.join(views, "ft8.html")).read())
        self.assertIn("autoOpen: autoOpen", open(os.path.join(STATIC, "js", "radio_session.js")).read())


class DisplayPage(unittest.TestCase):
    def test_each_row_is_an_option_of_the_radio(self):
        """Every row of the Display page's cards is an option the GUI's socket knows by that name (cfg_socket.cpp:
        disp_get reports it, disp_set takes it); a row it does not report is never shown. The walkie screen's mini
        waterfall (#302) is one of them, with the waterfall's"""
        page = open(os.path.join(TOP, "webserver", "src", "x6100_webserver", "views", "display.html")).read()
        cards = page[page.index("const CARDS = ["):page.index("let D = {};")]
        rows = re.findall(r'^    \["([a-z0-9_]+)", "', cards, re.M)
        sock = source("cfg_socket.cpp")
        self.assertGreater(len(rows), 60)
        self.assertEqual(len(rows), len(set(rows)))
        for name in rows:
            self.assertIn('"%s"' % name, sock, name)
        self.assertIn('{"walkie_wf", &cfg_sm.p_walkie_wf, 0, 1}', sock)
        wf = cards[cards.index('["wf", "Spectrum and waterfall"'):cards.index('["layers", ')]
        self.assertIn('["walkie_wf", "Walkie mini waterfall", ', wf)


class RemoteWords(unittest.TestCase):
    def test_watching_before_the_apps_commands(self):
        src = source("remote_server.c")
        body = src[src.index("static void handle_text(client_t *c, char *text)"):]
        for app, watch in (("wefax", '!strcmp(a[0], "wefax") && n >= 3 && !strcmp(a[1], "rows")'),
                           ("ft8", '!strcmp(a[0], "ft8") || !strcmp(a[0], "js8")) && n >= 3 && !strcmp(a[1], "wf")'),
                           ("js8", '!strcmp(a[0], "ft8") || !strcmp(a[0], "js8")) && n >= 3 && !strcmp(a[1], "wf")')):
            self.assertLess(body.index(watch), body.index('!strcmp(a[0], "%s") && n >= 2)' % app), app)
        self.assertLess(body.index('!strncmp(text, "js8 send ", 9)'), body.index("strtok(text,"))

    def test_each_app_its_words(self):
        """FT8's and WeFax's commands are looked up in their own tables, JS8's words in js8_words only"""
        src = source("remote_server.c")
        others = src[src.index("static bool app_ft8_cmd("):src.index("static bool app_js8_cmd(")]
        self.assertNotIn("js8_words", others)
        self.assertIn('{"send", REMOTE_JS8_SEND}', src[src.index("} js8_words[] = "):])

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
