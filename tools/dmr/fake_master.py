#!/usr/bin/env python3
"""A fake DMR master on 127.0.0.1 for the DMR app's tests and screenshots: it answers a hotspot as the MMDVM
"Homebrew" protocol does (the 2015 specification by DL5DI, G4KLX and DG1HT, as MMDVM's hotspots and FreeDMR use it),
and keeps every packet it gets, so a test can say what the hotspot sent:

  RPTL <id>               RPTACK <salt>, or MSTNAK <id> for an ID it does not know
  RPTK <id> <sha256>      RPTACK <id> when it is SHA-256 of the salt and the password, else MSTNAK
  RPTC <id> <302 bytes>   RPTACK <id>, or MSTNAK when the callsign is not the one the ID belongs to
  RPTO <id> <options>     RPTACK <id> (or MSTNAK, refuse_options)
  RPTPING <id>            MSTPONG <id> while logged in, else MSTNAK
  RPTCL <id>              the hotspot logged out
and, sent by the test: calls of voice (DMRD: a voice header, voice frames A-F, a terminator), data frames, a beacon
request (RPTSBKN), MSTCL, MSTNAK, or silence.

    python3 fake_master.py --serve <port> <id> <password> <callsign> [--script [TG]]   (the screenshots' master)

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import hashlib
import os
import socket
import struct
import sys
import threading
import time

# The AMBE+2 silence frame DMR sends between words, three to a burst with the sync between (its 48 bits here zeros)
SILENCE = bytes([0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B])


def burst_of(frame9, sync=b"\0" * 6):
    """A 33-byte voice burst: frame, frame's first half, 48 bits of sync, its second half, frame"""
    bits = "".join(f"{b:08b}" for b in frame9)
    mid = "".join(f"{b:08b}" for b in sync)
    allbits = bits + bits[:36] + mid + bits[36:] + bits
    return int(allbits, 2).to_bytes(33, "big")


SILENCE_BURST = burst_of(SILENCE)


class FakeMaster:
    def __init__(self, ids, password, callsigns, port=0):
        self.ids = set(ids)                     # hotspot IDs it lets in
        self.password = password
        self.callsigns = callsigns              # hotspot ID -> the callsign it must say
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", port))     # this machine only
        self.port = self.sock.getsockname()[1]
        self.sock.settimeout(0.1)
        self.lock = threading.Lock()
        self.got = []                           # (time, packet) of every packet
        self.peer = None
        self.salt = {}
        self.state = {}                         # hotspot ID -> "login", "auth", "config", "online"
        self.config = {}                        # hotspot ID -> the RPTC fields
        self.options = {}                       # hotspot ID -> the RPTO text
        self.silent = False                     # answers nothing
        self.refuse_options = False
        self.seq = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(2)
        self.sock.close()

    def packets(self, word=None):
        with self.lock:
            return [p for _, p in self.got if word is None or p.startswith(word)]

    def words(self):
        """The packet types it got, by their words"""
        out = []
        for p in self.packets():
            for w in (b"RPTPING", b"RPTCL", b"RPTL", b"RPTK", b"RPTC", b"RPTO"):
                if p.startswith(w):
                    out.append(w.decode())
                    break
            else:
                out.append(p[:7].decode("ascii", "replace"))
        return out

    def send(self, pkt):
        if self.peer:
            self.sock.sendto(pkt, self.peer)

    def serve(self):
        while not self.stop.is_set():
            try:
                d, a = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.got.append((time.monotonic(), d))
                self.peer = a
            if not self.silent:
                self.answer(d)

    def answer(self, d):
        for w in (b"RPTPING", b"RPTCL", b"RPTL", b"RPTK", b"RPTC", b"RPTO"):
            if d.startswith(w):
                break
        else:
            return
        i = int.from_bytes(d[len(w):len(w) + 4], "big")
        idb = d[len(w):len(w) + 4]
        nak = b"MSTNAK" + idb
        if w == b"RPTL":
            if i not in self.ids:
                return self.send(nak)
            self.salt[i] = os.urandom(4)
            self.state[i] = "auth"
            self.send(b"RPTACK" + self.salt[i])
        elif w == b"RPTK":
            ok = self.state.get(i) == "auth" and d[8:40] == hashlib.sha256(self.salt[i] +
                                                                             self.password.encode()).digest()
            self.state[i] = "config" if ok else "login"
            self.send(b"RPTACK" + idb if ok else nak)
        elif w == b"RPTC":
            c = self.parse_config(d)
            ok = self.state.get(i) == "config" and c is not None and c["callsign"] == self.callsigns.get(i)
            if c is not None:
                self.config[i] = c
            self.state[i] = "online" if ok else "login"
            self.send(b"RPTACK" + idb if ok else nak)
        elif w == b"RPTO":
            self.options[i] = d[8:].decode("ascii", "replace")
            self.send(nak if self.refuse_options or self.state.get(i) != "online" else b"RPTACK" + idb)
        elif w == b"RPTPING":
            self.send(b"MSTPONG" + idb if self.state.get(i) == "online" else nak)
        elif w == b"RPTCL":
            self.state[i] = "closed"

    @staticmethod
    def parse_config(d):
        if len(d) != 302:
            return None
        t = d[8:].decode("ascii", "replace")
        f, k = {}, 0
        for name, n in (("callsign", 8), ("rx", 9), ("tx", 9), ("power", 2), ("cc", 2), ("lat", 8), ("lon", 9),
                        ("height", 3), ("location", 20), ("description", 19), ("slots", 1), ("url", 124),
                        ("software", 40), ("package", 40)):
            f[name] = t[k:k + n].strip()
            k += n
        return f

    # ---------- what the test sends ----------

    def dmrd(self, src, dst, flags, stream, payload=SILENCE_BURST, rptr=0, seq=None):
        if seq is None:
            self.seq = (self.seq + 1) & 0xFF
            seq = self.seq
        pkt = (b"DMRD" + bytes([seq]) + src.to_bytes(3, "big") + dst.to_bytes(3, "big") + rptr.to_bytes(4, "big") +
               bytes([flags]) + struct.pack(">I", stream) + payload + b"\0\0")
        self.send(pkt)
        return seq

    def call(self, src, dst, stream, bursts, slot=2, private=False, header=True, terminator=True, pace=0.06,
             payloads=None):
        """A call: its voice header, bursts voice frames (A with sync, B-F), its terminator; the payloads sent"""
        base = (0x80 if slot == 2 else 0) | (0x40 if private else 0)
        sent = []
        if header:
            self.dmrd(src, dst, base | 0x20 | 1, stream)
        for n in range(bursts):
            p = payloads[n % len(payloads)] if payloads else SILENCE_BURST
            self.dmrd(src, dst, base | (0x10 if n % 6 == 0 else 0x00) | (n % 6), stream, p)
            sent.append(p)
            time.sleep(pace)
        if terminator:
            self.dmrd(src, dst, base | 0x20 | 2, stream)
        return sent


def serve_main(argv):
    """The screenshots' master: one hotspot, and with --script calls, once it is logged in, a short call, then a long
    one (12 s), and again, from DMR IDs no one has (999 is no country's)"""
    port, hid, password, callsign = int(argv[0]), int(argv[1]), argv[2], argv[3]
    m = FakeMaster([hid], password, {hid: callsign}, port)
    stream = 1
    while True:
        time.sleep(1)
        if "--script" in argv and m.peer and any(s == "online" for s in m.state.values()):
            for src, bursts, pause in ((9990003, 40, 2), (9990002, 200, 4)):
                stream += 1
                m.call(src, int(argv[argv.index("--script") + 1]) if len(argv) > argv.index("--script") + 1 else 91,
                       stream, bursts)
                time.sleep(pause)


if __name__ == "__main__":
    if len(sys.argv) > 5 and sys.argv[1] == "--serve":
        serve_main(sys.argv[2:])
    else:
        sys.exit(__doc__)
