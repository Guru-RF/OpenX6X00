#!/usr/bin/env python3
"""The DMR app's helper, rootfs/usr/sbin/x6100-dmr, on this PC: run on a SOCK_SEQPACKET socket pair as the app
(src/dialog_dmr.c) starts it, against fake_master.py on 127.0.0.1. Nothing else is contacted: the settings name
127.0.0.1, the name lookup is a server of the test's on 127.0.0.1 (or off), and every session checks that the master
got nothing but the listener's packets. What the helper says is read by the app's own parser (src/dmr_proto.c,
through dmr_proto_dump) and checked there.

  - the login: the ID, the password's proof, the description (the callsign, no frequency, our own software), the
    options with the talkgroup; the keepalive; the logout when the app closes;
  - only RPTL, RPTK, RPTC, RPTO, RPTPING and RPTCL ever reach the master, whatever it sends (voice to us, a beacon
    request, data, private calls): never DMRD;
  - the calls: a voice header, voice frames passed on byte for byte, a terminator; a late entry ended by silence;
    another call over one, a private call not to us, data, a frame twice: dropped;
  - the master closing, dropping the hotspot, falling silent: a new login after a wait; one that refuses the
    password, the ID or the callsign: given up at once, no loop;
  - settings missing, a BrandMeister master: refused before any packet;
  - the password: set and cleared through stdin, for root alone, never in what the helper writes or says;
  - Test, the web page's; the names from a lookup, asked again when old (30 days; a day for an ID not known), an
    empty answer never wiping a name known; the web page's status file;
  - the listening log: a line a call, the short ones left out, the full log set aside;
  - the map: the stations heard in the last 24 hours in their callsign's country (the repository's cty.dat), the
    one talking in the city of its record (a radioid.net and a place search of the test's, on 127.0.0.1), asked
    once and kept, and only while a map is looked at; the map's file gone when nobody looks; "map" alone asks
    nobody.

    python3 -I -B dmr_test.py
"""
import hashlib
import importlib.util
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
import time
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-dmr")
CTY = os.path.join(HERE, "..", "..", "third-party", "cty", "cty.dat")
DUMP = os.path.join(HERE, "dmr_proto_dump")
_spec = importlib.util.spec_from_file_location("fake_master", os.path.join(HERE, "fake_master.py"))
fake_master = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fake_master)

DMR_ID = "9990001"          # made up for the test: no network ever sees it
HID = 999000101
CALL = "TE5T"
PASSWORD = "s3cret-Hotspot#pw"
ALLOWED = {"RPTL", "RPTK", "RPTC", "RPTO", "RPTPING", "RPTCL"}
FAST = {"X6100_DMR_PING": "0.3", "X6100_DMR_DEAD": "1.5", "X6100_DMR_RETRY": "0.4", "X6100_DMR_STEP": "0.3",
        "X6100_FW": "9.9.9-test"}


class Names(BaseHTTPRequestHandler):
    """radioid.net's answer for an ID, on 127.0.0.1: the IDs asked kept in asked; one in unknown has no answer"""
    asked = []
    unknown = set()

    def do_GET(self):
        i = self.path.rsplit("=", 1)[-1]
        Names.asked.append(int(i))
        res = [] if int(i) in Names.unknown else [{"id": int(i), "callsign": "ON4ABC", "fname": "Jan",
                                                    "city": "Gent", "state": "Oost-Vlaanderen", "country": "Belgium"}]
        body = json.dumps({"results": res}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class PlaceSearch(BaseHTTPRequestHandler):
    """The place search's answer, on 127.0.0.1: what was asked kept in asked"""
    asked = []

    def do_GET(self):
        PlaceSearch.asked.append(urllib.parse.unquote(self.path.rsplit("q=", 1)[-1]))
        body = json.dumps([{"lat": "51.0538", "lon": "3.7251", "display_name": "Gent"}]).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class Session:
    """The helper running as the app runs it: its records read in a thread"""

    def __init__(self, t, args, env):
        self.a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.p = subprocess.Popen([sys.executable, "-I", HELPER, *args], stdin=b, stdout=b, stderr=subprocess.PIPE,
                                  env=env)
        b.close()
        self.lines, self.voice = [], []
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        while True:
            try:
                r = self.a.recv(4096)
            except OSError:
                return
            if not r:
                return
            with self.lock:
                if r[0] == 0:
                    self.voice.append(r[1:])
                else:
                    self.lines.append(r.decode())

    def wait_line(self, start, timeout=6.0):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            with self.lock:
                for line in self.lines:
                    if line.startswith(start):
                        return line
            time.sleep(0.02)
        raise AssertionError(f"no line {start!r} in {self.lines}")

    def count(self, start):
        with self.lock:
            return sum(1 for line in self.lines if line.startswith(start))

    def close(self):
        """The app closes: the end of its socket"""
        self.a.shutdown(socket.SHUT_RDWR)
        self.a.close()
        try:
            return self.p.wait(5)
        finally:
            self.p.stderr.close()


class DmrHelper(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="dmr-test.")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.master = fake_master.FakeMaster([HID], PASSWORD, {HID: CALL})
        self.addCleanup(self.master.close)
        self.data = os.path.join(self.dir, "dmr")
        self.log = os.path.join(self.dir, "app_logs", "dmr.txt")
        self.status = os.path.join(self.dir, "dmr.json")
        self.db = os.path.join(self.dir, "params.db")
        con = sqlite3.connect(self.db)
        con.execute("CREATE TABLE params (name TEXT PRIMARY KEY, val TEXT)")
        con.execute("INSERT INTO params VALUES ('callsign', ?)", (CALL + "/P",))
        con.commit()
        con.close()
        self.conf()
        self.set_password(PASSWORD)
        self.map = os.path.join(self.dir, "map.json")
        self.want = os.path.join(self.dir, "map.want")
        self.env = dict(os.environ, X6100_DMR_DIR=self.data, X6100_DMR_LOG=self.log, X6100_DMR_STATUS=self.status,
                        X6100_PARAMS_DB=self.db, X6100_DMR_RADIOID="", X6100_DMR_MAP=self.map,
                        X6100_DMR_MAP_WANT=self.want, X6100_DMR_PLACE="", X6100_CTY=CTY, **FAST)
        self.sessions = []

    def tearDown(self):
        for s in self.sessions:
            if s.p.poll() is None:
                s.p.kill()
                s.p.wait()
            s.a.close()
            if not s.p.stderr.closed:
                s.p.stderr.close()
        # whatever happened, the master got the listener's packets only
        words = set(self.master.words())
        self.assertTrue(words <= ALLOWED, f"sent to the master: {words - ALLOWED}")
        self.assertFalse(self.master.packets(b"DMRD"))
        for p in self.master.packets():
            self.assertNotIn(PASSWORD.encode(), p)
        for path in (self.log, self.status):
            if os.path.exists(path):
                with open(path, encoding="utf-8") as f:
                    self.assertNotIn(PASSWORD, f.read())

    def conf(self, **kw):
        c = {"NETWORK": "freedmr", "HOST": "127.0.0.1", "PORT": str(self.master.port), "DMR_ID": DMR_ID, "SSID": "01",
             "OPTIONS": "", "TALKGROUPS": "91 World|2062 Belgium"}
        c.update(kw)
        os.makedirs(self.data, exist_ok=True)
        with open(os.path.join(self.data, "dmr.conf"), "w") as f:
            f.write("".join(f"{k}={v}\n" for k, v in c.items()))

    def run_cmd(self, *args, stdin=None):
        env = getattr(self, "env", None) or dict(os.environ, X6100_DMR_DIR=self.data, X6100_DMR_LOG=self.log)
        return subprocess.run([sys.executable, "-I", HELPER, *args], input=stdin, capture_output=True, text=True,
                              env=env, timeout=30)

    def set_password(self, pw):
        r = self.run_cmd("set-password", stdin=pw + "\n")
        self.assertEqual(r.returncode, 0, r.stderr)

    def start(self, *args, env=None):
        s = Session(self, ["run", *args], env or self.env)
        self.sessions.append(s)
        return s

    def online(self, tg="91"):
        s = self.start(tg)
        s.wait_line("state online")
        return s

    def parsed(self, s):
        """What the app makes of the session's lines (src/dmr_proto.c)"""
        with s.lock:
            text = "".join(line + "\n" for line in s.lines)
        r = subprocess.run([DUMP], input=text, capture_output=True, text=True, check=True)
        out = {}
        for line in r.stdout.splitlines():
            k, _, v = line.partition("=")
            out.setdefault(k, []).append(v)
        return out

    # ---------- the login ----------

    def test_login(self):
        s = self.online()
        self.assertEqual(self.master.words()[:4], ["RPTL", "RPTK", "RPTC", "RPTO"])
        c = self.master.config[HID]
        self.assertEqual(c["callsign"], CALL)                   # the radio's own, its /P left off
        self.assertEqual((c["rx"], c["tx"]), ("000000000", "000000000"))    # no frequency: no radio for DMR
        self.assertEqual(c["software"], "OpenX6X00 9.9.9-test")
        self.assertEqual(c["package"], "OpenX6X00 DMR listener")
        self.assertEqual(c["description"], "Listen only")
        self.assertEqual((c["lat"], c["lon"]), ("0.000000", "00.000000"))     # MMDVM's %08f and %09f
        self.assertEqual(c["slots"], "4")
        self.assertEqual(self.master.options[HID], "TS2=91;DIAL=0;VOICE=0;")
        self.assertEqual(s.wait_line("state online"), f"state online 127.0.0.1 {HID} 91")
        time.sleep(1.0)
        self.assertGreaterEqual(len(self.master.packets(b"RPTPING")), 2)
        with open(self.status) as f:
            st = json.load(f)
        self.assertEqual((st["state"], st["hotspot"], st["tg"], st["network"]), ("online", HID, 91, "FreeDMR"))
        self.assertEqual(s.close(), 0)
        self.assertEqual(self.master.words()[-1], "RPTCL")
        self.assertFalse(os.path.exists(self.status))
        p = self.parsed(s)
        self.assertEqual((p["conn"], p["master"], p["hotspot"], p["tg"]), (["online"], ["127.0.0.1"], [str(HID)],
                                                                         ["91"]))

    def test_options_typed(self):
        self.conf(NETWORK="other", OPTIONS="TS1={tg};TS2=9;")
        s = self.online("2062")
        self.assertEqual(self.master.options[HID], "TS1=2062;TS2=9;")
        s.close()

    def test_no_options(self):
        """No talkgroup chosen: no RPTO, the master's own talkgroups"""
        self.conf(NETWORK="other")
        s = self.online("0")
        self.assertNotIn("RPTO", self.master.words())
        s.close()

    # ---------- the calls ----------

    def test_call(self):
        s = self.online()
        payloads = [bytes([i] * 33) for i in range(6)]
        sent = self.master.call(9990001, 91, 0x1111, 12, payloads=payloads)
        s.wait_line("end 9990001")
        self.assertEqual(s.wait_line("call"), "call 9990001 91 group 2")
        self.assertEqual(s.voice, sent)                         # byte for byte, in order
        secs = float(s.wait_line("end").split()[2])
        self.assertTrue(0.4 < secs < 2.0, secs)
        p = self.parsed(s)
        self.assertEqual((p["calls"], p["ends"], p["talking"]), (["1"], ["1"], ["0"]))
        self.assertEqual(p["heard"], [f"9990001 9990001 91 {round(secs)}"])
        s.close()

    def test_late_entry(self):
        """No voice header: the call starts with its first voice frame and ends after a second of silence"""
        s = self.online()
        self.master.call(9990001, 91, 0x2222, 5, header=False, terminator=False)
        self.assertEqual(s.wait_line("call"), "call 9990001 91 group 2")
        t0 = time.monotonic()
        s.wait_line("end 9990001")
        self.assertGreater(time.monotonic() - t0, 0.6)
        self.assertEqual(len(s.voice), 5)
        s.close()

    def test_dropped(self):
        s = self.online()
        m = self.master
        m.dmrd(9990001, 91, 0x80 | 0x20 | 1, 0x3333)                       # a call starts
        m.dmrd(9990001, 91, 0x80 | 0x10, 0x3333, bytes([1] * 33))
        m.dmrd(4444444, 92, 0x80 | 0x10, 0x4444, bytes([2] * 33))           # another over it: dropped
        seq = m.dmrd(9990001, 91, 0x80 | 0x01, 0x3333, bytes([3] * 33))
        m.dmrd(9990001, 91, 0x80 | 0x01, 0x3333, bytes([3] * 33), seq=seq)  # the same frame again: dropped
        m.dmrd(9990001, 91, 0x80 | 0x20 | 3, 0x3333, bytes([4] * 33))       # data in the call: not voice
        m.dmrd(9990001, 91, 0x80 | 0x20 | 2, 0x3333)                       # its terminator
        s.wait_line("end 9990001")
        self.assertEqual(s.voice, [bytes([1] * 33), bytes([3] * 33)])
        m.dmrd(5555555, 1234567, 0x80 | 0x40 | 0x10, 0x5555, bytes([5] * 33))   # private, not to us: dropped
        m.dmrd(6666666, int(DMR_ID), 0x40 | 0x10, 0x6666, bytes([6] * 33))     # private, to our ID: heard
        self.assertEqual(s.wait_line("call 6666666"), f"call 6666666 {DMR_ID} private 1")
        m.send(b"RPTSBKN" + HID.to_bytes(4, "big"))                          # a beacon request: not answered
        m.dmrd(7777777, 91, 0x80 | 0x20 | 6, 0x7777, bytes([7] * 33))        # data alone: no call
        time.sleep(1.5)
        self.assertEqual(s.voice[2:], [bytes([6] * 33)])
        self.assertEqual(s.count("call"), 2)
        s.close()

    def names_server(self):
        Names.asked, Names.unknown = [], set()
        httpd = HTTPServer(("127.0.0.1", 0), Names)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        return dict(self.env, X6100_DMR_RADIOID=f"http://127.0.0.1:{httpd.server_port}/api/dmr/user/?id={{}}")

    def names_file(self):
        with open(os.path.join(self.data, "names.json"), encoding="utf-8") as f:
            return json.load(f)

    def test_names(self):
        s = self.start("91", env=self.names_server())
        s.wait_line("state online")
        self.master.call(9990001, 91, 0x8888, 4)
        self.assertEqual(s.wait_line("name 9990001"), "name 9990001 ON4ABC Jan")
        s.wait_line("end 9990001")
        self.master.call(9990001, 91, 0x9999, 2)
        self.assertEqual(s.wait_line("call 9990001 91 group 2 "), "call 9990001 91 group 2 ON4ABC Jan")
        s.wait_line("end 9990001")
        self.assertEqual(self.parsed(s)["heard"][0], "9990001 ON4ABC Jan 91 0")
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.data, "names.json")).st_mode), 0o600)
        self.assertEqual(Names.asked, [9990001])                # once: the second call had it
        name, when = self.names_file()["9990001"]
        self.assertEqual(name, "ON4ABC Jan")
        self.assertLess(abs(time.time() - when), 60)
        s.close()

    # ---------- the listening log ----------

    def heard_log(self, name="heard.tsv"):
        with open(os.path.join(self.data, name), encoding="utf-8") as f:
            return [line.split("\t") for line in f.read().splitlines()]

    def test_heard_log(self):
        """Each call of half a second or more is a line of heard.tsv: its start, the ID, the name known then, the
        talkgroup, the seconds, the network, group or private; a shorter one is not; the log stays after the app
        closes and the next session adds to it"""
        with open(os.path.join(self.data, "names.json"), "w") as f:
            json.dump({"9990002": ["ON4ABC\tJan\n", int(time.time())]}, f)     # a tab and a line end in a name
        s = self.online()
        t0 = time.time()
        self.master.call(9990001, 91, 0x1111, 12)               # 0.66 s, no name known
        s.wait_line("end 9990001")
        self.master.call(9990003, 91, 0x2222, 3)                # 0.12 s: heard, not logged
        s.wait_line("end 9990003")
        self.master.call(9990002, int(DMR_ID), 0x3333, 12, slot=1, private=True)
        s.wait_line("end 9990002")
        s.close()
        rows = self.heard_log()
        self.assertEqual([r[1:4] + r[5:] for r in rows],
                         [["9990001", "", "91", "FreeDMR", "group"],
                          ["9990002", "ON4ABC Jan", DMR_ID, "FreeDMR", "private"]])
        for r in rows:
            self.assertEqual(len(r), 7)
            self.assertLess(abs(int(r[0]) - t0), 30)            # the start, Unix time
            self.assertEqual(r[4], "1")                         # 0.66 s, rounded
        with open(self.log) as f:
            self.assertEqual(f.read().count("heard 999000"), 3)     # the app's log has the short one too
        s = self.online("2062")
        self.master.call(9990001, 2062, 0x4444, 12)
        s.wait_line("end 9990001")
        s.close()
        self.assertEqual([r[3] for r in self.heard_log()], ["91", DMR_ID, "2062"])

    def test_heard_log_full(self):
        """Over 2 MB the log becomes heard.tsv.1, the one before it gone, and a new one starts"""
        path = os.path.join(self.data, "heard.tsv")
        line = "1700000000\t9990009\tOLD1 Old\t91\t5\tFreeDMR\tgroup\n"
        with open(path, "w") as f:
            f.write(line * (2 * 1024 * 1024 // len(line) + 1))
        with open(path + ".1", "w") as f:
            f.write("the one before\n")
        s = self.online()
        self.master.call(9990001, 91, 0x1111, 12)
        s.wait_line("end 9990001")
        s.close()
        self.assertEqual([r[1] for r in self.heard_log()], ["9990001"])
        old = self.heard_log("heard.tsv.1")
        self.assertGreater(len(old), 40000)
        self.assertEqual(old[0][1], "9990009")

    # ---------- the map ----------

    def map_servers(self):
        env = self.names_server()
        PlaceSearch.asked = []
        httpd = HTTPServer(("127.0.0.1", 0), PlaceSearch)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        return dict(env, X6100_DMR_PLACE=f"http://127.0.0.1:{httpd.server_port}/search?format=json&limit=1&q={{}}")

    def look(self):
        """Somebody looks at a map"""
        with open(self.want, "a"):
            os.utime(self.want, None)

    def wait_map(self, ok, timeout=8.0):
        t0 = time.monotonic()
        d = None
        while time.monotonic() - t0 < timeout:
            try:
                with open(self.map, encoding="utf-8") as f:
                    d = json.load(f)
                if ok(d):
                    return d
            except (OSError, ValueError):
                pass
            time.sleep(0.05)
        raise AssertionError(f"the map: {d}")

    def kept_log(self, now):
        """A listening log from before: a station 2 hours ago, one 25 hours ago, one whose name came later, one
        radioid.net has no callsign for"""
        rows = [(now - 25 * 3600, 9990011, "DL1ABC Old", 91, 9), (now - 7200, 9990012, "W1ABC Sam", 3100, 20),
                (now - 3600, 9990013, "", 91, 4), (now - 1800, 9990014, "Club station", 91, 4),
                (now - 900, 9990012, "W1ABC Sam", 91, 30)]
        with open(os.path.join(self.data, "heard.tsv"), "w") as f:
            f.write("".join("%d\t%d\t%s\t%d\t%d\tFreeDMR\tgroup\n" % r for r in rows))
        with open(os.path.join(self.data, "names.json"), "w") as f:
            json.dump({"9990013": ["JA1ABC Ken", now]}, f)

    def test_map(self):
        now = int(time.time())
        self.kept_log(now)
        env = self.map_servers()
        s = self.start("91", env=env)
        s.wait_line("state online")
        time.sleep(1.0)
        self.assertFalse(os.path.exists(self.map))              # nobody looks: no file
        self.master.call(9990001, 91, 0x1111, 12)
        s.wait_line("end 9990001")
        self.assertEqual(PlaceSearch.asked, [])                 # and no place asked for
        self.assertEqual(Names.asked, [9990001])                # the name, as before
        self.assertFalse(os.path.exists(os.path.join(self.data, "places.json")))

        self.look()
        d = self.wait_map(lambda d: len(d["stations"]) == 3)
        self.assertEqual((d["hours"], d["talker"]), (24, None))
        st = d["stations"]
        self.assertEqual([(x["id"], x["call"], x["tg"], x["country"], x["recent"]) for x in st],
                         [(9990001, "ON4ABC", 91, "Belgium", True),         # heard just now, the newest first
                          (9990012, "W1ABC", 91, "United States", False),   # once, with its last call
                          (9990013, "JA1ABC", 91, "Japan", False)])         # its name from names.json
        self.assertTrue(49 < st[0]["lat"] < 52.5 and 2 < st[0]["lon"] < 7, st[0])       # Belgium, moved a little
        self.assertTrue(860 < st[1]["ago"] < 930, st[1])

        t = threading.Thread(target=self.master.call, args=(9990001, 91, 0x2222, 80))    # 4.8 s
        t.start()
        d = self.wait_map(lambda d: d["talker"] and d["talker"].get("exact"))
        self.assertEqual(d["talker"], {"id": 9990001, "call": "ON4ABC", "name": "Jan", "tg": 91, "city": "Gent",
                                       "country": "Belgium", "lat": 51.054, "lon": 3.725, "exact": True})
        t.join()
        s.wait_line("end 9990001")
        self.assertEqual(PlaceSearch.asked, ["Gent, Oost-Vlaanderen, Belgium"])
        self.assertEqual(Names.asked, [9990001, 9990001])       # the record, once more for its city
        with open(os.path.join(self.data, "places.json")) as f:
            self.assertEqual(json.load(f)["9990001"]["city"], "Gent")
        self.master.call(9990001, 91, 0x3333, 60)               # heard again: nothing asked again
        s.wait_line("end 9990001")
        self.assertEqual((len(PlaceSearch.asked), len(Names.asked)), (1, 2))
        d = self.wait_map(lambda d: d["talker"] is None and d["stations"][0]["ago"] < 5)
        s.close()
        self.assertFalse(os.path.exists(self.map))              # the app closed: the file goes with it

    def test_map_country_only(self):
        """No place search (or no city): the station talking is drawn in its callsign's country, not exact"""
        self.look()
        s = self.start("91", env=self.names_server())
        s.wait_line("state online")
        t = threading.Thread(target=self.master.call, args=(9990001, 91, 0x2222, 80))
        t.start()
        d = self.wait_map(lambda d: d["talker"] and d["talker"].get("city"))
        t.join()
        self.assertEqual((d["talker"]["exact"], d["talker"]["country"], d["talker"]["city"]), (False, "Belgium", "Gent"))
        self.assertTrue(49 < d["talker"]["lat"] < 52.5)
        s.close()

    def test_map_not_looked_at(self):
        """The map's file goes once nobody looks any more"""
        self.look()
        s = self.online()
        self.wait_map(lambda d: True)
        old = time.time() - 120
        os.utime(self.want, (old, old))
        t0 = time.monotonic()
        while os.path.exists(self.map) and time.monotonic() - t0 < 6:
            time.sleep(0.05)
        self.assertFalse(os.path.exists(self.map))
        s.close()

    def test_map_once(self):
        """x6100-dmr map: from what is kept, asking nobody, to stdout or to a file; the app's talker when it listens"""
        now = int(time.time())
        self.kept_log(now)
        with open(os.path.join(self.data, "places.json"), "w") as f:
            json.dump({"9990013": {"call": "JA1ABC", "name": "Ken", "city": "Tokyo", "country": "Japan",
                                   "lat": 35.68, "lon": 139.69}}, f)
        env = self.map_servers()
        r = subprocess.run([sys.executable, "-I", HELPER, "map"], capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        d = json.loads(r.stdout)
        self.assertEqual([x["call"] for x in d["stations"]], ["W1ABC", "JA1ABC"])
        self.assertIsNone(d["talker"])
        with open(self.status, "w") as f:
            json.dump({"state": "online", "talker": {"id": 9990013, "name": "JA1ABC Ken", "tg": 91}}, f)
        out = os.path.join(self.dir, "once.json")
        r = subprocess.run([sys.executable, "-I", HELPER, "map", out], capture_output=True, text=True, env=env,
                           timeout=30)
        self.assertEqual((r.returncode, r.stdout), (0, ""), r.stderr)
        with open(out) as f:
            d = json.load(f)
        self.assertEqual(d["talker"], {"id": 9990013, "call": "JA1ABC", "name": "Ken", "tg": 91, "city": "Tokyo",
                                       "country": "Japan", "lat": 35.68, "lon": 139.69, "exact": True})
        with open(self.status, "w") as f:                       # a talker without a place kept: not asked for
            json.dump({"state": "online", "talker": {"id": 9990001, "name": "", "tg": 91}}, f)
        self.look()
        r = subprocess.run([sys.executable, "-I", HELPER, "map"], capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(json.loads(r.stdout)["talker"]["call"], "")
        self.assertEqual((Names.asked, PlaceSearch.asked), ([], []))
        self.assertEqual(self.master.packets(), [])             # and the master hears nothing of it

    def test_names_asked_again(self):
        """Old names are shown and asked again (30 days; a day for an ID radioid.net did not know); a fresh one is
        not; an empty answer keeps the name known"""
        now = int(time.time())
        day = 86400
        kept = {"9990001": "OLD1 Old",                          # from before the time was kept: old
                "9990002": ["OLD2 Old", now - 31 * day],        # a name of 31 days
                "9990003": ["FRESH Name", now - 29 * day],      # of 29 days: not asked
                "9990004": ["", now - 2 * day],                 # not known two days ago
                "9990005": ["", now - 3600],                    # not known an hour ago: not asked
                "9990006": ["GONE Name", now - 40 * day]}       # old, but radioid.net has no answer now
        os.makedirs(self.data, exist_ok=True)
        with open(os.path.join(self.data, "names.json"), "w", encoding="utf-8") as f:
            json.dump(kept, f)
        env = self.names_server()
        Names.unknown = {9990006}
        s = self.start("91", env=env)
        s.wait_line("state online")
        calls = [(9990001, "OLD1 Old", "ON4ABC Jan"), (9990002, "OLD2 Old", "ON4ABC Jan"),
                 (9990003, "FRESH Name", None), (9990004, "", "ON4ABC Jan"), (9990005, "", None),
                 (9990006, "GONE Name", None)]
        for n, (i, shown, found) in enumerate(calls):
            self.master.call(i, 91, 0x1000 + n, 2)
            line = s.wait_line(f"call {i} ")
            self.assertEqual(line, f"call {i} 91 group 2" + (f" {shown}" if shown else ""))
            if found:
                self.assertEqual(s.wait_line(f"name {i}"), f"name {i} {found}")
            s.wait_line(f"end {i}")
            time.sleep(0.3)                                     # the lookup, if any, over before the next call
        self.assertEqual(Names.asked, [9990001, 9990002, 9990004, 9990006])
        self.assertEqual(s.count("name 9990006"), 0)            # the empty answer told nothing
        got = self.names_file()
        for i, name in ((9990001, "ON4ABC Jan"), (9990002, "ON4ABC Jan"), (9990004, "ON4ABC Jan"),
                        (9990006, "GONE Name")):
            self.assertEqual(got[str(i)][0], name)
            self.assertLess(abs(now - got[str(i)][1]), 60)      # asked now
        self.assertEqual(got["9990003"], kept["9990003"])
        self.assertEqual(got["9990005"], kept["9990005"])
        self.assertEqual(list(got)[-4:], ["9990001", "9990002", "9990004", "9990006"])   # the newest asked last
        s.close()

    # ---------- lost and refused ----------

    def test_master_closes(self):
        s = self.online()
        self.master.send(b"MSTCL" + HID.to_bytes(4, "big"))
        self.assertTrue(s.wait_line("state retry").startswith("state retry 0 the master closed the link"))
        t0 = time.monotonic()
        while self.master.words().count("RPTL") < 2 and time.monotonic() - t0 < 5:
            time.sleep(0.05)
        self.assertEqual(self.master.words().count("RPTL"), 2)
        s.close()

    def test_master_silent(self):
        s = self.online()
        self.master.silent = True
        s.wait_line("state retry 0 the master stopped answering")
        self.master.silent = False
        while s.count("state online") < 2:
            time.sleep(0.05)
        s.close()

    def test_master_forgets(self):
        """MSTNAK while online (the master restarted): a new login, which works"""
        s = self.online()
        self.master.state[HID] = "login"
        while s.count("state online") < 2:
            time.sleep(0.05)
        self.assertEqual(s.wait_line("state retry"), "state retry 0 the master dropped the hotspot")
        s.close()

    def refused(self, why, **conf):
        if conf:
            self.conf(**conf)
        s = self.start("91")
        self.assertEqual(s.p.wait(10), 1)
        self.assertEqual(s.wait_line("error"), "error " + why)
        self.assertEqual(self.parsed(s)["conn"], ["error"])
        s.p.stderr.close()
        return s

    def test_wrong_password(self):
        self.set_password("not-the-password")
        self.refused("the master refused the hotspot password")
        self.assertEqual(self.master.words(), ["RPTL", "RPTK"])     # once: no loop

    def test_unknown_id(self):
        self.refused(f"the master refused hotspot ID {HID + 1}: is the DMR ID registered?", SSID="02")
        self.assertEqual(self.master.words(), ["RPTL"])

    def test_not_the_callsign(self):
        self.master.callsigns[HID] = "OTHER"
        self.refused("the master refused the hotspot's description (is the callsign the DMR ID's?)")
        self.assertEqual(self.master.words(), ["RPTL", "RPTK", "RPTC"])

    def test_options_refused(self):
        self.master.refuse_options = True
        self.refused("the master refused the options")

    def test_no_answer(self):
        """A master that never answers: the login step three times, then a wait"""
        self.master.silent = True
        s = self.start("91")
        s.wait_line("state retry 0 the master does not answer", timeout=8)
        self.assertEqual(self.master.words(), ["RPTL"] * 3)
        s.close()

    def test_closed_during_login(self):
        """The app closed while the master does not answer: the login stops and the helper logs out at once"""
        self.master.silent = True
        s = self.start("91")
        while not self.master.packets(b"RPTL"):
            time.sleep(0.02)
        t0 = time.monotonic()
        self.assertEqual(s.close(), 0)
        self.assertLess(time.monotonic() - t0, 2)
        self.assertEqual(self.master.words(), ["RPTL", "RPTCL"])

    def test_settings_missing(self):
        cases = [({"DMR_ID": ""}, "no DMR ID: set yours on the radio's web page (DMR)"),
                 ({"DMR_ID": "12345"}, "no DMR ID: set yours on the radio's web page (DMR)"),
                 ({"SSID": "00"}, "the hotspot's two digits are 01 to 99 (web page, DMR)"),
                 ({"HOST": ""}, "no master: set the network's server on the radio's web page (DMR)"),
                 ({"PORT": "70000"}, "the master's port is 1 to 65535 (web page, DMR)"),
                 ({"HOST": "3021.master.brandmeister.network"},
                  "BrandMeister keeps its hotspot protocol for hotspots with a radio module: not for this listener"),
                 ({"HOST": "BrandMeister.Network."},
                  "BrandMeister keeps its hotspot protocol for hotspots with a radio module: not for this listener"),
                 ({"OPTIONS": "TS2={tg};" + "x" * 300}, "the options are up to 255 plain characters (web page, DMR)")]
        for conf, why in cases:
            with self.subTest(conf=conf):
                self.conf(**conf)
                self.refused(why)
        self.conf()
        self.run_cmd("clear-password")
        self.refused("no hotspot password: set it on the radio's web page (DMR)")
        self.set_password(PASSWORD)
        con = sqlite3.connect(self.db)
        con.execute("UPDATE params SET val = '' WHERE name = 'callsign'")
        con.commit()
        con.close()
        self.refused("no callsign: set yours in Settings > General (3 to 8 letters and digits)")
        self.assertEqual(self.master.packets(), [])     # not one packet for any of them

    # ---------- the password, Test ----------

    def test_password(self):
        path = os.path.join(self.data, "password")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        with open(path) as f:
            self.assertEqual(f.read(), PASSWORD + "\n")
        for bad in ("", "a\tb", "x" * 65):
            r = self.run_cmd("set-password", stdin=bad + "\n")
            self.assertEqual(r.returncode, 1)
            self.assertNotIn(bad or "@@", r.stderr if bad else "")
        with open(path) as f:
            self.assertEqual(f.read(), PASSWORD + "\n")             # a refused one leaves it
        s = self.online()
        self.master.call(9990001, 91, 0xaaaa, 3)
        s.wait_line("end 9990001")
        self.assertEqual(s.close(), 0)
        for line in s.lines:
            self.assertNotIn(PASSWORD, line)
        self.assertEqual(self.run_cmd("clear-password").returncode, 0)
        self.assertFalse(os.path.exists(path))
        self.assertEqual(self.run_cmd("clear-password").returncode, 0)     # none to clear: fine

    def test_test_command(self):
        r = self.run_cmd("test")
        self.assertEqual((r.returncode, r.stdout.strip()), (0, f"ok FreeDMR 127.0.0.1:{self.master.port}, hotspot {HID}"))
        self.assertEqual(self.master.words(), ["RPTL", "RPTK", "RPTC", "RPTO", "RPTCL"])
        self.assertEqual(self.master.options[HID], "TS2=91;DIAL=0;VOICE=0;")    # the first talkgroup of the list
        self.set_password("wrong-one")
        r = self.run_cmd("test")
        self.assertEqual((r.returncode, r.stdout.strip()), (1, "error the master refused the hotspot password"))
        with open(self.log) as f:
            log = f.read()
        self.assertIn("test: logged in to 127.0.0.1", log)
        self.assertNotIn("wrong-one", log)

    def test_helper_sends_from_one_place(self):
        """The helper's source: the socket's sendto is called in send() alone, and send() checks the packet's word"""
        with open(HELPER) as f:
            src = f.read()
        self.assertEqual(src.count(".sendto("), 1)
        body = src[src.index("    def send(self, pkt):"):src.index("    def recv(self):")]
        self.assertIn(".sendto(", body)
        self.assertIn('ALLOWED = {b"RPTL": 8, b"RPTK": 40, b"RPTC": 302, b"RPTO": None, b"RPTPING": 11, '
                      'b"RPTCL": 9}', src)
        self.assertNotIn('b"DMRD" +', src)


if __name__ == "__main__":
    if not os.access(DUMP, os.X_OK):
        sys.exit(f"no {DUMP}: make dmr_proto_dump")
    unittest.main(verbosity=1)
