#!/usr/bin/env python3
"""The SVXConnect app's helper as the app runs it, on this PC: svxconnect --stdio (tools/svxconnect/build.sh's native
build, with our patches) on a SOCK_SEQPACKET socket pair as src/voice_net.c starts it, against SVXConnect's own fake
reflector (build/fakerefl, from its tests) on 127.0.0.1. Nothing else is contacted: the configuration names
127.0.0.1 and the test fails on any other address in what the helper says. What the helper says is read by the app's
own parser (src/svx_proto.c, through svx_proto_dump) and checked there:

  - the login, the talkgroups with their priorities from the settings, the selection;
  - a talker on a talkgroup of higher priority pulls the selection over, its audio comes as 16 kHz records (first
    byte 0), the recent list has the over when it ends;
  - an over of ours: ptt on, audio records to the reflector, ptt off; the reflector takes the frames;
  - ptt on while someone talks on the talkgroup, or with none selected: refused, with why;
  - the lock, and choosing a talkgroup while locked;
  - tx_timeout_sec ends an over by itself;
  - the end of the socket ends the helper, with exit code 0;
  - --cert-status, and the error line when the certificate is missing.

    SVX_BIN=<a native svxconnect> python3 -I -B svx_helper_test.py    (default: build.sh's native build)
"""
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
DUMP = os.path.join(HERE, "svx_proto_dump")


def helper():
    b = os.environ.get("SVX_BIN")
    if not b:
        r = subprocess.run(["bash", os.path.join(HERE, "..", "svxconnect", "build.sh")], capture_output=True,
                           text=True)
        if r.returncode != 0:
            sys.exit("tools/svxconnect/build.sh failed:\n" + r.stderr[-3000:])
        b = r.stdout.strip()
    fr = os.path.join(os.path.dirname(b), "fakerefl")
    for p in (b, fr):
        if not os.access(p, os.X_OK):
            sys.exit(f"no {p}")
    return b, fr


BIN, FAKEREFL = helper()
FRAME = struct.pack("<320h", *[((i * 7919) % 2000) - 1000 for i in range(320)])    # 20 ms at 16 kHz


class Helper(unittest.TestCase):
    def setUp(self):
        self.run_dir = tempfile.mkdtemp(prefix="svx-helper.")
        self.addCleanup(shutil.rmtree, self.run_dir, True)
        self.pki = os.path.join(self.run_dir, "pki")
        os.makedirs(self.pki)
        self.fr = subprocess.Popen([FAKEREFL, self.pki, "TEST"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   text=True, bufsize=1)
        self.addCleanup(self.stop_fakerefl)
        first = self.fr.stdout.readline().split()
        self.assertEqual(first[0], "port")
        self.port = int(first[1])
        self.dump = subprocess.Popen([DUMP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.addCleanup(self.stop_dump)
        self.t0 = time.monotonic()
        self.state = {}
        self.events = []        # (event, why)
        self.fr_said = []
        self.audio_in = 0
        self.feeding = False    # pump sends a 20 ms frame of mic audio every 20 ms
        self.fed = 0.0
        self.p = None

    def stop_fakerefl(self):
        try:
            self.fr.stdin.write("quit\n")
            self.fr.stdin.flush()
            self.fr.wait(5)
        except (OSError, subprocess.TimeoutExpired):
            self.fr.kill()
            self.fr.wait()
        self.fr.stdin.close()
        self.fr.stdout.close()

    def stop_dump(self):
        self.dump.stdin.close()
        self.dump.wait(5)
        self.dump.stdout.close()

    def conf(self, **extra):
        c = {"callsign": "TEST", "email": "test@example.org", "reflector": "127.0.0.1", "port": self.port,
             "pki_dir": self.pki, "switchable": "8, 1745", "monitored": "8++, 1745+, 9990", "default_tg": 8,
             "tx_timeout_sec": 120, "ctl_fifo": "", "status_file": "", "log_file": "",
             "lock_file": os.path.join(self.run_dir, "lock")}
        c.update(extra)
        path = os.path.join(self.run_dir, "svxconnect.conf")
        with open(path, "w") as f:
            f.write("".join(f"{k} = {v}\n" for k, v in c.items()))
        return path

    def start(self, **extra):
        self.sock, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.err = os.path.join(self.run_dir, "stderr.txt")
        with open(self.err, "w") as e:
            self.p = subprocess.Popen([BIN, "--stdio", "-c", self.conf(**extra)], stdin=theirs, stdout=theirs, stderr=e)
        theirs.close()
        self.addCleanup(self.end)

    def end(self):
        self.sock.close()
        if self.p and self.p.poll() is None:
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()

    def feed(self, line):
        """A line of the helper's through the app's parser"""
        self.assertNotRegex(line, r"\b(?!127\.0\.0\.1\b)\d+\.\d+\.\d+\.\d+\b", "an address other than 127.0.0.1")
        ms = int((time.monotonic() - self.t0) * 1000)
        self.dump.stdin.write(f"{ms} {line}\n")
        self.dump.stdin.flush()
        out = self.dump.stdout.readline().rstrip("\n")
        self.state = dict(kv.split("=", 1) for kv in out.split("|"))
        if self.state["ev"] != "none":
            self.events.append((self.state["ev"], self.state["why"]))

    def pump(self, sec, until=None):
        """What the helper and the fake reflector say for sec seconds, or until until() holds"""
        end = time.monotonic() + sec
        while time.monotonic() < end:
            if until and until():
                return True
            if self.feeding and time.monotonic() - self.fed >= 0.02:
                self.sock.send(b"\x00" + FRAME)
                self.fed = time.monotonic()
            r, _, _ = select.select([self.sock, self.fr.stdout], [], [], 0.01 if self.feeding else 0.05)
            if self.sock in r:
                d = self.sock.recv(65536)
                if not d:
                    return until() if until else False
                if d[0] == 0:
                    self.assertEqual((len(d) - 1) % 2, 0)
                    self.audio_in += 1
                else:
                    for line in d.decode().splitlines():
                        self.feed(line)
            if self.fr.stdout in r:
                self.fr_said.append(self.fr.stdout.readline().strip())
        return bool(until and until())

    def fr_cmd(self, line):
        self.fr.stdin.write(line + "\n")
        self.fr.stdin.flush()

    def send(self, line):
        self.sock.send(line.encode() + b"\n")

    def connected(self):
        self.start()
        self.assertTrue(self.pump(10, lambda: self.state.get("conn") == "2" and self.state.get("host")), self.state)
        self.assertTrue(self.pump(3, lambda: "login" in self.fr_said))

    def test_session(self):
        self.connected()
        s = self.state
        self.assertEqual((s["host"], s["tg"], s["locked"]), ("127.0.0.1", "8", "0"))
        self.assertEqual(s["tgs"], "8:2:s,1745:1:s,9990:0:")
        self.assertEqual(s["cert"], "valid")

        # A talker on TG 1745 (priority 1) pulls the selection over from a quiet TG 8; its audio plays
        self.fr_cmd("talker start 1745 ON4XYZ")
        self.fr_cmd("audio 50")
        self.assertTrue(self.pump(5, lambda: self.state.get("talker") == "ON4XYZ"), self.state)
        self.assertEqual((self.state["tg"], self.state["active"]), ("1745", "1745:ON4XYZ"))
        self.assertTrue(self.pump(5, lambda: self.audio_in >= 40), self.audio_in)
        self.fr_cmd("talker stop 1745 ON4XYZ")
        self.assertTrue(self.pump(5, lambda: self.state.get("recent", "").startswith("1745:ON4XYZ:")), self.state)
        self.assertEqual(self.state["active"], "")

        # An over of ours: the reflector takes its frames
        self.send("ptt on")
        self.assertTrue(self.pump(3, lambda: self.state.get("tx") == "1"))
        self.assertEqual(self.events[-1], ("tx_on", ""))
        for _ in range(50):
            self.sock.send(b"\x00" + FRAME)
            time.sleep(0.02)
        self.send("ptt off")
        self.assertTrue(self.pump(3, lambda: self.state.get("tx") == "0"))
        self.assertEqual(self.events[-1][0], "tx_off")
        self.assertTrue(self.pump(3, lambda: any(f.startswith("audio ") and int(f.split()[1]) >= 40
                                                 for f in self.fr_said)), self.fr_said)

        # Someone on the talkgroup: refused, with who
        self.fr_cmd("talker start 8 ON7ABC")
        self.assertTrue(self.pump(3, lambda: self.state.get("talker") == "ON7ABC"), self.state)
        self.assertEqual(self.state["tg"], "8")                 # priority 2 pulled it back
        self.send("ptt on")
        self.assertTrue(self.pump(3, lambda: self.events[-1][0] == "refused"))
        self.assertEqual(self.events[-1], ("refused", "ON7ABC is talking on TG 8"))
        self.assertEqual(self.state["tx"], "0")
        self.fr_cmd("talker stop 8 ON7ABC")

        # The lock, and choosing while locked
        self.send("lock on")
        self.send("tg 1745")
        self.assertTrue(self.pump(3, lambda: self.state.get("tg") == "1745" and self.state.get("locked") == "1"),
                        self.state)
        self.fr_cmd("talker start 8 ON7ABC")             # priority 2, but locked: it stays
        self.pump(1.5)
        self.assertEqual((self.state["tg"], self.state["active"]), ("1745", "8:ON7ABC"))
        self.assertEqual(self.state["talker"], "")       # not on ours: not whose audio plays

        # The socket's end ends it
        self.sock.close()
        self.assertEqual(self.p.wait(5), 0)

    def test_tx_timeout(self):
        """tx_timeout_sec (the web page's TX time-out) ends an over by itself"""
        self.start(tx_timeout_sec=2)
        self.assertTrue(self.pump(10, lambda: self.state.get("conn") == "2"))
        self.feeding = True
        self.send("ptt on")
        self.assertTrue(self.pump(3, lambda: self.state.get("tx") == "1"))
        t = time.monotonic()
        self.assertTrue(self.pump(6, lambda: self.state.get("tx") == "0"))
        self.assertGreater(time.monotonic() - t, 1.5, self.events)
        self.assertEqual(self.events[-1][0], "tx_off")
        self.assertIn("timeout", self.events[-1][1].lower())    # it says why

    def test_no_audio_ends_it(self):
        """An over the mic gives nothing for ends after a second (svxconnect's watchdog): the app sends the hand mic
        from the over's start, and the Bluetooth mic's over starts with its first audio (src/svx.c)"""
        self.start()
        self.assertTrue(self.pump(10, lambda: self.state.get("conn") == "2"))
        self.send("ptt on")
        self.assertTrue(self.pump(3, lambda: self.state.get("tx") == "1"))
        self.assertTrue(self.pump(3, lambda: self.state.get("tx") == "0"))
        self.assertEqual(self.events[-1], ("tx_off", "the microphone delivers no audio"))

    def test_monitoring_only(self):
        """Monitoring only (TG 0, as after Idle): nothing to talk on, an over is refused with why. Start on 0 is the
        first switchable talkgroup (svxconnect's tgm_init), not monitoring only as its example.conf says"""
        self.start(default_tg=0)
        self.assertTrue(self.pump(10, lambda: self.state.get("conn") == "2"))
        self.assertEqual(self.state["tg"], "8")
        self.send("tg 0")
        self.assertTrue(self.pump(3, lambda: self.state.get("tg") == "0"), self.state)
        self.feeding = True
        self.send("ptt on")
        self.assertTrue(self.pump(3, lambda: self.events and self.events[-1][0] == "refused"), self.events)
        self.assertNotEqual(self.events[-1][1], "")
        self.assertEqual(self.state["tx"], "0")

    def test_cert_status_and_not_enrolled(self):
        conf = self.conf()
        r = subprocess.run([BIN, "--cert-status", "-c", conf], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        kv = dict(item.split("=", 1) for item in r.stdout.split())
        self.assertEqual((kv["status"], kv["usable"], kv["request"]), ("valid", "1", "0"))
        self.assertGreater(int(kv["not_after"]), time.time())

        for name in os.listdir(self.pki):
            if name.endswith(".crt") and name != "ca-bundle.crt":
                os.remove(os.path.join(self.pki, name))
        r = subprocess.run([BIN, "--cert-status", "-c", conf], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 1)
        self.assertIn("status=missing", r.stdout)
        self.start()
        self.pump(5, lambda: self.p.poll() is not None)
        self.assertEqual(self.events, [("error", "not enrolled: no certificate yet")])
        self.assertEqual(self.p.wait(5), 4)
        with open(self.err) as f:
            self.assertNotIn("PRIVATE KEY", f.read())


if __name__ == "__main__":
    unittest.main()
