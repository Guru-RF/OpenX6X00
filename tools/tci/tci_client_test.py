#!/usr/bin/env python3
"""A TCI client of the plainest kind, for the host test of src/tci_server.c (see Makefile):
a raw WebSocket on 127.0.0.1:50001 (TCI_TEST_PORT) that walks through what the VFO-Knob does
on a connection, what the browser pages ask for, the audio formats, then what WSJT-X's TCI
client does when it opens, hears and transmits. Standard library only."""
import base64
import hashlib
import math
import os
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", int(os.environ.get("TCI_TEST_PORT", "50001"))
PLAYED = os.environ.get("TCI_TEST_PLAYED")      # what the stub played in the last transmission


def amplitude(x, f, fs):
    """The amplitude of the f component of x (Goertzel; x holds whole cycles of f)"""
    w = 2 * math.pi * f / fs
    c, s1, s2 = 2 * math.cos(w), 0.0, 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return 2 * math.hypot(s1 - s2 * math.cos(w), s2 * math.sin(w)) / len(x)


def tone_check(name, x, f, fs, level, image=None):
    """x is a tone of f Hz at level: its level, what is left beside it (noise, distortion,
    images, a lost or doubled sample), and the image at image Hz, all in dB"""
    a = amplitude(x, f, fs)
    rest = math.sqrt(max(sum(v * v for v in x) / len(x) - a * a / 2, 1e-30)) / (a / math.sqrt(2))
    out = f"{name}: {f} Hz at {a:.5g} ({20 * math.log10(a / level):+.3f} dB), the rest {20 * math.log10(rest):.1f} dB"
    if image:
        im = amplitude(x, image, fs) / a
        out += f", image at {image} Hz {20 * math.log10(max(im, 1e-30)):.1f} dB"
        assert im < 10 ** (-80 / 20), out
    print(out)
    assert abs(20 * math.log10(a / level)) < 0.05, out
    return rest


def played():
    """The samples (int16 at 44.1 kHz) the last transmission played, from the stub's file"""
    if not PLAYED:
        return None
    with open(PLAYED, "rb") as f:
        raw = f.read()
    return struct.unpack(f"<{len(raw) // 2}h", raw)


class WS:
    def __init__(self, rcvbuf=None, headers=""):
        """headers: more request headers, "Name: value\\r\\n" lines (a browser's, Tailscale's proxy's)"""
        self.binary, self.pongs = [], []
        self.rate = None    # bytes a second read at most (a slow link), None: as fast as they come
        if rcvbuf:          # set before connect: the window is offered from it
            self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            self.s.settimeout(5)
            self.s.connect((HOST, PORT))
        else:
            self.s = socket.create_connection((HOST, PORT), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{headers}\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.s.recv(1024)
            if not chunk:
                raise RuntimeError("closed during the handshake")
            resp += chunk
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        assert b"101" in head.split(b"\r\n")[0], head
        assert want.encode() in head, "bad Sec-WebSocket-Accept"

    def send(self, opcode, payload):
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        m = int.from_bytes((mask * (n // 4 + 1))[:n], "little")
        self.s.sendall(head + mask + (int.from_bytes(payload, "little") ^ m).to_bytes(n, "little"))

    def text(self, s):
        self.send(1, s.encode())

    def slow(self, rate):
        """From now on read rate bytes a second at most"""
        self.rate, self.t0, self.got = rate, time.time(), 0

    def _fill(self, n):
        while len(self.buf) < n:
            size = 65536
            if self.rate:
                size = min(int(self.rate * (time.time() - self.t0)) - self.got, 4096)
                if size <= 0:
                    time.sleep(0.002)
                    continue
            chunk = self.s.recv(size)
            if not chunk:
                raise RuntimeError("closed")
            self.buf += chunk
            if self.rate:
                self.got += len(chunk)

    def frame(self):
        """(opcode, payload) of the next frame"""
        self._fill(2)
        b0, b1 = self.buf[0], self.buf[1]
        n, pos = b1 & 0x7F, 2
        if n == 126:
            self._fill(4)
            n, pos = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            self._fill(10)
            n, pos = struct.unpack(">Q", self.buf[2:10])[0], 10
        assert not (b1 & 0x80), "server frames must not be masked"
        self._fill(pos + n)
        payload = self.buf[pos:pos + n]
        self.buf = self.buf[pos + n:]
        return b0 & 0x0F, payload

    def wait_text(self, prefix, timeout=3.0):
        """The next text frame starting with prefix; binary frames and others are kept aside"""
        end = time.time() + timeout
        while time.time() < end:
            op, p = self.frame()
            if op == 1:
                line = p.decode()
                if line.startswith(prefix):
                    return line
            elif op == 2:
                self.binary.append(p)
            elif op == 0xA:
                self.pongs.append(p)
        raise AssertionError(f"no '{prefix}' within {timeout} s")

    def wait_chrono(self, timeout=1.0):
        """The header of the next TX_CHRONO frame"""
        end = time.time() + timeout
        while time.time() < end:
            op, p = self.frame()
            if op == 2 and struct.unpack("<8I", p[:32])[6] == 3:
                return struct.unpack("<8I", p[:32])
        raise AssertionError(f"no TX_CHRONO within {timeout} s")


def main():
    ws = WS()
    greeting = []
    while True:
        op, p = ws.frame()
        assert op == 1, f"unexpected frame {op} in the greeting"
        greeting.append(p.decode())
        if greeting[-1] == "ready;":
            break
    print("greeting:", " ".join(greeting))
    assert greeting[0].startswith("protocol:"), greeting[0]
    assert greeting[-2] == "start;", "the state ends with start;, the radio runs"
    assert "device:OpenX6X00;" in greeting
    assert any(g.startswith("vfo:0,0,") for g in greeting)
    assert any(g.startswith("modulation:0,") for g in greeting)
    assert any(g.startswith("rx_filter_band:0,") for g in greeting)

    ws.text("vfo:0,0,14074000;")
    print(ws.wait_text("vfo:0,0,14074000;"))
    ws.text("vfo:0,0,14074000;")                     # the same again: echoed to us
    print(ws.wait_text("vfo:0,0,14074000;"))
    ws.text("modulation:0,usb;")
    print(ws.wait_text("modulation:0,usb;"))
    ws.text("rx_filter_band:0,200,2400;")
    print(ws.wait_text("rx_filter_band:0,200,2400;"))
    ws.text("modulation:0,lsb;")
    print(ws.wait_text("modulation:0,lsb;"))
    print(ws.wait_text("rx_filter_band:0,-2400,-200;"))   # the passband below the carrier
    ws.text("agc_mode:0,slow;")
    print(ws.wait_text("agc_mode:0,slow;"))
    ws.text("agc_mode:0;")
    print(ws.wait_text("agc_mode:0,slow;"))
    ws.text("rit_offset:0,120;")
    print(ws.wait_text("rit_enable:0,true;"))
    print(ws.wait_text("rit_offset:0,120;"))
    ws.text("rit_enable:0,false;")
    print(ws.wait_text("rit_offset:0,0;"))
    ws.text("drive:0,50;")
    print(ws.wait_text("drive:0,50;"))

    ws.text("trx:0,true;")
    print(ws.wait_text("trx:0,true;"))
    ws.text("tx_sensors_enable:true;")
    print(ws.wait_text("tx_sensors:0,"))
    ws.text("trx:0,false;")
    print(ws.wait_text("trx:0,false;"))

    ws.text("rx_sensors_enable:true;")
    print(ws.wait_text("rx_sensors_enable:true;"))
    print(ws.wait_text("rx_smeter:0,0,"))
    print(ws.wait_text("rx_channel_sensors:0,0,"))

    # The knob asks for 24 kHz and int16, never for the channels: one channel, 512 a frame, as
    # this server always sent it (the rest of its format follows, announced)
    assert "audio_samplerate:48000;" in greeting and "audio_stream_sample_type:float32;" in greeting
    assert "audio_stream_channels:2;" in greeting and "audio_stream_samples:2048;" in greeting
    ws.text("audio_samplerate:24000;")
    print(ws.wait_text("audio_samplerate:24000;"), ws.wait_text("audio_stream_channels:1;"),
          ws.wait_text("audio_stream_samples:512;"))
    ws.text("audio_stream_sample_type:int16;")
    print(ws.wait_text("audio_stream_sample_type:int16;"))
    ws.text("audio_start:0;")
    print(ws.wait_text("audio_start:0;"))
    rx = rx_frames(ws, 0.8)
    f = rx[0]
    hdr = struct.unpack("<8I", f[:32])
    print(f"audio frame: {len(f)} bytes, rate {hdr[1]}, format {hdr[2]}, length {hdr[5]}, type {hdr[6]}, ch {hdr[7]}")
    assert len(f) == 64 + 1024 and hdr[1:3] == (24000, 0) and hdr[5:8] == (512, 1, 1), hdr
    x = [v / 32768 for f in rx[1:] for v in struct.unpack("<512h", f[64:])]
    tone_check("knob, int16 24 kHz mono", x[:12000], 1000, 24000, 8000 / 32768)
    ws.text("audio_stop:0;")
    print(ws.wait_text("audio_stop:0;"))

    # TCI audio transmission: the server asks for frames with TX_CHRONO, we answer with silence
    ws.text("trx:0,true,tci;")
    print(ws.wait_text("trx:0,true;"))
    chronos, times = 0, []
    end = time.time() + 1
    while time.time() < end and chronos < 10:
        op, p = ws.frame()
        if op == 2 and struct.unpack("<8I", p[:32])[6] == 3:
            # the knob answers every chrono with 512 int16 at 24 kHz, whatever it says: it must
            # say that, every 21.3 ms
            assert struct.unpack("<8I", p[:32])[1:3] + struct.unpack("<8I", p[:32])[5:8] == (24000, 0, 512, 3, 1)
            chronos += 1
            times.append(time.time())
            hdr = struct.pack("<8I", 0, 24000, 0, 0, 0, 512, 2, 1) + bytes(32)
            ws.send(2, hdr + bytes(1024))
    print(f"chrono frames answered: {chronos}, 512 int16 at 24 kHz, every "
          f"{1000 * (times[-1] - times[0]) / (len(times) - 1):.2f} ms")
    assert chronos >= 5
    ws.text("trx:0,false;")
    print(ws.wait_text("trx:0,false;"))
    if PLAYED:
        assert abs(len(played()) - chronos * 512 * 44100 / 24000) < 3, len(played())
        print(f"played: {len(played())} samples at 44.1 kHz")

    # A frame of another shape than its header says ends the transmission before any of it is
    # played: what WSJT-X sent to the int16 chrono, float32 stereo pairs under its int16 header
    ws.text("trx:0,true,tci;")
    print(ws.wait_text("trx:0,true;"))
    ws.wait_chrono()
    hdr = struct.pack("<8I", 0, 24000, 0, 0, 0, 512, 2, 0) + bytes(32)
    ws.send(2, hdr + struct.pack("<1024f", *[0.5 * math.sin(i * 0.2) for i in range(1024)]))
    t0 = time.time()
    print(ws.wait_text("trx:0,false;", timeout=0.3), f"(unkeyed {1000 * (time.time() - t0):.0f} ms after the frame)")
    if PLAYED:
        assert len(played()) == 0, f"{len(played())} samples of a misread frame played"
        print("misread frame: nothing played")

    ws.send(9, b"hello")
    end = time.time() + 2
    while not ws.pongs and time.time() < end:
        op, p = ws.frame()
        if op == 0xA:
            ws.pongs.append(p)
    assert ws.pongs and ws.pongs[0] == b"hello", "no pong"
    print("pong ok")
    ws.send(8, struct.pack(">H", 1000))
    browser_audio()
    formats()
    wsjtx_open()
    wsjtx_slow_link()
    funnel()
    print("ALL OK")


# What Tailscale's proxy adds to a request from the internet through Funnel (tailscaled sets the first itself)
FUNNEL = "Tailscale-Funnel-Request: ?1\r\nX-Forwarded-For: 203.0.113.7\r\nX-Forwarded-Proto: https\r\n"


REVOKED = os.environ.get("TCI_TEST_REVOKED")  # while it exists, the stub's paired browser is revoked
TOKEN, LISTENER = "0123456789abcdef0123456789abcdef", "11111111111111111111111111111111"


def nothing_for(ws, seconds):
    """No frame at all for seconds: a close counts as none; what came if anything"""
    got, end = [], time.time() + seconds
    ws.s.settimeout(0.05)
    try:
        while time.time() < end:
            try:
                op, p = ws.frame()
                got.append((op, p[:40]))
            except (socket.timeout, TimeoutError):
                pass
    except RuntimeError:
        pass
    ws.s.settimeout(5)
    return got


def closed_within(ws, seconds):
    end = time.time() + seconds
    ws.s.settimeout(0.05)
    try:
        while time.time() < end:
            try:
                ws.frame()
            except (socket.timeout, TimeoutError):
                pass
    except (RuntimeError, OSError):
        return True
    finally:
        try:
            ws.s.settimeout(5)
        except OSError:
            pass
    return False


def funnel_refused(addr="203.0.113.7"):
    try:
        WS(headers=FUNNEL.replace("203.0.113.7", addr))
    except (RuntimeError, OSError):
        return True
    return False


def funnel():
    """Issue #58: a funnel made by hand ("tailscale funnel 50001") put TCI, which has no login, on the
    internet. Over Funnel (tailscaled's header) only the browser Panel's own sound and microphone go:
    nothing before "auth TOKEN" of a browser paired at home, within 5 s; then the audio commands and a
    key with its audio for a session that may transmit; one at a time; closed when its session is
    revoked; wrong keys pause Funnel. A request from the tailnet (Tailscale's proxy, no Funnel header)
    is taken as before"""
    ws = WS(headers=FUNNEL)
    t0 = time.time()
    got = nothing_for(ws, 2)
    assert not got, f"over Funnel before a key: {got}"
    assert closed_within(ws, 4.5), "over Funnel without a key: not closed"
    print(f"funnel: nothing before a key, closed after {time.time() - t0:.1f} s")

    ws = WS(headers=FUNNEL)
    ws.text("audio_start:0;")
    assert closed_within(ws, 1) and not ws.binary, "over Funnel, a command first: not closed"
    print("funnel: a command before the key: closed, unanswered")

    ws = WS(headers=FUNNEL)
    ws.text("auth:" + TOKEN + ";")
    greeting = []
    while not greeting or greeting[-1] != "ready;":
        op, p = ws.frame()
        if op == 1:
            greeting.append(p.decode())
    print(f"funnel: the key of a browser paired at home: the greeting ({len(greeting)} lines)")
    assert funnel_refused("198.51.100.2"), "a second over Funnel was taken"
    print("funnel: a second one over Funnel refused")
    ws.text("vfo:0,0,14250000;modulation:0,cw;drive:0,100;")
    got = nothing_for(ws, 0.6)
    assert not [p for op, p in got if op == 1 and p.startswith((b"vfo:", b"modulation:", b"drive:"))], got
    print("funnel: tuning, mode and drive left unanswered and undone")
    ws.text("audio_samplerate:24000;audio_stream_sample_type:int16;audio_stream_channels:1;"
            "audio_stream_samples:512;audio_start:0;")
    print("funnel:", ws.wait_text("audio_start:0;"), "- RX audio frames:", len(rx_frames(ws, 0.5)))
    ws.text("trx:0,true;")
    print("funnel: the radio's own microphone:", ws.wait_text("trx:0,false;"))
    ws.text("trx:0,true,tci;")
    print("funnel: keyed with the page's audio:", ws.wait_text("trx:0,true;"))
    for _ in range(5):
        ws.wait_chrono()
        ws.send(2, struct.pack("<8I", 0, 24000, 0, 0, 0, 512, 2, 1) + bytes(32) + bytes(1024))
    ws.text("trx:0,false;")
    print("funnel: unkeyed:", ws.wait_text("trx:0,false;"))
    open(REVOKED, "w").close()
    t0 = time.time()
    assert closed_within(ws, 3.5), "over Funnel, its session revoked: not closed"
    os.remove(REVOKED)
    print(f"funnel: its session revoked at home: closed after {time.time() - t0:.1f} s")

    ws = WS(headers=FUNNEL)
    ws.text("auth " + LISTENER)
    ws.wait_text("ready;")
    ws.text("trx:0,true,tci;")
    print("funnel: a session that may not transmit, keying:", ws.wait_text("trx:0,false;"))
    ws.send(8, struct.pack(">H", 1000))

    for i in range(3):
        ws = WS(headers=FUNNEL.replace("203.0.113.7", f"198.51.100.{10 + i}"))
        ws.text("auth:" + "%032x" % (i + 7) + ";")
        assert closed_within(ws, 1), "a wrong key over Funnel: not closed"
    assert funnel_refused("198.51.100.20"), "Funnel taken after wrong keys"
    print("funnel: three wrong keys (the stub's rule): Funnel refused")
    ws = WS(headers="X-Forwarded-For: 100.101.102.104\r\nTailscale-User-Login: op@example.com\r\n")
    print("tailnet through the proxy:", ws.wait_text("protocol:"))
    ws.send(8, struct.pack(">H", 1000))
    ws = WS()
    print("the LAN:", ws.wait_text("protocol:"))
    ws.send(8, struct.pack(">H", 1000))


def rx_frames(ws, seconds):
    """The RX audio frames of the next seconds"""
    rx, end = [], time.time() + seconds
    while time.time() < end:
        op, p = ws.frame()
        if op == 2 and struct.unpack("<8I", p[:32])[6] == 1:
            rx.append(p)
    assert rx, "no RX audio frame"
    return rx


def browser_audio():
    """The browser pages ask for the format they play, all four, before audio_start"""
    ws = WS()
    ws.wait_text("ready;")
    ws.text("audio_samplerate:24000;audio_stream_sample_type:int16;audio_stream_channels:1;"
            "audio_stream_samples:512;audio_start:0;")
    print(ws.wait_text("audio_start:0;"))
    rx = rx_frames(ws, 0.5)
    hdr = struct.unpack("<8I", rx[0][:32])
    assert len(rx[0]) == 64 + 1024 and hdr[1:3] == (24000, 0) and hdr[5:8] == (512, 1, 1), hdr
    print(f"browser: {len(rx[0])} bytes, rate {hdr[1]}, format {hdr[2]}, length {hdr[5]}, ch {hdr[7]}")
    ws.send(8, struct.pack(">H", 1000))


def formats():
    """What the server has not got is answered with what it is, out of range is limited, and the
    frame says what it holds: int24 stereo at 48 kHz, 100 samples a frame"""
    ws = WS()
    ws.wait_text("ready;")
    ws.text("audio_samplerate:12000;")
    print(ws.wait_text("audio_samplerate:"), end=" ")
    ws.text("audio_stream_channels:7;")
    print(ws.wait_text("audio_stream_channels:"), end=" ")
    ws.text("audio_stream_samples:50;")
    print(ws.wait_text("audio_stream_samples:"), end=" ")
    ws.text("audio_stream_sample_type:INT24;")
    print(ws.wait_text("audio_stream_sample_type:"), end=" ")
    ws.text("audio_stream_sample_type:int8;")
    print(ws.wait_text("audio_stream_sample_type:"))
    ws.text("audio_start:0;")
    ws.wait_text("audio_start:0;")
    rx = rx_frames(ws, 0.6)
    hdr = struct.unpack("<8I", rx[0][:32])
    print(f"int24: {len(rx[0])} bytes, rate {hdr[1]}, format {hdr[2]}, length {hdr[5]}, ch {hdr[7]}")
    assert len(rx[0]) == 64 + 600 and hdr[1:3] == (48000, 1) and hdr[5:8] == (200, 1, 2), hdr
    left = []
    for f in rx[2:]:
        b = f[64:]
        left += [int.from_bytes(b[i:i + 3], "little", signed=True) / 8388608 for i in range(0, len(b), 6)]
        assert b[0::6] == b[3::6] and b[1::6] == b[4::6] and b[2::6] == b[5::6], "left and right differ"
    tone_check("int24 48 kHz stereo, 100 a frame", left[:24000], 1000, 48000, 8000 / 32768, image=23000)
    ws.send(8, struct.pack(">H", 1000))


def wsjtx_open():
    """What WSJT-X's TCI client ("TCI Client RX1", Transceiver/TCITransceiver.cpp, the same in
    JTDX) does on Test CAT, read as its parser reads it: do_start collects what the server says
    for two seconds, sends split_enable:false;, and stops with "TCI SDR is not switched on"
    unless a start; came; then the audio, the sensors, a frequency and a mode. At the end its
    rig power off sends stop;."""
    ws = WS()
    st = {"power": False, "esdr3": False, "rx": None, "mode": None, "split": None, "audio": False, "trx": None}
    rx, chronos, tx = [], [], {"phase": 0.0, "frames": 0, "keyed": True}

    def take(text):     # onMessageReceived: ';' between the commands, ':' then ','
        for cmd in [c for c in text.split(";") if c]:
            parts = [x for x in cmd.split(":") if x]
            name, args = parts[0], [a for a in parts[-1].split(",") if a]
            if name == "start":
                st["power"] = True
            elif name == "stop":
                st["power"] = False
            elif name == "protocol":
                st["esdr3"] = args[0] == "ExpertSDR3"
            elif name == "vfo" and args[:2] == ["0", "0"]:
                st["rx"] = args[2]
            elif name == "modulation" and args[0] == "0":
                st["mode"] = (args[2] if args[1] == "0" else args[1]).lower() if st["esdr3"] else args[1]
            elif name == "split_enable" and args[0] == "0":
                st["split"] = args[1]
            elif name == "audio_start" and args[0] == "0":
                st["audio"] = True
            elif name == "audio_stop" and args[0] == "0":
                st["audio"] = False
            elif name == "trx" and args[0] == "0":
                st["trx"] = args[1] == "true"

    def binary(p):      # onBinaryReceived
        h = struct.unpack("<16I", p[:64])
        if h[6] == 1 and st["audio"]:
            rx.append(p)    # writeAudioData(data, length): length floats, left of each pair taken
        elif h[6] == 3:
            # a frame sized for length pairs, header copied (its channel word never written),
            # filled with length floats: length / 2 pairs, left and right the same
            chronos.append((time.time(), h))
            n = h[5]
            pairs = []
            for _ in range(n // 2):
                v = 0.5 * math.sin(tx["phase"])
                tx["phase"] += 2 * math.pi * 1500 / 48000
                pairs += [v, v]
            if tx["keyed"]:     # what went before its trx:0,false; is played, what crosses it not
                tx["frames"] += n // 2
            head = struct.pack("<8I", h[0], h[1], h[2], 0, 0, n, 2, 0xDEADBEEF) + os.urandom(32)
            ws.send(2, head + struct.pack(f"<{n}f", *pairs) + os.urandom(4 * n))

    def collect(seconds, until=None):
        end = time.time() + seconds
        while time.time() < end and not (until and until()):
            ws.s.settimeout(max(0.01, end - time.time()))
            try:
                op, p = ws.frame()
            except socket.timeout:
                break
            if op == 1:
                take(p.decode())
            elif op == 2:
                binary(p)
        ws.s.settimeout(5)

    collect(2.0)                                    # mysleep6(2000)
    ws.text("split_enable:false;")
    assert st["power"], "WSJT-X: TCI SDR is not switched on"
    assert st["esdr3"] and st["rx"] and st["mode"], st
    print(f"wsjtx: open, power on, rx {st['rx']}, mode {st['mode']}")
    ws.text("audio_start:0;")
    collect(0.5, lambda: st["audio"])               # mysleep6(500)
    assert st["audio"], "WSJT-X: TCI Audio could not be switched on"
    assert st["split"] == "false", st
    ws.text("rx_sensors_enable:true,500;")
    ws.text("tx_sensors_enable:false,500;")
    ws.text("vfo:0,0,7074000;")                     # do_frequency: mysleep7(2000)
    collect(2.0, lambda: st["rx"] == "7074000")
    assert st["rx"] == "7074000", f"WSJT-X: TCI failed set rxfreq ({st['rx']})"
    ws.text("modulation:0,digu;")                   # mysleep8(1000)
    collect(1.0, lambda: st["mode"] == "digu")
    assert st["mode"] == "digu", f"WSJT-X: TCI failed set mode ({st['mode']})"
    print(f"wsjtx: tuned {st['rx']}, mode {st['mode']}")

    # It asked for nothing: TCI's defaults, 48 kHz float32 stereo, 2048 pairs a frame
    collect(1.5)                                    # a period's audio, or the start of one
    hdr = struct.unpack("<8I", rx[0][:32])
    print(f"wsjtx RX audio: {len(rx)} frames of {len(rx[0])} bytes, rate {hdr[1]}, format {hdr[2]}, "
          f"length {hdr[5]}, ch {hdr[7]}")
    assert len(rx[0]) == 64 + 4096 * 4 and hdr[1:3] == (48000, 3) and hdr[5:8] == (4096, 1, 2), hdr
    left = []
    for f in rx[1:]:
        v = struct.unpack("<4096f", f[64:])
        assert v[0::2] == v[1::2], "left and right differ"
        left += v[0::2]
    rest = tone_check("wsjtx RX, float32 48 kHz stereo", left[:48000], 1000, 48000, 8000 / 32768, image=23000)
    assert rest < 10 ** (-70 / 20)

    # Keyed as WSJT-X keys, in the digu its Mode Data/Pkt set (#17), every TX_CHRONO answered
    assert st["mode"] == "digu", st["mode"]
    ws.text("trx:0,true,tci;")
    collect(1.2)
    assert st["trx"] is True, "WSJT-X: TCI failed to set ptt"
    tx["keyed"] = False
    ws.text("trx:0,false;")
    collect(1.0, lambda: st["trx"] is False)
    assert st["trx"] is False
    h = chronos[0][1]
    span = (chronos[-1][0] - chronos[0][0]) / (len(chronos) - 1)
    print(f"wsjtx TX_CHRONO: {len(chronos)}, rate {h[1]}, format {h[2]}, length {h[5]}, ch {h[7]}, "
          f"every {1000 * span:.2f} ms")
    assert h[1:3] == (48000, 3) and h[5:8] == (2048, 3, 2), h
    assert abs(span - 1024 / 48000) < 0.002, span
    if PLAYED:
        out = played()
        want = tx["frames"] * 44100 / 48000
        print(f"wsjtx TX played: {len(out)} samples at 44.1 kHz for {tx['frames']} pairs at 48 kHz ({want:.0f})")
        assert abs(len(out) - want) < 3, (len(out), want)
        # the level: 0.5 of full scale, as int16 through x 32767, the linear interpolation's
        # sinc^2 at 1.5 kHz of 48 kHz (-0.03 dB); beside it, that interpolation's own products
        rest = tone_check("wsjtx TX, the left channel played", [v / 32768 for v in out[2000:2000 + 29400]], 1500,
                          44100, 0.5 * 32767 / 32768 * 0.9968)
        assert rest < 10 ** (-50 / 20)
    ws.text("audio_stop:0;")
    st["power"] = None
    ws.text("stop;")                                # rig power off: the radio stays on
    collect(1.0, lambda: st["power"] is not None)
    assert st["power"] is True, "a stop; is answered with the radio as it is: on"
    ws.send(8, struct.pack(">H", 1000))
    print("wsjtx: closed, the radio still on")


def wsjtx_slow_link():
    """WSJT-X keyed on a link slower than its RX audio (385 kB/s at TCI's defaults): what the
    radio queues for it fills with RX audio, and the TX_CHRONO asking for the next frame of the
    over must not wait behind that, or the answer comes later than the 500 ms the radio waits
    for TX audio and it unkeys in the middle of the over. Read at 250 kB/s through a small
    receive buffer with the queue full: still keyed after 3 s, every chrono answered and
    played. The queue is a ring sent in pieces, the chronos go in between its frames: every
    frame read must be whole and as it was queued."""
    rate, keyed_s = 250000, 3.0
    ws = WS(rcvbuf=4096)
    ws.wait_text("ready;")
    ws.text("modulation:0,usb;")
    ws.wait_text("modulation:0,usb;")
    ws.text("audio_start:0;")
    ws.wait_text("audio_start:0;")
    ws.slow(rate)
    rx, unkeyed, chronos, pairs = [0, 0], None, [], 0

    def read(seconds, answer):
        nonlocal unkeyed, pairs
        end = time.time() + seconds
        while time.time() < end:
            op, p = ws.frame()
            if op == 1 and p.startswith(b"trx:0,false;") and unkeyed is None:
                unkeyed = time.time()
            elif op == 2 and struct.unpack("<8I", p[:32])[6] == 1:
                h = struct.unpack("<8I", p[:32])
                assert len(p) == 64 + 4096 * 4 and h[1:3] == (48000, 3) and h[5:8] == (4096, 1, 2), (len(p), h)
                v = struct.unpack("<4096I", p[64:])
                assert v[0::2] == v[1::2], "a frame cut or mixed: left and right differ"
                rx[0] += 1
                rx[1] += len(p)
            elif op == 2 and struct.unpack("<8I", p[:32])[6] == 3 and answer:
                h = struct.unpack("<8I", p[:32])
                assert len(p) == 64 and h[1:3] == (48000, 3) and h[5:8] == (2048, 3, 2), (len(p), h)
                chronos.append(time.time())
                n = h[5]        # WSJT-X's frame: sized for n pairs, n floats of audio, then the rest
                ws.send(2, struct.pack("<8I", h[0], h[1], h[2], 0, 0, n, 2, 0xDEADBEEF) + bytes(32) + bytes(8 * n))
                pairs += n // 2

    read(2.5, False)                                # the radio's queue fills with RX audio
    print(f"slow link: RX audio read at {rx[1] / 2.5 / 1000:.0f} kB/s of the stream's 385 for 2.5 s, "
          f"{rx[0]} frames whole")
    rx[:] = [0, 0]
    t_key = time.time()
    ws.text("trx:0,true,tci;")
    read(keyed_s, True)
    gaps = [b - a for a, b in zip(chronos, chronos[1:])]
    out = (f"slow link, keyed: {len(chronos)} TX_CHRONO in {keyed_s:.0f} s, the first "
           f"{1000 * (chronos[0] - t_key) if chronos else -1:.0f} ms after the key, the longest gap "
           f"{1000 * max(gaps) if gaps else -1:.0f} ms, the last {1000 * (t_key + keyed_s - chronos[-1]) if chronos else -1:.0f} "
           f"ms before the end; RX audio meanwhile {rx[1] / keyed_s / 1000:.0f} kB/s")
    print(out)
    assert unkeyed is None, f"unkeyed {unkeyed - t_key:.2f} s into the over: " + out
    assert len(chronos) >= 0.9 * keyed_s * 48000 / 1024, out
    assert chronos[0] - t_key < 0.25 and max(gaps) < 0.25 and t_key + keyed_s - chronos[-1] < 0.25, out
    ws.slow(None)
    ws.text("trx:0,false;")
    ws.wait_text("trx:0,false;", timeout=3)
    if PLAYED:
        out, want = played(), pairs * 44100 / 48000
        print(f"slow link: played {len(out)} samples at 44.1 kHz for the {pairs} pairs answered ({want:.0f})")
        assert abs(len(out) - want) < 3, (len(out), want)
    ws.send(8, struct.pack(">H", 1000))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("FAIL:", e)
        sys.exit(1)
