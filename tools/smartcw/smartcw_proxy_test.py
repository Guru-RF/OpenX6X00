#!/usr/bin/env python3
"""SmartCW's page on its own port (webserver smartcw.py) on a PC, against a stand-in SmartCW on 127.0.0.1. See the
Makefile:

    python3 -I -B tools/smartcw/smartcw_proxy_test.py [-v]

The stand-in records every byte the proxy sends it, for a moment after the request too, and answers as SmartCW does
(Connection: close, Access-Control-Allow-Origin: *); its /api/events is a live stream that sends an event when the
test says so. The proxy's SmartCW interface is the test's (smartcw0, or none), and the socket's hold on it a stand-in
that notes the name; one case holds it to lo by the real SO_BINDTODEVICE. The radio's names are the test's:
192.168.1.10 and x6100.local.

1. Reads go to SmartCW by any name, a change (POST) only by one of the radio's names, with no Sec-Fetch-Site
   cross-site and an Origin of the page's own name and port (the configurator's own, on port 80, passes). Through
   Tailscale Funnel nothing goes.
2. One request a connection: SmartCW gets the head with Connection: close and the declared body, nothing after it (a
   second request sent behind the first never reaches it), and no cookie.
3. A head the proxy can't read the way SmartCW would (bare LF, two lengths, a folded line, a chunked body, too long)
   is refused before SmartCW is asked anything.
4. SmartCW's answer comes back without its permissions for other sites, with Connection: close; its live events as
   they come, and the browser's close ends SmartCW's connection.
5. SmartCW not plugged in: 503 with what to do; plugged in and not answering: 502.
6. The configurator's menu links to it, by the name the page was opened by, in a new tab, only while it runs and
   SmartCW is plugged in. This one needs bottle (BOTTLE=<a folder with bottle.py>, as tools/secrets), and is skipped
   without it.

WEBSERVER=<a webserver/src> tests another web server.
"""
import os
import queue
import socket
import sys
import tempfile
import threading
import time
import types
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + \
    [os.environ.get("WEBSERVER") or os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
from x6100_webserver import smartcw  # noqa: E402

NAMES = ("192.168.1.10", "x6100.local")
QUIET = 0.3     # s the stand-in keeps reading after the request: anything the proxy sends after it is recorded


class FakeSmartCW:
    """A stand-in SmartCW web server on 127.0.0.1: each connection's bytes go to self.got, a list per connection"""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.got = queue.Queue()        # the bytes of each connection, once it is over
        self.accepted = 0
        self.event = threading.Event()  # /api/events: send the second event
        self.stream_closed = threading.Event()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            conn, _ = self.sock.accept()
            self.accepted += 1
            threading.Thread(target=self.one, args=(conn,), daemon=True).start()

    def one(self, conn):
        data = b""
        conn.settimeout(5)
        try:
            while b"\r\n\r\n" not in data:
                more = conn.recv(65536)
                if not more:
                    break
                data += more
            head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
            length = 0
            for line in head.split("\r\n")[1:]:
                name, _, value = line.partition(":")
                if name.lower() == "content-length":
                    length = int(value)
            while len(data) < len(head) + 4 + length:
                more = conn.recv(65536)
                if not more:
                    break
                data += more
            if head.startswith("GET /api/events "):
                self.stream(conn)
            else:
                body = b'{"ok":true}'
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                             b"Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"
                             % len(body) + body)
            conn.settimeout(QUIET)
            try:
                while True:
                    more = conn.recv(65536)
                    if not more:
                        break
                    data += more
            except OSError:
                pass
        except OSError:
            pass
        conn.close()
        self.got.put(data)

    def stream(self, conn):
        """SmartCW's live events: the head and a first event, a second when the test says, then until the proxy
        ends the connection"""
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
                     b"Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\nretry: 1500\n\n"
                     b'data: {"e":"k","d":1}\n\n')
        self.event.wait(10)
        conn.sendall(b'data: {"e":"k","d":0}\n\n')
        conn.settimeout(10)
        try:
            while conn.recv(65536):
                pass
            self.stream_closed.set()
        except OSError:
            pass

    def next(self, secs=5):
        return self.got.get(timeout=secs)


fake = None


def setUpModule():
    global fake
    fake = FakeSmartCW()


class ProxyCase(unittest.TestCase):
    iface = "smartcw0"

    def setUp(self):
        self.bound = []
        while not fake.got.empty():
            fake.got.get()
        self.proxy = smartcw.Proxy(lambda host: host in NAMES, target=("127.0.0.1", fake.port),
                                   iface=lambda: self.iface, bind=lambda s, i: self.bound.append(i))
        self.server = smartcw.make_server("127.0.0.1", 0, self.proxy)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def exchange(self, data, pieces=()):
        """data to the proxy (and pieces after it, a moment apart), its whole answer back"""
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as c:
            c.sendall(data)
            for p in pieces:
                time.sleep(0.1)
                c.sendall(p)
            out = b""
            while True:
                more = c.recv(65536)
                if not more:
                    return out
                out += more

    def forwarded(self, data, **kw):
        """data through the proxy: (its answer, what SmartCW got)"""
        accepted = fake.accepted
        answer = self.exchange(data, **kw)
        self.assertEqual(fake.accepted, accepted + 1, "SmartCW was not asked")
        return answer, fake.next()

    def refused(self, data, status):
        """data refused with that status, SmartCW never asked"""
        accepted = fake.accepted
        answer = self.exchange(data)
        self.assertTrue(answer.startswith(b"HTTP/1.1 %d " % status), answer[:200])
        time.sleep(0.05)
        self.assertEqual(fake.accepted, accepted, "SmartCW was asked")
        return answer


def post(path, body, host="192.168.1.10:8073", extra=""):
    return (f"POST {path} HTTP/1.1\r\nHost: {host}\r\nContent-Type: application/x-www-form-urlencoded\r\n{extra}"
            f"Content-Length: {len(body)}\r\n\r\n").encode() + body


class Rules(ProxyCase):
    """1. The configurator's rules"""

    def test_read_by_any_name(self):
        answer, got = self.forwarded(b"GET /api/state HTTP/1.1\r\nHost: rebind.example:8073\r\n\r\n")
        self.assertTrue(answer.startswith(b"HTTP/1.1 200 OK"))
        self.assertTrue(got.startswith(b"GET /api/state HTTP/1.1\r\n"))

    def test_post_by_own_names(self):
        for host in ("192.168.1.10:8073", "X6100.local:8073", "x6100.local."):
            answer, got = self.forwarded(post("/api/send", b"text=cq", host=host))
            self.assertTrue(answer.startswith(b"HTTP/1.1 200 OK"), host)
            self.assertTrue(got.endswith(b"\r\n\r\ntext=cq"), host)

    def test_post_by_other_name(self):
        for host in ("rebind.example:8073", "", "[::1"):
            answer = self.refused(post("/api/bootloader", b"", host=host), 403)
            self.assertIn(b"not one of the radio's own names", answer)
        self.refused(b"POST /api/send HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 403)   # no Host at all

    def test_cross_site(self):
        self.refused(post("/api/send", b"text=cq", extra="Sec-Fetch-Site: cross-site\r\n"), 403)
        answer, _ = self.forwarded(post("/api/send", b"text=cq", extra="Sec-Fetch-Site: same-origin\r\n"))
        self.assertTrue(answer.startswith(b"HTTP/1.1 200 OK"))

    def test_origin(self):
        for origin in ("http://192.168.1.10:8073", "http://192.168.1.10"):   # its own page, the configurator's
            answer, _ = self.forwarded(post("/api/tune", b"ms=1000", extra=f"Origin: {origin}\r\n"))
            self.assertTrue(answer.startswith(b"HTTP/1.1 200 OK"), origin)
        for origin in ("http://rebind.example", "http://192.168.1.10:8080", "null", "http://192.168.1.10:x",
                       "http://[::1"):
            self.refused(post("/api/tune", b"ms=1000", extra=f"Origin: {origin}\r\n"), 403)

    def test_funnel(self):
        for req in (b"GET / HTTP/1.1\r\nHost: x6100.tail.ts.net\r\nTailscale-Funnel-Request: ?1\r\n\r\n",
                    post("/api/send", b"text=cq", extra="tailscale-funnel-request: ?1\r\n")):
            answer = self.refused(req, 403)
            self.assertIn(b"not on the internet", answer)


class OneRequest(ProxyCase):
    """2. One request a connection"""

    def test_head_for_smartcw(self):
        answer, got = self.forwarded(b"GET / HTTP/1.1\r\nHost: 192.168.1.10:8073\r\nConnection: keep-alive\r\n"
                                     b"Keep-Alive: timeout=5\r\nCookie: pat=secret\r\nAccept: */*\r\n\r\n")
        self.assertEqual(got, b"GET / HTTP/1.1\r\nHost: 192.168.1.10:8073\r\nAccept: */*\r\nConnection: close\r\n\r\n")

    def test_pipelined_request_dropped(self):
        second = b"POST /api/bootloader HTTP/1.1\r\nHost: rebind.example\r\nContent-Length: 0\r\n\r\n"
        answer, got = self.forwarded(b"GET /api/state HTTP/1.1\r\nHost: 192.168.1.10:8073\r\n\r\n" + second)
        self.assertNotIn(b"bootloader", got)
        self.assertEqual(answer.count(b"HTTP/1.1 "), 1)

    def test_body_as_declared(self):
        second = b"POST /api/bootloader HTTP/1.1\r\nHost: rebind.example\r\nContent-Length: 0\r\n\r\n"
        answer, got = self.forwarded(post("/api/send", b"text=cq+cq") + second)
        self.assertTrue(got.endswith(b"Connection: close\r\n\r\ntext=cq+cq"), got)

    def test_body_in_pieces(self):
        req = post("/api/send", b"text=cq+de+on6ure")
        answer, got = self.forwarded(req[:-10], pieces=(req[-10:-4], req[-4:] + b"GET / HTTP/1.1\r\n\r\n"))
        self.assertTrue(got.endswith(b"\r\n\r\ntext=cq+de+on6ure"), got)

    def test_expect(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as c:
            c.sendall(post("/api/send", b"text=cq", extra="Expect: 100-continue\r\n")[:-7])
            self.assertEqual(c.recv(100), b"HTTP/1.1 100 Continue\r\n\r\n")
            c.sendall(b"text=cq")
            self.assertTrue(c.recv(100).startswith(b"HTTP/1.1 200 OK"))
        got = fake.next()
        self.assertNotIn(b"Expect", got)
        self.assertTrue(got.endswith(b"\r\n\r\ntext=cq"))


class BadHeads(ProxyCase):
    """3. Heads refused before SmartCW is asked"""

    def test_bad(self):
        for req in (b"GET / HTTP/1.1\r\nHost: a\nX-Other: b\r\n\r\n",
                    b"GET / HTTP/1.1\r\nHost: a\rX: b\r\n\r\n",
                    post("/api/send", b"text=cq", extra="Content-Length: 7\r\n"),
                    post("/api/send", b"text=cq", extra="Host: 192.168.1.10\r\n"),
                    b"POST /api/send HTTP/1.1\r\nHost: 192.168.1.10\r\nContent-Length: +7\r\n\r\ntext=cq",
                    b"GET / HTTP/1.1\r\nHost: 192.168.1.10\r\n folded: on\r\n\r\n",
                    b"GET / HTTP/1.1\r\nHost : 192.168.1.10\r\n\r\n",
                    b"GET http://rebind.example/ HTTP/1.1\r\nHost: 192.168.1.10\r\n\r\n",
                    b"get / HTTP/1.1\r\n\r\n",
                    b"GET / HTTP/2\r\n\r\n"):
            self.refused(req, 400)

    def test_chunked(self):
        self.refused(b"POST /config.txt HTTP/1.1\r\nHost: 192.168.1.10\r\nTransfer-Encoding: chunked\r\n\r\n"
                     b"5\r\nhello\r\n0\r\n\r\n", 501)

    def test_too_long(self):
        self.refused(b"GET / HTTP/1.1\r\nHost: 192.168.1.10\r\nX: " + b"a" * smartcw.HEAD_MAX + b"\r\n\r\n", 431)
        self.refused(post("/config.txt", b"", extra=f"Content-Length: {smartcw.BODY_MAX + 1}\r\n")
                     .replace(b"Content-Length: 0\r\n", b""), 413)


class Answers(ProxyCase):
    """4. SmartCW's answers"""

    def test_answer_head(self):
        answer, _ = self.forwarded(b"GET /api/state HTTP/1.1\r\nHost: 192.168.1.10:8073\r\n\r\n")
        head, _, body = answer.partition(b"\r\n\r\n")
        self.assertNotIn(b"Access-Control", head)
        self.assertEqual(head.count(b"Connection:"), 1)
        self.assertIn(b"\r\nConnection: close", head)
        self.assertEqual(body, b'{"ok":true}')

    def test_events_as_they_come(self):
        fake.event.clear()
        fake.stream_closed.clear()
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as c:
            c.sendall(b"GET /api/events HTTP/1.1\r\nHost: 192.168.1.10:8073\r\nAccept: text/event-stream\r\n\r\n")
            got = b""
            while b'"d":1' not in got:
                got += c.recv(65536)
            head = got.split(b"\r\n\r\n")[0]
            self.assertIn(b"text/event-stream", head)
            self.assertNotIn(b"keep-alive", head)
            self.assertNotIn(b"Access-Control", head)
            fake.event.set()
            while b'"d":0' not in got:
                got += c.recv(65536)
            self.assertFalse(fake.stream_closed.is_set())
        self.assertTrue(fake.stream_closed.wait(3), "SmartCW's connection still open after the browser's close")


class NotThere(ProxyCase):
    """5. SmartCW not plugged in, or not answering"""

    def test_not_plugged_in(self):
        self.iface = None
        answer = self.refused(b"GET / HTTP/1.1\r\nHost: 192.168.1.10:8073\r\n\r\n", 503)
        self.assertIn(b"SmartCW is not plugged in", answer)
        self.assertIn(b"USB port", answer)

    def test_not_answering(self):
        closed = socket.socket()
        closed.bind(("127.0.0.1", 0))
        self.proxy.target = closed.getsockname()   # bound, not listening: refused
        answer = self.exchange(b"GET / HTTP/1.1\r\nHost: 192.168.1.10:8073\r\n\r\n")
        closed.close()
        self.assertTrue(answer.startswith(b"HTTP/1.1 502 "))
        self.assertIn(b"smartcw0", answer)

    def test_held_to_its_interface(self):
        self.forwarded(b"GET / HTTP/1.1\r\nHost: 192.168.1.10:8073\r\n\r\n")
        self.assertEqual(self.bound, ["smartcw0"])

    def test_real_bind(self):
        """The real SO_BINDTODEVICE: held to lo it reaches the stand-in, held to an interface that is not there
        it does not"""
        probe = socket.socket()
        try:
            smartcw.bind_to_device(probe, "lo")
        except PermissionError:
            self.skipTest("SO_BINDTODEVICE needs CAP_NET_RAW on this kernel")
        finally:
            probe.close()
        self.proxy.bind = smartcw.bind_to_device
        self.proxy.iface = lambda: "lo"
        answer, _ = self.forwarded(b"GET / HTTP/1.1\r\nHost: 192.168.1.10:8073\r\n\r\n")
        self.assertTrue(answer.startswith(b"HTTP/1.1 200 OK"))
        self.proxy.iface = lambda: "smartcw9"
        self.refused(b"GET / HTTP/1.1\r\nHost: 192.168.1.10:8073\r\n\r\n", 502)

    def test_iface(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(smartcw.smartcw_iface(d))
            for n in ("eth0", "lo", "smartcw1", "smartcw0", "wlan0"):
                os.mkdir(os.path.join(d, n))
            self.assertEqual(smartcw.smartcw_iface(d), "smartcw0")
            self.assertIsNone(smartcw.smartcw_iface(os.path.join(d, "none")))


def configurator():
    """The configurator's app (apps.py), None without bottle"""
    try:
        import cgi  # noqa: F401
    except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for forms only
        sys.modules["cgi"] = types.ModuleType("cgi")
    try:
        import bottle  # noqa: F401
    except ImportError:
        return None
    from x6100_webserver import apps
    return apps


class MenuLink(unittest.TestCase):
    """6. The configurator's menu"""

    def setUp(self):
        self.apps = configurator()
        if not self.apps:
            self.skipTest("no bottle: give BOTTLE=<a folder with bottle.py>")
        from x6100_webserver import settings
        self.settings = settings
        self.port, self.iface = settings.SMARTCW_PORT, smartcw.smartcw_iface

    def tearDown(self):
        self.settings.SMARTCW_PORT, smartcw.smartcw_iface = self.port, self.iface

    def menu(self, host):
        env = {}
        setup_testing_defaults(env)
        env.update(HTTP_HOST=host, PATH_INFO="/time")
        status = []
        page = b"".join(self.apps.app(env, lambda s, h: status.append(s))).decode()
        self.assertTrue(status[0].startswith("200"), status)
        return [line.strip() for line in page.splitlines() if ">SmartCW</a>" in line]

    def test_link(self):
        for port, iface in ((0, "smartcw0"), (8073, None)):
            self.settings.SMARTCW_PORT = port
            smartcw.smartcw_iface = lambda i=iface: i
            self.assertEqual(self.menu("192.168.1.10"), [], (port, iface))
        self.settings.SMARTCW_PORT = 8073
        smartcw.smartcw_iface = lambda: "smartcw0"
        for host, href in (("192.168.1.10", "http://192.168.1.10:8073/"), ("X6100.local:80", "http://x6100.local:8073/"),
                           ("x6100.local:443", "http://x6100.local:8073/"), ("[fe80::1]:80", "http://[fe80::1]:8073/")):
            links = self.menu(host)
            self.assertEqual(len(links), 1, host)
            self.assertIn(f'href="{href}"', links[0])
            self.assertIn('target="_blank" rel="noopener"', links[0])


if __name__ == "__main__":
    unittest.main()
