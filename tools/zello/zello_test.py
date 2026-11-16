#!/usr/bin/env python3
"""Host test of rootfs/usr/sbin/x6100-zello, the Zello app's helper, on a PC. Nothing contacts Zello: the saved
samples of samples/ (mksamples.py), and a stand-in Channel API server on 127.0.0.1; every other address is refused
for the whole test (socket.getaddrinfo and socket.create_connection wrapped). `make` in tools/zello:

    python3 -I -B zello_test.py [-v]

1. The websocket: a client frame against RFC 6455's own example, the length forms, the server's bytes of
   server_stream.bin fed in pieces of every size (fragments, a ping between them, a close).
2. Zello's messages: codec_header, the binary audio frame of audio_frame.bin, the channel's state from the saved
   transcript channel_events.jsonl (who talks, the lines for the GUI).
3. The token: an RSA key made here (PKCS#1 and PKCS#8), the JWT checked with its public half; keys that can't work.
4. Opus with the PC's libopus: the saved packet decoded, a tone through the encoder and back.
5. The secrets: never in the log, nor in what the GUI or the web page's Test gets.
6. run against the stand-in server, over the GUI's socket as the app has it: logon with the signed token, the channel,
   a talker's audio to the GUI, a message from the GUI to the channel, the network pulled (the quick reconnect with
   the refresh_token), a refresh_token refused (a new token at once), a dead line (pings unanswered), the app closing
   (the close frame); an account refused (given up, said once); test.
"""
import base64
import hashlib
import importlib.machinery
import importlib.util
import json
import math
import os
import queue
import random
import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
SAMPLES = os.path.join(HERE, "samples")
HELPER = os.path.join(TOP, "rootfs", "usr", "sbin", "x6100-zello")

WORK = tempfile.mkdtemp(prefix="x6100-zello.")
os.environ.update(X6100_ZELLO_DIR=os.path.join(WORK, "zello"), X6100_ZELLO_LOG=os.path.join(WORK, "logs", "zello.txt"),
                  X6100_ZELLO_PING="0.4", X6100_ZELLO_DEAD="1.5", X6100_ZELLO_RETRY="0.2", X6100_FW="9.9.9-test")
os.environ.pop("X6100_ZELLO_URL", None)

# Only this PC: a test never reaches Zello or anything else
LOCAL = ("127.0.0.1", "localhost")
_getaddrinfo, _create_connection = socket.getaddrinfo, socket.create_connection


def local_getaddrinfo(host, *a, **kw):
    if host not in LOCAL:
        raise AssertionError(f"the test tried to reach {host}")
    return _getaddrinfo(host, *a, **kw)


def local_create_connection(address, *a, **kw):
    if address[0] not in LOCAL:
        raise AssertionError(f"the test tried to reach {address[0]}")
    return _create_connection(address, *a, **kw)


socket.getaddrinfo, socket.create_connection = local_getaddrinfo, local_create_connection

loader = importlib.machinery.SourceFileLoader("x6100_zello", HELPER)
spec = importlib.util.spec_from_loader("x6100_zello", loader)
z = importlib.util.module_from_spec(spec)
loader.exec_module(z)

USER, PASSWORD, ISSUER = "zellofan42", "Zello-acct-pw-77", "WyJNVFl4TWpNME5UWTNPRGt3IiwiZGV2In0="
REFRESH = ("refresh-token-secret-one", "refresh-token-secret-two")
CHANNEL = "Test channel"


# ---------- an RSA key made here, in PEM ----------

def is_prime(n, rnd):
    if n < 2:
        return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, s = n - 1, 0
    while d % 2 == 0:
        d, s = d // 2, s + 1
    for _ in range(24):
        x = pow(rnd.randrange(2, n - 1), d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def prime(bits, rnd):
    while True:
        c = rnd.getrandbits(bits) | (3 << (bits - 2)) | 1
        if is_prime(c, rnd):
            return c


def der(tag, body):
    n = len(body)
    if n < 128:
        return bytes([tag, n]) + body
    k = (n.bit_length() + 7) // 8
    return bytes([tag, 0x80 | k]) + n.to_bytes(k, "big") + body


def der_int(x):
    b = x.to_bytes((x.bit_length() + 8) // 8, "big")    # a leading 0 keeps it positive
    return der(0x02, b)


def make_key(bits=1024, seed=6100):
    rnd = random.Random(seed)
    e = 65537
    while True:
        p, q = prime(bits // 2, rnd), prime(bits // 2, rnd)
        if p != q and math.gcd(e, (p - 1) * (q - 1)) == 1 and (p * q).bit_length() == bits:
            break
    n, d = p * q, pow(e, -1, (p - 1) * (q - 1))
    nums = (0, n, e, d, p, q, d % (p - 1), d % (q - 1), pow(q, -1, p))
    pkcs1 = der(0x30, b"".join(der_int(x) for x in nums))
    alg = der(0x30, bytes.fromhex("06092a864886f70d0101010500"))
    pkcs8 = der(0x30, der_int(0) + alg + der(0x04, pkcs1))
    return (n, e), pem("RSA PRIVATE KEY", pkcs1), pem("PRIVATE KEY", pkcs8)


def pem(kind, data):
    b = base64.b64encode(data).decode()
    return f"-----BEGIN {kind}-----\n" + "\n".join(b[i:i + 64] for i in range(0, len(b), 64)) + f"\n-----END {kind}-----\n"


PUBLIC, PEM1, PEM8 = make_key()
KEY_LINES = [line for line in PEM8.splitlines() if "-----" not in line and len(line) >= 8]   # 64 each, the last part
SECRETS = (USER, PASSWORD, ISSUER, *REFRESH, *KEY_LINES)


def b64d(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def token_ok(token, public=PUBLIC):
    """The JWT's claims when its RS256 signature is right, else None"""
    head, body, sig = token.split(".")
    n, e = public
    k = (n.bit_length() + 7) // 8
    t = bytes.fromhex("3031300d060960864801650304020105000420") + hashlib.sha256(f"{head}.{body}".encode()).digest()
    want = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    if pow(int.from_bytes(b64d(sig), "big"), e, n).to_bytes(k, "big") != want:
        return None
    if json.loads(b64d(head)) != {"alg": "RS256", "typ": "JWT"}:
        return None
    return json.loads(b64d(body))


def write_account(pem_text=PEM8, user=USER, password=PASSWORD, issuer=ISSUER):
    os.makedirs(z.DIR, exist_ok=True)
    with open(z.ACCOUNT, "w") as f:
        f.write(f"user={user}\npassword={password}\nissuer={issuer}\n")
    with open(z.KEY, "w") as f:
        f.write(pem_text)
    with open(z.CHANNELS, "w") as f:
        f.write(CHANNEL + "\nSecond\n")


def read_log():
    try:
        with open(z.LOG) as f:
            return f.read()
    except OSError:
        return ""


def sample(name, mode="rb"):
    with open(os.path.join(SAMPLES, name), mode) as f:
        return f.read()


# ---------- 1. the websocket ----------

class WebSocket(unittest.TestCase):
    def test_client_frame_rfc6455(self):
        # RFC 6455 5.7: "Hello", masked with 37 fa 21 3d
        self.assertEqual(z.ws_frame(1, "Hello", bytes.fromhex("37fa213d")).hex(), "818537fa213d7f9f4d5158")

    def test_lengths(self):
        r = z.FrameReader()
        for n in (0, 1, 125, 126, 127, 65535, 65536, 70000):
            payload = bytes(i & 0xFF for i in range(n))
            f = z.ws_frame(2, payload, b"\x01\x02\x03\x04")
            self.assertEqual(f[1] & 0x7F, n if n < 126 else 126 if n < 65536 else 127)
            self.assertEqual(r.feed(f), [(2, payload)])

    def test_server_stream_in_pieces(self):
        data = sample("server_stream.bin")
        events = [json.loads(line) for line in sample("channel_events.jsonl", "r").splitlines()]
        audio = sample("audio_frame.bin")
        want = [(1, json.dumps(events[1], separators=(",", ":")).encode()), (9, b"ping!"),
                (1, json.dumps(events[2], separators=(",", ":")).encode()),
                (2, audio + b"\x00" * (130 - len(audio))), (10, b"pong"), (8, b"\x03\xe8bye")]
        for size in range(1, 40):
            r, got = z.FrameReader(), []
            for i in range(0, len(data), size):
                got += r.feed(data[i:i + size])
            self.assertEqual(got, want, f"pieces of {size}")

    def test_bad_streams(self):
        with self.assertRaises(z.Dropped):
            z.FrameReader().feed(b"\x80\x03abc")                       # a continuation with no start
        with self.assertRaises(z.Dropped):
            z.FrameReader().feed(b"\x82\x7f" + struct.pack(">Q", z.MAX_MESSAGE + 1))
        r = z.FrameReader()
        piece = struct.pack(">Q", 65536) + bytes(65536)
        r.feed(b"\x02\x7f" + piece)                                     # a start, then continuations
        with self.assertRaises(z.Dropped):                               # too long in fragments
            for _ in range(20):
                r.feed(b"\x00\x7f" + piece)


# ---------- 2. Zello's messages ----------

class Messages(unittest.TestCase):
    def test_codec_header(self):
        self.assertEqual(z.parse_codec_header("gD4BPA=="), (16000, 1, 60))     # Zello's own example
        self.assertEqual(z.parse_codec_header(z.make_codec_header()), (16000, 1, 20))
        for bad in ("", None, "gD4B", "!!!!", "AAAAAA==", base64.b64encode(b"\x80\x3e\x01\x14\x00").decode()):
            self.assertIsNone(z.parse_codec_header(bad), bad)

    def test_audio_frame(self):
        data = sample("audio_frame.bin")
        sid, pid, opus = z.parse_audio(data)
        self.assertEqual((sid, pid), (7, 42))
        self.assertEqual(opus, data[9:])
        self.assertEqual(z.make_audio(7, 42, opus), data)
        self.assertIsNone(z.parse_audio(data[:9]))
        self.assertIsNone(z.parse_audio(b"\x02" + data[1:]))               # type 2 is not audio

    def test_channel_transcript(self):
        ch = z.Channel(CHANNEL)
        got = []
        for line in sample("channel_events.jsonl", "r").splitlines():
            got.append(ch.event(json.loads(line)))
        self.assertEqual(got, [
            ([], None),                                                    # the logon's answer: the session's
            (["channel 3 Test channel", "state online"], None),
            (["talker alice"], ("start", 7)),
            ([], ("start", 8)),                                            # bob over alice: alice still shown
            (["talker bob"], ("stop", 7)),
            (["talker"], ("stop", 8)),
            ([], None),                                                    # a stream never started
            ([], None),                                                    # a picture
            ([], None),                                                    # a picture's stream
            ([], None),                                                    # an error: kept, no line
            (["channel - Test channel", "state offline channel offline"], None),
        ])
        self.assertEqual(ch.error, "channel is not ready")
        self.assertEqual(z.say(ch.error), "the channel is not ready yet")

    def test_hostile_names(self):
        ch = z.Channel(CHANNEL)
        lines, _ = ch.event({"command": "on_stream_start", "stream_id": 1, "from": "eve\nstate online\x00"})
        self.assertEqual(lines, ["talker eve state online"])              # one line, never a second
        lines, _ = ch.event({"command": "on_channel_status", "status": "online", "users_online": -5,
                             "channel": "x" * 300})
        self.assertEqual(lines[0], "channel - " + "x" * 64)


# ---------- 3. the token ----------

class Token(unittest.TestCase):
    def test_pkcs8_and_pkcs1(self):
        for text in (PEM8, PEM1, PEM8.replace("\n", "\\n")):
            claims = token_ok(z.make_token(ISSUER, text, now=1763300000))
            self.assertEqual(claims, {"iss": ISSUER, "exp": 1763300060})

    def test_keys_that_cannot_work(self):
        with self.assertRaises(z.Fail):
            z.make_token("", PEM8)
        with self.assertRaises(z.Fail):
            z.make_token(ISSUER, "")
        lines = PEM8.splitlines()
        for bad in ("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----", "\n".join(lines[:-3] + lines[-1:]),
                    PEM8.replace(KEY_LINES[3], KEY_LINES[3][::-1])):
            with self.assertRaises(z.Fail):
                z.make_token(ISSUER, bad)
        _, small, _ = make_key(bits=512, seed=1)
        with self.assertRaises(z.Fail):                                    # under 1024 bits
            z.make_token(ISSUER, small)


# ---------- 4. Opus ----------

class OpusCodec(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.opus = z.Opus()
        except z.Fail:
            raise unittest.SkipTest("no libopus on this PC")

    def test_saved_packet(self):
        d = self.opus.decoder()
        pcm = d.decode(sample("audio_frame.bin")[9:])
        d.close()
        self.assertEqual(len(pcm), 320 * 2)
        s = struct.unpack("<320h", pcm)
        self.assertGreater(max(abs(v) for v in s), 1000)

    def test_round_trip(self):
        e, d = self.opus.encoder(), self.opus.decoder()
        peak = 0
        for k in range(10):
            pcm = struct.pack("<320h", *[int(8000 * math.sin(2 * math.pi * 1000 * (k * 320 + i) / 16000))
                                         for i in range(320)])
            pkt = e.encode(pcm)
            self.assertTrue(0 < len(pkt) < 400)
            out = d.decode(pkt)
            self.assertEqual(len(out), 640)
            peak = max(peak, max(abs(v) for v in struct.unpack("<320h", out)))
        e.close()
        d.close()
        self.assertGreater(peak, 4000)


# ---------- 5. the secrets in the log ----------

class Secrets(unittest.TestCase):
    def test_log_hides_them(self):
        path = os.path.join(WORK, "logs", "hide.txt")
        lg = z.Log(path)
        lg.hide = z.secrets_of({"user": USER, "password": PASSWORD, "issuer": ISSUER}, PEM8)
        lg("logon " + " ".join(SECRETS[:3] + tuple(KEY_LINES)) + " done")
        with open(path) as f:
            text = f.read()
        for s in (USER, PASSWORD, ISSUER, *KEY_LINES):
            self.assertNotIn(s, text)
        self.assertIn("logon (hidden) (hidden)", text)
        self.assertIn("done", text)


# ---------- 6. run, against a stand-in Channel API server ----------

class Conn:
    """One websocket accepted by the stand-in server"""

    def __init__(self, sock, request):
        self.sock, self.request = sock, request
        self.frames = z.FrameReader()
        self.pending = []
        self.pings = 0
        self.answer_pings = True

    def send(self, op, payload):
        if isinstance(payload, str):
            payload = payload.encode()
        n = len(payload)
        head = bytes([0x80 | op]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n))
        self.sock.sendall(head + payload)

    def json(self, m):
        self.send(1, json.dumps(m))

    def recv(self, timeout=5.0):
        """The next text or binary message (op, payload); pings answered; (8, ...) at the close; None at the end"""
        end = time.monotonic() + timeout
        while True:
            while self.pending:
                op, payload = self.pending.pop(0)
                if op == 9:
                    self.pings += 1
                    if self.answer_pings:
                        self.send(10, payload)
                    continue
                return op, payload
            left = end - time.monotonic()
            if left <= 0:
                raise AssertionError("nothing from the helper")
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                return None
            self.pending += self.frames.feed(data)

    def command(self, timeout=5.0):
        while True:
            m = self.recv(timeout)
            if m is None or m[0] == 8:
                return m
            if m[0] == 1:
                return json.loads(m[1])


class Server:
    """Accepts websockets on 127.0.0.1 and gives each to the next scene of the test"""

    def __init__(self, scenes):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.url = f"ws://127.0.0.1:{self.listener.getsockname()[1]}/ws"
        self.scenes = list(scenes)
        self.errors = []
        self.done = threading.Event()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        try:
            for scene in self.scenes:
                sock, _ = self.listener.accept()
                head = b""
                while b"\r\n\r\n" not in head:
                    head += sock.recv(4096)
                lines = head.decode("latin-1").split("\r\n")
                hdr = {k.strip().lower(): v.strip() for k, _, v in (line.partition(":") for line in lines[1:] if line)}
                key = hdr["sec-websocket-key"]
                accept = base64.b64encode(hashlib.sha1((key + z.GUID).encode()).digest()).decode()
                sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
                c = Conn(sock, (lines[0], hdr))
                try:
                    scene(c)
                finally:
                    sock.close()
        except Exception as e:       # said by the test's thread
            self.errors.append(e)
        finally:
            self.done.set()

    def close(self):
        self.listener.close()


class GuiSide:
    """The app's end of the helper's socket: the lines in order, the audio apart (it comes on its own thread)"""

    def __init__(self, sock):
        self.sock = sock
        self.audio = []
        self.lines = []         # every line, for the checks at the end
        self.backlog = []       # lines not looked at by expect yet

    def take(self, timeout):
        """One record into audio or backlog; False when none came"""
        self.sock.settimeout(max(0.01, timeout))
        try:
            rec = self.sock.recv(65536)
        except socket.timeout:
            return False
        if not rec:
            raise AssertionError("the helper closed its socket")
        if rec[:1] == b"\x00":
            self.audio.append(rec[1:])
        else:
            line = rec.decode().rstrip("\n")
            self.lines.append(line)
            self.backlog.append(line)
        return True

    def expect(self, want, timeout=8.0):
        """Lines up to want (whole, or its start when it ends with ...); the lines before it"""
        end = time.monotonic() + timeout
        before = []
        while True:
            while self.backlog:
                line = self.backlog.pop(0)
                if line == want or (want.endswith("...") and line.startswith(want[:-3])):
                    self.last = line
                    return before
                before.append(line)
            if time.monotonic() >= end:
                raise AssertionError(f"no {want!r}; had {before}")
            self.take(end - time.monotonic())

    def wait_audio(self, n, timeout=5.0):
        end = time.monotonic() + timeout
        while len(self.audio) < n and time.monotonic() < end:
            self.take(0.05)
        return len(self.audio)


def tone(k):
    return struct.pack("<320h", *[int(6000 * math.sin(2 * math.pi * 700 * (k * 320 + i) / 16000)) for i in range(320)])


class Run(unittest.TestCase):
    def setUp(self):
        write_account()
        if os.path.exists(z.LOG):
            os.remove(z.LOG)
        self.seen = {}

    def start(self, scenes):
        server = Server(scenes)
        self.addCleanup(server.close)
        a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.addCleanup(b.close)
        self.addCleanup(a.close)
        result = {}
        t = threading.Thread(target=lambda: result.setdefault("rc", z.run(CHANNEL, z.Gui(b), server.url)), daemon=True)
        t.start()
        return server, GuiSide(a), t, result, a, b

    def logon(self, c, refresh=None):
        m = c.command()
        self.assertEqual(m["command"], "logon")
        self.assertEqual((m["username"], m["password"], m["channels"]), (USER, PASSWORD, [CHANNEL]))
        if refresh:
            self.assertEqual(m.get("refresh_token"), refresh)
            self.assertNotIn("auth_token", m)
        else:
            self.assertNotIn("refresh_token", m)
            claims = token_ok(m["auth_token"])
            self.assertIsNotNone(claims, "the token's signature")
            self.assertEqual(claims["iss"], ISSUER)
            self.assertLessEqual(abs(claims["exp"] - time.time() - 60), 5)
        req, hdr = c.request
        self.assertEqual(req, "GET /ws HTTP/1.1")
        self.assertEqual(hdr["user-agent"], "OpenX6X00/9.9.9-test")
        self.assertNotIn(USER, json.dumps(hdr))
        return m

    def online(self, c):
        c.json({"command": "on_channel_status", "channel": CHANNEL, "status": "online", "users_online": 3})

    def test_a_whole_session(self):
        audio = sample("audio_frame.bin")
        talk_got = {}

        def first(c):           # logon, a talker, a message from the radio, then the network gone
            m = self.logon(c)
            c.json({"seq": m["seq"], "success": True, "refresh_token": REFRESH[0]})
            self.online(c)
            c.json({"command": "on_stream_start", "type": "audio", "codec": "opus", "codec_header": "gD4BFA==",
                    "packet_duration": 20, "stream_id": 7, "channel": CHANNEL, "from": "alice"})
            for pid in range(1, 4):
                c.send(2, z.make_audio(7, pid, audio[9:]))
            c.send(2, z.make_audio(8, 1, audio[9:]))        # a stream never started: dropped
            c.json({"command": "on_stream_stop", "stream_id": 7})
            m = c.command()
            self.assertEqual((m["command"], m["channel"], m["type"], m["codec"], m["packet_duration"]),
                             ("start_stream", CHANNEL, "audio", "opus", 20))
            self.assertEqual(z.parse_codec_header(m["codec_header"]), (16000, 1, 20))
            c.json({"seq": m["seq"], "success": True, "stream_id": 99})
            packets = []
            while True:
                op, payload = c.recv()
                if op == 2:
                    packets.append(z.parse_audio(payload))
                    continue
                m = json.loads(payload)
                break
            talk_got["packets"], talk_got["stop"] = packets, m
            # the network pulled: the socket just ends

        def second(c):          # back at once with the refresh_token, which Zello no longer takes
            m = self.logon(c, refresh=REFRESH[0])
            c.json({"seq": m["seq"], "success": False, "error": "invalid token"})

        def third(c):           # a new signed token; then the line goes dead (pings unanswered)
            m = self.logon(c)
            c.json({"seq": m["seq"], "success": True, "refresh_token": REFRESH[1]})
            self.online(c)
            c.answer_pings = False
            while True:
                m = c.recv(timeout=6)
                if m is None or m[0] == 8:
                    break
            talk_got["pings_unanswered"] = c.pings

        def fourth(c):          # the refresh_token again; the app closes
            m = self.logon(c, refresh=REFRESH[1])
            c.json({"seq": m["seq"], "success": True})
            self.online(c)
            talk_got["closing"] = c.recv(timeout=10)

        server, gui, t, result, a, b = self.start([first, second, third, fourth])
        gui.expect("state connecting")
        gui.expect("channel 3 Test channel")
        gui.expect("state online")
        gui.expect("talker alice")
        self.assertEqual(gui.wait_audio(3), 3)
        self.assertTrue(all(len(p) == 640 for p in gui.audio))
        gui.expect("talker")

        a.send(b"talk start\n")
        gui.expect("talk on")
        for k in range(5):
            a.send(b"\x00" + tone(k))
        a.send(b"\x00" + tone(5)[:320])            # half a frame: kept for the next, never sent alone
        time.sleep(0.2)
        a.send(b"talk stop\n")
        gui.expect("talk off")
        gui.expect("state retry 0 ...")
        gui.expect("state connecting")             # second: refresh_token refused
        before = gui.expect("state connecting")    # third, at once: no retry line in between
        self.assertEqual(before, [])
        gui.expect("state online")
        gui.expect("state retry ...", timeout=10)
        self.assertEqual(gui.last, "state retry 0 no answer from Zello in 1 s")
        gui.expect("state online")
        a.close()
        t.join(10)
        self.assertFalse(t.is_alive())
        self.assertEqual(result["rc"], 0)
        server.done.wait(10)
        self.assertEqual(server.errors, [])

        pk = talk_got["packets"]
        self.assertEqual([(p[0], p[1]) for p in pk], [(99, i) for i in range(1, 6)])
        dec = z.Opus().decoder()
        self.assertTrue(all(len(dec.decode(p[2])) == 640 for p in pk))
        self.assertEqual((talk_got["stop"]["command"], talk_got["stop"]["stream_id"]), ("stop_stream", 99))
        self.assertGreaterEqual(talk_got["pings_unanswered"], 2)
        self.assertEqual(talk_got["closing"][0], 8)          # the app closed: a websocket close, not just gone
        self.assertNotIn("error", " ".join(gui.lines))

        text = read_log()
        for s in SECRETS:
            self.assertNotIn(s, text)
        for s in SECRETS:
            self.assertNotIn(s, "\n".join(gui.lines))
        for said in ("logged on, channel Test channel", "talking", "talk ended", "connection lost after",
                     "logon refused (refresh_token): invalid token", "dead: nothing from Zello",
                     "end: the app closed"):
            self.assertIn(said, text)

    def test_account_refused(self):
        def scene(c):
            m = self.logon(c)
            c.json({"seq": m["seq"], "success": False, "error": "not authorized"})

        server, gui, t, result, a, b = self.start([scene])
        gui.expect("state connecting")
        gui.expect("error user name or password refused")
        t.join(5)
        self.assertEqual(result["rc"], 1)
        self.assertIn("given up: user name or password refused", read_log())

    def test_no_account_no_connection(self):
        os.remove(z.ACCOUNT)
        server, gui, t, result, a, b = self.start([])
        gui.expect("error no Zello account: the web page's Zello page")
        t.join(5)
        self.assertEqual(result["rc"], 1)

    def test_talk_refused_and_channel_offline(self):
        def scene(c):
            m = self.logon(c)
            c.json({"seq": m["seq"], "success": True})
            c.json({"command": "on_channel_status", "channel": CHANNEL, "status": "offline"})
            self.online(c)
            m = c.command()
            c.json({"seq": m["seq"], "success": False, "error": "listen only connection"})
            c.recv(timeout=10)

        server, gui, t, result, a, b = self.start([scene])
        gui.expect("state offline channel offline")
        gui.expect("state online")
        a.send(b"talk start\n")
        gui.expect("talk refused this channel is listen only")
        a.close()
        t.join(10)
        self.assertEqual(result["rc"], 0)

    def test_web_page_test(self):
        def ok(c):
            m = self.logon(c)
            c.json({"seq": m["seq"], "success": True})
            self.online(c)
            c.recv(timeout=5)

        def refused(c):
            m = self.logon(c)
            c.json({"seq": m["seq"], "success": False, "error": "invalid token"})

        server = Server([ok, refused])
        self.addCleanup(server.close)
        self.assertEqual(z.test("", server.url), "ok Test channel, 3 online")
        line = z.test("", server.url)
        self.assertEqual(line, "error token refused: check the Issuer, the Private Key and the radio's clock")
        server.done.wait(5)
        self.assertEqual(server.errors, [])
        os.remove(z.ACCOUNT)
        self.assertEqual(z.test("", server.url), "error no Zello account: user name and password first")


if __name__ == "__main__":
    try:
        unittest.main()
    finally:
        shutil.rmtree(WORK, ignore_errors=True)
