#!/usr/bin/env python3
"""Host test of the Winlink modem socket (src/winlink.c), protocol versions 1 and 2.

Starts winlink_host (src/winlink.c on the stubs, its receiver a 1 kHz tone) and speaks to it as a modem does:
ardopcf's version 1 (12000 Hz, keys of 30 s) and version 2 (the rate and the longest key in the hello). For each:
the receiver's R messages (their size, how many a second, the tone in them: the GUI records at the session's
rate), a transmission of a 1 kHz tone (what the player got: as long as what was sent and still 1 kHz, so it
was resampled from the session's rate), the key limit, the hellos refused (rate, version), messages before
the hello (ignored) and a second hello (ignored). A digital app opened during a key: stopped, the next key
refused. An SWR trip during a key: the hold on that band and near its frequency, another band keys (#147); one
counted as a key ends is that key's, one counted seconds later another transmission's, no hold. And the radio
file the Winlink page reads: Winlink's cap of 7 W, 5 W with the X6200 on its battery.
Prints PROTO OK at the end; exits 1 at the first failure.
Run from `make` (make test).
"""

import array
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HDR = struct.Struct("=BBH")
PLAY_RATE = 44100
RADIO_FILE = os.path.join(HERE, "build", "radio")     # WINLINK_RADIO_FILE in stub_env.h, from HERE


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

    def __init__(self, tmp, name="a", rig_port=14532, env=None):
        self.sock = os.path.join(tmp, name + ".sock")
        self.played = os.path.join(tmp, name + ".played")
        self.log = os.path.join(tmp, name + ".log")
        self.app = os.path.join(tmp, name + ".app")
        self.swr = os.path.join(tmp, name + ".swr")
        self.swr_late = os.path.join(tmp, name + ".swr_late")
        self.rig_port = rig_port
        env = dict(os.environ, WL_SOCK=self.sock, WL_RIG_PORT=str(rig_port), WL_PLAYED=self.played, WL_TONE="1000",
                   WL_APP=self.app, WL_SWR=self.swr, WL_SWR_LATE=self.swr_late, **(env or {}))
        # here: the radio file is build/radio (stub_env.h)
        self.p = subprocess.Popen([os.path.join(HERE, "winlink_host")], env=env, stderr=open(self.log, "w"), cwd=HERE)
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

    def open_app(self, name):
        """The digital app the GUI has open (main_screen_digital_app), None for none"""
        if name is None:
            os.remove(self.app)
            return
        with open(self.app + ".tmp", "w") as f:
            f.write(name)
        os.rename(self.app + ".tmp", self.app)

    def swr_trip(self):
        """The SWR protection stops the key on the air now, or the next one (WL_SWR)"""
        open(self.swr, "w").close()

    def swr_trip_late(self):
        """A trip the GUI counts at its next look, outside any key (WL_SWR_LATE)"""
        open(self.swr_late, "w").close()

    def tune(self, hz):
        """Pat's QSY through the rigctld: its answer, "RPRT 0" once the radio is there"""
        with socket.create_connection(("127.0.0.1", self.rig_port), timeout=3) as s:
            s.sendall(f"F {hz}\n".encode())
            return s.recv(64).decode().strip()

    @staticmethod
    def radio():
        """The radio file's lines, {} while there is none"""
        try:
            with open(RADIO_FILE) as f:
                return dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
        except FileNotFoundError:
            return {}

    def wait_radio(self, want, timeout=5.0):
        """Whether the radio file (the Winlink page's) came to have the values in want, a dict of its lines, and
        those lines as last read"""
        end, radio = time.time() + timeout, {}
        while time.time() < end:
            radio = self.radio()
            if all(radio.get(k) == v for k, v in want.items()):
                break
            time.sleep(0.1)
        got = {k: radio.get(k) for k in want}
        return got == want, got

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
            ok, radio = host.wait_radio({"modem": "connected", "pwr": "5.0", "cap": "7.0", "cap_battery": "0"})
            check(ok, f"the radio file: PWR 5 W, Winlink's cap 7 W, not the battery's ({radio})")
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

            print("a digital app opened during a key")
            m = Modem(host.sock)
            m.send("H", 2, [12000, 30])
            check(host.wait_log("protocol 2, 12000 Hz, keys of 30 s at most"), "the hello")
            opens = threading.Timer(1.0, host.open_app, ("FT4",))
            opens.start()
            m.transmit(12000, 3.0)
            opens.join()
            tx += 1
            played, stopped = host.wait_played(tx)
            n = len(played)
            check(stopped and 0.8 * PLAY_RATE <= n <= 1.5 * PLAY_RATE, f"3 s sent, FT4 opened after 1 s: stopped there "
                  f"({n} samples)")
            check(host.wait_log("Winlink: TX stopped, FT4 is open"), "the message: TX stopped, FT4 is open")
            m.transmit(12000, 0.5)
            check(host.wait_log("winlink: not keyed, FT4 is open, close it"), "the next key refused")
            check(host.played_count() == tx, "and not keyed")
            host.open_app(None)
            m.transmit(12000, 0.5)
            tx += 1
            played, stopped = host.wait_played(tx)
            check(not stopped, f"FT4 closed: the next key goes out ({len(played)} samples)")
            gone = host.text().count("the modem gone")
            m.close()
            session(host, gone + 1)

            print("an SWR trip on 30 m: no TX on 30 m for 5 minutes, 40 m keys at once (#147)")
            m = Modem(host.sock)
            m.send("H", 1)
            check(host.wait_log("protocol 1, 12000 Hz, keys of 30 s at most", 3), "the hello")
            check(host.tune(10145000) == "RPRT 0", "Pat's QSY to 10.145.000 MHz, on 30 m")
            trips = threading.Timer(0.5, host.swr_trip)
            trips.start()
            m.transmit(12000, 2.0)
            trips.join()
            tx += 1
            played, stopped = host.wait_played(tx)
            n = len(played)
            check(stopped and n < 1.5 * PLAY_RATE, f"2 s sent, high SWR after 0.5 s: stopped there ({n} samples)")
            check(host.wait_log("winlink: SWR trip"), "the trip counted for Winlink")
            m.transmit(12000, 0.5)
            check(host.wait_log("winlink: not keyed, high SWR"), "the next key on 10.145.000 MHz: refused")
            check(host.played_count() == tx, "and not keyed")
            check(host.tune(10120000) == "RPRT 0", "Pat's QSY to 10.120.000 MHz, 30 m still")
            m.transmit(12000, 0.5)
            check(host.wait_log("winlink: not keyed, high SWR", 2), "refused there too")
            check(host.played_count() == tx, "and not keyed")
            check(host.tune(7102000) == "RPRT 0", "Pat's QSY to 7.102.000 MHz, on 40 m")
            m.transmit(12000, 0.5)
            tx += 1
            played, stopped = host.wait_played(tx)
            check(not stopped, f"40 m keys during the 30 m hold ({len(played)} samples)")
            check(host.wait_log("Winlink: no TX on 30m for 5 minutes, check the antenna"), "the hold said with its band")
            check(host.wait_log("winlink: not keyed, high SWR on 30m, the hold ends in 5 min", 2),
                  "each refusal names the band and the minutes left")
            ok, radio = host.wait_radio({"swr_hold_where": "on 30m", "swr_hold_here": "0"})
            check(ok and 240 < int(host.radio().get("swr_hold", 0)) <= 300,
                  f"the radio file: the hold on 30 m with its seconds left, not where the radio is ({radio})")
            check(host.tune(10145000) == "RPRT 0", "back to 10.145.000 MHz")
            ok, radio = host.wait_radio({"swr_hold_where": "on 30m", "swr_hold_here": "1"})
            check(ok, f"the radio file: the hold where the radio is ({radio})")
            m.transmit(12000, 0.5)
            check(host.wait_log("winlink: not keyed, high SWR on 30m", 3), "the hold is still on 30 m: refused")
            check(host.played_count() == tx, "and not keyed")

            print("an SWR trip on 7.250 MHz, outside the band table: the hold within 100 kHz of it")
            check(host.tune(7250000) == "RPRT 0", "Pat's QSY to 7.250.000 MHz")
            trips = threading.Timer(0.5, host.swr_trip)
            trips.start()
            m.transmit(12000, 2.0)
            trips.join()
            tx += 1
            played, stopped = host.wait_played(tx)
            check(stopped, f"stopped by the SWR protection ({len(played)} samples)")
            check(host.wait_log("Winlink: no TX near 7.250.000 MHz for 5 minutes, check the antenna"),
                  "the hold said with its frequency")
            check(host.tune(7300000) == "RPRT 0", "Pat's QSY to 7.300.000 MHz, 50 kHz from the trip")
            m.transmit(12000, 0.5)
            check(host.wait_log("winlink: not keyed, high SWR near 7.250.000 MHz, the hold ends in 5 min"),
                  "refused, near the trip")
            check(host.played_count() == tx, "and not keyed")
            check(host.tune(7400000) == "RPRT 0", "Pat's QSY to 7.400.000 MHz, 150 kHz from it")
            m.transmit(12000, 0.5)
            tx += 1
            played, stopped = host.wait_played(tx)
            check(not stopped, f"keys there ({len(played)} samples)")

            print("a trip counted as a key ends is that key's, one 2.5 s later another transmission's (#147)")
            host.swr_trip_late()
            m.transmit(12000, 0.5)
            check(host.wait_log("Winlink: no TX near 7.400.000 MHz for 5 minutes, check the antenna"),
                  "counted at once after the key on 7.400.000 MHz: the hold there")
            check(host.wait_log("winlink: not keyed, high SWR near 7.400.000 MHz", 1, 3), "and that key refused")
            check(host.played_count() == tx, "not keyed")
            check(host.tune(7102000) == "RPRT 0", "Pat's QSY to 7.102.000 MHz, 40 m")
            m.transmit(12000, 0.5)
            tx += 1
            played, stopped = host.wait_played(tx)
            check(not stopped, f"keys there ({len(played)} samples)")
            time.sleep(2.5)
            host.swr_trip_late()
            m.transmit(12000, 0.5)
            tx += 1
            played, stopped = host.wait_played(tx)
            check(not stopped, f"a trip 2.5 s after that key: not Winlink's, the next one keys ({len(played)} samples)")
            check(host.wait_log("another transmission's: no hold"), "the GUI's log says why")
            check("no TX on 40m" not in host.text(), "and no hold on 40 m said")
            m.close()
        finally:
            host.stop()

        print("the X6200 on its battery: PWR no higher than 5 W")
        host = Host(tmp, "b", rig_port=14533, env={"WL_PWR_MAX": "5"})
        try:
            m = Modem(host.sock)
            m.send("H", 1)
            ok, radio = host.wait_radio({"modem": "connected", "cap": "5.0", "cap_battery": "1"})
            check(ok, f"the radio file: Winlink's cap 5 W, the battery's ({radio})")
            m.close()
        finally:
            host.stop()

        print("a frequency the radio may not transmit on (#121): the key refused, the page told why")
        host = Host(tmp, "d", rig_port=14535, env={"WL_TX_NO": "outside the amateur bands (Settings > TX Cal)"})
        try:
            m = Modem(host.sock)
            m.send("H", 2, [12000, 30])
            check(host.wait_log("protocol 2, 12000 Hz"), "the hello")
            why = "14.105.000 MHz: outside the amateur bands (Settings > TX Cal)"
            ok, radio = host.wait_radio({"modem": "connected", "simplex": why})
            check(ok, f"the radio file before a key says why ({radio})")
            m.transmit(12000, 0.5)
            check(host.wait_log("winlink: not keyed, " + why), "the key refused, the log says why")
            ok, radio = host.wait_radio({"refused": why})
            check(ok and host.played_count() == 0, f"the page's refused line says it, nothing played ({radio})")
            m.close()
        finally:
            host.stop()

        print("no data from the base (#152): the key refused, the page told why")
        host = Host(tmp, "e", rig_port=14536, env={"WL_NO_DATA": "1"})
        try:
            m = Modem(host.sock)
            m.send("H", 2, [12000, 30])
            check(host.wait_log("protocol 2, 12000 Hz"), "the hello")
            why = "no data from the base"
            ok, radio = host.wait_radio({"modem": "connected", "simplex": why})
            check(ok, f"the radio file before a key says why ({radio})")
            m.transmit(12000, 0.5)
            check(host.wait_log("winlink: not keyed, " + why), "the key refused, the log says why")
            ok, radio = host.wait_radio({"refused": why})
            check(ok and host.played_count() == 0, f"the page's refused line says it, nothing played ({radio})")
            m.close()
        finally:
            host.stop()

        print("a transverter's TX power limit of 0.5 W: PWR no higher, and said as the transverter's (#104)")
        host = Host(tmp, "c", rig_port=14534, env={"WL_PWR_MAX": "0.5", "WL_PWR_XVTR": "0.5"})
        try:
            m = Modem(host.sock)
            m.send("H", 1)
            ok, radio = host.wait_radio({"modem": "connected", "cap": "0.5", "cap_battery": "0",
                                         "cap_transverter": "1"})
            check(ok, f"the radio file: Winlink's cap 0.5 W, the transverter's, not the battery's ({radio})")
            m.close()
        finally:
            host.stop()
    print("PROTO OK")


if __name__ == "__main__":
    main()
