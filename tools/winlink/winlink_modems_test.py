#!/usr/bin/env python3
"""The real Winlink modems against src/winlink.c on the PC (winlink_host).

1. ardopcf, protocol version 1, as x6100-winlink starts it: it connects at 12000 Hz with keys of 30 s, and the
   ID frame it is asked for (SENDID on its host port) is played whole. A call nobody answers ends in
   CONNECT TO <call> FAILED, on the host port and on its console (x6100-winlink's "no answer"). Its clock then
   10 minutes on (clockskip.so): no 10 minute ID, the ConReq frames carried its call; the same after a call of
   one ConReq (no repeat) and after a Ping; after a DataNAK frame (no call in it), the 10 minute ID.
   PROTOCOLMODE takes FEC and ARQ (Pat's); a mode it does not have is a FAULT and leaves the mode as it was.
2. Mercury through x6x00-mercury, protocol version 2, and Pat v1.0.1's varahf transport, as on the radio: two
   stations, each a winlink_host and a bridge, what one transmits the other hears (with a little noise). Pat B
   listens; Pat A sends B a message peer to peer. Pat starts the modem (Mercury answers VERSION), connects,
   sends, disconnects, and the message is in B's inbox. The keys' level is Mercury 1.9.21's (x6100-winlink's
   tx_gain_db = -4 under 1.9.25, which alone is 4 dB louder). Then the stop: with a session up, x6x00-mercury has
   Mercury end it on the air first, so the other station's Mercury says DISCONNECTED at once, and it stops within
   its bound and keys no more; with none it stops at once; a second SIGTERM during the goodbye stops it at once;
   a goodbye nobody answers still ends.
3. XARA (our patched build, its --audio x6x00 on the GUI's socket, protocol version 2 at 12000 Hz) and Pat's varahf,
   the same way, twice: with the settings x6100-winlink writes for the free speed (no `registered`), B's XARA says
   LINK UNREGISTERED and no level goes past VARA's free 4; with `registered` on A (the Winlink page's VARA licence
   tick box), B's says LINK REGISTERED, as VARA's callee follows the caller's bit. Both stop on SIGTERM. First, at
   the free speed and before any Pat: A's XARA calls B's, which neither answers nor keys (on the radio XARA starts
   with listening off; Pat B's listen turns it on). The free run has calls of a European kind (ON4AAA, ON4BBB), the
   registered one US calls. Both XARAs identify in CW as Pat's session closes, with either kind: Pat sends CWID ON,
   which upstream's XARA takes only for a US call and our patch 0004 for any. Before any Pat, with no CWID ON,
   A's call that B does not answer ends without one with the European call, as upstream's does. Then, with CWID ON,
   a call cancelled in its reply window (ABORT, as Pat's force disconnect) and a tune elsewhere half a second later,
   as Pat's tune back: A's XARA keys its ID at once (patch 0005), on the call's frequency, and the radio tunes after
   that key.

Run it in a network namespace with loopback only (`make modems` does, with unshare -rn): Pat then reaches
nothing outside, no Winlink web service, no CMS. Pat's own account check before a connect is also skipped
(its state file says it was done) and an HTTP proxy that is not there takes anything else.
Prints MODEMS OK at the end; exits 1 at the first failure. --keep DIR keeps the logs there.
"""

import argparse
import array
import json
import math
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PLAY_RATE = 44100
CALL_A, CALL_B = "N0AAA", "N0BBB"
GOODBYE_S = 42                         # x6x00-mercury's bound for a session ended on the air at its stop
# Calls that are not of the US kind, for XARA's run at the free speed: without CWID ON from its host XARA identifies in
# CW only with a US call
CALL_EU_A, CALL_EU_B = "ON4AAA", "ON4BBB"


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def check(cond, msg):
    if not cond:
        fail(msg)
    print("  ok:", msg)


def wait_for(fn, timeout, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(step)
    return False


def text(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


class Procs:
    """The programs started, stopped at the end whatever happens."""

    def __init__(self):
        self.procs = []

    def start(self, argv, log, env=None, stdin=None):
        p = subprocess.Popen(argv, stdout=open(log, "w"), stderr=subprocess.STDOUT, env=env, stdin=stdin,
                             start_new_session=True)
        self.procs.append(p)
        return p

    def stop(self, p, wait=10.0):
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(wait)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()

    def stop_all(self):
        for p in reversed(self.procs):
            self.stop(p, 5.0)


def host(procs, tmp, name, rig_port, air_in=None, air_out=None):
    """A winlink_host: its socket, its log, what it played."""
    h = {"sock": os.path.join(tmp, name + ".sock"), "log": os.path.join(tmp, name + "-gui.log"),
         "played": os.path.join(tmp, name + ".played"), "rig": rig_port}
    env = dict(os.environ, WL_SOCK=h["sock"], WL_RIG_PORT=str(rig_port), WL_PLAYED=h["played"])
    if air_in:
        env["WL_AIR_IN"] = air_in
    if air_out:
        env["WL_AIR_OUT"] = air_out
    procs.start([os.path.join(HERE, "winlink_host")], h["log"], env)
    if not wait_for(lambda: os.path.exists(h["sock"]), 5):
        fail(f"winlink_host {name} made no socket")
    return h


def played(h):
    """The transmissions of a winlink_host so far: (samples, stopped) each."""
    return [(int(l.split()[1]), l.split()[3] == "1") for l in text(h["played"] + ".log").splitlines()]


def peak_db(h):
    """The peak of a winlink_host's last transmission as it was played, dBFS"""
    a = array.array("h")
    with open(h["played"], "rb") as f:
        a.frombytes(f.read())
    return 20 * math.log10(max(max(a), -min(a), 1) / 32768)


def freqs(h):
    """The frequency each transmission of a winlink_host went out on, Hz"""
    return [int(l.split()[5]) for l in text(h["played"] + ".log").splitlines()]


def rig(h, cmd):
    """One command to a winlink_host's rigctld (Pat's hamlib rig), its answer"""
    with socket.create_connection(("127.0.0.1", h["rig"]), timeout=3) as s:
        s.sendall(cmd.encode() + b"\n")
        return s.recv(64).decode().strip()


def read_until(c, done, timeout):
    """What ardopcf's host port says until done(what) or the timeout."""
    got, end = b"", time.time() + timeout
    c.settimeout(0.5)
    while time.time() < end and not done(got):
        try:
            d = c.recv(4096)
        except socket.timeout:
            continue
        if not d:
            break
        got += d
    return got


def ardop_test(procs, tmp, ardopcf):
    print("ardopcf, protocol 1")
    # nobody on its air: noise only, not busy (a tone is busy, and the 10 minute ID waits for a channel not busy)
    h = host(procs, tmp, "ardop", 14540, os.path.join(tmp, "air-ardop"))
    log = os.path.join(tmp, "ardopcf.log")
    skip = os.path.join(HERE, "clockskip.so")       # its LD_PRELOAD: each SIGUSR1, its clock 10 minutes on
    if not os.path.exists(skip):
        fail(f"{skip} is not there: make clockskip.so")
    a = procs.start([ardopcf, "--nologfile", "18515", f"SOCK:{h['sock']}", f"SOCK:{h['sock']}"], log,
                    env=dict(os.environ, LD_PRELOAD=skip))
    check(wait_for(lambda: "the modem connected, protocol 1, 12000 Hz, keys of 30 s at most" in text(h["log"]), 10),
          "ardopcf connected: 12000 Hz, keys of 30 s")
    if not wait_for(lambda: _port_open(18515), 10):
        fail("ardopcf's host port is not there")
    c = socket.create_connection(("127.0.0.1", 18515), timeout=5)
    replies = b""
    for cmd in ("INITIALIZE", f"MYCALL {CALL_A}", "SENDID"):
        c.sendall(cmd.encode() + b"\r")
        time.sleep(0.3)
    c.settimeout(0.5)
    try:
        while True:
            d = c.recv(4096)
            if not d:
                break
            replies += d
    except socket.timeout:
        pass
    print("   ardopcf answered:", replies.decode(errors="replace").replace("\r", " | ").strip())
    check(b"FAULT" not in replies, "no FAULT from ardopcf")
    check(wait_for(lambda: len(played(h)) >= 1, 15), "its ID frame keyed the radio")
    n, stopped = played(h)[0]
    check(not stopped and 1.0 * PLAY_RATE <= n <= 3.0 * PLAY_RATE,
          f"and was played whole: {n / PLAY_RATE:.2f} s at 44.1 kHz")
    c.sendall(f"ARQCALL {CALL_B} 2\r".encode())       # nobody there: two ConReq frames, then it gives up
    r = read_until(c, lambda r: b"STATUS " in r and r.endswith(b"\r"), 30)
    print("   ardopcf answered:", r.decode(errors="replace").replace("\r", " | ").strip())
    failed = f"CONNECT TO {CALL_B} FAILED"
    check(b"NEWSTATE DISC" in r and f"STATUS {failed}".encode() in r,
          f"a call nobody answers: NEWSTATE DISC, STATUS {failed}")
    check(f"[STATUS: {failed}]" in text(log), "and its console says so")
    def ids():
        return text(log).count("Sending Frame Type IDFrame")
    id0 = ids()
    def no_id(what):
        a.send_signal(signal.SIGUSR1)
        check(not wait_for(lambda: ids() > id0, 3), f"10 minutes on, no 10 minute ID: {what} carried our call")
    no_id("the ConReq and its repeat")                # the repeat: RemodulateLastFrame
    keys = len(played(h))
    c.sendall(f"ARQCALL {CALL_B} 1\r".encode())       # one ConReq, no repeat: SendARQConnectRequest only
    r = read_until(c, lambda r: b"STATUS " in r and r.endswith(b"\r"), 30)
    print("   ardopcf answered:", r.decode(errors="replace").replace("\r", " | ").strip())
    check(f"STATUS {failed}".encode() in r and len(played(h)) == keys + 1, f"one ConReq, no repeat, {failed}")
    no_id("the one ConReq")
    c.sendall(f"PING {CALL_B} 1\r".encode())          # one Ping, nobody answers
    r = read_until(c, lambda r: r.endswith(b"\r"), 5)
    check(f"PING {CALL_B} 1".encode() in r.split(b"\r") and wait_for(lambda: len(played(h)) == keys + 2, 10),
          "a Ping keyed the radio")
    time.sleep(3)                                     # its repeat time (2 s) gone: the Pings are over, no repeat
    check(len(played(h)) == keys + 2, "and was not repeated")
    no_id("the Ping")
    c.sendall(b"TXFRAME DataNAK\r")                  # a frame without our call
    check(wait_for(lambda: len(played(h)) == keys + 3, 10), "a DataNAK frame keyed the radio")
    a.send_signal(signal.SIGUSR1)
    check(wait_for(lambda: ids() == id0 + 1 and len(played(h)) == keys + 4, 10),
          "10 minutes on, its 10 minute ID keyed the radio")
    c.sendall(b"PROTOCOLMODE FEC\rPROTOCOLMODE BOGUS\rPROTOCOLMODE\rPROTOCOLMODE ARQ\r")   # ARQ: Pat's init
    r = read_until(c, lambda r: b"\rPROTOCOLMODE now ARQ\r" in b"\r" + r, 5)
    print("   ardopcf answered:", r.decode(errors="replace").replace("\r", " | ").strip())
    check([l for l in r.split(b"\r") if l.startswith((b"PROTOCOLMODE", b"FAULT"))]
          == [b"PROTOCOLMODE now FEC", b"FAULT Syntax Err: PROTOCOLMODE BOGUS", b"PROTOCOLMODE FEC",
              b"PROTOCOLMODE now ARQ"],
          "PROTOCOLMODE FEC and ARQ taken; a mode other than ARQ, RXO or FEC: FAULT, the mode still FEC")
    c.close()
    procs.stop(a)
    check(wait_for(lambda: "the modem gone" in text(h["log"]), 5), "ardopcf gone: the GUI saw it go")


def _port_open(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        return True
    except OSError:
        return False


def mercury_station(procs, tmp, name, call, port, mercury, air_in, air_out):
    h = host(procs, tmp, name, 14550 + port % 100, air_in, air_out)
    home = os.path.join(tmp, name + "-home")
    os.makedirs(home)
    ini = os.path.join(tmp, name + "-mercury.ini")
    with open(ini, "w") as f:
        f.write(f"""[main]
ui_enabled = false
waterfall_enabled = false
arq_tcp_base_port = {port}
broadcast_tcp_port = {port - 200}
callsign = {call}

[audio]
tx_gain_db = -4.0
tx_delay_ms = 10

[channel]
busy_detect = false

[tnc]
keepalive_s = 60

[store]
enabled = false
""")
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"))
    log = os.path.join(tmp, name + "-mercury.log")
    h["bridge"] = ([mercury, f"SOCK:{h['sock']}", "-C", ini], env, log)
    m = procs.start(h["bridge"][0], log, env)
    return h, m, log


def mercury_again(procs, h, nth):
    """The station's x6x00-mercury started again on the same winlink_host, a log of its own"""
    argv, env, log = h["bridge"]
    log = f"{log}.{nth}"
    return procs.start(argv, log, env), log


def _keyed_now(h):
    """The winlink_host has a key on"""
    return text(h["log"]).count("[stub] keyed") > text(h["log"]).count("[stub] unkeyed")


def pat_env(tmp, name, call):
    """Pat's directories, its config and its account state for one station, all under tmp."""
    d = os.path.join(tmp, name + "-pat")
    for sub in ("config", "data", "state", "mbox"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    state = os.path.join(d, "state", "pat")
    os.makedirs(state, exist_ok=True)
    # Pat checks the Winlink account (api.winlink.org) before a connect, at most every 100 days: done, it says
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    for n in ("account-confirmed", "version_report", "pw_recovery_email_check"):
        with open(os.path.join(state, f".{n}_{call}.json"), "w") as f:
            json.dump(now, f)
    return d


def pat_cmd(pat, d, port, call, *args, listen=False):
    cfg = os.path.join(d, "config", "config.json")
    with open(cfg, "w") as f:
        json.dump({"mycall": call, "locator": "JO21", "version_reporting_disabled": True,
                   "listen": [], "hamlib_rigs": {},
                   "varahf": {"addr": f"127.0.0.1:{port}", "bandwidth": 2300, "rig": "", "ptt_ctrl": False}}, f)
    argv = [pat, "--config", cfg, "--mbox", os.path.join(d, "mbox"), "--log", os.path.join(d, "pat.log"),
            "--event-log", os.path.join(d, "events.log")]
    if listen:
        argv += ["--listen", "varahf"]
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "config"), XDG_DATA_HOME=os.path.join(d, "data"),
               XDG_STATE_HOME=os.path.join(d, "state"), HOME=d,
               HTTPS_PROXY="http://127.0.0.1:9", HTTP_PROXY="http://127.0.0.1:9", NO_PROXY="")
    return argv + list(args), env


def version_check(port):
    """Pat's start of the modem: its start-up commands, then VERSION. Pat-Vara's Version() waits, with no timeout,
    for a line that starts VERSION or WRONG. Mercury 1.9.21 said WRONG to CWID ON, which ended that wait only when
    it came after the question; 1.9.25 takes CWID ON (and sends no CW ID), so its VERSION line alone ends it"""
    c = socket.create_connection(("127.0.0.1", port), timeout=5)
    cmds = ("PUBLIC ON", "CWID ON", "COMPRESSION TEXT", f"MYCALL {CALL_A}", "LISTEN OFF")
    for cmd in cmds:
        c.sendall(cmd.encode() + b"\r")
    got = read_until(c, lambda g: g.split(b"\r").count(b"OK") >= len(cmds) or b"WRONG" in g, 5)
    check(got.split(b"\r").count(b"OK") == len(cmds) and b"WRONG" not in got,
          f"Mercury takes Pat's start-up commands, CWID ON too ({got!r})")
    c.sendall(b"VERSION\r")
    got = read_until(c, lambda g: any(l.startswith(b"VERSION") for l in g.split(b"\r")[:-1]), 3)
    line = next((l for l in got.split(b"\r") if l.startswith(b"VERSION")), b"").decode()
    check(line == "VERSION Mercury 1.9.25", f"and VERSION with a VERSION line, what Pat waits for ({line!r})")
    c.close()


def mercury_test(procs, tmp, mercury, pat):
    print("Mercury through x6x00-mercury, protocol 2, and Pat's varahf, two stations")
    air_a, air_b = os.path.join(tmp, "air-a"), os.path.join(tmp, "air-b")
    ha, ma, la = mercury_station(procs, tmp, "a", CALL_A, 18300, mercury, air_a, air_b)
    hb, mb, lb = mercury_station(procs, tmp, "b", CALL_B, 18310, mercury, air_b, air_a)
    for h, n in ((ha, "A"), (hb, "B")):
        check(wait_for(lambda: "the modem connected, protocol 2, 8000 Hz, keys of 40 s at most" in text(h["log"]), 15),
              f"station {n}: x6x00-mercury connected, 8000 Hz, keys of 40 s")
    for log, n, port in ((la, "A", 18300), (lb, "B", 18310)):
        check(wait_for(lambda: f"Listening on 127.0.0.1 TCP port {port}" in text(log), 15),
              f"station {n}: Mercury's TNC on 127.0.0.1:{port}")
    version_check(18300)

    da, db = pat_env(tmp, "a", CALL_A), pat_env(tmp, "b", CALL_B)
    # B listens; its stdin stays open, or its interactive prompt would end
    argv, env = pat_cmd(pat, db, 18310, CALL_B, "interactive", listen=True)
    pb = procs.start(argv, os.path.join(db, "out.txt"), env, stdin=subprocess.PIPE)
    check(wait_for(lambda: "Listening for incoming traffic on varahf" in text(os.path.join(db, "out.txt")), 20),
          "Pat B started Mercury and listens on varahf")

    # A's message to B, peer to peer only
    subject = f"Mercury P2P test {int(time.time())}"
    body = "Sent by Pat over Mercury through x6x00-mercury and the GUI's modem socket.\n" * 20
    argv, env = pat_cmd(pat, da, 18300, CALL_A, "compose", "--p2p-only", "-s", subject, CALL_B)
    r = subprocess.run(argv, input=body, text=True, env=env, capture_output=True, timeout=30)
    check(r.returncode == 0, f"Pat A composed a message to {CALL_B} ({len(body)} bytes)")

    print("   connecting (Mercury on the air at 8 kHz: a minute or more)")
    out_a = os.path.join(da, "connect.txt")
    argv, env = pat_cmd(pat, da, 18300, CALL_A, "connect", f"varahf:///{CALL_B}?p2p=true")
    t0 = time.time()
    with open(out_a, "w") as f:
        r = subprocess.run(argv, stdout=f, stderr=subprocess.STDOUT, env=env, timeout=600)
    secs = time.time() - t0
    out = text(out_a) + text(os.path.join(da, "pat.log"))
    print("   Pat A:\n     " + "\n     ".join(text(out_a).strip().splitlines()[-25:]))
    # Its version, or nothing when the WRONG for CWID ON came after it asked: either way Version() returned
    check("VARA modem (" in out and ") initialized" in out, "Pat A started Mercury")
    check("Connected to" in out, "Pat A connected to B")
    check(r.returncode == 0, f"Pat A's session ended well ({secs:.0f} s)")

    inbox = os.path.join(db, "mbox", CALL_B, "in")
    got = [n for n in os.listdir(inbox)] if os.path.isdir(inbox) else []
    found = any(subject in text(os.path.join(inbox, n)) for n in got)
    check(found, f"the message is in B's inbox ({len(got)} there)")
    keys_a, keys_b = played(ha), played(hb)
    check(keys_a and keys_b, f"both radios keyed: A {len(keys_a)} times, B {len(keys_b)} times")
    longest = max(n for n, _ in keys_a + keys_b) / PLAY_RATE
    check(not any(s for _, s in keys_a + keys_b), f"no key stopped by the GUI (the longest {longest:.1f} s)")
    # The level on the GUI's socket, as the radio's player gets it: each station's last key (A's a data mode, B's
    # the MFSK of its last answer, or the other way round). Mercury 1.9.25 alone peaks at -2 dBFS (-2.5 MFSK), 4 dB
    # over 1.9.21 to 1.9.24; with x6100-winlink's tx_gain_db = -4 it is their -6 dBFS (-6.5 MFSK) again
    for h, n in ((ha, "A"), (hb, "B")):
        db = peak_db(h)
        check(-7.5 <= db <= -5.5, f"station {n}: its last key peaks at {db:.1f} dBFS, the level of Mercury 1.9.21")
    pb.stdin.close()
    procs.stop(pb)

    # The stop. With a session up, x6x00-mercury first has Mercury end it on the air (1.9.25's goodbye, one
    # DISCONNECT) while its audio thread still carries the key: the other station is let go at once, not by its
    # watchdog 240 s later. With none it stops at once, as before
    def session():
        cs = [socket.create_connection(("127.0.0.1", p), timeout=5) for p in (18300, 18310)]
        for c, call in zip(cs, (CALL_A, CALL_B)):
            c.sendall(f"MYCALL {call}\rLISTEN ON\r".encode())
        cs[0].sendall(f"CONNECT {CALL_A} {CALL_B}\r".encode())
        for c, n in zip(cs, "AB"):
            got = read_until(c, lambda g: b"CONNECTED " in g, 60)
            check(b"CONNECTED " in got, f"a session of the two Mercurys alone, for the stop: {n} says CONNECTED")
        return cs

    def keyed(h):
        return text(h["log"]).count("[stub] keyed")

    def quiet_after(h, n):
        keys = keyed(h)
        time.sleep(1.5)
        check(keyed(h) == keys, f"station {n}: no key after the stop")

    ca, cb = session()
    keys, sent, gone = keyed(ha), len(played(ha)), text(ha["log"]).count("the modem gone")
    t0 = time.time()
    ma.send_signal(signal.SIGTERM)
    got = read_until(cb, lambda g: b"DISCONNECTED" in g, GOODBYE_S)
    heard = time.time() - t0
    check(b"DISCONNECTED" in got, f"A stopped with the session up: B's Mercury says DISCONNECTED {heard:.1f} s later, "
                                  "by A's goodbye on the air")
    try:
        ma.wait(GOODBYE_S + 13 - heard)
    except subprocess.TimeoutExpired:
        fail(f"station A: x6x00-mercury still runs {GOODBYE_S + 13} s after SIGTERM")
    secs = time.time() - t0
    check(ma.returncode == 0 and secs < GOODBYE_S + 13,
          f"station A: x6x00-mercury stopped {secs:.1f} s after SIGTERM, within its {GOODBYE_S} s and the stop "
          f"(code {ma.returncode})")
    check("x6x00-mercury: a session is up, ending it on the air first" in text(la)
          and "x6x00-mercury: the goodbye is over" in text(la), "station A: its log says the goodbye, and that it is over")
    check(wait_for(lambda: text(ha["log"]).count("the modem gone") == gone + 1, 5), "station A: the GUI saw it go")
    ends = played(ha)[sent:]
    check(keyed(ha) > keys and ends and not ends[-1][1],
          f"station A: the goodbye was keyed through the GUI and played to its end ({len(ends)} "
          f"transmission{'s' if len(ends) != 1 else ''} after SIGTERM, the last {ends[-1][0] / PLAY_RATE:.1f} s)"
          if ends else "station A: the goodbye was keyed through the GUI")
    quiet_after(ha, "A")
    # B has no session any more: its stop is at once
    time.sleep(3)
    check(wait_for(lambda: not _keyed_now(hb), 20), "station B: unkeyed after the session's end")
    t0 = time.time()
    procs.stop(mb, 20.0)
    secs = time.time() - t0
    check(mb.returncode == 0 and secs < 3 and "ending it on the air first" not in text(lb),
          f"station B: with no session up x6x00-mercury stopped at once, in {secs:.1f} s (code {mb.returncode}), "
          "no goodbye")
    quiet_after(hb, "B")
    ca.close()
    cb.close()

    # A second SIGTERM during the goodbye stops at once (x6100-winlink passes on one it gets meanwhile): the GUI
    # cuts the key when the modem goes. B is left with a session whose other end is gone: its goodbye gets no
    # answer, and its stop still ends in time
    ma, la = mercury_again(procs, ha, 2)
    mb, lb = mercury_again(procs, hb, 2)
    for log, n, port in ((la, "A", 18300), (lb, "B", 18310)):
        check(wait_for(lambda: f"Listening on 127.0.0.1 TCP port {port}" in text(log), 15),
              f"station {n}: x6x00-mercury started again")
    for h, n in ((ha, "A"), (hb, "B")):
        check(wait_for(lambda: text(h["log"]).count("the modem connected, protocol 2") >= 2, 15),
              f"station {n}: on the GUI's socket again")
    ca, cb = session()
    gone = text(ha["log"]).count("the modem gone")
    t0 = time.time()
    ma.send_signal(signal.SIGTERM)
    check(wait_for(lambda: "ending it on the air first" in text(la), 3, 0.05), "station A: the goodbye began")
    time.sleep(0.5)
    ma.send_signal(signal.SIGTERM)
    try:
        ma.wait(5)
    except subprocess.TimeoutExpired:
        fail("station A: x6x00-mercury still runs 5 s after the second SIGTERM")
    secs = time.time() - t0
    check(ma.returncode == 0 and "the goodbye is cut short by a second stop" in text(la)
          and wait_for(lambda: text(ha["log"]).count("the modem gone") == gone + 1, 5),
          f"station A: a second SIGTERM during the goodbye stopped it at once, {secs:.1f} s after the first (code "
          f"{ma.returncode}), the GUI saw it go")
    quiet_after(ha, "A")
    t0 = time.time()
    mb.send_signal(signal.SIGTERM)
    try:
        mb.wait(GOODBYE_S + 13)
    except subprocess.TimeoutExpired:
        fail(f"station B: x6x00-mercury still runs {GOODBYE_S + 13} s after SIGTERM, its goodbye unanswered")
    secs = time.time() - t0
    said = [l for l in text(lb).splitlines() if "x6x00-mercury: the goodbye is" in l]
    check(mb.returncode == 0 and said,
          f"station B: its goodbye with nobody there to answer still ended, {secs:.1f} s after SIGTERM (code "
          f"{mb.returncode}; {said[-1].split('x6x00-mercury: ')[-1] if said else '-'})")
    quiet_after(hb, "B")
    ca.close()
    cb.close()


def supervisor(root):
    """rootfs/usr/sbin/x6100-winlink as a module, its paths under root"""
    import importlib.machinery
    import importlib.util
    os.environ["X6100_WINLINK_ROOT"] = root
    path = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-winlink")
    loader = importlib.machinery.SourceFileLoader("x6100_winlink", path)
    mod = importlib.util.module_from_spec(importlib.util.spec_from_loader("x6100_winlink", loader))
    sys.dont_write_bytecode = True
    loader.exec_module(mod)
    return mod


def xara_conf(path, call, sock, port, registered):
    """xara.conf as x6100-winlink writes it (its own xara_conf), with this test's socket and port"""
    wl = supervisor(os.path.dirname(path))
    want = {"mycall": call, "xara_bandwidth": 2300, "vara_licence": registered, "xara_path_ms": ""}
    body = wl.xara_conf(want).replace(wl.ARDOP_SOCK, sock).replace(f"port = {wl.MERCURY_PORT}\n", f"port = {port}\n")
    if sock not in body or f"port = {port}\n" not in body:
        fail("x6100-winlink's xara.conf has no socket or port line this test can set")
    with open(path, "w") as f:
        f.write(body)


def xara_station(procs, tmp, name, call, port, xara, air_in, air_out, registered):
    h = host(procs, tmp, name, 14550 + port % 100, air_in, air_out)
    home = os.path.join(tmp, name + "-home")
    os.makedirs(home)
    conf = os.path.join(tmp, name + "-xara.conf")
    xara_conf(conf, call, h["sock"], port, registered)
    # As x6100-winlink starts it: no XARA_* or STATE_DIRECTORY (no saved web UI settings), HOME of its own
    env = {k: v for k, v in os.environ.items() if not k.startswith("XARA_") and k != "STATE_DIRECTORY"}
    env.update(HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"))
    log = os.path.join(tmp, name + "-xara.log")
    x = procs.start([xara, "--config", conf], log, env)
    return h, x, log


def xara_client(port, timeout=3):
    """A client on XARA's command port. XARA takes one at a time: while it has one its port listens for no other, and
    it listens again only once it sees that one gone, so a client right after another can be refused. Tried again"""
    got = []

    def once():
        try:
            got.append(socket.create_connection(("127.0.0.1", port), timeout=5))
            return True
        except ConnectionRefusedError:
            return False

    if not wait_for(once, timeout):
        fail(f"XARA took no client on 127.0.0.1:{port} in {timeout} s")
    return got[0]


def unanswered_call(ha, hb, la, lb, port_a, call_a, call_b):
    """A's XARA calls B's, which x6100-winlink started and no Pat has used: B does not answer and does not key, so a
    radio left with Winlink on does not transmit for whoever calls it. Our patch starts XARA with listening off on
    the radio; Pat sends LISTEN ON only to listen, which it never does there"""
    c = xara_client(port_a)     # right after the VERSION client
    c.sendall(f"CONNECT {call_a} {call_b}\r".encode())
    check(wait_for(lambda: len(played(ha)) >= 3, 40),
          f"A's XARA called {call_b}, which has no Pat: {len(played(ha))} calls")
    time.sleep(3)
    tx_b = [l.strip() for l in text(lb).splitlines() if "  tx " in l or "PENDING" in l or "PTT ON" in l]
    check(not played(hb) and not tx_b, f"B's XARA did not answer and did not key ({len(played(hb))} keys, {tx_b[:3]})")
    c.sendall(b"ABORT\r")
    got = read_until(c, lambda g: b"DISCONNECTED\r" in g, 15)
    c.close()
    check(b"DISCONNECTED\r" in got, "A's XARA gave the call up on ABORT")
    check("  tx cw" not in text(la), f"and sent no CW ID: its host sent no CWID ON, and {call_a} is not a US call")


def cancelled_call(ha, la, port_a, call_a, call_b):
    """A call cancelled in its reply window, then a tune elsewhere: Pat's force disconnect (ABORT, and DISCONNECTED
    faked at once), then a second later its tune back to where the radio was (here half a second, inside the 1.3 s
    reply window that upstream's XARA waits out before its ID, which then went out there). With CWID ON, A's XARA
    keys its CW ID at once (patch 0005), on the call's frequency, and the radio tunes once that key is over"""
    c = xara_client(port_a)
    keys, call_hz, ids = len(played(ha)), int(rig(ha, "f")), text(la).count("  tx cw")
    c.sendall(f"CWID ON\rCONNECT {call_a} {call_b}\r".encode())
    check(wait_for(lambda: len(played(ha)) > keys, 10), f"A's XARA called {call_b} again, with CWID ON")
    c.sendall(b"ABORT\r")      # in the reply window: the request is played out
    time.sleep(0.5)
    away = call_hz + 3000
    check(rig(ha, f"F {away}") == "RPRT 0", f"a tune to {away} Hz half a second after the ABORT, answered at once")
    got = read_until(c, lambda g: b"DISCONNECTED\r" in g, 15)
    c.close()
    check(b"DISCONNECTED\r" in got and text(la).count("  tx cw") == ids + 1,
          "A's XARA gave the call up on ABORT, and identified in CW")
    time.sleep(1)   # the GUI plays out the key's last audio after P 0, then logs it
    after, hz = played(ha)[keys + 1:], freqs(ha)[keys + 1:]
    check(len(after) == 1 and not after[0][1] and after[0][0] / PLAY_RATE >= 2.0 and hz == [call_hz],
          f"its ID was the only key after the ABORT, played whole on the call's {call_hz} Hz "
          f"({[(round(n / PLAY_RATE, 1), f) for (n, _), f in zip(after, hz)]})")
    check(rig(ha, "f") == str(away), f"and the radio went to {away} Hz after it")


def levels(log):
    """The speed levels XARA reported to Pat (BITRATE (n) ...), sent and heard"""
    return [int(m) for m in re.findall(r"< BITRATE \((\d+)\)", text(log))]


def xara_test(procs, tmp, xara, pat, registered):
    tag = "full" if registered else "free"
    # The free run with calls of a European kind, the registered one with US calls: the CW ID differs (below)
    call_a, call_b = (CALL_A, CALL_B) if registered else (CALL_EU_A, CALL_EU_B)
    print(f"XARA through --audio x6x00, protocol 2, and Pat's varahf, two stations ({call_a}, {call_b}), "
          f"{'registered on A (the licence tick box)' if registered else 'the free speed'}")
    port_a, port_b = (18340, 18350) if registered else (18320, 18330)
    air_a, air_b = os.path.join(tmp, "air-xa-" + tag), os.path.join(tmp, "air-xb-" + tag)
    ha, xa, la = xara_station(procs, tmp, "xa-" + tag, call_a, port_a, xara, air_a, air_b, registered)
    hb, xb, lb = xara_station(procs, tmp, "xb-" + tag, call_b, port_b, xara, air_b, air_a, False)
    for h, log, n in ((ha, la, "A"), (hb, lb, "B")):
        check(wait_for(lambda: "the modem connected, protocol 2, 12000 Hz, keys of 30 s at most" in text(h["log"]), 15),
              f"station {n}: XARA connected, 12000 Hz, keys of 30 s")
        check(wait_for(lambda: f"connected to the GUI's socket {h['sock']}" in text(log), 5),
              f"station {n}: and says so in its log, as x6100-winlink reads it")
    # From its log, not by a connection: XARA takes one client on its port and refuses the next while it has one
    for port, log, n in ((port_a, la, "A"), (port_b, lb, "B")):
        check(wait_for(lambda: f"cmd 127.0.0.1:{port} data 127.0.0.1:{port + 1}" in text(log), 10),
              f"station {n}: XARA's commands on 127.0.0.1:{port}, its data on {port + 1}")
    c = socket.create_connection(("127.0.0.1", port_a), timeout=5)
    got, end = b"", time.time() + 3
    c.sendall(b"VERSION\r")
    while not any(l.startswith(b"VERSION") for l in got.split(b"\r")) and time.time() < end:
        got += c.recv(4096)
    c.close()
    line = next((l for l in got.split(b"\r") if l.startswith(b"VERSION")), b"").decode()
    check(line == "VERSION VARA HF v4.9.0", f"VERSION answered with a VERSION line, what Pat waits for ({line!r})")
    if not registered:
        unanswered_call(ha, hb, la, lb, port_a, call_a, call_b)
        cancelled_call(ha, la, port_a, call_a, call_b)
    marks = {la: len(text(la)), lb: len(text(lb))}    # the session's part of the logs, from here

    da, db = pat_env(tmp, "xa-" + tag, call_a), pat_env(tmp, "xb-" + tag, call_b)
    argv, env = pat_cmd(pat, db, port_b, call_b, "interactive", listen=True)
    pb = procs.start(argv, os.path.join(db, "out.txt"), env, stdin=subprocess.PIPE)
    check(wait_for(lambda: "Listening for incoming traffic on varahf" in text(os.path.join(db, "out.txt")), 20),
          "Pat B started XARA and listens on varahf")
    subject = f"XARA P2P test {tag} {int(time.time())}"
    body = "Sent by Pat over XARA on the GUI's modem socket.\n" * 30
    argv, env = pat_cmd(pat, da, port_a, call_a, "compose", "--p2p-only", "-s", subject, call_b)
    r = subprocess.run(argv, input=body, text=True, env=env, capture_output=True, timeout=30)
    check(r.returncode == 0, f"Pat A composed a message to {call_b} ({len(body)} bytes)")

    print("   connecting (VARA HF at 2300 Hz on the PC's two stations)")
    out_a = os.path.join(da, "connect.txt")
    argv, env = pat_cmd(pat, da, port_a, call_a, "connect", f"varahf:///{call_b}?p2p=true")
    t0 = time.time()
    with open(out_a, "w") as f:
        r = subprocess.run(argv, stdout=f, stderr=subprocess.STDOUT, env=env, timeout=600)
    secs = time.time() - t0
    out = text(out_a) + text(os.path.join(da, "pat.log"))
    print("   Pat A:\n     " + "\n     ".join(text(out_a).strip().splitlines()[-25:]))
    check("VARA modem (" in out and ") initialized" in out, "Pat A started XARA")
    check("Connected to" in out, "Pat A connected to B")
    check(r.returncode == 0, f"Pat A's session ended well ({secs:.0f} s)")
    inbox = os.path.join(db, "mbox", call_b, "in")
    got = [n for n in os.listdir(inbox)] if os.path.isdir(inbox) else []
    check(any(subject in text(os.path.join(inbox, n)) for n in got), f"the message is in B's inbox ({len(got)} there)")

    lv = levels(la) + levels(lb)
    check(lv, f"both XARAs reported the speed: levels {sorted(set(lv))}")
    if registered:
        check("< REGISTERED " + call_a in text(la), "A's XARA says REGISTERED with its call (the tick box's line)")
        check("< LINK REGISTERED" in text(lb), "B's XARA says LINK REGISTERED: it follows the caller's bit")
        check(max(lv) > 4, f"and the link went past the free levels (the highest {max(lv)})")
    else:
        check("< REGISTERED " + call_a not in text(la), "A's XARA says REGISTERED without a call: not registered")
        check("< LINK UNREGISTERED" in text(lb) and "< LINK REGISTERED" not in text(lb),
              "B's XARA says LINK UNREGISTERED")
        check(max(lv) <= 4, f"no level past the free 4 (the highest {max(lv)})")
    # Pat sends CWID ON as it starts the modem, and each side identifies in CW as the session closes, whatever the
    # kind of call (patch 0004; upstream's XARA drops CWID ON and identifies only with a US call). The ID is the
    # end of a key like any other, through the GUI's socket and checks: played whole. B's waits out A's (blank words)
    kind = "US" if registered else "European"
    for log, h, n in ((la, ha, "A"), (lb, hb, "B")):
        since = lambda: text(log)[marks[log]:]
        check("> Cwid(true)" in since(), f"Pat asked {n}'s XARA for a CW ID (CWID ON)")
        ided = lambda: "  tx cw" in since() and since().rfind("< DISCONNECTED") > since().rfind("  tx cw")
        check(wait_for(ided, 20), f"{n}'s XARA identified in CW as the session closed ({kind} calls)")
        time.sleep(1)   # the GUI plays out the key's last audio after P 0, then logs it
        n_s, stopped = played(h)[-1]
        check(n_s / PLAY_RATE >= 2.0 and not stopped, f"its last key, the ID's, played whole ({n_s / PLAY_RATE:.1f} s)")
    keys_a, keys_b = played(ha), played(hb)
    check(keys_a and keys_b, f"both radios keyed: A {len(keys_a)} times, B {len(keys_b)} times")
    longest = max(n for n, _ in keys_a + keys_b) / PLAY_RATE
    check(not any(s for _, s in keys_a + keys_b), f"no key stopped by the GUI (the longest {longest:.1f} s)")
    pb.stdin.close()
    procs.stop(pb)
    for x, h, n in ((xa, ha, "A"), (xb, hb, "B")):
        procs.stop(x)
        # 130: XARA's own code for a stop by a signal, the PTT released
        check(x.returncode == 130 and wait_for(lambda: "the modem gone" in text(h["log"]), 5),
              f"station {n}: XARA stopped on SIGTERM, the GUI saw it go")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ardopcf", required=True)
    ap.add_argument("--mercury", required=True)
    ap.add_argument("--xara", help="XARA built for this PC (tools/xara/build.sh --host); without it, no XARA test")
    ap.add_argument("--pat", required=True)
    ap.add_argument("--keep", help="keep the logs in this directory")
    ap.add_argument("--only", choices=("ardop", "mercury", "xara"))
    a = ap.parse_args()
    need = {"ardop": [a.ardopcf], "mercury": [a.mercury], "xara": [a.xara]}
    for p in sum(need.values(), []) if a.only is None else need[a.only]:
        if p and not os.access(p, os.X_OK):
            fail(f"{p} is not there: cmake --build <the GUI's PC build> --target ardopcf x6x00-mercury, "
                 "and tools/xara/build.sh --host for XARA")
    if a.only == "xara" and not a.xara:
        fail("--only xara needs --xara")
    if any(_port_open(p) for p in (18300, 18515, 18320, 18340)):
        fail("ports 18300, 18320, 18340 or 18515 are taken")
    tmp = tempfile.mkdtemp(prefix="winlink-modems-")
    procs = Procs()
    try:
        if a.only in (None, "ardop"):
            ardop_test(procs, tmp, os.path.abspath(a.ardopcf))
        if a.only in (None, "mercury"):
            mercury_test(procs, tmp, os.path.abspath(a.mercury), shutil.which(a.pat) or os.path.abspath(a.pat))
        if a.only in (None, "xara") and not a.xara:
            print("XARA: not tested, no --xara (tools/xara/build.sh --host builds it)")
        if a.only in (None, "xara") and a.xara:
            for registered in (False, True):
                xara_test(procs, tmp, os.path.abspath(a.xara), shutil.which(a.pat) or os.path.abspath(a.pat),
                          registered)
    finally:
        procs.stop_all()
        if a.keep:
            shutil.copytree(tmp, a.keep, dirs_exist_ok=True, ignore=shutil.ignore_patterns("*.sock", "air-*"))
        shutil.rmtree(tmp, ignore_errors=True)
    print("MODEMS OK")


if __name__ == "__main__":
    main()
