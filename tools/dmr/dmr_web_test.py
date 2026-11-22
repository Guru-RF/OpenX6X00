#!/usr/bin/env python3
"""The web DMR page's Listening log (webserver apps.py: /api/dmr/log, log.adi, log.csv) and Map (/api/dmr/map) on
this PC, over a heard.tsv as rootfs/usr/sbin/x6100-dmr writes it (dmr_test.py checks that it does). Nothing is
contacted.

  - the list: the newest first, heard.tsv.1 before heard.tsv, a name filled in from names.json when the call was
    logged without one, a private call, lines that are no call's left out; the 500 newest and "more";
  - the search: part of a callsign or a name, a DMR ID, a talkgroup, the days (the radio's local time); a search that
    is not one refused;
  - the ADIF: our logs' header, a record for each call with a callsign, the oldest first, as a listening report
    (SWL Y, DIGITALVOICE / DMR, INTERNET, no BAND and no FREQ), UTC, the lengths in bytes (a name with an umlaut),
    every record read back by the web server's own ADIF reader;
  - the CSV: all the calls, a cell that a spreadsheet would take for a formula made text;
  - the map: the stations kept, from the helper itself (its "map", asked once in 20 s) while no helper listens; a
    listening helper's own file while it is fresh, with the station talking; the page's look told to the helper.

    BOTTLE=<a folder with bottle.py> python3 -I -B dmr_web_test.py      (without bottle: skipped)
"""
import csv
import io
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import time
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
HELPER = os.path.join(TOP, "rootfs", "usr", "sbin", "x6100-dmr")
CTY = os.path.join(TOP, "third-party", "cty", "cty.dat")


def import_web():
    sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(TOP, "webserver", "src")]
    sys.dont_write_bytecode = True
    try:
        import cgi  # noqa: F401
    except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for forms only
        sys.modules["cgi"] = types.ModuleType("cgi")
        sys.modules["cgi"].FieldStorage = object
    try:
        import bottle  # noqa: F401
    except ImportError:
        return None
    from x6100_webserver import apps, settings
    return apps, settings


WEB = import_web()
T0 = 1795012000             # 2026-11-18 14:26:40 UTC
DAY = 86400


def row(start, src, name, tg, secs, network="FreeDMR", kind="group"):
    return f"{start}\t{src}\t{name}\t{tg}\t{secs}\t{network}\t{kind}\n"


@unittest.skipIf(WEB is None, "no bottle: give BOTTLE=<a folder with bottle.py>")
class DmrLog(unittest.TestCase):
    def setUp(self):
        apps, settings = WEB
        self.tmp = tempfile.mkdtemp(prefix="dmr-web-test.")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        saved = {k: getattr(settings, k) for k in ("DMR_DIR", "DB_PATH", "APP_VERSION")}
        self.addCleanup(lambda: [setattr(settings, k, v) for k, v in saved.items()])
        settings.DMR_DIR = os.path.join(self.tmp, "dmr")
        settings.DB_PATH = os.path.join(self.tmp, "params.db")
        settings.APP_VERSION = os.path.join(self.tmp, "app-version")
        os.makedirs(settings.DMR_DIR)
        with open(settings.APP_VERSION, "w") as f:
            f.write("9.9.9\n")
        con = sqlite3.connect(settings.DB_PATH)
        con.execute("CREATE TABLE params (name TEXT PRIMARY KEY, val TEXT)")
        con.execute("INSERT INTO params VALUES ('callsign', 'te5t/p')")
        con.commit()
        con.close()
        saved_tz = os.environ.get("TZ")
        os.environ["TZ"] = "UTC-2"          # the radio two hours east of UTC: the days are local, the files UTC
        time.tzset()
        self.addCleanup(self.tz_back, saved_tz)
        self.write("heard.tsv.1", row(T0 - 3 * DAY, 9990001, "ON4ABC Jan", 91, 12))
        self.write("heard.tsv",
                   row(T0, 9990002, "", 2062, 7)                                    # the name came later
                   + "not a call\n" + "x\t9990003\tA\t91\t1\n" + "\n"
                   + row(T0 + 60, 9990003, "", 91, 3)                               # an ID nobody knows
                   + row(T0 + 120, 9990004, "DL1JUR Jürgen", 9990000, 5, kind="private")
                   + row(T0 + 9 * 3600 + 1800, 9990005, "Club station", 91, 2, network="example.org")
                   + row(T0 + DAY, 9990006, "=HYPERLINK(1) x", 91, 4)
                   + f"{T0 + 2 * DAY}\t9990001\tON4ABC Jan\t91\t65\n")              # no network: an older line
        self.write("names.json", json.dumps({"9990002": ["PA0XYZ Piet", T0], "9990001": "ON4ABC Jan",
                                             "9990003": ["", T0]}))

    @staticmethod
    def tz_back(tz):
        os.environ.pop("TZ", None)
        if tz is not None:
            os.environ["TZ"] = tz
        time.tzset()

    def write(self, name, text):
        with open(os.path.join(WEB[1].DMR_DIR, name), "w", encoding="utf-8") as f:
            f.write(text)

    def get(self, path, query="", status=200):
        """The route's answer as the server sends it: (the body in bytes, the headers)"""
        apps = WEB[0]
        out = {}

        def start(st, headers):
            out["status"], out["headers"] = int(st.split()[0]), dict(headers)

        env = {"REQUEST_METHOD": "GET", "PATH_INFO": path, "QUERY_STRING": query, "SERVER_NAME": "radio",
               "SERVER_PORT": "80", "REMOTE_ADDR": "127.0.0.1", "wsgi.input": io.BytesIO(), "wsgi.errors": sys.stderr,
               "wsgi.url_scheme": "http", "HTTP_HOST": "127.0.0.1"}
        body = b"".join(apps.app(env, start))
        self.assertEqual(out["status"], status, body[:300])
        return body, out["headers"]

    def rows(self, query=""):
        return json.loads(self.get("/api/dmr/log", query)[0])["rows"]

    def test_list(self):
        d = json.loads(self.get("/api/dmr/log")[0])
        self.assertFalse(d["more"])
        r = d["rows"]
        self.assertEqual([x["id"] for x in r], [9990001, 9990006, 9990005, 9990004, 9990003, 9990002, 9990001])
        self.assertEqual(r[0], {"start": T0 + 2 * DAY, "local": "2026-11-20 16:26:40", "id": 9990001, "call": "ON4ABC",
                                "name": "Jan", "tg": 91, "secs": 65, "network": "", "private": False})
        self.assertEqual((r[5]["call"], r[5]["name"]), ("PA0XYZ", "Piet"))          # from names.json
        self.assertEqual((r[4]["call"], r[4]["name"]), ("", ""))
        self.assertEqual((r[3]["call"], r[3]["name"], r[3]["private"], r[3]["tg"]), ("DL1JUR", "Jürgen", True, 9990000))
        self.assertEqual((r[2]["call"], r[2]["name"], r[2]["network"]), ("", "Club station", "example.org"))
        self.assertEqual(r[6]["local"], "2026-11-15 16:26:40")

    def test_search(self):
        ids = lambda q: [x["id"] for x in self.rows(q)]     # noqa: E731
        self.assertEqual(ids("q=on4"), [9990001, 9990001])
        self.assertEqual(ids("q=piet"), [9990002])
        self.assertEqual(ids("q=9990003"), [9990003])
        self.assertEqual(ids("q=J%C3%BCrgen"), [9990004])
        self.assertEqual(ids("tg=2062"), [9990002])
        self.assertEqual(ids("tg=91&q=ON4ABC&from=2026-11-16"), [9990001])
        # T0 + 9.5 h is 23:56 UTC on the 18th and 01:56 on the 19th where the radio is
        self.assertEqual(ids("from=2026-11-19&to=2026-11-19"), [9990006, 9990005])
        self.assertEqual(ids("to=2026-11-15"), [9990001])
        self.assertEqual(ids("q=nobody"), [])
        for bad in ("tg=abc", "from=19-11-2026", "to=2026-11-19x", "q=" + "a" * 41):
            self.assertIn("msg", json.loads(self.get("/api/dmr/log", bad, 400)[0]))
            self.get("/api/dmr/log.adi", bad, 400)
            self.get("/api/dmr/log.csv", bad, 400)

    def test_many(self):
        self.write("heard.tsv", "".join(row(T0 + i, 9990001, "ON4ABC Jan", 91, 1) for i in range(700)))
        d = json.loads(self.get("/api/dmr/log")[0])
        self.assertTrue(d["more"])
        self.assertEqual(len(d["rows"]), 500)
        self.assertEqual(d["rows"][0]["start"], T0 + 699)
        adif = self.get("/api/dmr/log.adi")[0]
        self.assertEqual(adif.count(b"<eor>"), 701)             # the downloads: all of them, and heard.tsv.1's

    def test_adif(self):
        apps = WEB[0]
        body, headers = self.get("/api/dmr/log.adi")
        self.assertRegex(headers["Content-Disposition"], r'^attachment; filename="x6100-dmr-heard-\d{8}-\d{4}\.adi"$')
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))
        head, parts = apps.adif_split(body)
        self.assertTrue(head.startswith(b"OpenX6X00 DMR listening log"))
        self.assertIn(b"<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <PROGRAMVERSION:5>9.9.9 <eoh>", head)
        recs = [p for p in parts if p.strip()]
        f = [apps.adif_fields(r) for r in recs]
        # the oldest first; the ID nobody knows, the name without a callsign and the formula's "callsign" are no report
        self.assertEqual([x["call"] for x in f], ["ON4ABC", "PA0XYZ", "DL1JUR", "ON4ABC"])
        for x in f:
            self.assertEqual((x["mode"], x["submode"], x["swl"], x["prop_mode"]), ("DIGITALVOICE", "DMR", "Y", "INTERNET"))
            self.assertEqual(x["station_callsign"], "TE5T/P")
            self.assertNotIn("band", x)
            self.assertNotIn("freq", x)
            self.assertNotIn("rst_sent", x)
        self.assertEqual((f[0]["qso_date"], f[0]["time_on"], f[0]["time_off"], f[0]["name"]),
                         ("20261115", "142640", "142652", "Jan"))
        self.assertEqual(f[0]["comment"], "Heard on DMR over the internet: TG 91, FreeDMR, 12 s, DMR ID 9990001")
        self.assertEqual(f[2]["comment"], "Heard on DMR over the internet: private call, FreeDMR, 5 s, DMR ID 9990004")
        self.assertEqual(f[3]["comment"], "Heard on DMR over the internet: TG 91, 65 s, DMR ID 9990001")
        self.assertEqual((f[3]["time_on"], f[3]["time_off"]), ("142640", "142745"))
        # a call over midnight UTC ends the next day
        self.write("heard.tsv", row(T0 + 9 * 3600 + 33 * 60 + 10, 9990001, "ON4ABC Jan", 91, 30))
        self.write("heard.tsv.1", "")
        x = apps.adif_fields([p for p in apps.adif_split(self.get("/api/dmr/log.adi")[0])[1] if p.strip()][0])
        self.assertEqual((x["qso_date"], x["time_on"], x["qso_date_off"], x["time_off"]),
                         ("20261118", "235950", "20261119", "000020"))

    def test_adif_bytes(self):
        """A length is the value's bytes: the name with an umlaut is one byte longer than its characters"""
        self.write("heard.tsv", row(T0, 9990004, "DL1JUR Jürgen", 91, 5))
        body = self.get("/api/dmr/log.adi")[0]
        self.assertIn("<NAME:7>Jürgen ".encode(), body)
        self.assertIn(b"<CALL:6>DL1JUR <QSO_DATE:8>20261118 <TIME_ON:6>142640 <QSO_DATE_OFF:8>20261118 "
                      b"<TIME_OFF:6>142645 <MODE:12>DIGITALVOICE <SUBMODE:3>DMR <SWL:1>Y <PROP_MODE:8>INTERNET ", body)
        self.assertTrue(body.endswith(b"<STATION_CALLSIGN:6>TE5T/P <eor>\n"))

    def test_csv(self):
        body, headers = self.get("/api/dmr/log.csv", "from=2026-11-18")
        self.assertRegex(headers["Content-Disposition"], r'^attachment; filename="x6100-dmr-heard-\d{8}-\d{4}\.csv"$')
        rows = list(csv.reader(io.StringIO(body.decode("utf-8"))))
        self.assertEqual(rows[0], ["UTC", "CALL", "NAME", "DMR_ID", "TALKGROUP", "PRIVATE", "SECONDS", "NETWORK"])
        self.assertEqual(rows[1], ["2026-11-20 14:26:40", "ON4ABC", "Jan", "9990001", "91", "", "65", ""])
        self.assertEqual(rows[2], ["2026-11-19 14:26:40", "", "'=HYPERLINK(1) x", "9990006", "91", "", "4", "FreeDMR"])
        self.assertEqual(rows[4][1:6], ["DL1JUR", "Jürgen", "9990004", "9990000", "Y"])
        self.assertEqual(rows[5][1:4], ["", "", "9990003"])
        self.assertEqual(len(rows), 7)

    def test_map(self):
        apps, settings = WEB
        saved = {k: getattr(settings, k) for k in ("DMR_CMD", "DMR_MAP", "DMR_MAP_WANT")}
        self.addCleanup(lambda: [setattr(settings, k, v) for k, v in saved.items()])
        settings.DMR_CMD = HELPER
        settings.DMR_MAP = os.path.join(self.tmp, "map.json")
        settings.DMR_MAP_WANT = os.path.join(self.tmp, "map.want")
        for k, v in {"X6100_CTY": CTY, "X6100_DMR_STATUS": os.path.join(self.tmp, "none.json"),
                     "X6100_DMR_MAP_WANT": settings.DMR_MAP_WANT, "X6100_DMR_RADIOID": "", "X6100_DMR_PLACE": "",
                     "X6100_DMR_LOG": os.path.join(self.tmp, "log.txt")}.items():
            self.addCleanup(os.environ.pop, k, None)
            os.environ[k] = v
        apps.dmr_map_kept.update(at=0.0, map=None)
        now = int(time.time())
        self.write("heard.tsv", row(now - 3600, 9990001, "ON4ABC Jan", 91, 12) + row(now - 60, 9990002, "", 2062, 7)
                   + row(now - 30 * 3600, 9990007, "W1ABC Sam", 91, 5))
        d = json.loads(self.get("/api/dmr/map")[0])
        self.assertTrue(os.path.exists(settings.DMR_MAP_WANT))          # the helper is told somebody looks
        self.assertEqual((d["live"], d["talker"], d["hours"]), (False, None, 24))
        self.assertEqual([(s["id"], s["call"], s["country"], s["recent"]) for s in d["stations"]],
                         [(9990002, "PA0XYZ", "Netherlands", True), (9990001, "ON4ABC", "Belgium", False)])
        self.write("heard.tsv", "")                                     # asked again only after 20 s
        self.assertEqual(len(json.loads(self.get("/api/dmr/map")[0])["stations"]), 2)
        # a helper that listens writes its own
        live = {"t": now, "hours": 24, "stations": [], "talker": {"id": 9990001, "call": "ON4ABC", "name": "Jan",
                                                                   "tg": 91, "city": "Gent", "country": "Belgium",
                                                                   "lat": 51.054, "lon": 3.725, "exact": True}}
        with open(settings.DMR_MAP, "w") as f:
            json.dump(live, f)
        self.assertEqual(json.loads(self.get("/api/dmr/map")[0]), dict(live, live=True))
        old = time.time() - 60
        os.utime(settings.DMR_MAP, (old, old))                          # it stopped: the stations kept again
        self.assertFalse(json.loads(self.get("/api/dmr/map")[0])["live"])
        settings.DMR_CMD = os.path.join(self.tmp, "no-such-helper")     # no helper: an empty map, no error
        apps.dmr_map_kept.update(at=0.0, map=None)
        self.assertEqual(json.loads(self.get("/api/dmr/map")[0]),
                         {"t": 0, "hours": 24, "stations": [], "talker": None, "live": False})

    def test_empty(self):
        shutil.rmtree(WEB[1].DMR_DIR)
        self.assertEqual(json.loads(self.get("/api/dmr/log")[0]), {"rows": [], "more": False})
        self.assertTrue(self.get("/api/dmr/log.adi")[0].endswith(b"<eoh>\n"))
        self.assertEqual(len(self.get("/api/dmr/log.csv")[0].splitlines()), 1)


if __name__ == "__main__":
    unittest.main(verbosity=1)
