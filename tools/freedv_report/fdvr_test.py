#!/usr/bin/env python3
"""rootfs/usr/sbin/x6100-freedv-reporter against a fake FreeDV Reporter server in this file (Engine.IO 4 and Socket.IO
5 over a websocket on 127.0.0.1, plain and TLS). Nothing here goes to qso.freedv.org.

  python3 tools/freedv_report/fdvr_test.py            (X6100_FDVR=path: another copy of the helper)

The cases: the upgrade request and the auth, nothing before connection_successful and the state after it, each event
and its timing (one freq_change a second at most), the pings, a QSY request (a line for the GUI, nothing sent), a
dropped line and the retry with the state again, the backoff, no internet at all (refused, no answer, no name: one
line, quiet retries, the end of stdin or SIGTERM ends it at once), TLS with and without a trusted certificate, the
GUI's socket pair as stdin and stdout, and the goodbye at the end.
"""
import base64
import hashlib
import json
import os
import queue
import select
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.environ.get("X6100_FDVR") or os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin",
                                                      "x6100-freedv-reporter")
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PATH = "/socket.io/?EIO=4&transport=websocket"
OPEN = '0{"sid":"eio1","upgrades":[],"pingInterval":25000,"pingTimeout":20000,"maxPayload":1000000}'
CALL, GRID, VERSION = "N0CALL", "FN31pr", "OpenX6X00 0.3.12"

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("  FAIL", what)
    return ok


# The fake server

class Conn:
    """The server's side of one websocket"""

    def __init__(self, sock, request, headers, rest):
        self.sock, self.request, self.headers, self.buf = sock, request, headers, bytearray(rest)
        self.closed_with = None      # the close frame's code, once one came
        self.eof = False

    def frame(self, op, payload, fin=True):
        n = len(payload)
        head = bytes([(0x80 if fin else 0) | op])
        head += bytes([n]) if n < 126 else bytes([126]) + n.to_bytes(2, "big") if n < 65536 else \
            bytes([127]) + n.to_bytes(8, "big")
        self.sock.sendall(head + payload)

    def send(self, text):
        self.frame(0x1, text.encode())

    def recv(self, timeout=1.0):
        """The next text message, ("pong", payload) for a pong, None: nothing in time or the end"""
        deadline = time.monotonic() + timeout
        while True:
            m = self.parse()
            if m is not None:
                return m
            if self.eof:
                return None
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.sock], [], [], left)
            if not r:
                return None
            try:
                chunk = self.sock.recv(65536)
            except (ssl.SSLWantReadError, BlockingIOError):
                continue
            except OSError:
                chunk = b""
            if not chunk:
                self.eof = True
            self.buf += chunk

    def parse(self):
        b = self.buf
        if len(b) < 2:
            return None
        op, masked, n, i = b[0] & 0x0F, b[1] & 0x80, b[1] & 0x7F, 2
        if n == 126:
            if len(b) < 4:
                return None
            n, i = int.from_bytes(b[2:4], "big"), 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, i = int.from_bytes(b[2:10], "big"), 10
        if len(b) < i + 4 + n:
            return None
        check(masked, "a client frame is masked")
        mask = b[i:i + 4]
        payload = bytes(c ^ mask[k % 4] for k, c in enumerate(b[i + 4:i + 4 + n]))
        del self.buf[:i + 4 + n]
        if op == 0x8:
            self.closed_with = int.from_bytes(payload[:2], "big") if len(payload) >= 2 else 0
            self.eof = True
            return None
        if op == 0xA:
            return ("pong", payload)
        return payload.decode()

    def quiet(self, secs):
        """Nothing came for secs"""
        m = self.recv(secs)
        return m is None and not self.eof, m

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Server:
    def __init__(self, tls=None):
        self.lsock = socket.socket()
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind(("127.0.0.1", 0))
        self.lsock.listen(8)
        self.port = self.lsock.getsockname()[1]
        self.tls = tls

    def url(self, scheme="ws", host="127.0.0.1"):
        return f"{scheme}://{host}:{self.port}{PATH}"

    def accept(self, timeout=5.0, answer=True):
        """The next connection, upgraded; None when none came in time"""
        r, _, _ = select.select([self.lsock], [], [], timeout)
        if not r:
            return None
        sock, _ = self.lsock.accept()
        sock.settimeout(5)
        if self.tls:
            try:
                sock = self.tls.wrap_socket(sock, server_side=True)
            except (ssl.SSLError, OSError):
                sock.close()
                return "tls failed"
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = sock.recv(4096)
            if not chunk:
                sock.close()
                return None
            head += chunk
        head, rest = head.split(b"\r\n\r\n", 1)
        lines = head.decode().split("\r\n")
        headers = {k.strip().lower(): v.strip() for k, _, v in (ln.partition(":") for ln in lines[1:])}
        if answer:
            accept = base64.b64encode(hashlib.sha1((headers.get("sec-websocket-key", "") + GUID).encode()).digest())
            sock.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        sock.setblocking(False)
        return Conn(sock, lines[0], headers, rest)

    def close(self):
        self.lsock.close()


# The helper

class Helper:
    def __init__(self, url, *, args=None, env=None, pair=False):
        e = dict(os.environ, X6100_FDVR_URL=url, X6100_FW="0.3.12", X6100_FDVR_RETRY="0.3", X6100_FDVR_TIMEOUT="2")
        e.update(env or {})
        argv = [sys.executable, SCRIPT] + (args if args is not None else [CALL, GRID, "0", "14236000", VERSION])
        self.lines = queue.Queue()
        if pair:        # as the GUI does it: one SOCK_SEQPACKET socket pair as stdin and stdout
            self.mine, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            self.p = subprocess.Popen(argv, stdin=theirs.fileno(), stdout=theirs.fileno(), env=e)
            theirs.close()
            threading.Thread(target=self.read_pair, daemon=True).start()
        else:
            self.mine = None
            self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=e)
            threading.Thread(target=self.read_pipe, daemon=True).start()

    def read_pipe(self):
        for raw in self.p.stdout:
            self.lines.put((time.monotonic(), raw.decode().rstrip("\n")))

    def read_pair(self):
        buf = b""
        while True:
            try:
                data = self.mine.recv(4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.put((time.monotonic(), line.decode()))

    def send(self, line):
        if self.mine:
            self.mine.send((line + "\n").encode())
        else:
            self.p.stdin.write((line + "\n").encode())
            self.p.stdin.flush()

    def line(self, timeout=2.0):
        try:
            return self.lines.get(timeout=timeout)[1]
        except queue.Empty:
            return None

    def no_line(self, secs):
        return self.line(secs) is None

    def end(self):
        """Its stdin closed, as the GUI does: the time until it exits, and its code"""
        t0 = time.monotonic()
        if self.mine:
            self.mine.shutdown(socket.SHUT_WR)
        else:
            self.p.stdin.close()
        try:
            code = self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            code = self.p.wait()
        return time.monotonic() - t0, code

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


def event(text):
    """A client's Socket.IO event: (name, data)"""
    if not isinstance(text, str) or not text.startswith("42"):
        return None, text
    ev = json.loads(text[2:])
    return ev[0], ev[1] if len(ev) > 1 else None


def handshake(srv, h, *, open_packet=OPEN):
    """Up to connection_successful and the three events after it; the connection"""
    c = srv.accept()
    if not check(isinstance(c, Conn), "the helper connects"):
        return None
    c.send(open_packet)
    m = c.recv()
    check(isinstance(m, str) and m.startswith("40"), f"40 + auth after the open packet ({m!r})")
    c.send('40{"sid":"sio1"}')
    c.send('42["connection_successful",{}]')
    got = [event(c.recv()) for _ in range(3)]
    check([g[0] for g in got] == ["freq_change", "tx_report", "message_update"], f"the state after connect ({got})")
    check(h.line() == "connected", "'connected' for the GUI")
    return c


# The cases

def case_handshake():
    print("handshake, auth, nothing before connection_successful, the state after it")
    srv = Server()
    h = Helper(srv.url())
    c = srv.accept()
    check(isinstance(c, Conn), "a connection")
    check(c.request == "GET " + PATH + " HTTP/1.1", f"the request line ({c.request})")
    hd = c.headers
    check(hd.get("upgrade", "").lower() == "websocket" and "upgrade" in hd.get("connection", "").lower(),
          "Upgrade: websocket, Connection: Upgrade")
    check(hd.get("sec-websocket-version") == "13", "Sec-WebSocket-Version: 13")
    check(len(base64.b64decode(hd.get("sec-websocket-key", ""))) == 16, "a 16-byte Sec-WebSocket-Key")
    check(hd.get("host") == f"127.0.0.1:{srv.port}", f"Host ({hd.get('host')})")
    check(hd.get("user-agent", "").startswith("OpenX6X00/0.3.12"), f"User-Agent ({hd.get('user-agent')})")
    ok, m = c.quiet(0.4)
    check(ok, f"nothing before the open packet ({m!r})")
    c.send(OPEN)
    m = c.recv()
    check(isinstance(m, str) and m.startswith("40"), f"40 after the open packet ({m!r})")
    auth = json.loads(m[2:]) if isinstance(m, str) and m.startswith("40") else {}
    check(auth == {"role": "report_wo", "callsign": CALL, "grid_square": GRID, "version": VERSION, "rx_only": False,
                   "os": "linux", "protocol_version": 2}, f"the auth ({auth})")
    check(list(auth)[:2] == ["role", "callsign"], "the auth's order as FreeDV's")
    c.send('40{"sid":"sio1"}')
    # Events from the GUI before connection_successful: kept (freq, tx, message) or dropped (rx)
    h.send("freq 7177000")
    h.send("tx 1")
    h.send("rx DL1ABC 7")
    h.send("msg  hello\tworld ")
    ok, m = c.quiet(0.6)
    check(ok, f"nothing sent before connection_successful ({m!r})")
    c.send('42["connection_successful",{}]')
    got = [event(c.recv()) for _ in range(3)]
    check(got == [("freq_change", {"freq": 7177000}), ("tx_report", {"mode": "RADEV1", "transmitting": True}),
                  ("message_update", {"message": "hello world"})], f"the state after connect ({got})")
    ok, m = c.quiet(0.5)
    check(ok, f"no rx_report kept from before ({m!r})")
    check(h.line() == "connected", "'connected' for the GUI")
    t, code = h.end()
    check(t < 1.0 and code == 0, f"the end of stdin ends it: {t:.2f} s, code {code}")
    check(c.recv(1) == "41", "Socket.IO's disconnect at the end")
    c.recv(1)
    check(c.closed_with == 1000, f"the websocket's close 1000 at the end ({c.closed_with})")
    c.close()
    srv.close()


def case_events():
    print("events and their timing, the pings")
    srv = Server()
    h = Helper(srv.url())
    c = handshake(srv, h)
    t0 = time.monotonic()
    h.send("tx 1")
    m, t = c.recv(), time.monotonic() - t0
    check(event(m) == ("tx_report", {"mode": "RADEV1", "transmitting": True}) and t < 0.3, f"tx 1 at once ({m!r} {t:.2f})")
    h.send("tx 1")
    ok, m = c.quiet(0.3)
    check(ok, f"the same TX state is not sent again ({m!r})")
    h.send("tx 0")
    check(event(c.recv()) == ("tx_report", {"mode": "RADEV1", "transmitting": False}), "tx 0")
    t0 = time.monotonic()
    h.send("rx dl1abc -3")
    m, t = c.recv(), time.monotonic() - t0
    check(event(m) == ("rx_report", {"callsign": "DL1ABC", "mode": "RADEV1", "snr": -3}) and t < 0.3,
          f"rx_report at once ({m!r} {t:.2f})")
    h.send("rx not-a-call 3")
    h.send("rx K1ABC x")
    ok, m = c.quiet(0.3)
    check(ok, f"a bad rx line is not sent ({m!r})")
    h.send("msg QRV 40 m, dipole")
    check(event(c.recv()) == ("message_update", {"message": "QRV 40 m, dipole"}), "message_update")
    # The knob turning: one freq_change a second at most, the last frequency
    time.sleep(1.1)
    t0 = time.monotonic()
    h.send("freq 14236100")
    m = c.recv()
    t1 = time.monotonic()
    check(event(m) == ("freq_change", {"freq": 14236100}) and t1 - t0 < 0.3, f"a freq_change at once ({m!r})")
    for f in range(14236200, 14237000, 100):
        h.send(f"freq {f}")
        time.sleep(0.05)
    m = c.recv(2)
    t2 = time.monotonic()
    check(event(m) == ("freq_change", {"freq": 14236900}), f"the last frequency ({m!r})")
    check(0.9 <= t2 - t1 <= 1.6, f"a second after the one before ({t2 - t1:.2f} s)")
    ok, m = c.quiet(1.2)
    check(ok, f"one freq_change for the turn ({m!r})")
    h.send("freq 14236900")
    ok, m = c.quiet(1.2)
    check(ok, f"the same frequency is not sent again ({m!r})")
    # Engine.IO's ping, the websocket's ping
    c.send("2")
    check(c.recv(0.3) == "3", "Engine.IO ping 2 -> pong 3")
    c.frame(0x9, b"abc")
    check(c.recv(0.3) == ("pong", b"abc"), "websocket ping -> pong")
    # What the client does not use: ignored, the connection stays
    c.send('42["new_connection",{"sid":"x","callsign":"K1ABC"}]')
    c.send("4 bad")
    c.send("42not json")
    c.frame(0x2, b"\x00\x01")
    c.send("6")
    ok, m = c.quiet(0.4)
    check(ok and h.no_line(0.1), f"unused packets ignored ({m!r})")
    h.send("tx 1")
    check(event(c.recv()) == ("tx_report", {"mode": "RADEV1", "transmitting": True}), "still connected after them")
    h.end()
    c.close()
    srv.close()


def case_qsy():
    print("a QSY request: one line for the GUI, nothing sent")
    srv = Server()
    h = Helper(srv.url())
    c = handshake(srv, h)
    c.send('42["qsy_request",{"callsign":"K1ABC","frequency":7177000,"message":"pse QSY\\n now"}]')
    check(h.line() == "qsy K1ABC 7177000 pse QSY now", "the qsy line")
    ok, m = c.quiet(1.5)
    check(ok, f"nothing sent for it: no freq_change, no tx ({m!r})")
    # In two frames (a fragmented message), and with an ack id
    msg = b'42["qsy_request",{"callsign":"VK2XYZ","frequency":14236000,"message":""}]'
    c.frame(0x1, msg[:20], fin=False)
    c.frame(0x0, msg[20:])
    check(h.line() == "qsy VK2XYZ 14236000", "a fragmented one")
    c.send('4212["qsy_request",{"callsign":"G4XYZ","frequency":3643000,"message":"hi"}]')
    check(h.line() == "qsy G4XYZ 3643000 hi", "one with an ack id")
    c.send('42["qsy_request",{"callsign":"G4XYZ","frequency":"7177000","message":"hi"}]')
    c.send('42["qsy_request",{"callsign":"G4XYZ","frequency":true,"message":"hi"}]')
    check(h.no_line(0.5), "a QSY with a bad frequency is dropped")
    h.end()
    c.close()
    srv.close()


def case_drop():
    print("a dropped line, the retry, the state again; ping timeout, close, refused auth")
    srv = Server()
    h = Helper(srv.url(), env={"X6100_FDVR_RETRY": "0.3"})
    c = handshake(srv, h)
    h.send("freq 3643000")
    h.send("tx 1")
    h.send("msg back soon")
    for _ in range(2):
        c.recv(1.5)
    c.recv(1.5)
    c.recv(1.5)
    t0 = time.monotonic()
    c.sock.close()                  # the line drops
    check(h.line() == "offline closed by the server", "one offline line when it drops")
    c = srv.accept(3)
    t = time.monotonic() - t0
    check(isinstance(c, Conn) and 0.25 <= t <= 1.0, f"the retry after the first wait ({t:.2f} s)")
    c.send(OPEN)
    c.recv()
    c.send('40{"sid":"sio2"}')
    c.send('42["connection_successful",{}]')
    got = [event(c.recv()) for _ in range(3)]
    check(got == [("freq_change", {"freq": 3643000}), ("tx_report", {"mode": "RADEV1", "transmitting": True}),
                  ("message_update", {"message": "back soon"})], f"the state again after the reconnect ({got})")
    check(h.line() == "connected", "'connected' again")
    # No ping in pingInterval + pingTimeout: the line is dead
    c.close()
    check(h.line() == "offline closed by the server", "offline")
    c = srv.accept(3)
    t0 = time.monotonic()
    c.send('0{"sid":"eio3","upgrades":[],"pingInterval":300,"pingTimeout":200,"maxPayload":1000000}')
    c.recv()
    c.send('40{"sid":"sio3"}')
    c.send('42["connection_successful",{}]')
    for _ in range(3):
        c.recv()
    check(h.line() == "connected", "connected")
    c.send("2")
    check(c.recv(0.3) == "3", "a pong")
    t1 = time.monotonic()
    line = h.line(2)
    t = time.monotonic() - t1
    check(line == "offline no ping from the server" and 0.4 <= t <= 1.0, f"no ping: dropped after 0.5 s ({line} {t:.2f})")
    c.recv(0.5)
    check(c.eof, "the client closed it")
    c.close()
    # Engine.IO close
    c = srv.accept(3)
    c.send(OPEN)
    c.recv()
    c.send('40{"sid":"sio4"}')
    c.send('42["connection_successful",{}]')
    for _ in range(3):
        c.recv()
    h.line()
    c.send("1")
    check(h.line() == "offline closed by the server", "Engine.IO close")
    c.close()
    # Failing again before a connection_successful: retried, quietly (the GUI was told once)
    c = srv.accept(3)
    c.send(OPEN)
    c.recv()
    c.send('44{"message":"bad auth"}')
    check(isinstance(srv.accept(3), Conn), "retried after a connect error")
    check(h.no_line(0.5), "no line for the failures after the first")
    h.end()
    srv.close()
    # The auth refused (Socket.IO connect error) at the first try, and no answer to the auth
    srv = Server()
    h = Helper(srv.url(), env={"X6100_FDVR_RETRY": "0.3"})
    c = srv.accept(3)
    c.send(OPEN)
    c.recv()
    c.send('44{"message":"bad auth"}')
    check(h.line() == "offline refused: bad auth", "a connect error")
    c.close()
    h.end()
    srv.close()


def case_backoff():
    print("the backoff: the waits double up to the longest, one offline line")
    srv = Server()
    h = Helper(srv.url(), env={"X6100_FDVR_RETRY": "0.2", "X6100_FDVR_RETRY_MAX": "0.8"})
    times = []
    for _ in range(6):
        r, _, _ = select.select([srv.lsock], [], [], 3)
        if not r:
            break
        s, _ = srv.lsock.accept()
        times.append(time.monotonic())
        s.close()                   # accepted and closed at once
    gaps = [round(b - a, 2) for a, b in zip(times, times[1:])]
    want = [0.2, 0.4, 0.8, 0.8, 0.8]
    check(len(gaps) == 5 and all(w * 0.85 <= g <= w * 1.15 + 0.3 for g, w in zip(gaps, want)),
          f"the waits {gaps}, about {want}")
    lines = []
    while (line := h.line(0.1)) is not None:
        lines.append(line)
    check(len(lines) == 1 and lines[0].startswith("offline"), f"one offline line ({lines})")
    h.end()
    srv.close()


def case_no_internet():
    print("no internet: refused, no answer, no name; quiet; the end of stdin and SIGTERM end it at once")
    # Nothing listens on the port
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    h = Helper(f"ws://127.0.0.1:{port}{PATH}", env={"X6100_FDVR_RETRY": "0.2", "X6100_FDVR_RETRY_MAX": "0.4"})
    check(h.line(2) == "offline refused", "one line: refused")
    check(h.no_line(2.0), "then quiet, while it tries again")
    check(h.p.poll() is None, "and it keeps running")
    t, code = h.end()
    check(t < 0.5 and code == 0, f"the end of stdin ends it ({t:.2f} s, code {code})")
    # A server that takes the connection and never answers: the end of stdin and SIGTERM during the attempt
    srv = Server()
    h = Helper(srv.url(), env={"X6100_FDVR_TIMEOUT": "1"})
    t0 = time.monotonic()
    line = h.line(3)
    t = time.monotonic() - t0
    check(line in ("offline no answer", "offline no websocket answer") and 0.8 <= t <= 2.0,
          f"no answer in time ({line} {t:.2f} s)")
    h.end()
    h = Helper(srv.url(), env={"X6100_FDVR_TIMEOUT": "30"})
    time.sleep(1.0)                 # in the attempt now, waiting for the answer
    t, code = h.end()
    check(t < 0.5 and code == 0, f"the end of stdin during an attempt ends it ({t:.2f} s)")
    h = Helper(srv.url(), env={"X6100_FDVR_TIMEOUT": "30"})
    time.sleep(1.0)
    t0 = time.monotonic()
    h.p.send_signal(signal.SIGTERM)
    code = h.p.wait(5)
    check(time.monotonic() - t0 < 0.5 and code == -signal.SIGTERM, f"SIGTERM ends it ({code})")
    srv.close()
    # A name that does not resolve (.invalid, RFC 2606): the resolver's own time, then one line
    h = Helper(f"ws://fdvr-test.invalid{PATH}", env={"X6100_FDVR_RETRY": "0.5"})
    line = h.line(25)
    check(line in ("offline name not found", "offline no network"), f"no name: one line ({line})")
    t, code = h.end()
    check(t < 0.5 and code == 0, f"the end of stdin ends it ({t:.2f} s)")


def case_args():
    print("bad arguments")
    for args, want in (([], "error usage"), (["", GRID, "0", "1", VERSION], "error no callsign"),
                       ([CALL, "", "0", "1", VERSION], "error no locator"),
                       ([CALL, "ZZ99", "0", "1", VERSION], "error no locator")):
        h = Helper("ws://127.0.0.1:9" + PATH, args=args)
        line = h.line(3) or ""
        code = h.p.wait(5)
        check(line.startswith(want) and code == 2, f"{args}: {line!r} {code}")


def case_role_report():
    print("role report (the bench): the station list read and dropped, a QSY in a bulk_update")
    srv = Server()
    h = Helper(srv.url(), env={"X6100_FDVR_ROLE": "report"})
    c = srv.accept()
    c.send(OPEN)
    auth = json.loads(c.recv()[2:])
    check(auth["role"] == "report", f"role report ({auth['role']})")
    c.send('40{"sid":"s"}')
    c.send('42["connection_successful",{}]')
    for _ in range(3):
        c.recv()
    h.line()
    c.send('42["bulk_update",[["new_connection",{"sid":"a","callsign":"K1ABC"}],["freq_change",{"sid":"a","freq":1}],'
           '["qsy_request",{"callsign":"K1ABC","frequency":7177000,"message":"x"}]]]')
    check(h.line() == "qsy K1ABC 7177000 x", "the QSY inside the bulk_update")
    check(h.no_line(0.3), "nothing for the rest")
    h.end()
    c.close()
    srv.close()


def case_pair():
    print("the GUI's socket pair as stdin and stdout")
    srv = Server()
    h = Helper(srv.url(), pair=True)
    c = handshake(srv, h)
    h.send("rx K1ABC 12")
    check(event(c.recv()) == ("rx_report", {"callsign": "K1ABC", "mode": "RADEV1", "snr": 12}), "a line through it")
    c.send('42["qsy_request",{"callsign":"K1ABC","frequency":7177000,"message":"x"}]')
    check(h.line() == "qsy K1ABC 7177000 x", "a line back")
    t, code = h.end()
    check(t < 1.0 and code == 0, f"its end ends the helper ({t:.2f} s)")
    c.close()
    srv.close()


def case_tls():
    print("TLS: a trusted certificate, and one that is not")
    d = tempfile.mkdtemp(prefix="fdvr-tls-")
    cert, key = os.path.join(d, "cert.pem"), os.path.join(d, "key.pem")
    try:
        r = subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj",
                            "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", "-keyout", key, "-out", cert],
                           capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        r = None
    if not r or r.returncode != 0:
        print("  skipped: no openssl command")
        return
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv = Server(tls=ctx)
    h = Helper(srv.url("wss", "localhost"), env={"SSL_CERT_FILE": cert})
    c = handshake(srv, h)
    check(c is not None and c.headers.get("host") == f"localhost:{srv.port}", "over TLS")
    if c:
        h.send("tx 1")
        check(event(c.recv()) == ("tx_report", {"mode": "RADEV1", "transmitting": True}), "an event over TLS")
        c.send('42["qsy_request",{"callsign":"K1ABC","frequency":7177000,"message":"tls"}]')
        check(h.line() == "qsy K1ABC 7177000 tls", "a QSY over TLS")
    t, code = h.end()
    check(t < 1.0 and code == 0, "the end over TLS")
    if c:
        c.close()
    # Not trusted: no connection, one line
    h = Helper(srv.url("wss", "localhost"), env={"SSL_CERT_FILE": os.path.join(d, "none.pem"),
                                                  "SSL_CERT_DIR": d})
    check(srv.accept(3) == "tls failed", "the server sees the client give up")
    line = h.line(3) or ""
    check(line.startswith("offline certificate:"), f"an untrusted certificate is refused ({line})")
    h.end()
    srv.close()
    for f in (cert, key):
        os.remove(f)
    os.rmdir(d)


def main():
    sys.dont_write_bytecode = True
    for case in (case_handshake, case_events, case_qsy, case_drop, case_backoff, case_no_internet, case_args,
                 case_role_report, case_pair, case_tls):
        case()
    if failures:
        print(f"{len(failures)} failed")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
