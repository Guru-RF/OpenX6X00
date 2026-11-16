#!/usr/bin/env python3
"""The Weather app's helper (rootfs/usr/sbin/x6100-meteo) and the web Weather page's API (webserver apps.py), with
the services' answers saved in samples/ (made by hand in each service's own form) and the helper's network replaced by
a stand-in that serves them by address. Nothing here goes to any service.

    python3 -I -B tools/meteo/meteo_test.py [-v]          (BOTTLE=<a folder with bottle.py> for the web part)

1. The helper's own parts: the position rounded to 0.1 degree, the request's headers (OpenX6X00/<version> and the
   repository, nothing of the user), the place's time zone (with and without its rules), texts brought to the radio's
   fonts, the credits each service asks for.
2. Each service's answer read: Open-Meteo's forecast; Nominatim's place, kept for good, asked only when a person
   asked, never twice within a second, switched to another server or off by /mnt/meteo/nominatim.url; the NWS's
   GeoJSON; MeteoAlarm's feed (the place's own by their areas' names, the country's other orange and red ones, green,
   cancelled, test and expired ones left out, the GUI's language); the CAP feeds of SMN and INMET (RSS, CAP 1.2 with
   and without its namespace prefix, CDATA, entities, the position in a polygon or near one, each message read once
   and kept, old ones dropped, a message that is not CAP passed over).
3. Warnings worked out from the forecast (gusts, rain, thunderstorms, hail, heat, cold, UV; today and tomorrow only),
   and when: a country without a service, a service that does not answer.
4. The commands as the GUI runs them: what is written, an answer reused for 2 minutes, the warnings' check never
   asking Nominatim; run as a program with the saved answers (X6100_METEO_SAMPLES, as the PC screenshots run it).
5. With bottle: the page's /api/meteo by the radio's position from the GUI's socket (meteo_pos: GPS, the country's
   capital), by the QTH locator and the country of params.db when the GUI does not answer, by a locator typed, and
   its refusals.
"""
import calendar
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import socket
import sqlite3
import stat
import subprocess
import sys
import tempfile
import threading
import types
import unittest
import urllib.error
import urllib.parse
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "..", ".."))
HELPER = os.environ.get("X6100_METEO") or os.path.join(TOP, "rootfs", "usr", "sbin", "x6100-meteo")
SAMPLES = os.path.join(HERE, "samples")
VERSION = "0.3.12"
AGENT = f"OpenX6X00/{VERSION} (+https://github.com/Guru-RF/OpenX6X00)"
NOW = calendar.timegm((2026, 11, 16, 12, 0, 0))     # the samples' day, noon UTC
BRUSSELS = (50.8467, 4.3525)
DENVER = (39.7392, -104.9903)
BUENOS_AIRES = (-34.6037, -58.4500)
PORTO_ALEGRE = (-30.0346, -51.2177)


def sample(name):
    with open(os.path.join(SAMPLES, name), encoding="utf-8") as f:
        return f.read()


def load_helper(tmp):
    """The helper as a module, its files in tmp, its clock at NOW, its network the stand-in"""
    os.environ["X6100_METEO_TMP"] = os.path.join(tmp, "tmp")
    os.environ["X6100_METEO_DIR"] = os.path.join(tmp, "data")
    os.environ["X6100_FW"] = VERSION
    os.environ.pop("X6100_METEO_SAMPLES", None)
    os.makedirs(os.environ["X6100_METEO_TMP"])
    loader = importlib.machinery.SourceFileLoader("x6100_meteo", HELPER)
    spec = importlib.util.spec_from_loader("x6100_meteo", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


class Net:
    """The stand-in network: answers by the address's start, the requests seen"""

    def __init__(self):
        self.answers = {}       # address prefix -> text, or an int: that HTTP status
        self.seen = []

    def urlopen(self, req, timeout=None, context=None):
        url = req.full_url
        self.seen.append({"url": url, "headers": {k.lower(): v for k, v in req.header_items()}})
        for prefix in sorted(self.answers, key=len, reverse=True):
            if url.startswith(prefix):
                body = self.answers[prefix]
                if isinstance(body, int):
                    raise urllib.error.HTTPError(url, body, "stand-in", {}, io.BytesIO(b""))
                return io.BytesIO(body.encode("utf-8"))
        raise urllib.error.URLError("no route to the stand-in")

    def urls(self, part=""):
        return [s["url"] for s in self.seen if part in s["url"]]


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="x6100-meteo.")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.m = load_helper(self.tmp)
        self.net = Net()
        self.m.urlopen = self.net.urlopen
        self.clock = [NOW]
        self.slept = []
        self.m.now = lambda: self.clock[0]
        self.m.sleep = lambda s: (self.slept.append(s), self.clock.__setitem__(0, self.clock[0] + s))
        self.net.answers = {
            "https://api.open-meteo.com/": sample("open-meteo.json"),
            "https://nominatim.openstreetmap.org/": sample("nominatim.json"),
            "https://feeds.meteoalarm.org/": sample("meteoalarm.json"),
            "https://api.weather.gov/": sample("nws.json"),
            "https://ssl.smn.gob.ar/CAP/AR.php": sample("smn.xml"),
            "https://apiprevmet3.inmet.gov.br/avisos/rss": sample("inmet.xml"),
        }
        for n in os.listdir(SAMPLES):
            if n.startswith("smn-"):
                self.net.answers["https://ssl.smn.gob.ar/CAP/files/" + n[4:]] = sample(n)
            elif n.startswith("inmet-"):
                self.net.answers["https://apiprevmet3.inmet.gov.br/avisos/rss/" + n[6:]] = sample(n)

    def zone(self, name="Europe/Brussels", abbr="CET", offset=3600):
        return self.m.Zone(name, abbr, offset)

    def run_cmd(self, *args):
        out = io.StringIO()
        saved, sys.stdout = sys.stdout, out
        try:
            rc = self.m.main(["x6100-meteo"] + [str(a) for a in args])
        finally:
            sys.stdout = saved
        return rc, out.getvalue().strip()

    def read(self, path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)


# ---- 1. the helper's own parts ----

class Parts(Case):
    def test_rounded(self):
        self.assertEqual(self.m.rounded(*BRUSSELS), (50.8, 4.4))
        self.assertEqual(self.m.rounded(-34.6037, -58.4612), (-34.6, -58.5))
        lat, lon = self.m.rounded(-0.04, -0.04)
        self.assertEqual(f"{lat:.1f},{lon:.1f}", "0.0,0.0", "no -0.0")

    def test_headers(self):
        self.m.fetch("x", "https://api.open-meteo.com/v1/forecast?x=1", lang="nl")
        h = self.net.seen[0]["headers"]
        self.assertEqual(h["user-agent"], AGENT)
        self.assertEqual(h["accept-language"], "nl")
        self.assertEqual(set(h), {"user-agent", "accept", "accept-language"}, "nothing else about the radio")

    def test_errors(self):
        self.net.answers["https://x.test/"] = 503
        with self.assertRaisesRegex(self.m.Fail, "^HTTP 503$"):
            self.m.fetch("x", "https://x.test/a")
        with self.assertRaisesRegex(self.m.Fail, "no route"):
            self.m.fetch("x", "https://elsewhere.test/")
        self.net.answers["https://x.test/"] = "<html>"
        with self.assertRaisesRegex(self.m.Fail, "not JSON"):
            self.m.fetch_json("x", "https://x.test/")

    def test_times(self):
        z = self.zone()
        self.assertEqual(z.text(self.m.epoch_of("2026-11-16T15:00:00+01:00", z)), "2026-11-16 15:00")
        self.assertEqual(z.text(self.m.epoch_of("2026-11-16T14:00:00Z", z)), "2026-11-16 15:00")
        self.assertEqual(z.text(self.m.epoch_of("2026-11-16T11:00:00-0300", z)), "2026-11-16 15:00")
        self.assertEqual(z.text(self.m.epoch_of("2026-11-16 15:00", z)), "2026-11-16 15:00", "no offset: the place's")
        self.assertEqual(z.text(self.m.epoch_of("2026-07-01T12:00:00Z", z)), "2026-07-01 14:00", "summer time")
        self.assertIsNone(self.m.epoch_of("tomorrow", z))
        bare = self.m.Zone("", "GMT-3", -10800)
        self.assertEqual(bare.text(self.m.epoch_of("2026-11-16T15:00:00Z", bare)), "2026-11-16 12:00",
                         "without its rules: the offset Open-Meteo gave")

    def test_fit(self):
        self.assertEqual(self.m.fit("Łódź", "pl"), "Lódz")
        self.assertEqual(self.m.fit("Kraków – “Wawel” 20 °C", "en"), "Kraków – “Wawel” 20 °C")
        self.assertEqual(self.m.fit("北京 Beijing", "zh_CN"), "北京 Beijing")
        self.assertEqual(self.m.fit("北京 Beijing", "en"), " Beijing")
        self.assertEqual(self.m.fit_all({"a": ["Ħal Għargħur"], "n": 3}, "en"), {"a": ["Hal Gharghur"], "n": 3})

    def test_credits(self):
        self.assertEqual(self.m.credits("MeteoAlarm", {"cc": "BE"}),
                         ["Weather data by Open-Meteo.com (CC BY 4.0)", "Place: © OpenStreetMap contributors (ODbL)",
                          "Warnings: EUMETNET – MeteoAlarm (CC BY 4.0)"])
        self.assertEqual(self.m.credits("SMN+forecast", None),
                         ["Weather data by Open-Meteo.com (CC BY 4.0)", "Warnings: SMN Argentina (CC BY 4.0)",
                          "Warnings worked out by OpenX6X00 from Open-Meteo's forecast, not official"])
        self.assertIn("Warnings: INMET Brazil", self.m.credits("INMET", None))
        self.assertIn("Warnings: US National Weather Service", self.m.credits("NWS", None))

    def test_service_by_country(self):
        src = self.m.official_src
        self.assertEqual([src(c) for c in ("US", "PR", "BE", "GB", "AR", "BR", "UY", "JP", "")],
                         ["NWS", "NWS", "MeteoAlarm", "MeteoAlarm", "SMN", "INMET", "", "", ""])


# ---- 2. each service's answer ----

class OpenMeteo(Case):
    def test_forecast(self):
        cur, days, sun, zone = self.m.open_meteo(*BRUSSELS)
        url = self.net.urls("open-meteo")[0]
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
        self.assertEqual((q["latitude"], q["longitude"], q["timezone"], q["forecast_days"]),
                         (["50.8"], ["4.4"], ["auto"], ["5"]))
        self.assertIn("current", q)
        self.assertEqual((zone.name, zone.abbr, zone.offset), ("Europe/Brussels", "CET", 3600))
        self.assertEqual((cur["temp"], cur["code"], cur["text"], cur["vis_km"], cur["time"]),
                         (9.4, 61, "Light rain", 18.2, "13:45"))
        self.assertEqual(len(days), 5)
        self.assertEqual(days[4], {"date": "2026-11-20", "code": 71, "text": "Light snow", "tmin": -2.8, "tmax": 3.1,
                                   "prob": 60, "rain_mm": 2.2, "uv": 0.8, "wind_max": 14.3, "gust_max": 28.8,
                                   "wind_dir": 40, "rise": "08:10", "set": "16:42"})
        self.assertEqual(sun, {"rise": "08:04", "set": "16:46"})

    def test_two_days_for_the_check(self):
        self.m.open_meteo(*BRUSSELS, days=2, current=False)
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.net.urls("open-meteo")[0]).query)
        self.assertEqual(q["forecast_days"], ["2"])
        self.assertNotIn("current", q)

    def test_not_a_forecast(self):
        with self.assertRaisesRegex(self.m.Fail, "not a forecast"):
            self.m.parse_open_meteo({"error": True, "reason": "x"})
        _, days, _, _ = self.m.parse_open_meteo({"daily": {"time": ["2026-11-16", "junk"], "temperature_2m_max": [1]}})
        self.assertEqual([d["date"] for d in days], ["2026-11-16"])


class Nominatim(Case):
    def test_asked(self):
        geo, err = self.m.place_of(*BRUSSELS, "en", True)
        self.assertIsNone(err)
        self.assertEqual(geo, {"city": "Brussels", "county": "", "state": "Brussels-Capital", "country": "Belgium",
                               "cc": "BE"})
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.net.urls("nominatim")[0]).query)
        self.assertEqual((q["lat"], q["lon"], q["zoom"], q["format"], q["accept-language"]),
                         (["50.8"], ["4.4"], ["10"], ["jsonv2"], ["en"]))
        self.assertEqual(self.m.place_text(geo, *BRUSSELS), "Brussels, Brussels-Capital, Belgium")
        # kept for good: the same square again asks nothing
        self.m.place_of(50.79, 4.41, "en", True)
        self.assertEqual(len(self.net.urls("nominatim")), 1)
        self.assertIn("50.8,4.4,en", self.read(self.m.PLACES))

    def test_not_asked(self):
        self.assertEqual(self.m.place_of(*BRUSSELS, "en", False), (None, None))
        self.assertEqual(self.net.seen, [], "on a timer: no request")
        self.m.place_of(*BRUSSELS, "en", True)
        geo, _ = self.m.place_of(*BRUSSELS, "en", False)
        self.assertEqual(geo["city"], "Brussels", "a square known: its name from places.json")
        self.assertEqual(self.m.place_text(None, *BRUSSELS), "JO20eu", "unknown: the locator")

    def test_language(self):
        self.m.place_of(*BRUSSELS, "es", True)
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.net.urls("nominatim")[0]).query)
        self.assertEqual(q["accept-language"], ["es,en"])
        self.m.place_of(*BRUSSELS, "en", True)
        self.assertEqual(len(self.net.urls("nominatim")), 2, "kept by language")

    def test_a_second_apart(self):
        self.m.place_of(*BRUSSELS, "en", True)
        self.m.place_of(51.2, 4.4, "en", True)
        self.assertEqual(len(self.net.urls("nominatim")), 2)
        self.assertEqual(len(self.slept), 1)
        self.assertAlmostEqual(self.slept[0], 1.0, places=3)
        self.clock[0] += 5
        self.m.place_of(52.0, 4.4, "en", True)
        self.assertEqual(len(self.slept), 1, "a while after: no wait")

    def test_at_sea_and_errors(self):
        self.net.answers["https://nominatim.openstreetmap.org/"] = sample("nominatim-sea.json")
        self.assertEqual(self.m.place_of(51.5, 2.5, "en", True), (None, None))
        self.assertEqual(self.m.load_json(self.m.PLACES, {}), {}, "the sea is not kept: asked again")
        self.net.answers["https://nominatim.openstreetmap.org/"] = 429
        geo, err = self.m.place_of(51.5, 2.5, "en", True)
        self.assertEqual((geo, err), (None, "OpenStreetMap: HTTP 429"))

    def test_another_server_or_off(self):
        os.makedirs(self.m.DATA, exist_ok=True)
        with open(self.m.NOMINATIM_CONF, "w") as f:
            f.write("https://geo.example.org/nominatim/\n")
        self.net.answers["https://geo.example.org/"] = sample("nominatim.json")
        self.m.place_of(*BRUSSELS, "en", True)
        self.assertTrue(self.net.urls()[0].startswith("https://geo.example.org/nominatim/reverse?"))
        with open(self.m.NOMINATIM_CONF, "w") as f:
            f.write("off\n")
        self.assertEqual(self.m.place_of(51.0, 3.0, "en", True), (None, None))
        self.assertEqual(len(self.net.seen), 1)
        with open(self.m.NOMINATIM_CONF, "w") as f:
            f.write("file:///etc/passwd\n")
        self.assertEqual(self.m.nominatim_base(), self.m.NOMINATIM, "not an address: the public one")

    def test_letters_the_fonts_lack(self):
        self.net.answers["https://nominatim.openstreetmap.org/"] = sample("nominatim-pl.json")
        geo, _ = self.m.place_of(51.7592, 19.456, "pl", True)
        self.assertEqual(self.m.fit(self.m.place_text(geo, 0, 0), "pl"), "Lódz, Lódz Voivodeship, Polska")


class Nws(Case):
    def test_alerts(self):
        z = self.zone("America/Denver", "MST", -25200)
        al = self.m.nws_alerts(*DENVER, z)
        self.assertEqual(self.net.urls("weather.gov"), ["https://api.weather.gov/alerts/active?point=39.7,-105.0"])
        self.assertEqual(self.net.seen[0]["headers"]["accept"], "application/geo+json")
        self.assertEqual(len(al), 1, "the expired one and the test left out")
        a = al[0]
        self.assertEqual((a["src"], a["level"], a["color"], a["event"], a["mine"]),
                         ("NWS", 3, "orange", "Winter Storm Warning", True))
        self.assertEqual((a["from"], a["to"], a["sent"]), ("2026-11-16 17:00", "2026-11-17 11:00", "2026-11-16 03:12"))
        self.assertTrue(a["desc"].startswith("* WHAT...Heavy snow expected."))
        self.assertEqual(a["instr"], "Travel could be very difficult.")
        self.assertEqual(a["areas"], "Denver; Arapahoe")


class MeteoAlarm(Case):
    GEO = {"city": "Brussels", "county": "", "state": "Brussels-Capital", "country": "Belgium", "cc": "BE"}

    def test_feed(self):
        al = self.m.meteoalarm_alerts("BE", self.GEO, "en", self.zone())
        self.assertEqual(self.net.urls("meteoalarm"), ["https://feeds.meteoalarm.org/api/v1/warnings/feeds-belgium"])
        self.assertEqual([(a["event"], a["level"], a["mine"]) for a in al],
                         [("Yellow wind warning", 2, True), ("Orange coastal event warning", 3, False)],
                         "the place's own first; the country's yellow ones elsewhere, green, cancelled, test and "
                         "expired ones left out")
        a = al[0]
        self.assertEqual((a["from"], a["to"], a["sent"], a["areas"]),
                         ("2026-11-16 15:00", "2026-11-17 09:00", "2026-11-16 09:51", "Brussels"))
        self.assertEqual(a["desc"], "Gusts up to 70 km/h, mainly in the afternoon. 1", "one line, no recolour mark")
        self.assertEqual(a["instr"], "Secure loose objects.")

    def test_language(self):
        al = self.m.meteoalarm_alerts("BE", self.GEO, "nl", self.zone())
        self.assertEqual(al[0]["event"], "Geel windwaarschuwing")
        self.assertEqual(al[0]["areas"], "Brussel")
        al = self.m.meteoalarm_alerts("BE", self.GEO, "de", self.zone())
        self.assertEqual(al[0]["event"], "Yellow wind warning", "a language not there: English")

    def test_same_place(self):
        sp = self.m.same_place
        self.assertTrue(sp("Bruxelles-Capitale / Brussels-Capital", self.GEO))
        self.assertFalse(sp("West Flanders", self.GEO))
        self.assertFalse(sp("Bru", self.GEO), "too short to tell")
        self.assertFalse(sp("Brussels", None), "no place known: none is its own")


class CapFeeds(Case):
    def test_smn(self):
        al = self.m.cap_feed_alerts("SMN", *BUENOS_AIRES, "es", self.zone("America/Argentina/Buenos_Aires", "-03", -10800))
        fetched = self.net.urls("smn.gob.ar")
        self.assertEqual(fetched[0], "https://ssl.smn.gob.ar/CAP/AR.php")
        self.assertEqual(len(fetched), 5, "the feed and its four CAP messages, not the page without .xml")
        self.assertEqual([(a["event"], a["mine"], a["level"]) for a in al],
                         [("Tormentas", True, 2), ("Viento", False, 3)],
                         "inside its polygon; within 150 km; Chubut far away and the expired one left out; the "
                         "colour named before the severity")
        a = al[0]
        self.assertEqual(a["areas"], "Ciudad Autónoma de Buenos Aires", "entities undone")
        self.assertEqual(a["desc"], "El área será afectada por tormentas de variada intensidad, algunas fuertes, con "
                                    "ráfagas & granizo.", "CDATA")
        self.assertEqual((a["from"], a["to"], a["sent"]), ("2026-11-16 18:00", "2026-11-17 06:00", "2026-11-16 09:00"))
        # each message read once and kept: the next run asks for the feed alone
        self.net.seen = []
        al2 = self.m.cap_feed_alerts("SMN", *BUENOS_AIRES, "es", self.zone("America/Argentina/Buenos_Aires", "-03", -10800))
        self.assertEqual(self.net.urls(), ["https://ssl.smn.gob.ar/CAP/AR.php"])
        self.assertEqual(al2, al)

    def test_old_messages_dropped(self):
        self.m.cap_feed_alerts("SMN", *BUENOS_AIRES, "es", self.zone())
        self.assertEqual(len(os.listdir(self.m.CAP_DIR)), 4)
        self.clock[0] += 4 * 86400
        self.m.cap_prune()
        self.assertEqual(os.listdir(self.m.CAP_DIR), [])

    def test_inmet(self):
        z = self.zone("America/Sao_Paulo", "-03", -10800)
        al = self.m.cap_feed_alerts("INMET", *PORTO_ALEGRE, "pt", z)
        self.assertEqual(self.net.urls("inmet")[0], "https://apiprevmet3.inmet.gov.br/avisos/rss")
        self.assertEqual(len(al), 1, "the message that is not CAP passed over")
        a = al[0]
        self.assertEqual((a["src"], a["event"], a["level"], a["mine"], a["areas"]),
                         ("INMET", "Tempestade", 3, True, "Metropolitana de Porto Alegre"))
        self.assertEqual((a["from"], a["to"]), ("2026-11-16 18:00", "2026-11-17 23:59"))
        self.assertTrue(a["instr"].startswith("Em caso de rajadas de vento"))
        self.assertEqual(self.m.cap_feed_alerts("INMET", -3.1, -60.0, "pt", z), [], "Manaus: far from it")

    def test_feed_down(self):
        self.net.answers["https://ssl.smn.gob.ar/CAP/AR.php"] = 500
        with self.assertRaisesRegex(self.m.Fail, "HTTP 500"):
            self.m.cap_feed_alerts("SMN", *BUENOS_AIRES, "es", self.zone())

    def test_message_forms(self):
        z = self.zone()
        self.assertEqual(self.m.cap_alerts("<alert><status>Exercise</status><info></info></alert>", 0, 0, "SMN", "es", z),
                         [])
        self.assertEqual(self.m.cap_alerts("<alert><status>Actual</status></alert>", 0, 0, "SMN", "es", z), [])
        self.assertEqual(self.m.cap_alerts(sample("smn-AR-20261116-0001.xml").replace("<polygon>", "<polygon>x "),
                                           *BUENOS_AIRES, "SMN", "es", z)[0]["mine"], True, "a point that is not one")


# ---- 3. warnings worked out from the forecast ----

class Computed(Case):
    def test_levels(self):
        days = [{"date": "2026-11-16", "gust_max": 95, "rain_mm": 120, "code": 96, "tmax": 41, "tmin": -21, "uv": 11},
                {"date": "2026-11-17", "gust_max": 64, "rain_mm": 49, "code": 95, "tmax": 35, "tmin": -11, "uv": 10},
                {"date": "2026-11-18", "gust_max": 150, "code": 99}]
        al = self.m.computed_alerts(days, "Somewhere", self.zone())
        got = [(a["day"], a["calc"], a["level"], a["value"]) for a in al]
        self.assertEqual(got, [(0, "gust", 3, 95), (0, "rain", 3, 120), (0, "hail", 3, None), (0, "heat", 3, 41),
                               (0, "cold", 3, -21), (0, "uv", 2, 11), (1, "storm", 2, None)],
                         "below each threshold nothing; the third day not looked at")
        self.assertTrue(all(a["kind"] == "computed" and a["src"] == "forecast" and a["mine"] for a in al))
        self.assertEqual((al[0]["from"], al[0]["to"]), ("2026-11-16 00:00", "2026-11-16 23:59"))
        self.assertEqual(al[3]["event"], "Heat: high of 41 °C")
        again = self.m.computed_alerts(days, "Somewhere", self.zone())
        self.assertEqual([a["id"] for a in al], [a["id"] for a in again], "the same id each time: shown once")
        top = self.m.computed_alerts([{"date": "2026-11-16", "gust_max": 130, "rain_mm": 160, "tmax": 45}], "", self.zone())
        self.assertEqual([a["level"] for a in top], [4, 4, 4])

    def test_when(self):
        z = self.zone()
        days = [{"date": "2026-11-16", "gust_max": 100}]
        al, src, err = self.m.gather_alerts("UY", -34.9, -56.2, None, "Montevideo", days, "es", z)
        self.assertEqual((src, err, [a["calc"] for a in al]), ("forecast", [], ["gust"]), "no service for Uruguay")
        self.assertEqual(self.net.seen, [])
        self.net.answers["https://feeds.meteoalarm.org/"] = 503
        al, src, err = self.m.gather_alerts("BE", *BRUSSELS, MeteoAlarm.GEO, "Brussels", days, "en", z)
        self.assertEqual((src, err, [a["calc"] for a in al]), ("MeteoAlarm+forecast", ["MeteoAlarm: HTTP 503"], ["gust"]))
        for odd in ('{"warnings": {"odd": 1}}', "[1, 2]"):
            self.net.answers["https://feeds.meteoalarm.org/"] = odd
            al, src, err = self.m.gather_alerts("BE", *BRUSSELS, MeteoAlarm.GEO, "Brussels", days, "en", z)
            self.assertEqual((src, err), ("MeteoAlarm+forecast", ["MeteoAlarm: not a warnings feed"]), odd)
        self.net.answers["https://feeds.meteoalarm.org/"] = '{"warnings": []}'
        al, src, err = self.m.gather_alerts("BE", *BRUSSELS, MeteoAlarm.GEO, "Brussels", days, "en", z)
        self.assertEqual((src, al, err), ("MeteoAlarm", [], []), "none in force: none, and none worked out")


# ---- 4. the commands ----

class Commands(Case):
    def test_update(self):
        out = os.path.join(self.tmp, "out.json")
        rc, line = self.run_cmd("update", "--asked", *BRUSSELS, "BE", "GPS", "en", out)
        self.assertEqual((rc, line), (0, "ok"))
        d = self.read(out)
        self.assertEqual((d["place"], d["cc"], d["country"], d["tz"], d["tz_abbr"], d["alerts_src"]),
                         ("Brussels, Brussels-Capital, Belgium", "BE", "Belgium", "Europe/Brussels", "CET", "MeteoAlarm"))
        self.assertEqual(d["pos"], {"lat": 50.8467, "lon": 4.3525, "loc": "JO20eu", "src": "GPS"})
        self.assertEqual((d["updated"], d["fetched"], d["errors"]), ("13:00", NOW, []))
        self.assertEqual(len(d["days"]), 5)
        self.assertEqual([a["event"] for a in d["alerts"]], ["Yellow wind warning", "Orange coastal event warning"])
        self.assertIn("Warnings: EUMETNET – MeteoAlarm (CC BY 4.0)", d["credits"])
        for url in self.net.urls():
            self.assertNotIn("50.846", url, "only the rounded position leaves")
            self.assertNotIn("4.352", url)
        # the same place within 2 minutes: the answer as it is
        n = len(self.net.seen)
        self.clock[0] += 100
        self.assertEqual(self.run_cmd("update", *BRUSSELS, "BE", "GPS", "en", out), (0, "ok"))
        self.assertEqual(len(self.net.seen), n)
        self.clock[0] += 30
        self.run_cmd("update", *BRUSSELS, "BE", "GPS", "en", out)
        self.assertEqual(len(self.net.seen), n + 2, "older: Open-Meteo and MeteoAlarm again, the place kept")

    def test_update_on_a_timer(self):
        out = os.path.join(self.tmp, "out.json")
        rc, line = self.run_cmd("update", *BRUSSELS, "-", "Country", "en", out)
        self.assertEqual((rc, line), (0, "ok"))
        self.assertEqual(self.net.urls("nominatim"), [], "not asked by a person: no Nominatim")
        d = self.read(out)
        self.assertEqual((d["place"], d["alerts_src"]), ("JO20eu", "forecast"),
                         "no place, no country: the locator, the warnings worked out")
        self.assertEqual(d["credits"], ["Weather data by Open-Meteo.com (CC BY 4.0)",
                                        "Warnings worked out by OpenX6X00 from Open-Meteo's forecast, not official"])
        self.assertEqual([a["calc"] for a in d["alerts"]], ["gust"], "68 km/h today")

    def test_the_place_abroad(self):
        self.net.answers["https://nominatim.openstreetmap.org/"] = sample("nominatim-us.json")
        out = os.path.join(self.tmp, "out.json")
        self.run_cmd("update", "--asked", *DENVER, "BE", "GPS", "en", out)
        d = self.read(out)
        self.assertEqual((d["cc"], d["alerts_src"]), ("US", "NWS"), "the place's own country, not the radio's")

    def test_alerts(self):
        out = os.path.join(self.tmp, "alerts.json")
        rc, line = self.run_cmd("alerts", *BRUSSELS, "BE", "GPS", "en", out)
        self.assertEqual((rc, line), (0, "ok 0"), "no place known: none of MeteoAlarm's is the place's own")
        self.assertEqual(self.net.urls("nominatim"), [])
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.net.urls("open-meteo")[0]).query)
        self.assertEqual(q["forecast_days"], ["2"])
        self.run_cmd("update", "--asked", *BRUSSELS, "BE", "GPS", "en", os.path.join(self.tmp, "out.json"))
        rc, line = self.run_cmd("alerts", *BRUSSELS, "BE", "GPS", "en", out)
        self.assertEqual(line, "ok 1", "the place known from places.json")
        d = self.read(out)
        self.assertEqual((d["tz_abbr"], d["alerts_src"], len(d["alerts"])), ("CET", "MeteoAlarm", 2))

    def test_errors(self):
        self.assertEqual(self.run_cmd("update", "x", "4"), (1, "error no position (LAT LON)"))
        self.assertEqual(self.run_cmd("update", "91", "4"), (1, "error no position (LAT LON)"))
        self.net.answers["https://api.open-meteo.com/"] = 502
        rc, line = self.run_cmd("update", *BRUSSELS, "BE", "GPS", "en", os.path.join(self.tmp, "out.json"))
        self.assertEqual((rc, line), (1, "error Open-Meteo: HTTP 502"))
        self.assertEqual(self.net.urls("meteoalarm"), [], "no weather: the warnings not asked for either")
        self.net.answers = {}
        rc, line = self.run_cmd("alerts", *BRUSSELS, "BE", "GPS", "en", os.path.join(self.tmp, "a.json"))
        self.assertEqual(rc, 1, "no internet: neither the service nor the forecast")
        self.assertEqual(self.m.main(["x6100-meteo"]), 2)

    def test_args(self):
        p = self.m.parse_args
        self.assertEqual(p(["--asked", "50.8", "4.3", "be", "GPS", "nl"]),
                         (50.8, 4.3, "BE", "GPS", "nl", "", True))
        self.assertEqual(p(["50.8", "4.3", "-", "-", "", "/x"]), (50.8, 4.3, "", "", "en", "/x", False))
        self.assertEqual(p(["50.8", "4.3", "B;E", "G/P$S", "es; rm"]), (50.8, 4.3, "", "GPS", "esrm", "", False))


class AsAProgram(unittest.TestCase):
    """As the GUI runs it, and the PC screenshots with X6100_METEO_SAMPLES (the warnings' dates are the samples',
    which the real clock may have passed: not looked at here)"""

    def test_samples(self):
        tmp = tempfile.mkdtemp(prefix="x6100-meteo.")
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        env = dict(os.environ, X6100_METEO_TMP=tmp, X6100_METEO_DIR=os.path.join(tmp, "data"),
                   X6100_METEO_SAMPLES=SAMPLES, X6100_FW=VERSION)
        out = os.path.join(tmp, "out.json")
        r = subprocess.run([sys.executable, "-I", "-B", HELPER, "update", "--asked", *map(str, BRUSSELS), "BE", "GPS",
                            "en", out], capture_output=True, text=True, timeout=60, env=env)
        self.assertEqual(r.stdout.strip(), "ok", r.stderr)
        with open(out, encoding="utf-8") as f:
            d = json.load(f)
        self.assertEqual((d["place"], d["tz"], d["alerts_src"]),
                         ("Brussels, Brussels-Capital, Belgium", "Europe/Brussels", "MeteoAlarm"))
        self.assertTrue(os.access(HELPER, os.X_OK) and os.stat(HELPER).st_mode & stat.S_IXOTH, "executable")


# ---- 5. the web page's API (bottle) ----

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


@unittest.skipIf(WEB is None, "no bottle: give BOTTLE=<a folder with bottle.py>")
class Web(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="x6100-meteo-web.")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        apps, settings = WEB
        saved = {k: getattr(settings, k) for k in ("METEO_CMD", "METEO_FILE", "METEO_WEB_FILE", "CFG_SOCK", "DB_PATH",
                                                   "APP_VERSION")}
        self.addCleanup(lambda: [setattr(settings, k, v) for k, v in saved.items()])
        # the helper behind a script that notes its arguments, its answers the saved ones
        self.argv = os.path.join(self.tmp, "argv")
        settings.METEO_CMD = os.path.join(self.tmp, "x6100-meteo")
        with open(settings.METEO_CMD, "w") as f:
            f.write(f'#!/bin/sh\necho "$@" >> {self.argv}\nexec {sys.executable} -I -B {HELPER} "$@"\n')
        os.chmod(settings.METEO_CMD, 0o755)
        settings.METEO_FILE = os.path.join(self.tmp, "meteo.json")
        settings.METEO_WEB_FILE = os.path.join(self.tmp, "meteo-web.json")
        settings.CFG_SOCK = os.path.join(self.tmp, "cfg.sock")
        settings.DB_PATH = os.path.join(self.tmp, "params.db")
        settings.APP_VERSION = os.path.join(self.tmp, "app-version")
        with open(settings.APP_VERSION, "w") as f:
            f.write(VERSION + "\n")
        self.param("qth", "JO20ft")
        self.param("country", "BE")
        env = {"X6100_METEO_TMP": self.tmp, "X6100_METEO_DIR": os.path.join(self.tmp, "data"),
               "X6100_METEO_SAMPLES": SAMPLES}
        for k, v in env.items():
            self.addCleanup(os.environ.pop, k, None)
            os.environ[k] = v
        os.environ.pop("X6100_FW", None)      # the web server's own: from APP_VERSION

    def param(self, name, val):
        con = sqlite3.connect(WEB[1].DB_PATH)
        con.execute("CREATE TABLE IF NOT EXISTS params (name TEXT PRIMARY KEY, val TEXT)")
        con.execute("INSERT OR REPLACE INTO params (name, val) VALUES (?, ?)", (name, val))
        con.commit()
        con.close()

    def gui(self, answer):
        """A stand-in for the GUI's socket (src/cfg_socket.cpp): one answer line to each request"""
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(WEB[1].CFG_SOCK)
        srv.listen(4)
        self.addCleanup(srv.close)
        asked = []

        def serve():
            while True:
                try:
                    c, _ = srv.accept()
                except OSError:
                    return
                with c, c.makefile("rb") as f:
                    asked.append(json.loads(f.readline()))
                    c.sendall(json.dumps(answer).encode() + b"\n")
        threading.Thread(target=serve, daemon=True).start()
        return asked

    def get(self, path):
        path, _, query = path.partition("?")
        env = {"REQUEST_METHOD": "GET", "PATH_INFO": path, "QUERY_STRING": query, "HTTP_HOST": "192.168.1.10"}
        setup_testing_defaults(env)
        env["wsgi.input"] = io.BytesIO(b"")
        got = {}
        out = WEB[0].app(env, lambda status, hdrs, exc=None: got.update(status=int(status.split()[0])))
        data = b"".join(out)
        return got["status"], data

    def args(self):
        with open(self.argv) as f:
            return [line.split() for line in f.read().splitlines()]

    def test_grid_center(self):
        gc = WEB[0].meteo_grid_center
        self.assertEqual(gc("JO20"), (50.5, 5.0))
        lat, lon = gc("jo20ab")
        self.assertAlmostEqual(lat, 50 + 1 / 24 + 1 / 48)
        self.assertAlmostEqual(lon, 4 + 1 / 24)
        for bad in ("", "JO2", "JO20a", "ZZ00", "JO20zz", "JO20ab5"):
            self.assertIsNone(gc(bad), bad)

    def test_page(self):
        st, data = self.get("/meteo")
        self.assertEqual(st, 200)
        self.assertIn(b'href="/meteo"', data, "in the menu")
        self.assertIn(b"Settings &gt; General &gt; STATION", data)
        self.assertIn(b"https://open-meteo.com/", data, "the credit's link")

    def test_by_the_radios_position(self):
        asked = self.gui({"t": "pos", "lat": BRUSSELS[0], "lon": BRUSSELS[1], "src": "GPS", "cc": "BE",
                          "country": "Belgium", "lang": "nl"})
        st, data = self.get("/api/meteo?asked=1")
        self.assertEqual(st, 200, data)
        d = json.loads(data)
        self.assertEqual(asked, [{"cmd": "meteo_pos"}])
        self.assertEqual((d["where"], d["place"], d["alerts_src"]), ("GPS", "Brussels, Brussels-Capital, Belgium",
                                                                     "MeteoAlarm"))
        self.assertEqual(self.args(), [["update", "--asked", "50.84670", "4.35250", "BE", "GPS", "nl",
                                        WEB[1].METEO_FILE]], "the GUI's answer file, shared")
        self.get("/api/meteo?asked=0")
        self.assertEqual(self.args()[1][1], "50.84670", "on the page's timer: not --asked")

    def test_the_countrys_capital(self):
        self.gui({"t": "pos", "lat": 50.8333, "lon": 4.3333, "src": "Country", "cc": "BE", "country": "Belgium",
                  "lang": "en"})
        st, data = self.get("/api/meteo")
        self.assertEqual(st, 200, data)
        self.assertEqual(json.loads(data)["where"], "Country Belgium")
        self.assertEqual(self.args()[0][3:5], ["BE", "Country"])

    def test_without_the_gui(self):
        st, data = self.get("/api/meteo")
        self.assertEqual(st, 200, data)
        self.assertEqual(json.loads(data)["where"], "Locator JO20ft")
        self.assertEqual(self.args()[0][2:6], ["4.45833", "BE", "Locator", "en"],
                         "JO20ft's centre, the country of params.db")

    def test_a_locator_typed(self):
        self.gui({"t": "pos", "lat": BRUSSELS[0], "lon": BRUSSELS[1], "src": "GPS", "cc": "BE", "lang": "en"})
        st, data = self.get("/api/meteo?grid=jo21&asked=1")
        self.assertEqual(st, 200, data)
        self.assertEqual(json.loads(data)["where"], "JO21")
        self.assertEqual(self.args(), [["update", "--asked", "51.50000", "5.00000", "BE", "Locator", "en",
                                        WEB[1].METEO_WEB_FILE]], "a file of its own")

    def test_refused(self):
        st, data = self.get("/api/meteo?grid=ZZ99")
        self.assertEqual(st, 400)
        self.assertEqual(json.loads(data)["status"], "error")
        self.param("qth", "")
        st, data = self.get("/api/meteo")
        self.assertEqual(st, 409)
        self.assertIn("No position", json.loads(data)["msg"])
        self.gui({"t": "pos", "src": "", "cc": "", "lang": "en"})
        st, _ = self.get("/api/meteo")
        self.assertEqual(st, 409, "the GUI has no position either")
        self.assertFalse(os.path.exists(self.argv), "the helper never run")

    def test_the_helpers_error(self):
        os.environ["X6100_METEO_SAMPLES"] = os.path.join(self.tmp, "none")
        st, data = self.get("/api/meteo?asked=1")
        self.assertEqual(st, 502)
        self.assertEqual(json.loads(data)["msg"], "Open-Meteo: open-meteo.json: no sample")


if __name__ == "__main__":
    unittest.main()
