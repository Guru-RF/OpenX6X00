#!/usr/bin/env python3
"""Host test of the Winlink modem socket (src/winlink.c), protocol versions 1 and 2.

Starts winlink_host (src/winlink.c on the stubs, its receiver a 1 kHz tone) and speaks to it as a modem does:
ardopcf's version 1 (12000 Hz, keys of 30 s) and version 2 (the rate and the longest key in the hello). For each:
the receiver's R messages (their size, how many a second, the tone in them: the GUI records at the session's
rate), a transmission of a 1 kHz tone (what the player got: as long as what was sent and still 1 kHz, so it
was resampled from the session's rate), the key limit, the hellos refused (rate, version), messages before
the hello (ignored) and a second hello (ignored). Prints PROTO OK at the end; exits 1 at the first failure.
Run from `make` (make test).
"""

import array
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HDR = struct.Struct("=BBH")
PLAY_RATE = 44100


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def check(cond, msg):
    if not cond:
        fail(msg)
    print("  ok:", msg)


def tone_hz(samples, rate):
    """The tone's frequency from its zero crossings, the first and last 50 ms left out."""
    cut = rate // 20
    s = samples[cut:len(samples) - cut]
    if len(s) < rate // 10:
        return 0.0
    crossings = sum(1 for a, b in zip(s, s[1:]) if (a < 0) != (b < 0))
    return crossings / 2 / (len(s) / rate)


class Host:
    """winlink_host on its own socket, its log and what it played in a temporary directory."""

    def __init__(self, tmp, name="a", rig_port=14532):
        self.sock = os.path.join(tmp, name + ".sock")
        self.played = os.path.join(tmp, name + ".played")
        self.log = os.path.join(tmp, name + ".log")
        env = dict(os.environ, WL_SOCK=self.sock, WL_RIG_PORT=str(rig_port), WL_PLAYED=self.played, WL_TONE="1000")
        self.p = subprocess.Popen([os.path.join(HERE, "winlink_host")], env=env, stderr=open(self.log, "w"))
        for _ in range(100):
            if os.path.exists(self.sock):
                return
            time.sleep(0.05)
        fail("winlink_host made no socket")

    def text(self):
        with open(self.log) as f:
            return f.read()

    def wait_log(self, text, count=1, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.text().count(text) >= count:
                return True
            time.sleep(0.05)
        return False

    def played_count(self):
        try:
            with open(self.played + ".log") as f:
                return len(f.readlines())
        except FileNotFoundError:
            return 0

    def wait_played(self, count, timeout=15.0):
        """The transmission number count once it ended: its samples and whether it was stopped."""
        end = time.time() + timeout
        while time.time() < end:
            if self.played_count() >= count:
                with open(self.played + ".log") as f:
                    stopped = f.readlines()[count - 1].split()[3] == "1"
                a = array.array("h")
                with open(self.played, "rb") as f:
                    a.frombytes(f.read())
                return a, stopped
            time.sleep(0.05)
        fail(f"no transmission number {count} within {timeout} s")

    def stop(self):
        self.p.kill()
        self.p.wait()


class Modem:
    """A modem's end of the socket."""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.s.connect(path)

    def send(self, kind, arg=0, samples=()):
        self.s.send(HDR.pack(ord(kind), arg, len(samples)) + array.array("h", samples).tobytes())

    def recv(self, timeout):
        """One message (kind, arg, samples), None when nothing came, b'' when the GUI closed."""
        self.s.settimeout(timeout)
        try:
            m = self.s.recv(65536)
        except socket.timeout:
            return None
        if not m:
            return b""
        kind, arg, n = HDR.unpack_from(m)
        a = array.array("h")
        a.frombytes(m[HDR.size:])
        return chr(kind), arg, a

    def closed(self, timeout):
        """True when the GUI closes the connection within timeout, what it sent before read and dropped."""
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(max(0.01, end - time.time()))
            if m == b"":
                return True
            if m is None:
                return False
        return False

    def receiver(self, seconds):
        """The R messages of `seconds`: how many, the sizes seen, all their samples."""
        sizes, samples, msgs = set(), array.array("h"), 0
        end = time.time() + seconds
        while time.time() < end:
            m = self.recv(0.2)
            if m == b"":
                fail("the GUI closed the connection")
            if m and m[0] == "R":
                msgs += 1
                sizes.add(len(m[2]))
                samples.extend(m[2])
        return msgs, sizes, samples

    def transmit(self, rate, seconds, hz=1000.0, amplitude=10000):
        """P 1, a tone in 20 ms T messages on the wall clock and 100 ms ahead of it (as ardopcf), P 0."""
        import math
        block = rate // 50
        total = int(rate * seconds)
        tone = [int(amplitude * math.sin(2 * math.pi * hz * i / rate)) for i in range(total)]
        self.send("P", 1)
        t0, sent = time.time(), 0
        while sent < total:
            if sent < (time.time() - t0) * rate + rate // 10:
                self.send("T", 0, tone[sent:sent + block])
                sent += block
            else:
                time.sleep(0.005)
        self.send("P", 0)

    def close(self):
        self.s.close()


def session(host, gone):
    """Waits until the GUI has seen `gone` clients leave."""
    if not host.wait_log("the modem gone", gone):
        fail("the GUI did not see the modem go")


def main():
    with tempfile.TemporaryDirectory(prefix="winlink-test-") as tmp:
        host = Host(tmp)
        tx = 0      # transmissions so far
        gone = 0    # clients gone so far
        try:
            print("version 1, as ardopcf")
            m = Modem(host.sock)
            m.send("H", 1)
            check(host.wait_log("the modem connected, protocol 1, 12000 Hz, keys of 30 s at most"),
                  "the hello: 12000 Hz, keys of 30 s")
            msgs, sizes, rx = m.receiver(2.0)
            check(sizes == {240}, f"R messages of 240 samples ({sizes})")
            check(90 <= msgs <= 110, f"about 50 a second ({msgs} in 2 s)")
            hz = tone_hz(rx, 12000)
            check(abs(hz - 1000) < 20, f"the receiver's 1 kHz tone at 12000 Hz ({hz:.0f} Hz)")
            m.transmit(12000, 1.0)
            tx += 1
            played, stopped = host.wait_played(tx)
            n = len(played)
            check(not stopped and 0.97 * PLAY_RATE <= n <= 1.03 * PLAY_RATE,
                  f"1 s sent, 1 s played ({n} samples at 44.1 kHz)")
            hz = tone_hz(played, PLAY_RATE)
            check(abs(hz - 1000) < 15, f"the 1 kHz tone played at 1 kHz ({hz:.0f} Hz)")
            m.close()
            gone += 1
            session(host, gone)

            print("version 2, 8000 Hz, keys of 40 s (x6x00-mercury)")
            m = Modem(host.sock)
            m.send("H", 2, [8000, 40])
            check(host.wait_log("the modem connected, protocol 2, 8000 Hz, keys of 40 s at most"),
                  "the hello: 8000 Hz, keys of 40 s")
            msgs, sizes, rx = m.receiver(2.0)
            check(sizes == {160}, f"R messages of 160 samples ({sizes})")
            check(90 <= msgs <= 110, f"about 50 a second ({msgs} in 2 s)")
            hz = tone_hz(rx, 8000)
            check(abs(hz - 1000) < 20, f"the receiver's 1 kHz tone at 8000 Hz ({hz:.0f} Hz)")
            m.transmit(8000, 1.0)
            tx += 1
            played, stopped = host.wait_played(tx)
            n = len(played)
            check(not stopped and 0.97 * PLAY_RATE <= n <= 1.03 * PLAY_RATE,
                  f"1 s sent, 1 s played ({n} samples at 44.1 kHz)")
            hz = tone_hz(played, PLAY_RATE)
            check(abs(hz - 1000) < 15, f"the 1 kHz tone played at 1 kHz ({hz:.0f} Hz)")
            m.close()
            gone += 1
            session(host, gone)

            print("version 2, 12000 Hz, keys of 35 s")
            m = Modem(host.sock)
            m.send("H", 2, [12000, 35])
            check(host.wait_log("protocol 2, 12000 Hz, keys of 35 s at most"), "the hello: 12000 Hz, keys of 35 s")
            msgs, sizes, rx = m.receiver(1.0)
            check(sizes == {240}, f"R messages of 240 samples ({sizes})")
            m.close()
            gone += 1
            session(host, gone)

            print("version 2: the key limit is the hello's")
            m = Modem(host.sock)
            m.send("H", 2, [8000, 2])
            check(host.wait_log("protocol 2, 8000 Hz, keys of 2 s at most"), "the hello: keys of 2 s")
            m.transmit(8000, 3.0)
            tx += 1
            played, stopped = host.wait_played(tx)
            n = len(played)
            check(stopped and 1.9 * PLAY_RATE <= n <= 2.2 * PLAY_RATE, f"3 s sent, stopped after 2 s ({n} samples)")
            check(host.wait_log("Winlink: TX stopped after 2 s"), "the message: TX stopped after 2 s")
            m.close()
            gone += 1
            session(host, gone)

            print("version 2: a longer key than the GUI allows")
            m = Modem(host.sock)
            m.send("H", 2, [8000, 99])
            check(host.wait_log("the modem may key for 99 s, the GUI stops a key after 40"), "said in the log")
            check(host.wait_log("protocol 2, 8000 Hz, keys of 40 s at most", 2), "keys of 40 s, the GUI's most")
            m.close()
            gone += 1
            session(host, gone)

            print("hellos refused")
            for hello, said in (([8000 * 2, 40], "asks for 16000 Hz and keys of 40 s, not spoken here"),
                                ([8000, 0], "asks for 8000 Hz and keys of 0 s, not spoken here"),
                                (None, "the modem speaks protocol 3, the GUI 1 and 2")):
                m = Modem(host.sock)
                if hello is None:
                    m.send("H", 3)
                else:
                    m.send("H", 2, hello)
                check(m.closed(3.0), f"connection closed: {said}")
                check(host.wait_log(said), "said in the log")
                m.close()

            print("before the hello")
            m = Modem(host.sock)
            m.send("P", 1)
            m.send("T", 0, [1000] * 1200)
            m.send("P", 0)
            msgs, sizes, rx = m.receiver(1.5)
            check(msgs == 0 and host.played_count() == tx, "no receiver, nothing keyed")
            m.send("H", 1)
            check(host.wait_log("protocol 1, 12000 Hz, keys of 30 s at most", 2), "then the hello")
            msgs, sizes, rx = m.receiver(0.5)
            check(msgs > 10, f"and the receiver ({msgs} R)")
            m.send("H", 2, [8000, 40])
            check(host.wait_log("a second hello from the modem, ignored"), "a second hello: ignored")
            msgs, sizes, rx = m.receiver(0.5)
            check(sizes == {240}, "the session's rate kept")

            print("a new connection replaces the old one")
            m2 = Modem(host.sock)
            m2.send("H", 2, [8000, 40])
            check(host.wait_log("a new modem connection replaces the old one"), "said in the log")
            check(m.closed(3.0), "the old one closed")
            msgs, sizes, rx = m2.receiver(0.5)
            check(sizes == {160}, "the new one at its own rate")
            m2.close()
            m.close()
        finally:
            host.stop()
    print("PROTO OK")


if __name__ == "__main__":
    main()
