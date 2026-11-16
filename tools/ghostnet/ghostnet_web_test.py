#!/usr/bin/env python3
"""Host test of the web page GhostNet (webserver/src/x6100_webserver/ghostnet_web.py). See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/ghostnet/ghostnet_web_test.py

1. The radio's time zone read from its POSIX TZ string as the C library reads it (each zone of src/clock.c's list,
   through 2026, against time.localtime under that TZ), and a local time back to UTC as mktime does.
2. The schedule the image ships, read as src/ghostnet.c reads it (7 nets, 23 windows), the next window in UTC and in
   the radio's time across midnight and summer time, where the reminder comes, the times shown.
3. The settings: ghostnet.conf as the GUI writes it (the same keys in the same order as src/ghostnet.c), what the
   page may change and what it is refused.
4. The regional group, and the alpha-3 codes against src/ghostnet_iso3.inc.
5. The messages the JS8 app logs: heard, sent, translations, @GSTFLASH, bad lines, the stations.
6. Through bottle's WSGI app (skipped without BOTTLE): /api/ghostnet read and saved, a bad change refused, the page.
Its files go in data/ (the Makefile empties it first).
"""
import calendar
import io
import json
import os
import re
import sqlite3
import sys
import time
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
    cgi.FieldStorage = object
    sys.modules["cgi"] = cgi
try:
    import bottle  # noqa: F401
    from x6100_webserver import apps  # noqa: E402
except ImportError:
    bottle = None
from x6100_webserver import ghostnet_web as gn, settings  # noqa: E402
from x6100_webserver.iso3_web import ISO3  # noqa: E402

SHIPPED = os.path.join(TOP, "rootfs", "usr", "share", "x6100", "ghostnet_nets.txt")
DATA = os.path.join(HERE, "data")
runs = 0


def utc(y, mo, d, h, mi):
    return calendar.timegm((y, mo, d, h, mi, 0))


def fresh_dir():
    """A folder of its own for the next case, under data/"""
    global runs
    runs += 1
    path = os.path.join(DATA, f"web-{os.getpid()}-{runs}")     # new also when run again without make
    os.makedirs(path, exist_ok=True)
    return path


def gui_zones():
    with open(os.path.join(TOP, "src", "clock.c")) as f:
        return re.findall(r'\{ "[^"]+",\s*"([^"]+)" \}', f.read())


class Zones(unittest.TestCase):
    def setUp(self):
        self.addCleanup(self.restore, os.environ.get("TZ"))

    @staticmethod
    def restore(tz):
        if tz is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = tz
        time.tzset()

    def test_against_the_c_library(self):
        zones = gui_zones()
        self.assertGreaterEqual(len(zones), 20)
        for posix in zones:
            z = gn.Zone(posix)
            os.environ["TZ"] = posix
            time.tzset()
            for t in range(utc(2026, 1, 1, 0, 0), utc(2027, 1, 1, 0, 0), 3 * 3600 + 17 * 60):
                lt = time.localtime(t)
                self.assertEqual(z.offset(t), lt.tm_gmtoff, f"{posix} at {t}")
                self.assertEqual(z.name(t), lt.tm_zone, f"{posix} at {t}")
            # the changes to summer time and back, to the minute
            for t in range(utc(2026, 3, 7, 0, 0), utc(2026, 4, 6, 0, 0), 60 * 13):
                self.assertEqual(z.offset(t), time.localtime(t).tm_gmtoff, f"{posix} spring at {t}")
            for t in range(utc(2026, 9, 26, 0, 0), utc(2026, 11, 3, 0, 0), 60 * 13):
                self.assertEqual(z.offset(t), time.localtime(t).tm_gmtoff, f"{posix} autumn at {t}")

    def test_local_to_utc(self):
        for posix in ("CET-1CEST,M3.5.0,M10.5.0/3", "AEST-10AEDT,M10.1.0,M4.1.0/3", "<-03>3", "IST-5:30", "UTC0"):
            z = gn.Zone(posix)
            os.environ["TZ"] = posix
            time.tzset()
            for y, mo, d, hh, mm in ((2026, 1, 15, 21, 0), (2026, 7, 16, 21, 0), (2026, 11, 19, 23, 30),
                                     (2026, 2, 30, 8, 15), (2026, 11, 0, 12, 0)):
                want = time.mktime((y, mo, d, hh, mm, 0, 0, 0, -1))
                self.assertEqual(z.utc_of(y, mo, d, hh, mm), int(want), f"{posix} {y}-{mo}-{d} {hh}:{mm}")

    def test_bad_string_is_utc(self):
        z = gn.Zone("not a zone")
        self.assertEqual((z.offset(0), z.name(0)), (0, "UTC"))

    def test_radio_zone_file(self):
        d = fresh_dir()
        old = settings.TZ_CONF
        self.addCleanup(setattr, settings, "TZ_CONF", old)
        settings.TZ_CONF = os.path.join(d, "tz.conf")
        self.assertEqual(gn.radio_zone().name(0), "UTC")        # no file: UTC, as the GUI
        with open(settings.TZ_CONF, "w") as f:
            f.write("EST5EDT,M3.2.0,M11.1.0\n")
        self.assertEqual(gn.radio_zone().offset(utc(2026, 7, 1, 12, 0)), -4 * 3600)


class Schedule(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(SHIPPED) as f:
            cls.nets, cls.segs = gn.parse_schedule(f.read())

    def test_shipped_file(self):
        self.assertEqual(len(self.nets), 7)
        self.assertEqual(len(self.segs), 23)
        self.assertEqual(self.nets["EU"], "Europe")
        self.assertEqual(self.nets["B_AUSP"], "Bridge Australia-South Pacific")
        rtty = [g for g in self.segs if g["mode"] == "RTTY"]
        self.assertEqual({(g["net"], g["khz"]) for g in rtty}, {("NA", 7077), ("EU", 7077), ("AU", 7077)})
        na = self.segs[0]
        self.assertEqual((na["net"], na["wday"], na["hh"], na["mm"], na["minutes"], na["khz"], na["mode"]),
                         ("NA", 5, 1, 0, 30, 7107, "JS8"))

    def test_bad_lines(self):
        nets, segs = gn.parse_schedule("net X Some net # comment\n"
                                       "X Thu 25:00 30 7107 JS8 hour out of range\n"
                                       "X Fun 18:00 30 7107 JS8 no such day\n"
                                       "X Thu 18:00 0 7107 JS8 no length\n"
                                       "X Thu 18:00 30 71O7 JS8 not a number\n"
                                       "X Thu 18:00 30\n"
                                       "X thu 18:00 30 - ALE   a PC's window  \n"
                                       "\n# only a comment\n")
        self.assertEqual(nets, {"X": "Some net"})
        self.assertEqual(len(segs), 1)
        self.assertEqual((segs[0]["wday"], segs[0]["khz"], segs[0]["what"]), (4, 0, "a PC's window"))

    def test_next_utc(self):
        thu17 = utc(2026, 11, 19, 17, 0)          # a Thursday, before the European net
        self.assertEqual(gn.next_utc(thu17, 4, 18, 0, 30), utc(2026, 11, 19, 18, 0))
        self.assertEqual(gn.next_utc(utc(2026, 11, 19, 18, 10), 4, 18, 0, 30), utc(2026, 11, 19, 18, 0))   # on now
        self.assertEqual(gn.next_utc(utc(2026, 11, 19, 18, 30), 4, 18, 0, 30), utc(2026, 11, 26, 18, 0))   # over
        # a window across midnight UTC, still on early the next day
        self.assertEqual(gn.next_utc(utc(2026, 11, 20, 0, 20), 4, 23, 30, 70), utc(2026, 11, 19, 23, 30))
        # and the C test's own case: North America's net (Friday 01:00 UTC) seen on Thursday evening
        self.assertEqual(gn.next_utc(utc(2026, 11, 19, 22, 0), 5, 1, 0, 30), utc(2026, 11, 20, 1, 0))

    def test_next_local(self):
        uy = gn.Zone("<-03>3")
        # an own net Thursday 22:30 in Uruguay is Friday 01:30 UTC
        self.assertEqual(gn.next_local(utc(2026, 11, 19, 12, 0), uy, 4, 22, 30, 30), utc(2026, 11, 20, 1, 30))
        # seen at 01:45 UTC on Friday, it is still on
        self.assertEqual(gn.next_local(utc(2026, 11, 20, 1, 45), uy, 4, 22, 30, 30), utc(2026, 11, 20, 1, 30))
        cet = gn.Zone("CET-1CEST,M3.5.0,M10.5.0/3")
        # 21:00 on Thursday is 20:00 UTC in winter and 19:00 UTC in summer
        self.assertEqual(gn.next_local(utc(2026, 11, 16, 12, 0), cet, 4, 21, 0, 30), utc(2026, 11, 19, 20, 0))
        self.assertEqual(gn.next_local(utc(2026, 7, 13, 12, 0), cet, 4, 21, 0, 30), utc(2026, 7, 16, 19, 0))
        # the week of the change: Monday before is summer, the Thursday after is winter
        self.assertEqual(gn.next_local(utc(2026, 10, 23, 12, 0), cet, 4, 21, 0, 30), utc(2026, 10, 29, 20, 0))

    def test_when(self):
        self.assertEqual(gn.when(utc(2026, 11, 19, 18, 0), gn.Zone("UTC0")), "Thu 18:00 UTC")
        self.assertEqual(gn.when(utc(2026, 11, 19, 18, 0), gn.Zone("CET-1CEST,M3.5.0,M10.5.0/3")),
                         "Thu 18:00 UTC (19:00 CET)")
        self.assertEqual(gn.when(utc(2026, 11, 20, 1, 0), gn.Zone("EST5EDT,M3.2.0,M11.1.0")),
                         "Fri 01:00 UTC (Thu 20:00 EST)")
        self.assertEqual(gn.when(utc(2026, 7, 16, 18, 0), gn.Zone("CET-1CEST,M3.5.0,M10.5.0/3")),
                         "Thu 18:00 UTC (20:00 CEST)")

    def test_schedule_and_reminder(self):
        conf = dict(gn.DEFAULTS, remind=True, nets=["B_NAAU", "OWN"], own_on=True, own_day=6, own_time="09:00")
        now = utc(2026, 11, 21, 11, 0)             # Saturday, before the bridge North America-Australia
        rows = gn.schedule(conf, self.nets, self.segs, gn.Zone("UTC0"), now)
        self.assertTrue(all(rows[i]["start"] <= rows[i + 1]["start"] for i in range(len(rows) - 1)))
        naau = [r for r in rows if r["net"] == "B_NAAU"]
        self.assertEqual([(r["mode"], r["khz"], r["reminder"]) for r in naau],
                         [("JS8", 14107, True), ("ALE", 0, False), ("JS8", 3575, False)])   # the 3.575 one follows
        own = [r for r in rows if r["net"] == "OWN"]
        self.assertEqual(len(own), 1)
        self.assertEqual(own[0]["start"], utc(2026, 11, 28, 9, 0))   # this Saturday's is over: next week's
        self.assertTrue(own[0]["reminder"])
        eu = [r for r in rows if r["net"] == "EU"]
        self.assertTrue(eu and not any(r["reminder"] for r in eu) and not any(r["reminded"] for r in eu))
        # a window on now
        on = gn.schedule(conf, self.nets, self.segs, gn.Zone("UTC0"), utc(2026, 11, 21, 12, 10))
        self.assertTrue(on[0]["now"] and on[0]["net"] == "B_NAAU")
        # the reminder off: no stars, the nets still marked
        rows = gn.schedule(dict(conf, remind=False), self.nets, self.segs, gn.Zone("UTC0"), now)
        self.assertFalse(any(r["reminder"] for r in rows))
        self.assertTrue(any(r["reminded"] for r in rows))
        # days
        self.assertTrue(all(r["start"] <= now + 86400 for r in gn.schedule(conf, self.nets, self.segs,
                                                                            gn.Zone("UTC0"), now, days=1)))


class Conf(unittest.TestCase):
    def test_same_keys_as_the_gui(self):
        with open(os.path.join(TOP, "src", "ghostnet.c")) as f:
            c = f.read()
        writer = c[c.index("bool ghostnet_conf_save"):]
        writer = writer[:writer.index("fflush")]
        keys = re.findall(r'(?:"|\\n)(\w+)=%', writer)     # after the format's quote or a \n
        self.assertEqual(keys, list(gn.DEFAULTS))
        text = gn.conf_text(gn.DEFAULTS)
        self.assertEqual(re.findall(r"^(\w+)=", text, re.M), list(gn.DEFAULTS))

    def test_parse_as_the_gui(self):
        c = gn.parse_conf("remind=1\nremind_min=500\nnets=EU,OWN\nown_on=1\nown_name=Red hispana\nown_day=9\n"
                          "own_time=20:45\nown_min=3\nown_khz=7107\nown_mode=FT8\ndept=v-o-v2\nregion=@GNBELVOV\n"
                          "# remind=0\nnonsense\ntranslate_rx=1\n")
        self.assertTrue(c["remind"])
        self.assertEqual(c["remind_min"], 10)
        self.assertEqual(c["nets"], ["EU", "OWN"])
        self.assertEqual((c["own_name"], c["own_day"], c["own_time"], c["own_min"]), ("Red hispana", 4, "20:45", 30))
        self.assertEqual((c["own_mode"], c["dept"], c["region"]), ("JS8", "VOV", "@GNBELVOV"))
        self.assertTrue(c["translate_rx"] and not c["translate_tx"])
        self.assertEqual(gn.parse_conf(gn.conf_text(c)), c)                                 # round trip

    def test_changes(self):
        base = dict(gn.DEFAULTS, nets=[])
        ids = {"EU": "Europe", "NA": "North America"}
        c, err = gn.change_conf(base, {"remind": True, "remind_min": "15", "nets": ["EU", "OWN", "EU"],
                                       "own_on": True, "own_name": "  Club\nnet ", "own_day": "6",
                                       "own_time": "7:05", "own_min": 45, "own_khz": "7.1075", "own_mode": "RTTY",
                                       "region": "gnurymo", "dept": "mo", "translate_tx": True}, ids)
        self.assertIsNone(err)
        self.assertEqual((c["remind"], c["remind_min"], c["nets"]), (True, 15, ["EU", "OWN"]))
        self.assertEqual((c["own_name"], c["own_day"], c["own_time"], c["own_min"]), ("Club net", 6, "07:05", 45))
        self.assertEqual((c["own_khz"], c["own_mode"], c["region"], c["dept"]), (7108, "RTTY", "@GNURYMO", "MO"))
        self.assertTrue(c["translate_tx"] and not c["translate_rx"])
        self.assertEqual(base["nets"], [])                                                 # a copy changed
        refused = [{"remind": "yes"}, {"remind_min": 0}, {"remind_min": "121"}, {"remind_min": True},
                   {"own_day": 7}, {"own_min": 4}, {"own_khz": "1799"}, {"own_khz": "abc"}, {"own_time": "24:00"},
                   {"own_time": "9"}, {"own_mode": "FT8"}, {"nets": "EU"}, {"nets": ["XX"]},
                   {"own_name": "x" * 32}, {"dept": "ABCD"}, {"dept": "A-B"}, {"region": "@XYZ"},
                   {"region": "@GN" + "A" * 13}, {"translate_rx": 1}]
        for change in refused:
            c, err = gn.change_conf(base, change, ids)
            self.assertIsNone(c, change)
            self.assertTrue(err, change)
        c, err = gn.change_conf(base, {"region": "", "own_name": "  "}, ids)
        self.assertEqual((c["region"], c["own_name"]), ("", "Own net"))

    def test_region(self):
        conf = dict(gn.DEFAULTS, dept="MO")
        self.assertEqual(gn.region_tag(conf, "UY")[0], "@GNURYMO")
        self.assertEqual(gn.region_tag(dict(conf, dept=""), "be")[0], "@GNBEL")
        self.assertEqual(gn.region_tag(conf, "")[0], "")
        self.assertEqual(gn.region_tag(conf, "ZZ")[0], "")
        self.assertEqual(gn.region_tag(dict(conf, region="@GNARGBA"), "UY")[0], "@GNARGBA")

    def test_region_from_the_radios_country(self):
        """country_iso2() reads "country" from params.db as the GUI's country_code(); the group typed still wins"""
        conf = dict(gn.DEFAULTS, dept="MO")
        self.addCleanup(setattr, settings, "DB_PATH", settings.DB_PATH)
        settings.DB_PATH = os.path.join(fresh_dir(), "params.db")
        self.assertEqual(gn.country_iso2(), "")                 # no params.db
        self.assertEqual(gn.region_tag(conf)[0], "")
        con = sqlite3.connect(settings.DB_PATH)
        con.execute("CREATE TABLE params (name TEXT PRIMARY KEY, val TEXT)")
        con.commit()
        self.assertEqual(gn.country_iso2(), "")                 # no country set yet
        for val, want in (("UY", "UY"), ("be", "BE"), ("", ""), ("BEL", ""), ("1", "")):
            con.execute("INSERT OR REPLACE INTO params VALUES ('country', ?)", (val,))
            con.commit()
            self.assertEqual(gn.country_iso2(), want, val)
        con.execute("INSERT OR REPLACE INTO params VALUES ('country', 'UY')")
        con.commit()
        con.close()
        self.assertEqual(gn.region_tag(conf), ("@GNURYMO", "the radio's country and the department"))
        self.assertEqual(gn.region_tag(dict(conf, region="@GNARGBA"))[0], "@GNARGBA")

    def test_iso3_as_the_gui(self):
        with open(os.path.join(TOP, "src", "ghostnet_iso3.inc")) as f:
            rows = dict(re.findall(r'\{"([A-Z]{2})", "([A-Z]{3})"\}', f.read()))
        self.assertEqual(len(rows), 249)
        self.assertEqual(rows, ISO3)


class Messages(unittest.TestCase):
    def setUp(self):
        d = fresh_dir()
        old = settings.GHOSTNET_LOG
        self.addCleanup(setattr, settings, "GHOSTNET_LOG", old)
        settings.GHOSTNET_LOG = os.path.join(d, "ghostnet_log.txt")
        t = utc(2026, 11, 19, 18, 5)
        with open(settings.GHOSTNET_LOG + ".1", "w") as f:
            f.write(f"R\t{t - 600}\t3\tCX1ABC\t@GHOSTNET\t-12\t7107000\tCX1ABC: @GHOSTNET QRV GF15\n")
        with open(settings.GHOSTNET_LOG, "w") as f:
            f.write(f"R\t{t}\t7\tLU1AAA\t@GHOSTNET\t-5\t7107000\tLU1AAA: @GHOSTNET BUENAS NOCHES A TODOS\n"
                    f"T\t{t + 9}\t7\tLU1AAA\t@GHOSTNET\t-5\t7107000\tGood evening everyone\n"
                    f"X\t{t + 60}\t0\tON6URE\t@GHOSTNET\t0\t7107000\tON6URE: @GHOSTNET QRV JO20\n"
                    f"R\t{t + 120}\t8\tCX1ABC\t@GSTFLASH\t-8\t7107000\tCX1ABC: @GSTFLASH ROAD CLOSED\n"
                    f"R\t{t + 130}\t9\tCX2XYZ\tON6URE\t-20\t7107000\tCX2XYZ: ON6URE SEE @GSTFLASH/2 MSG\n"
                    "R\tnot a time\t1\tX\t\t0\t0\tbad\n"
                    "Z\t1\t1\tX\t\t0\t0\tunknown kind\n"
                    "R\t1\t1\n")

    def test_messages(self):
        msgs, stations = gn.messages(gn.Zone("UTC0"))
        self.assertEqual([m["from"] for m in msgs], ["CX2XYZ", "CX1ABC", "ON6URE", "LU1AAA", "CX1ABC"])
        self.assertEqual(msgs[3]["translation"], "Good evening everyone")
        self.assertEqual(msgs[2]["kind"], "X")
        self.assertEqual(msgs[2]["translation"], "")
        self.assertTrue(msgs[1]["flash"])
        self.assertFalse(msgs[0]["flash"])                      # @GSTFLASH/2 is not the group
        self.assertEqual(msgs[4]["local"], "11-19 17:55")
        self.assertEqual([s["call"] for s in stations], ["CX2XYZ", "CX1ABC", "LU1AAA"])   # not the operator's own
        cx = stations[1]
        self.assertEqual((cx["count"], cx["groups"], cx["snr"]), (2, ["@GHOSTNET", "@GSTFLASH"], -8))
        msgs, _ = gn.messages(gn.Zone("UTC0"), limit=2)
        self.assertEqual(len(msgs), 2)

    def test_no_log(self):
        settings.GHOSTNET_LOG = os.path.join(fresh_dir(), "none.txt")
        self.assertEqual(gn.messages(gn.Zone("UTC0")), ([], []))


@unittest.skipUnless(bottle, "no bottle: give BOTTLE=<a folder with bottle.py>")
class Web(unittest.TestCase):
    def setUp(self):
        d = fresh_dir()
        for name in ("GHOSTNET_CONF", "GHOSTNET_LOG", "GHOSTNET_NETS_USER", "GHOSTNET_NETS", "TZ_CONF", "DB_PATH"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        settings.GHOSTNET_CONF = os.path.join(d, "ghostnet.conf")
        settings.GHOSTNET_LOG = os.path.join(d, "ghostnet_log.txt")
        settings.GHOSTNET_NETS_USER = os.path.join(d, "ghostnet_nets.txt")
        settings.GHOSTNET_NETS = SHIPPED
        settings.TZ_CONF = os.path.join(d, "tz.conf")
        settings.DB_PATH = os.path.join(d, "params.db")
        with open(settings.TZ_CONF, "w") as f:
            f.write("CET-1CEST,M3.5.0,M10.5.0/3\n")

    def req(self, method, path, body=None, raw=None):
        data = raw if raw is not None else (b"" if body is None else json.dumps(body).encode())
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

    def test_read_and_save(self):
        code, r = self.req("GET", "/api/ghostnet")
        self.assertEqual(code, 200)
        self.assertEqual(r["conf"], dict(gn.DEFAULTS, nets=[]))
        self.assertEqual(len(r["nets"]), 7)
        self.assertEqual(len(r["schedule"]) >= 15, True)
        self.assertIn(r["zone"], ("CET", "CEST"))
        self.assertTrue(r["schedule"][0]["when"].endswith(")"))       # the radio's time beside UTC
        self.assertEqual(r["region"], "")
        code, r = self.req("POST", "/api/ghostnet", {"remind": True, "nets": ["EU", "OWN"], "own_on": True,
                                                     "own_name": "Club net", "own_time": "21:30"})
        self.assertEqual(code, 200, r)
        self.assertTrue(r["conf"]["remind"] and r["conf"]["own_on"])
        self.assertTrue(any(w["net"] == "OWN" for w in r["schedule"]))
        with open(settings.GHOSTNET_CONF) as f:
            text = f.read()
        self.assertIn("remind=1\n", text)
        self.assertIn("nets=EU,OWN\n", text)
        self.assertIn("own_time=21:30\n", text)
        self.assertEqual([n for n in os.listdir(os.path.dirname(settings.GHOSTNET_CONF)) if n.endswith(".tmp")], [])
        # refused, the file as it was
        code, r = self.req("POST", "/api/ghostnet", {"own_khz": "99999"})
        self.assertEqual(code, 400)
        self.assertIn("kHz", r["msg"])
        code, r = self.req("POST", "/api/ghostnet", raw=b"[1, 2]")
        self.assertEqual(code, 400)
        code, r = self.req("POST", "/api/ghostnet", raw=b"{not json")
        self.assertEqual(code, 400)
        with open(settings.GHOSTNET_CONF) as f:
            self.assertEqual(f.read(), text)
        # a schedule of the operator's on the DATA partition comes first
        with open(settings.GHOSTNET_NETS_USER, "w") as f:
            f.write("net T Test net\nT Mon 10:00 30 7107 JS8 Test\n")
        code, r = self.req("GET", "/api/ghostnet")
        self.assertEqual([n["id"] for n in r["nets"]], ["T"])

    def test_page(self):
        code, page = self.req("GET", "/ghostnet")
        self.assertEqual(code, 200)
        self.assertIn(b"GhostNet", page)
        self.assertIn(b'href="/ghostnet"', page)                # in the menu


if __name__ == "__main__":
    os.makedirs(DATA, exist_ok=True)
    out = io.StringIO()
    result = unittest.TextTestRunner(stream=out, verbosity=2 if "-v" in sys.argv else 1).run(
        unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    if not result.wasSuccessful() or "-v" in sys.argv:
        sys.stderr.write(out.getvalue())
    skipped = f", {len(result.skipped)} skipped" if result.skipped else ""
    print(f"ghostnet_web_test: {result.testsRun} tests, {len(result.failures) + len(result.errors)} failed{skipped}")
    sys.exit(0 if result.wasSuccessful() else 1)
