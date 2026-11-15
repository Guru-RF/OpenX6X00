#!/usr/bin/env python3
"""Wavelog's live link (#245), the web server's side (webserver wavelog_live.py), on a PC. See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/wavelog_live/wavelog_live_test.py [-v]

The paths of the radio are moved into a new folder first; the GUI's configurator socket is a stand-in (an abstract
socket: no file), and so is Wavelog, an HTTP server on 127.0.0.1 that answers api/radio as Wavelog's Api::radio does.

Radio to Wavelog
1. Feed, on a clock of the test's own: a value goes once it stayed SETTLE_S, never while the knob turns, at most one
   every MIN_GAP_S (a slow turn makes no more than 15 posts a minute, half of Wavelog's strictest example limit), the
   same value again after HEARTBEAT_S, the waits after failures and after Wavelog's 429, and again() after a Save.
2. The fields: radio (the model, or the name set), frequency (where the radio transmits), frequency_rx in split only,
   mode (the map below), power in W, cat_url only with Tune from Wavelog on, the key added at the post. The modes: an
   app's, FT8 by the dial, the voice sideband, CW for CW-R, FM, AM, a data mode with no name as USB-D or LSB-D.
3. One post against the stand-in: the path (index.php/api/radio), the key in the body; success, 429 with its
   retry_after, a read-only key and a refused key (refused: no more until a Save), a page that is not Wavelog's, no
   server at all, an API v2 token (refused before anything is sent).
4. The thread: it posts once the radio settled and again when it moves, says what went on the page, stops when the
   switch goes off; with no network it says it once (one log line, one line on the page) and tries again at longer and
   longer waits, not once a poll; with the GUI not answering it waits and posts nothing.
5. The settings (POST /api/wavelog/live): on only with the saved URL and key, Tune from Wavelog only with the live link
   on, a token made when it goes on and a new one on New address, gone when it goes off or the key is cleared; the
   name, the address and the origin checked. GET /api/wavelog never holds the token.
Wavelog to radio, /cat/<token>/<Hz>/<mode> through bottle's WSGI app
6. The token and the switch (404 for anything else, nothing sent to the GUI), one of the radio's own names in Host or
   the host of the address typed for the radio on the page (for this route alone, not through Funnel), Origin: the Wavelog URL's (or the one set for the browser) and no other, none and "null" refused, CORS for that
   origin alone; the preflight (Private Network Access) for that origin alone, GET only; HEAD refused; through Tailscale
   Funnel refused.
7. The frequency: inside the model's receive range (X6100 and X6200), JavaScript's 1000999.9999999999 taken as
   1001000, anything else refused. The modes: Wavelog's eleven to the radio's, any other refused.
8. No TX: the GUI is sent the tune command alone, with a frequency and a mode, and its refusal while the radio
   transmits comes back as 409; no GUI: 503.
9. The other routes as they were: no CORS header on them, the preflight not answered, a change from Wavelog's site
   refused.
"""
import http.server
import io
import json
import logging
import os
import shutil
import socket
import socketserver
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.parse
from unittest import mock
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(TOP, "webserver", "src")]
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
from x6100_webserver import apps, settings, wavelog_live as wl  # noqa: E402

HOST = "192.168.1.10"           # an address: one of the radio's own names
KEY = "wl1a2b3c4d5e6f7a"
WAVELOG = "https://log.example.org"
TS_NAME = "x6100.tail1234.ts.net"
TOKEN = "Tk3n-abcdefghijklmnopqr"


class Gui:
    """The GUI on the configurator socket: radio_get and tune, as src/wavelog_cat.c answers them"""

    def __init__(self, path):
        self.state = {"hz": 14074000, "tx_hz": 14074000, "mode": "USB-D", "log_mode": "FT8", "app": "FT8",
                      "pwr": 5.0, "tx": 0}
        self.transmitting = False
        self.cmds = []
        gui = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                line = self.rfile.readline()
                try:
                    cmd = json.loads(line)
                except ValueError:
                    return
                gui.cmds.append(cmd)
                if cmd.get("cmd") == "radio_get":
                    out = dict(gui.state, t="radio")
                elif cmd.get("cmd") == "tune":
                    if gui.transmitting:
                        out = {"t": "err", "err": "The radio is transmitting"}
                    else:
                        gui.state.update(hz=cmd["hz"], tx_hz=cmd["hz"], mode=cmd["mode"])
                        out = {"t": "ok"}
                else:
                    out = {"t": "err", "err": "Unknown command"}
                self.wfile.write(json.dumps(out).encode() + b"\n")

        self.server = socketserver.ThreadingUnixStreamServer(path, Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def tunes(self):
        return [c for c in self.cmds if c.get("cmd") != "radio_get"]


class Wavelog:
    """Wavelog's api/radio: 200 success, or what `answer` is set to (code, JSON or text)"""

    def __init__(self):
        self.posts = []
        self.answer = (200, {"status": "success"})
        wavelog = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
                try:
                    data = json.loads(body)
                except ValueError:
                    data = None
                wavelog.posts.append((self.path, data, self.headers.get("Content-Type")))
                code, out = wavelog.answer
                text = out if isinstance(out, str) else json.dumps(out)
                self.send_response(code)
                self.send_header("Content-Type", "text/html" if isinstance(out, str) else "application/json")
                self.end_headers()
                self.wfile.write(text.encode())

            def log_message(self, *a):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def closed_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="x6100-wl.")
        r = cls.root
        cls.saved = [(settings, n, getattr(settings, n)) for n in
                     ("FIELD_DIR", "CFG_SOCK", "WAVELOG_CMD", "HTTPS_PORT", "HTTP_PORT")]
        cls.saved += [(apps, n, getattr(apps, n)) for n in
                      ("TAILSCALE_STATUS", "TAILSCALE_CONF", "RESOLV_CONF", "OWN_NAMES_CONF", "radio_model")]
        settings.FIELD_DIR = r + "/field"
        settings.CFG_SOCK = f"\0x6100-wavelog-live-test-{os.getpid()}"   # abstract: no file
        settings.WAVELOG_CMD = os.path.join(TOP, "rootfs", "usr", "sbin", "x6100-wavelog")
        settings.HTTPS_PORT, settings.HTTP_PORT = 443, 80
        apps.TAILSCALE_STATUS, apps.TAILSCALE_CONF = r + "/ts_status", r + "/ts.conf"
        apps.RESOLV_CONF, apps.OWN_NAMES_CONF = r + "/resolv.conf", r + "/own_names.conf"
        os.makedirs(settings.FIELD_DIR)
        cls.model("x6100")

    @classmethod
    def tearDownClass(cls):
        for mod, name, val in reversed(cls.saved):
            setattr(mod, name, val)
        shutil.rmtree(cls.root, ignore_errors=True)

    @classmethod
    def model(cls, name):
        """The radio's model (x6100, x6200), as apps.radio_model reads it from the device tree"""
        apps.radio_model = lambda: name

    def conf(self, **kw):
        conf = {"enabled": "0", "url": WAVELOG, "key": KEY, "verify_tls": "1", "live": "1"}
        conf.update(kw)
        apps.wavelog_write({k: v for k, v in conf.items() if v is not None})

    def tailscale(self, state="running", funnel="off"):
        with open(apps.TAILSCALE_STATUS, "w") as f:
            f.write(f"state={state}\ntext=Connected\nname={TS_NAME}.\nfunnel={funnel}\n")

    def no_tailscale(self):
        try:
            os.remove(apps.TAILSCALE_STATUS)
        except FileNotFoundError:
            pass

    def req(self, method, path, headers=None, body=None, host=HOST):
        """(status, headers {lower name: value}, body bytes) through bottle's WSGI app"""
        path, _, query = path.partition("?")
        data = b"" if body is None else json.dumps(body).encode()
        env = {"REQUEST_METHOD": method, "PATH_INFO": urllib.parse.unquote_to_bytes(path).decode("latin1"),
               "QUERY_STRING": query,
               "HTTP_HOST": host, "CONTENT_TYPE": "application/json", "CONTENT_LENGTH": str(len(data)),
               "wsgi.input": io.BytesIO(data)}
        for k, v in (headers or {}).items():
            env["HTTP_" + k.upper().replace("-", "_")] = v
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])
            got["headers"] = {k.lower(): v for k, v in hdrs}

        out = apps.app(env, start)
        try:
            data = b"".join(out)
        finally:
            if hasattr(out, "close"):
                out.close()
        return got["status"], got["headers"], data


class FeedTest(unittest.TestCase):
    """1. When a value goes, on a clock of the test's own"""

    def run_polls(self, feed, values, start, end, step=wl.POLL_S):
        """Polls from start to end, values(t) the radio's value then: the times a post went (each one succeeds)"""
        sent, t = [], start
        while t < end - 1e-9:
            v = values(t)
            if feed.due(t, v):
                feed.done(t, v)
                sent.append(round(t, 2))
            t += step
        return sent

    def test_settle_and_heartbeat(self):
        feed = wl.Feed()
        sent = self.run_polls(feed, lambda t: "A", 0, 130)
        self.assertEqual(sent[0], wl.SETTLE_S, "the first post once the value settled")
        self.assertEqual(sent[1:], [round(wl.SETTLE_S + n * wl.HEARTBEAT_S, 2) for n in (1, 2)], "then once a minute")

    def test_knob_turning(self):
        feed = wl.Feed()
        self.run_polls(feed, lambda t: "A", 0, 10)
        # the knob turns for 20 s, a step at every poll: nothing goes; it stops at 30 s: the last value goes
        sent = self.run_polls(feed, lambda t: f"B{t}" if t < 30 else "C", 10, 40)
        self.assertEqual(sent, [round(30 + wl.SETTLE_S, 2)])

    def test_min_gap_and_rate(self):
        feed = wl.Feed()
        sent = self.run_polls(feed, lambda t: "A", 0, 2)
        self.assertEqual(len(sent), 1)
        # settled again 0.5 s later: it waits for MIN_GAP_S after the last post
        sent = self.run_polls(feed, lambda t: "B", 2, 10)
        self.assertEqual(sent, [round(sent[0], 2)])
        self.assertGreaterEqual(sent[0], wl.SETTLE_S + wl.MIN_GAP_S)
        # a slow turn, one step every 2 s for ten minutes: at most 60 / MIN_GAP_S a minute
        feed = wl.Feed()
        sent = self.run_polls(feed, lambda t: f"S{int(t // 2)}", 0, 600)
        per_min = max(sum(1 for s in sent if m <= s < m + 60) for m in range(0, 540, 10))
        self.assertLessEqual(per_min, 60 / wl.MIN_GAP_S)
        self.assertLessEqual(per_min, 30, "Wavelog's strictest example limit (its default for any endpoint)")
        self.assertGreaterEqual(per_min, 10)

    def test_waits_after_failures(self):
        feed = wl.Feed()
        t = wl.SETTLE_S
        self.assertTrue(feed.due(0, "A") or feed.due(t, "A"))
        waits = []
        for _ in range(7):
            feed.failed(t)
            t2 = t
            while not feed.due(t2, "A"):
                t2 = round(t2 + 0.5, 2)
            waits.append(round(t2 - t))
            t = t2
        self.assertEqual(waits, list(wl.BACKOFF_S) + [wl.BACKOFF_S[-1]] * 2)
        feed.done(t, "A")
        feed.failed(t + 70, wait=30)      # 429: Retry-After
        self.assertFalse(feed.due(t + 99, "A"))
        self.assertTrue(feed.due(t + 100, "A"))

    def test_again_after_a_save(self):
        feed = wl.Feed()
        self.run_polls(feed, lambda t: "A", 0, 5)
        self.assertFalse(feed.due(10, "A"))
        feed.again()
        self.assertTrue(feed.due(10, "A"), "the same value goes again after a Save")


class FieldsTest(Base):
    """2. The fields of api/radio and the modes"""

    def st(self, **kw):
        st = {"hz": 14074000, "tx_hz": 14074000, "mode": "USB-D", "log_mode": "FT8", "pwr": 5.0}
        st.update(kw)
        return st

    def test_fields(self):
        self.no_tailscale()
        conf = {"url": WAVELOG, "key": KEY, "live": "1"}
        self.assertEqual(wl.payload(conf, self.st(pwr=7.04)),
                         {"radio": "X6100", "frequency": 14074000, "mode": "FT8", "power": 7.0})
        body = wl.payload(dict(conf, radio_name="Shack X6100"), self.st(hz=14200000, tx_hz=14205000, mode="USB",
                                                                        log_mode="SSB"))
        self.assertEqual(body, {"radio": "Shack X6100", "frequency": 14205000, "frequency_rx": 14200000,
                                "mode": "USB", "power": 5.0})
        self.model("x6200")
        self.addCleanup(self.model, "x6100")
        self.assertEqual(wl.payload(conf, self.st())["radio"], "X6200")
        self.assertNotIn("cat_url", wl.payload(dict(conf, cat="1"), self.st()), "no token, no address")
        self.assertNotIn("cat_url", wl.payload(dict(conf, cat="0", cat_token=TOKEN), self.st()))
        self.assertNotIn("cat_url", wl.payload(dict(conf, live="0", cat="1", cat_token=TOKEN), self.st()))

    def test_modes(self):
        cases = [("USB-D", "FT8", "FT8"), ("USB-D", "FT4", "FT4"), ("USB-D", "PSK31", "PSK31"),
                 ("LSB-D", "OLIVIA", "OLIVIA"), ("USB-D", "CONTESTI", "CONTESTI"), ("USB-D", "", "USB-D"),
                 ("LSB-D", "", "LSB-D"), ("USB", "SSB", "USB"), ("LSB", "SSB", "LSB"), ("USB", "FT8", "FT8"),
                 ("CW", "CW", "CW"), ("CWR", "CW", "CW"), ("NFM", "FM", "FM"), ("WFM", "FM", "FM"),
                 ("AM", "AM", "AM"), ("SAM", "AM", "AM"), ("USB", "not a mode", "USB"), ("", "", "USB")]
        for radio, log, want in cases:
            with self.subTest(radio=radio, log=log):
                self.assertEqual(wl.wavelog_mode({"mode": radio, "log_mode": log}), want)

    def test_cat_url(self):
        conf = {"url": WAVELOG, "key": KEY, "live": "1", "cat": "1", "cat_token": TOKEN}
        self.tailscale()
        self.assertEqual(wl.cat_url(conf), f"https://{TS_NAME}/cat/{TOKEN}")
        self.tailscale(funnel="on")      # port 443 is tailscaled's: the address on the network
        lan = wl.lan_address()
        self.assertEqual(wl.cat_url(conf), f"https://{lan or socket.gethostname().lower() + '.local'}/cat/{TOKEN}")
        self.tailscale(state="stopped")
        self.assertFalse(wl.cat_url(conf).startswith(f"https://{TS_NAME}"))
        self.assertEqual(wl.cat_url(dict(conf, cat_url="https://192.168.1.10:8443")),
                         f"https://192.168.1.10:8443/cat/{TOKEN}")
        self.tailscale()
        with mock.patch.object(settings, "HTTPS_PORT", 0), mock.patch.object(settings, "HTTP_PORT", 8080):
            self.assertEqual(wl.cat_url(conf), f"http://{TS_NAME}:8080/cat/{TOKEN}")
        with mock.patch.object(settings, "HTTPS_PORT", 8443):
            self.assertEqual(wl.cat_url(conf), f"https://{TS_NAME}:8443/cat/{TOKEN}")

    def test_origins(self):
        for url, want in (("https://log.example.org/wavelog/index.php", "https://log.example.org"),
                          ("https://LOG.Example.org:443/", "https://log.example.org"),
                          ("http://192.168.1.5:8086/", "http://192.168.1.5:8086"), ("ftp://x.org", ""),
                          ("https://user@log.example.org", ""), ("null", ""), ("", ""),
                          ("http://[fd00::5]:80/", "http://[fd00::5]")):
            with self.subTest(url=url):
                self.assertEqual(wl.origin_of(url), want)
        self.assertEqual(wl.wavelog_origin({"url": WAVELOG + "/wavelog"}), WAVELOG)
        self.assertEqual(wl.wavelog_origin({"url": WAVELOG, "cat_origin": "http://wavelog.home.lan"}),
                         "http://wavelog.home.lan")


class PostTest(Base):
    """3. One post against the stand-in Wavelog"""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.wavelog = Wavelog()

    @classmethod
    def tearDownClass(cls):
        cls.wavelog.close()
        super().tearDownClass()

    def setUp(self):
        self.wavelog.posts.clear()
        self.wavelog.answer = (200, {"status": "success"})
        self.conf = {"url": self.wavelog.url, "key": KEY, "live": "1", "verify_tls": "1"}
        self.body = {"radio": "X6100", "frequency": 7074000, "mode": "FT8", "power": 5.0}

    def test_success(self):
        self.assertEqual(wl.post(self.conf, self.body), ("ok", ""))
        path, data, ctype = self.wavelog.posts[0]
        self.assertEqual(path, "/index.php/api/radio")
        self.assertEqual(data, dict(self.body, key=KEY))
        self.assertEqual(ctype, "application/json")

    def test_answers(self):
        cases = [((429, {"status": "failed", "reason": "Rate limit exceeded. Try again in 42 seconds.",
                         "retry_after": 42}), ("retry", "Wavelog's rate limit", 42)),
                 ((429, {"status": "failed", "retry_after": 9999}), ("retry", "Wavelog's rate limit", 300)),
                 ((403, {"status": "failed", "reason": "API key does not have write permissions"}),
                  ("refused", "API key does not have write permissions")),
                 ((401, {"status": "failed", "reason": "missing api key"}), ("refused", "missing api key")),
                 ((200, "<html>WiFi login</html>"), ("retry", "not Wavelog's answer (HTTP 200)", None)),
                 ((502, {"status": "failed"}), ("retry", "server HTTP 502", None)),
                 ((400, {"status": "failed", "reason": "invalid field(s): mode"}),
                  ("retry", "HTTP 400 invalid field(s): mode", wl.BACKOFF_S[-1]))]
        for answer, want in cases:
            with self.subTest(answer=answer):
                self.wavelog.answer = answer
                self.assertEqual(wl.post(self.conf, self.body), want)

    def test_no_server_and_v2_token(self):
        res = wl.post(dict(self.conf, url=f"http://127.0.0.1:{closed_port()}"), self.body)
        self.assertEqual(res[0], "retry")
        self.assertTrue(res[1].startswith("no connection"), res)
        res = wl.post(dict(self.conf, key="wl2_" + "a" * 40), self.body)
        self.assertEqual(res[0], "refused")
        self.assertIn("wl2_", res[1])
        self.assertEqual(self.wavelog.posts, [], "nothing sent with an API v2 token")


def wait_for(cond, timeout=10.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return False


class ThreadTest(Base):
    """4. The sender thread, its waits made short"""

    def setUp(self):
        self.wavelog = Wavelog()
        self.addCleanup(self.wavelog.close)
        self.gui = Gui(settings.CFG_SOCK)
        self.addCleanup(self.gui.close)
        for name, val in (("POLL_S", 0.02), ("SETTLE_S", 0.1), ("MIN_GAP_S", 0.2), ("HEARTBEAT_S", 30.0),
                          ("BACKOFF_S", (0.3, 0.6, 1.2, 2.4))):
            p = mock.patch.object(wl, name, val)
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(self.stop)
        self.no_tailscale()

    def stop(self):
        self.conf(live="0")
        t = wl.sender
        if t:
            t.join(5)

    def test_posts_and_stops(self):
        self.conf(url=self.wavelog.url)
        wl.start()
        self.assertTrue(wait_for(lambda: self.wavelog.posts), "nothing posted")
        self.assertEqual(self.wavelog.posts[0][1], {"key": KEY, "radio": "X6100", "frequency": 14074000,
                                                    "mode": "FT8", "power": 5.0})
        self.assertTrue(wait_for(lambda: wl.status["state"] == "ok"))
        self.assertIn("14.074.000 MHz FT8, 5 W, sent", wl.status["text"])
        self.gui.state.update(hz=7040000, tx_hz=7040000, mode="CW", log_mode="CW", app="")
        self.assertTrue(wait_for(lambda: self.wavelog.posts[-1][1]["frequency"] == 7040000), "the move not posted")
        self.assertEqual(self.wavelog.posts[-1][1]["mode"], "CW")
        time.sleep(0.5)
        self.assertLessEqual(len(self.wavelog.posts), 3, "posted while nothing changed")
        # off: the thread ends, nothing more goes
        st, hdr, data = self.req("POST", "/api/wavelog/live", body={"live": False})
        self.assertEqual(st, 200, data)
        self.assertTrue(wait_for(lambda: wl.sender is None), "the thread did not stop")
        n = len(self.wavelog.posts)
        self.gui.state.update(hz=3573000, tx_hz=3573000)
        time.sleep(0.5)
        self.assertEqual(len(self.wavelog.posts), n)
        self.assertEqual(json.loads(self.req("GET", "/api/wavelog")[2])["live"]["state"], "off")

    def test_no_network(self):
        self.conf(url=f"http://127.0.0.1:{closed_port()}")
        with mock.patch.object(wl, "post", wraps=wl.post) as post, \
                self.assertLogs(level="WARNING") as logs:
            wl.start()
            self.assertTrue(wait_for(lambda: wl.status["state"] == "retry"))
            first = wl.status["text"]
            time.sleep(2.5)
            tries = post.call_count
            self.conf(live="0")
            wl.sender.join(5)
        self.assertTrue(first.startswith("Not sent since ") and "no connection" in first, first)
        self.assertLessEqual(tries, 5, f"{tries} tries in 2.5 s: one a poll would be about 100")
        self.assertGreaterEqual(tries, 2, "it tries again")
        lines = [m for m in logs.output if "Wavelog live link" in m]
        self.assertEqual(len(lines), 1, lines)

    def test_no_gui(self):
        self.gui.close()
        self.conf(url=self.wavelog.url)
        wl.start()
        self.assertTrue(wait_for(lambda: wl.status["state"] == "noradio"))
        time.sleep(0.3)
        self.assertEqual(self.wavelog.posts, [])

    def test_refused_waits_for_a_save(self):
        self.wavelog.answer = (403, {"status": "failed", "reason": "API key does not have write permissions"})
        self.conf(url=self.wavelog.url)
        wl.start()
        self.assertTrue(wait_for(lambda: wl.status["state"] == "refused"))
        self.assertIn("write permissions", wl.status["text"])
        time.sleep(0.5)
        self.assertEqual(len(self.wavelog.posts), 1, "tried again before a Save")
        self.wavelog.answer = (200, {"status": "success"})
        st, hdr, data = self.req("POST", "/api/wavelog/live", body={"live": True, "radio_name": "Shack"})
        self.assertEqual(st, 200, data)
        self.assertTrue(wait_for(lambda: wl.status["state"] == "ok"), "not again after the Save")
        self.assertEqual(self.wavelog.posts[-1][1]["radio"], "Shack")


class SettingsTest(Base):
    """5. POST /api/wavelog/live and what GET /api/wavelog says"""

    def setUp(self):
        self.no_tailscale()
        p = mock.patch.object(wl, "start")      # no thread here
        p.start()
        self.addCleanup(p.stop)

    def save(self, **body):
        return self.req("POST", "/api/wavelog/live", body=body)

    def stored(self):
        return apps.wavelog_read()

    def test_needs_the_upload_settings(self):
        self.conf(key=None, live="0")
        st, _, data = self.save(live=True)
        self.assertEqual(st, 400)
        self.assertIn(b"API key", data)
        self.conf(live="0", key="wl2_" + "b" * 40)
        self.assertEqual(self.save(live=True)[0], 400)
        self.conf(live="0")
        st, _, data = self.save(live=False, cat=True)
        self.assertEqual(st, 400)
        self.assertIn(b"Show the radio in Wavelog", data)

    def test_token(self):
        self.conf(live="0")
        self.assertEqual(self.save(live=True)[0], 200)
        self.assertEqual(self.stored()["live"], "1")
        self.assertNotIn("cat_token", self.stored())
        self.assertEqual(self.save(live=True, cat=True)[0], 200)
        token = self.stored()["cat_token"]
        self.assertRegex(token, r"^[A-Za-z0-9_-]{24}$")
        self.save(live=True, cat=True, radio_name="Shack")
        self.assertEqual(self.stored()["cat_token"], token, "a Save keeps the address")
        self.save(live=True, cat=True, new_token=True)
        self.assertNotEqual(self.stored()["cat_token"], token, "New address")
        self.save(live=True, cat=False)
        self.assertNotIn("cat_token", self.stored())
        self.save(live=True, cat=True)
        self.assertIn("cat_token", self.stored())
        st, _, data = self.req("POST", "/api/wavelog", body={"remove": True})
        self.assertEqual(st, 200, data)
        conf = self.stored()
        self.assertEqual((conf.get("live"), conf.get("cat"), conf.get("cat_token")), ("0", "0", None))

    def test_checks(self):
        self.conf()
        for body in ({"radio_name": "x" * 33}, {"radio_name": "bad\nname"}, {"cat_url": "https://x6100/cat"},
                     {"cat_url": "x6100.local"}, {"cat_url": "https://a b"}, {"cat_origin": "log.example.org"},
                     {"cat_origin": "ftp://log.example.org"}):
            with self.subTest(body=body):
                self.assertEqual(self.save(live=True, **body)[0], 400)
        st, _, data = self.save(live=True, cat=True, radio_name="Shack 1", cat_url="https://192.168.1.10:8443/",
                                cat_origin="https://Wavelog.Home.lan/index.php")
        self.assertEqual(st, 200, data)
        conf = self.stored()
        self.assertEqual((conf["radio_name"], conf["cat_url"], conf["cat_origin"]),
                         ("Shack 1", "https://192.168.1.10:8443", "https://wavelog.home.lan"))

    def test_page_never_has_the_token(self):
        self.conf(cat="1", cat_token=TOKEN)
        st, _, data = self.req("GET", "/api/wavelog")
        self.assertEqual(st, 200)
        self.assertNotIn(TOKEN.encode(), data)
        live = json.loads(data)["live"]
        self.assertEqual((live["on"], live["cat"], live["radio_name_default"], live["cat_origin_default"]),
                         (True, True, "X6100", WAVELOG))
        self.assertTrue(live["cat_url_auto"].startswith("https://"))
        st, _, data = self.req("GET", "/wavelog")
        self.assertEqual(st, 200)
        self.assertNotIn(TOKEN.encode(), data)


class CatTest(Base):
    """6-9. /cat/<token>/<Hz>/<mode>"""

    def setUp(self):
        self.gui = Gui(settings.CFG_SOCK)
        self.addCleanup(self.gui.close)
        self.conf(cat="1", cat_token=TOKEN)
        self.model("x6100")

    def cat(self, freq="14074000", mode="usb", token=TOKEN, origin=WAVELOG, method="GET", headers=None, host=HOST):
        hdr = dict(headers or {})
        if origin is not None:
            hdr["Origin"] = origin
        return self.req(method, f"/cat/{token}/{freq}/{mode}", hdr, host=host)

    def assertRefused(self, res, code):
        st, hdr, data = res
        self.assertEqual(st, code, data)
        self.assertNotIn("access-control-allow-origin", hdr)
        self.assertNotIn("access-control-allow-private-network", hdr)
        self.assertEqual(self.gui.tunes(), [], "the GUI was asked to tune")

    def test_tunes(self):
        st, hdr, data = self.cat()
        self.assertEqual(st, 200, data)
        self.assertEqual(data, b"OK 14074000 USB\n")
        self.assertEqual(hdr["access-control-allow-origin"], WAVELOG)
        self.assertEqual(hdr["vary"], "Origin")
        self.assertEqual(self.gui.tunes(), [{"cmd": "tune", "hz": 14074000, "mode": "USB"}])
        page = json.loads(self.req("GET", "/api/wavelog")[2])["live"]
        self.assertTrue(page["tuned"].startswith("14.0740 MHz USB"), page["tuned"])

    def test_token_and_switch(self):
        self.assertRefused(self.cat(token="Tk3n-abcdefghijklmnopqX"), 404)
        self.assertRefused(self.cat(token="short"), 404)
        self.conf(cat="0", cat_token=TOKEN)
        self.assertRefused(self.cat(), 404)
        self.conf(live="0", cat="1", cat_token=TOKEN)
        self.assertRefused(self.cat(), 404)
        self.conf(cat="1", cat_token=TOKEN, key=None)
        self.assertRefused(self.cat(), 404)
        self.conf(cat="1")
        self.assertRefused(self.cat(token=""), 404)

    def test_host_and_funnel(self):
        self.assertRefused(self.cat(host="evil.example"), 403)
        self.assertRefused(self.cat(headers={"Tailscale-Funnel-Request": "?1"}), 403)
        self.assertEqual(self.cat(host=TS_NAME)[0], 200)

    def test_typed_host(self):
        """The host of an address typed on the page (a router's name for the radio), for this route alone"""
        self.conf(cat="1", cat_token=TOKEN, cat_url="https://radio.home.example:8443")
        self.assertRefused(self.cat(host="other.home.example"), 403)
        self.assertRefused(self.cat(host="radio.home.example", headers={"Tailscale-Funnel-Request": "?1"}), 403)
        self.assertEqual(self.req("POST", "/api/wavelog/live", {"Origin": "https://radio.home.example:8443"},
                                  body={"live": True}, host="radio.home.example:8443")[0], 403)
        self.assertEqual(self.cat(host="radio.home.example:8443")[0], 200)
        self.assertEqual(self.cat(host="Radio.Home.Example.")[0], 200)

    def test_origin(self):
        for origin in (None, "", "null", "https://evil.example", "http://log.example.org",
                       "https://log.example.org:8443", "https://log.example.org/",
                       "https://log.example.org.evil.example", "https://x@log.example.org"):
            with self.subTest(origin=origin):
                self.assertRefused(self.cat(origin=origin), 403)
        self.conf(cat="1", cat_token=TOKEN, url="https://log.example.org:443/wavelog/")
        self.assertEqual(self.cat()[0], 200)
        self.conf(cat="1", cat_token=TOKEN, cat_origin="http://wavelog.home.lan:8086")
        self.gui.cmds.clear()
        self.assertRefused(self.cat(), 403)
        st, hdr, _ = self.cat(origin="http://wavelog.home.lan:8086")
        self.assertEqual((st, hdr["access-control-allow-origin"]), (200, "http://wavelog.home.lan:8086"))

    def test_preflight(self):
        pna = {"Access-Control-Request-Method": "GET", "Access-Control-Request-Private-Network": "true"}
        st, hdr, data = self.cat(method="OPTIONS", headers=pna)
        self.assertEqual(st, 204, data)
        self.assertEqual((hdr["access-control-allow-origin"], hdr["access-control-allow-methods"],
                          hdr["access-control-allow-private-network"]), (WAVELOG, "GET", "true"))
        st, hdr, _ = self.cat(method="OPTIONS", headers={"Access-Control-Request-Method": "GET"})
        self.assertEqual(st, 204)
        self.assertNotIn("access-control-allow-private-network", hdr, "only when the browser asks")
        self.assertRefused(self.cat(method="OPTIONS", headers=pna, origin="https://evil.example"), 403)
        self.assertRefused(self.cat(method="OPTIONS", headers=pna, origin=None), 403)
        self.assertRefused(self.cat(method="OPTIONS", headers=pna, token="Tk3n-abcdefghijklmnopqX"), 404)
        self.assertRefused(self.cat(method="OPTIONS", headers=dict(pna, **{"Access-Control-Request-Method": "POST"})),
                           403)
        self.assertRefused(self.cat(method="OPTIONS", headers=dict(pna, **{"Access-Control-Request-Headers": "x-a"})),
                           403)
        self.assertEqual(self.gui.tunes(), [])

    def test_head_and_other_methods(self):
        st, hdr, _ = self.cat(method="HEAD")
        self.assertEqual(st, 405)
        self.assertEqual(self.gui.tunes(), [])
        st, _, _ = self.req("POST", f"/cat/{TOKEN}/14074000/usb", {"Origin": WAVELOG})
        self.assertIn(st, (403, 405))
        self.assertEqual(self.gui.tunes(), [])

    def test_frequency(self):
        for freq in ("99999", "55000001", "60000000", "69999999", "500000001", "1e7", "-7074000", "14074000.",
                     "0x1000", urllib.parse.quote("١٤٠٧٤٠٠٠"), "14074000" + "0" * 3):
            with self.subTest(freq=freq):
                st, hdr, _ = self.cat(freq=freq)
                self.assertEqual(st, 400)
                self.assertEqual(hdr["access-control-allow-origin"], WAVELOG, "Wavelog's page reads the refusal")
        self.assertEqual(self.gui.tunes(), [])
        for freq, hz in (("100000", 100000), ("55000000", 55000000), ("70000000", 70000000), ("500000000", 500000000),
                         ("1000999.9999999999", 1001000), ("14074000.5", 14074001), ("7074499.49", 7074499)):
            with self.subTest(freq=freq):
                self.gui.cmds.clear()
                self.assertEqual(self.cat(freq=freq)[0], 200)
                self.assertEqual(self.gui.tunes()[0]["hz"], hz)
        self.model("x6200")
        self.addCleanup(self.model, "x6100")
        self.assertEqual(self.cat(freq="60000000")[0], 200)
        self.assertEqual(self.cat(freq="1000000000")[0], 200)
        self.assertEqual(self.cat(freq="1000000001")[0], 400)

    def test_modes(self):
        want = {"lsb": "LSB", "usb": "USB", "cw": "CW", "am": "AM", "fm": "NFM", "rtty": "USB-D", "pkt": "USB-D",
                "dig": "USB-D", "pktusb": "USB-D", "pktlsb": "LSB-D", "pktfm": "NFM", "USB": "USB", "PktLsb": "LSB-D"}
        for mode, ours in want.items():
            with self.subTest(mode=mode):
                self.gui.cmds.clear()
                self.assertEqual(self.cat(mode=mode)[0], 200)
                self.assertEqual(self.gui.tunes(), [{"cmd": "tune", "hz": 14074000, "mode": ours}])
        self.gui.cmds.clear()
        for mode in ("psk", "ft8", "cwr", "usb-d", "wfm", "sam", "x"):
            with self.subTest(mode=mode):
                self.assertEqual(self.cat(mode=mode)[0], 400)
        self.assertEqual(self.gui.tunes(), [])

    def test_no_tx(self):
        for f, m in (("7074000", "cw"), ("14200000", "usb"), ("3573000", "pktusb"), ("145500000", "fm")):
            self.cat(freq=f, mode=m)
        self.assertTrue(self.gui.tunes())
        for c in self.gui.tunes():
            self.assertEqual(set(c), {"cmd", "hz", "mode"})
            self.assertEqual(c["cmd"], "tune")
        self.gui.transmitting = True
        st, hdr, data = self.cat(freq="7030000", mode="cw")
        self.assertEqual((st, data), (409, b"The radio is transmitting\n"))
        self.assertEqual(hdr["access-control-allow-origin"], WAVELOG)
        self.gui.close()
        self.assertEqual(self.cat()[0], 503)

    def test_other_routes_as_they_were(self):
        pna = {"Origin": WAVELOG, "Access-Control-Request-Method": "GET",
               "Access-Control-Request-Private-Network": "true"}
        for path in ("/api/wavelog", "/wavelog", "/api/remote", "/", "/cat/" + TOKEN):
            with self.subTest(path=path):
                st, hdr, _ = self.req("GET", path, {"Origin": WAVELOG})
                self.assertNotIn("access-control-allow-origin", hdr)
                st, hdr, _ = self.req("OPTIONS", path, pna)
                self.assertNotIn("access-control-allow-origin", hdr)
                self.assertNotIn("access-control-allow-private-network", hdr)
        for headers in ({"Origin": WAVELOG}, {"Sec-Fetch-Site": "cross-site"}):
            with self.subTest(headers=headers):
                st, _, _ = self.req("POST", "/api/wavelog/live", headers, body={"live": False})
                self.assertEqual(st, 403)
        st, _, _ = self.req("POST", "/api/wavelog/live", body={"live": True}, host="evil.example")
        self.assertEqual(st, 403)
        self.assertEqual(apps.wavelog_read()["live"], "1")


if __name__ == "__main__":
    logging.basicConfig(level=logging.ERROR)
    unittest.main()
