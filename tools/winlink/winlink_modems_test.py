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
   sends, disconnects, and the message is in B's inbox.
3. XARA (our patched build, its --audio x6x00 on the GUI's socket, protocol version 2 at 12000 Hz) and Pat's varahf,
   the same way, twice: with the settings x6100-winlink writes for the free speed (no `registered`), B's XARA says
   LINK UNREGISTERED and no level goes past VARA's free 4; with `registered` on A (the Winlink page's VARA licence
   tick box), B's says LINK REGISTERED, as VARA's callee follows the caller's bit. Both stop on SIGTERM. First, at
   the free speed and before any Pat: A's XARA calls B's, which neither answers nor keys (on the radio XARA starts
   with listening off; Pat B's listen turns it on). The free run has calls of a European kind (ON4AAA, ON4BBB), the
   registered one US calls. Both XARAs identify in CW as Pat's session closes, with either kind: Pat sends CWID ON,
   which upstream's XARA 0.1.3 takes only for a US call and our patch 0002 for any. Before any Pat, with no CWID ON,
   A's call that B does not answer ends without one with the European call, as upstream's does.

Run it in a network namespace with loopback only (`make modems` does, with unshare -rn): Pat then reaches
nothing outside, no Winlink web service, no CMS. Pat's own account check before a connect is also skipped
(its state file says it was done) and an HTTP proxy that is not there takes anything else.
Prints MODEMS OK at the end; exits 1 at the first failure. --keep DIR keeps the logs there.
"""

import argparse
import array
import json
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
         "played": os.path.join(tmp, name + ".played")}
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
tx_gain_db = 0.0
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
    m = procs.start([mercury, f"SOCK:{h['sock']}", "-C", ini], log, env)
    return h, m, log


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
    """Pat's start of the modem, the order that hung it: the WRONG for CWID ON read before VERSION is asked.
    Pat-Vara's Version() then waits, with no timeout, for a line that starts VERSION or WRONG"""
    c = socket.create_connection(("127.0.0.1", port), timeout=5)
    got = b""
    for cmd in ("PUBLIC ON", "CWID ON", "COMPRESSION TEXT", f"MYCALL {CALL_A}", "LISTEN OFF"):
        c.sendall(cmd.encode() + b"\r")
    end = time.time() + 5
    while b"WRONG\r" not in got and time.time() < end:
        got += c.recv(4096)
    check(b"WRONG\r" in got, "Mercury answers Pat's CWID ON with WRONG")
    got = b""
    c.sendall(b"VERSION\r")
    end = time.time() + 3
    while not any(l.startswith(b"VERSION") for l in got.split(b"\r")) and time.time() < end:
        got += c.recv(4096)
    line = next((l for l in got.split(b"\r") if l.startswith(b"VERSION")), b"").decode()
    check(line == "VERSION Mercury 1.9.21", f"and VERSION with a VERSION line, what Pat waits for ({line!r})")
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
    pb.stdin.close()
    procs.stop(pb)
    for m, h, n in ((ma, ha, "A"), (mb, hb, "B")):
        procs.stop(m)
        check(m.returncode == 0 and wait_for(lambda: "the modem gone" in text(h["log"]), 5),
              f"station {n}: x6x00-mercury stopped on SIGTERM, the GUI saw it go")


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
    # kind of call (patch 0002; upstream's XARA 0.1.3 drops CWID ON and identifies only with a US call). The ID is the
    # end of a key like any other, through the GUI's socket and checks: played whole. B's waits out A's (blank words)
    kind = "US" if registered else "European"
    for log, h, n in ((la, ha, "A"), (lb, hb, "B")):
        check("> Cwid(true)" in text(log), f"Pat asked {n}'s XARA for a CW ID (CWID ON)")
        ided = lambda: "  tx cw" in text(log) and text(log).rfind("< DISCONNECTED") > text(log).rfind("  tx cw")
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
