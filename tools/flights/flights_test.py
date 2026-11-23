#!/usr/bin/env python3
"""The Flights app's helper (rootfs/usr/sbin/x6100-flights) and the web Flights page's API (webserver apps.py), with
the services' answers saved in samples/ (made by hand in their readsb JSON) served by a stand-in on 127.0.0.1. Nothing
here goes to airplanes.live or adsb.lol.

    python3 -I -B tools/flights/flights_test.py [-v]          (BOTTLE=<a folder with bottle.py> for the web part)

1. The helper's own functions: the position rounded to 0.1 degree, the radius asked, distance and bearing, an
   aircraft cleaned into the screen's units, the answers parsed (an empty area, an answer with no list, not JSON).
2. The helper run as the GUI runs it, against the stand-in: what it sends (the rounded position, the radius, the
   User-Agent, nothing else), what it writes (the aircraft in range, from the position itself, the nearest first,
   the credit and the next poll), the answer kept 50 s for the GUI and the web page alike, adsb.lol when
   airplanes.live fails, a 429 and its Retry-After kept, a kept answer with the time it was fetched at and its next
   poll a minute after that, a second between two requests to one service, no internet, wrong arguments.
3. With bottle: the web page's /api/flights by a locator typed, by the radio's position from the GUI's socket
   (pos_get, not the place of the radio's country it falls back to), by the QTH locator when the GUI does not answer,
   and its refusals.
"""
import importlib.machinery
import importlib.util
import io
import json
import math
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.abspath(os.path.join(HERE, "..", ".."))
HELPER = os.environ.get("X6100_FLIGHTS") or os.path.join(TOP, "rootfs", "usr", "sbin", "x6100-flights")
SAMPLES = os.path.join(HERE, "samples")
HOME = (50.8467, 4.3525)          # the radio, exactly
SENT = (50.8, 4.4)                # as it is sent
VERSION = "0.3.12"
AGENT = f"OpenX6X00/{VERSION} (+https://github.com/Guru-RF/OpenX6X00)"


def load_helper(tmp):
    """The helper as a module, its files in tmp"""
    os.environ["X6100_FLIGHTS_TMP"] = tmp
    loader = importlib.machinery.SourceFileLoader("x6100_flights", HELPER)
    spec = importlib.util.spec_from_loader("x6100_flights", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def sample(name):
    with open(os.path.join(SAMPLES, name), "rb") as f:
        return f.read()


def vector_bearing(lat1, lon1, lat2, lon2):
    """The initial bearing as the direction of 2 seen in the tangent plane at 1 (unit vectors): another way than the
    helper's"""
    p1, l1, p2, l2 = map(math.radians, (lat1, lon1, lat2, lon2))
    b = (math.cos(p2) * math.cos(l2), math.cos(p2) * math.sin(l2), math.sin(p2))
    north = (-math.sin(p1) * math.cos(l1), -math.sin(p1) * math.sin(l1), math.cos(p1))
    east = (-math.sin(l1), math.cos(l1), 0.0)
    dot = lambda u, v: sum(x * y for x, y in zip(u, v))   # noqa: E731
    return math.degrees(math.atan2(dot(b, east), dot(b, north))) % 360


def cos_law_km(lat1, lon1, lat2, lon2):
    """Distance by the spherical law of cosines: another formula than the helper's haversine"""
    p1, p2, dl = math.radians(lat1), math.radians(lat2), math.radians(lon2 - lon1)
    return 6371.0 * math.acos(min(1.0, math.sin(p1) * math.sin(p2) + math.cos(p1) * math.cos(p2) * math.cos(dl)))


# ---- the stand-in services ----

class Stand:
    """What each service answers ("a" airplanes.live, "b" adsb.lol): (status, body, headers); and the requests seen"""
    answers = {}
    seen = []
    lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        svc = self.path.split("/")[1]
        with Stand.lock:
            Stand.seen.append({"svc": svc, "path": self.path, "headers": dict(self.headers), "t": time.time()})
        status, body, headers = Stand.answers.get(svc, (404, b"not here", {}))
        self.send_response(status)
        self.send_header("Content-Type", "application/json" if body[:1] in (b"{", b"[") else "text/html")
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class HelperCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        port = cls.server.server_address[1]
        cls.urls = (f"airplanes.live=http://127.0.0.1:{port}/a/{{lat}}/{{lon}}/{{nm}}|"
                    f"adsb.lol=http://127.0.0.1:{port}/b/{{lat}}/{{lon}}/{{nm}}")

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="x6100-flights.")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        Stand.seen = []
        Stand.answers = {"a": (200, sample("airplanes_live.json"), {}), "b": (200, sample("adsb_lol.json"), {})}

    def env(self, **kw):
        return dict(os.environ, X6100_FLIGHTS_TMP=self.tmp, X6100_FLIGHTS_URLS=kw.pop("urls", self.urls),
                    X6100_FW=VERSION, **kw)

    def run_helper(self, lat, lon, km, out=None, **kw):
        out = out or os.path.join(self.tmp, "out.json")
        r = subprocess.run([sys.executable, "-I", HELPER, "update", str(lat), str(lon), str(km), out],
                           capture_output=True, text=True, timeout=60, env=self.env(**kw))
        return r.stdout.strip(), out

    def read(self, path):
        with open(path) as f:
            return json.load(f)


class Functions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="x6100-flights.")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.h = load_helper(self.tmp)

    def test_rounded(self):
        self.assertEqual(self.h.rounded(*HOME), SENT)
        self.assertEqual(self.h.rounded(-33.8568, 151.2153), (-33.9, 151.2))
        self.assertEqual(self.h.rounded(40.75, -73.98), (40.8, -74.0))
        lat, lon = self.h.rounded(-0.04, 0.03)
        self.assertEqual("%.1f/%.1f" % (lat, lon), "0.0/0.0", "no -0.0 in the request")
        for lat, lon in ((50.85, 4.35), (12.34, -98.76), (-89.99, 179.99)):
            q = self.h.rounded(lat, lon)
            self.assertLessEqual(self.h.dist_brg(lat, lon, *q)[0], self.h.ROUND_KM, (lat, lon))

    def test_radius(self):
        self.assertEqual([self.h.radius_nm(k) for k in (1, 25, 50, 100, 200, 400)], [5, 18, 32, 59, 113, 221])
        self.assertEqual(self.h.radius_nm(10000), 250)

    def test_distance_bearing(self):
        for (a, b), (km, brg) in {((50, 4), (51, 4)): (111.19, 0), ((0, 0), (0, 1)): (111.19, 90),
                                  ((0, 0), (-1, 0)): (111.19, 180), ((0, 0), (0, -1)): (111.19, 270),
                                  ((50.8503, 4.3517), (48.8566, 2.3522)): (263.9, 213.7)}.items():
            d, g = self.h.dist_brg(*a, *b)
            self.assertAlmostEqual(d, km, delta=0.2, msg=(a, b))
            self.assertAlmostEqual(g, brg, delta=0.5, msg=(a, b))
        for lat, lon in ((51.10012, 4.90031), (50.86, 4.37), (49.0, 2.0), (-33.9, 151.2), (60.0, -170.0)):
            d, g = self.h.dist_brg(*HOME, lat, lon)
            self.assertAlmostEqual(d, cos_law_km(*HOME, lat, lon), delta=0.05)
            self.assertAlmostEqual(g, vector_bearing(*HOME, lat, lon), delta=0.01)

    def test_clean(self):
        ac = json.loads(sample("airplanes_live.json"))["ac"]
        c = self.h.clean(ac[0])
        self.assertEqual(c, {"hex": "4CA7B5", "call": "RYR4TP", "reg": "EI-EVJ", "type": "B738",
                             "desc": "BOEING 737-800", "lat": 51.10012, "lon": 4.90031, "alt_m": 11278,
                             "ground": False, "kmh": 828, "trk": 136, "vs": 0, "squawk": "2017", "emerg": False})
        g = self.h.clean(ac[1])
        self.assertTrue(g["ground"])
        self.assertIsNone(g["alt_m"])
        self.assertEqual(g["trk"], 252, "true_heading when there is no track")
        self.assertEqual(self.h.clean(ac[2])["vs"], -410, "ft/min to m/min")
        self.assertTrue(self.h.clean(ac[3])["emerg"])
        t = self.h.clean(ac[4])
        self.assertEqual((t["hex"], t["alt_m"], t["call"]), ("2A1B3C", 762, ""), "TIS-B: the ~ off, alt_geom")
        self.assertIsNone(self.h.clean(ac[5]), "no position")
        self.assertIsNone(self.h.clean(ac[6]), "a position 95 s old")
        self.assertIsNone(self.h.clean(ac[8]), "a position that is not a number")
        self.assertIsNone(self.h.clean(ac[9]), "not an object")
        self.assertIsNone(self.h.clean({"hex": "x", "lat": 91.0, "lon": 0.0}))
        self.assertIsNone(self.h.clean({"hex": "x", "lat": True, "lon": 0.0}))

    def test_parse(self):
        self.assertEqual(len(self.h.parse(sample("airplanes_live.json"))), 6)
        self.assertEqual(len(self.h.parse(sample("adsb_lol.json"))), 2)
        self.assertEqual(self.h.parse(sample("empty.json")), [])
        self.assertEqual(self.h.parse(sample("no_ac.json")), [], "nothing in the area: no list at all")
        self.assertEqual(self.h.parse(b'{"now": 1, "aircraft": [{"hex": "a", "lat": 1, "lon": 2}]}')[0]["hex"], "A")
        for bad in (sample("error.html"), b'{"error": "x"}', b"[]", b"", b"\xff\xfe"):
            with self.assertRaises(ValueError, msg=bad):
                self.h.parse(bad)

    def test_place(self):
        ac = self.h.parse(sample("airplanes_live.json"))
        near = self.h.place(ac, *HOME, 100)
        self.assertEqual([c["hex"] for c in near], ["2A1B3C", "44CDC6", "485A32", "4CA7B5"])
        self.assertEqual([c["hex"] for c in self.h.place(ac, *HOME, 200)][-1], "3C6DD5")
        self.assertEqual(near[0]["dist_km"], round(self.h.dist_brg(*HOME, 50.86, 4.37)[0], 1))
        self.assertLess(near[0]["dist_km"], 2.5, "from the position itself, not the rounded one (7 km from it)")
        self.assertEqual(near[0]["brg"], round(vector_bearing(*HOME, 50.86, 4.37)))
        self.assertTrue(all(0 <= c["brg"] < 360 for c in near))
        self.assertEqual(len(self.h.place([dict(ac[0], hex=str(i)) for i in range(300)], *HOME, 100)), 200)


class Helper(HelperCase):
    def test_request_and_answer(self):
        line, out = self.run_helper(*HOME, 100)
        self.assertEqual(line, "ok 4 60")
        self.assertEqual(len(Stand.seen), 1)
        r = Stand.seen[0]
        self.assertEqual(r["path"], "/a/50.8/4.4/59", "the rounded position, the radius in NM, nothing else")
        self.assertEqual(r["headers"].get("User-Agent"), AGENT)
        d = self.read(out)
        self.assertEqual((d["src"], d["credit"], d["km"], d["next"]), ("airplanes.live", "airplanes.live", 100, 60))
        self.assertEqual([a["hex"] for a in d["ac"]], ["2A1B3C", "44CDC6", "485A32", "4CA7B5"])
        self.assertEqual([a["call"] for a in d["ac"]], ["", "BEL7PC", "KLM29T", "RYR4TP"])
        self.assertAlmostEqual(d["ac"][3]["dist_km"], cos_law_km(*HOME, 51.10012, 4.90031), delta=0.1)
        self.assertRegex(d["time"], r"^\d\d:\d\d:\d\d$")
        self.assertNotIn("50.8467", json.dumps(d), "the exact position is in no file")

    def test_one_answer_for_fifty_seconds(self):
        self.run_helper(*HOME, 100)
        line, out = self.run_helper(50.84, 4.36, 25, out=os.path.join(self.tmp, "web.json"))
        self.assertEqual(line, "ok 3 60", "a smaller radius around the same 0.1 degree: the answer kept")
        self.assertEqual(len(Stand.seen), 1)
        self.assertEqual(self.read(out)["ac"][0]["dist_km"], round(cos_law_km(50.84, 4.36, 50.86, 4.37), 1))
        line, _ = self.run_helper(*HOME, 200)
        self.assertEqual(line, "ok 5 60", "a wider radius: asked again")
        self.assertEqual([r["path"] for r in Stand.seen], ["/a/50.8/4.4/59", "/a/50.8/4.4/113"])
        line, _ = self.run_helper(51.2, 4.4, 100)
        self.assertEqual(len(Stand.seen), 3, "another 0.1 degree: asked again")
        cache = os.path.join(self.tmp, "x6100-flights-cache.json")
        with open(cache) as f:
            c = json.load(f)
        c["t"] -= 51
        with open(cache, "w") as f:
            json.dump(c, f)
        self.run_helper(51.2, 4.4, 100)
        self.assertEqual(len(Stand.seen), 4, "kept 50 s, no longer")

    def test_a_kept_answer_has_its_own_time(self):
        """An answer 20 s old for the other one of app and page, or a narrower range: the time it was fetched at on
        it, and the next poll a minute after that, not a minute from now"""
        self.run_helper(*HOME, 200)
        cache = os.path.join(self.tmp, "x6100-flights-cache.json")
        with open(cache) as f:
            c = json.load(f)
        c["t"] -= 20
        with open(cache, "w") as f:
            json.dump(c, f)
        line, out = self.run_helper(*HOME, 100, out=os.path.join(self.tmp, "web.json"))
        self.assertRegex(line, r"^ok 4 (40|39)$")
        self.assertEqual(len(Stand.seen), 1)
        d = self.read(out)
        self.assertEqual(d["t"], int(c["t"]))
        self.assertEqual(d["time"], time.strftime("%H:%M:%S", time.localtime(c["t"])))
        self.assertIn(d["next"], (39, 40))

    def test_adsb_lol_when_airplanes_live_fails(self):
        Stand.answers["a"] = (500, b"oops", {})
        line, out = self.run_helper(*HOME, 100)
        self.assertEqual(line, "ok 2 60")
        d = self.read(out)
        self.assertEqual((d["src"], d["credit"], d["next"]), ("adsb.lol", "adsb.lol (ODbL)", 60))
        self.assertEqual([r["svc"] for r in Stand.seen], ["a", "b"])
        self.assertEqual(Stand.seen[1]["path"], "/b/50.8/4.4/59")
        self.assertEqual(Stand.seen[1]["headers"].get("User-Agent"), AGENT)
        # A minute later the GUI asks again: airplanes.live has had its 30 s, adsb.lol not yet its 60
        line, _ = self.run_helper(51.5, 4.4, 100)
        self.assertEqual(line, "error airplanes.live: next try in 30 s; adsb.lol: next try in 60 s")
        self.assertEqual(len(Stand.seen), 2, "nothing asked")

    def test_429_and_its_retry_after(self):
        Stand.answers["a"] = (429, b'{"msg": "slow down"}', {"Retry-After": "120"})
        Stand.answers["b"] = (200, sample("error.html"), {})
        t = time.time()
        line, _ = self.run_helper(*HOME, 100)
        self.assertEqual(line, "error airplanes.live: busy; adsb.lol: not an aircraft list")
        with open(os.path.join(self.tmp, "x6100-flights-state.json")) as f:
            state = json.load(f)
        self.assertAlmostEqual(state["airplanes.live"], t + 120, delta=5)
        self.assertAlmostEqual(state["adsb.lol"], t + 60, delta=5)
        line, _ = self.run_helper(51.5, 4.4, 100)
        self.assertRegex(line, r"^error airplanes\.live: next try in 1[12]\d s; adsb\.lol: next try in [56]\d s$")
        self.assertEqual(len(Stand.seen), 2)

    def test_nothing_around(self):
        Stand.answers["a"] = (200, sample("no_ac.json"), {})
        line, out = self.run_helper(*HOME, 50)
        self.assertEqual(line, "ok 0 60")
        self.assertEqual(self.read(out)["ac"], [])

    def test_a_second_between_two_requests(self):
        """The GUI and the web page at the same moment, around other points: one request after the other"""
        outs = [os.path.join(self.tmp, f"o{i}.json") for i in range(2)]
        procs = [subprocess.Popen([sys.executable, "-I", HELPER, "update", str(lat), "4.35", "100", out],
                                  stdout=subprocess.PIPE, text=True, env=self.env())
                 for lat, out in ((50.85, outs[0]), (51.85, outs[1]))]
        lines = [p.communicate(timeout=60)[0].strip() for p in procs]
        self.assertEqual(sorted(lines), ["ok 2 60", "ok 4 60"])
        self.assertEqual([r["svc"] for r in Stand.seen], ["a", "a"], "both from airplanes.live")
        self.assertGreaterEqual(Stand.seen[1]["t"] - Stand.seen[0]["t"], 0.9)

    def test_no_internet(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()           # nothing listens there
        line, out = self.run_helper(*HOME, 100, urls=f"airplanes.live=http://127.0.0.1:{port}/a")
        self.assertTrue(line.startswith("error airplanes.live: "), line)
        self.assertFalse(os.path.exists(out), "no answer file without an answer")
        line, _ = self.run_helper(*HOME, 100, urls="airplanes.live=http://no-such-host.invalid/a")
        self.assertEqual(line, "error no internet (WiFi?)")

    def test_wrong_arguments(self):
        for args in (("95", "4", "100"), ("50", "181", "100"), ("50", "4", "401"), ("50", "4", "0"), ("x", "4", "1")):
            line, _ = self.run_helper(*args)
            self.assertTrue(line.startswith("error "), (args, line))
        self.assertEqual(self.run_helper(95, 4, 100)[0], "error position or range out of bounds")
        r = subprocess.run([sys.executable, "-I", HELPER], capture_output=True, text=True, env=self.env())
        self.assertEqual(r.returncode, 1)
        self.assertEqual(Stand.seen, [])


# ---- the web page's API (bottle) ----

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
class Web(HelperCase):
    def setUp(self):
        super().setUp()
        apps, settings = WEB
        saved = {k: getattr(settings, k) for k in ("FLIGHTS_CMD", "FLIGHTS_FILE", "CFG_SOCK", "DB_PATH",
                                                   "APP_VERSION")}
        self.addCleanup(lambda: [setattr(settings, k, v) for k, v in saved.items()])
        settings.FLIGHTS_CMD = HELPER
        settings.FLIGHTS_FILE = os.path.join(self.tmp, "web.json")
        settings.CFG_SOCK = os.path.join(self.tmp, "cfg.sock")
        settings.DB_PATH = os.path.join(self.tmp, "params.db")
        settings.APP_VERSION = os.path.join(self.tmp, "app-version")
        with open(settings.APP_VERSION, "w") as f:
            f.write(VERSION + "\n")
        self.qth("JO20ft")
        for k, v in self.env().items():
            if k.startswith("X6100_"):
                self.addCleanup(os.environ.pop, k, None)
                os.environ[k] = v
        os.environ.pop("X6100_FW")      # the web server's own: from APP_VERSION

    def qth(self, grid):
        con = sqlite3.connect(WEB[1].DB_PATH)
        con.execute("CREATE TABLE IF NOT EXISTS params (name TEXT PRIMARY KEY, val TEXT)")
        con.execute("INSERT OR REPLACE INTO params (name, val) VALUES ('qth', ?)", (grid,))
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

    def test_locator_center(self):
        lc = WEB[0].locator_center
        self.assertEqual(lc("JO20"), (50.5, 5.0))
        lat, lon = lc("jo20ab")
        self.assertAlmostEqual(lat, 50 + 1 / 24 + 1 / 48)
        self.assertAlmostEqual(lon, 4 + 1 / 24)
        lat, lon = lc("JO20ab55")
        self.assertAlmostEqual(lat, 50 + 1 / 24 + 5 / 240 + 1 / 480)
        self.assertAlmostEqual(lon, 4 + 5 / 120 + 1 / 240)
        for bad in ("", "JO2", "JO20a", "ZZ00", "JO20zz", "JO20ab5", "J020"):
            self.assertIsNone(lc(bad), bad)

    def test_page(self):
        st, data = self.get("/flights")
        self.assertEqual(st, 200)
        self.assertIn(b'<option value="100" selected>100 km</option>', data)
        self.assertIn(b"adsb.lol (ODbL)", data)
        self.assertIn(b'href="/flights"', data, "in the menu")

    def test_by_the_radios_position(self):
        asked = self.gui({"t": "pos", "lat": HOME[0], "lon": HOME[1], "src": "GPS"})
        st, data = self.get("/api/flights?km=100")
        self.assertEqual(st, 200, data)
        d = json.loads(data)
        self.assertEqual(asked, [{"cmd": "pos_get"}])
        self.assertEqual((d["where"], d["credit"], d["next"]), ("GPS", "airplanes.live", 60))
        self.assertEqual([a["hex"] for a in d["ac"]], ["2A1B3C", "44CDC6", "485A32", "4CA7B5"])
        self.assertEqual(Stand.seen[0]["path"], "/a/50.8/4.4/59")
        self.assertEqual(Stand.seen[0]["headers"].get("User-Agent"), AGENT, "the version from app-version")

    def test_the_locator_the_gui_says(self):
        self.gui({"t": "pos", "lat": 50.5, "lon": 5.0, "src": "Locator"})
        self.qth("JO20")
        st, data = self.get("/api/flights?km=25")
        self.assertEqual(json.loads(data)["where"], "Locator JO20")
        self.assertEqual(Stand.seen[0]["path"], "/a/50.5/5.0/18")

    def test_without_the_gui(self):
        st, data = self.get("/api/flights?km=50")
        self.assertEqual(st, 200, data)
        self.assertEqual(json.loads(data)["where"], "Locator JO20ft")
        self.assertEqual(Stand.seen[0]["path"], "/a/50.8/4.5/32", "JO20ft: 50.81 N 4.46 E")

    def test_not_the_countrys_place(self):
        """pos_get, shared with the Weather page, falls back to the country's capital: aircraft around it are not
        the radio's, so the page takes the QTH locator of params.db, else none"""
        self.gui({"t": "pos", "lat": 50.8333, "lon": 4.3333, "src": "Country", "cc": "BE", "country": "Belgium"})
        st, data = self.get("/api/flights?km=50")
        self.assertEqual(st, 200, data)
        self.assertEqual(json.loads(data)["where"], "Locator JO20ft")
        self.qth("")
        st, data = self.get("/api/flights?km=50")
        self.assertEqual(st, 409, data)

    def test_a_locator_typed(self):
        st, data = self.get("/api/flights?km=200&grid=jo21")
        self.assertEqual(st, 200, data)
        self.assertEqual(json.loads(data)["where"], "JO21")
        self.assertEqual(Stand.seen[0]["path"], "/a/51.5/5.0/113")

    def test_refused(self):
        for q, st in (("km=30", 400), ("km=x", 400), ("km=100&grid=ZZ99", 400)):
            got, data = self.get("/api/flights?" + q)
            self.assertEqual(got, st, q)
            self.assertEqual(json.loads(data)["status"], "error")
        self.qth("")
        got, data = self.get("/api/flights?km=100")
        self.assertEqual(got, 409)
        self.assertIn("No position", json.loads(data)["msg"])
        self.gui({"t": "pos", "src": ""})
        got, _ = self.get("/api/flights?km=100")
        self.assertEqual(got, 409, "the GUI has no position either")
        self.assertEqual(Stand.seen, [])

    def test_the_helpers_error(self):
        Stand.answers["a"] = (500, b"", {})
        Stand.answers["b"] = (500, b"", {})
        got, data = self.get("/api/flights?km=100")
        self.assertEqual(got, 502)
        self.assertEqual(json.loads(data)["msg"], "airplanes.live: HTTP 500; adsb.lol: HTTP 500")


if __name__ == "__main__":
    unittest.main()
