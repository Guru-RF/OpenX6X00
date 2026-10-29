#!/usr/bin/env python3
"""The real Winlink modems against src/winlink.c on the PC (winlink_host).

1. ardopcf, protocol version 1, as x6100-winlink starts it: it connects at 12000 Hz with keys of 30 s, and the
   ID frame it is asked for (SENDID on its host port) is played whole.
2. Mercury through x6x00-mercury, protocol version 2, and Pat v1.0.0's varahf transport, as on the radio: two
   stations, each a winlink_host and a bridge, what one transmits the other hears (with a little noise). Pat B
   listens; Pat A sends B a message peer to peer. Pat starts the modem (Mercury answers VERSION), connects,
   sends, disconnects, and the message is in B's inbox.

Run it in a network namespace with loopback only (`make modems` does, with unshare -rn): Pat then reaches
nothing outside, no Winlink web service, no CMS. Pat's own account check before a connect is also skipped
(its state file says it was done) and an HTTP proxy that is not there takes anything else.
Prints MODEMS OK at the end; exits 1 at the first failure. --keep DIR keeps the logs there.
"""

import argparse
import array
import json
import os
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


def ardop_test(procs, tmp, ardopcf):
    print("ardopcf, protocol 1")
    h = host(procs, tmp, "ardop", 14540)
    log = os.path.join(tmp, "ardopcf.log")
    a = procs.start([ardopcf, "--nologfile", "18515", f"SOCK:{h['sock']}", f"SOCK:{h['sock']}"], log)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ardopcf", required=True)
    ap.add_argument("--mercury", required=True)
    ap.add_argument("--pat", required=True)
    ap.add_argument("--keep", help="keep the logs in this directory")
    ap.add_argument("--only", choices=("ardop", "mercury"))
    a = ap.parse_args()
    for p in (a.ardopcf, a.mercury) if a.only is None else ((a.ardopcf,) if a.only == "ardop" else (a.mercury,)):
        if not os.access(p, os.X_OK):
            fail(f"{p} is not there: cmake --build <the GUI's PC build> --target ardopcf x6x00-mercury")
    if _port_open(18300) or _port_open(18515):
        fail("ports 18300 or 18515 are taken")
    tmp = tempfile.mkdtemp(prefix="winlink-modems-")
    procs = Procs()
    try:
        if a.only in (None, "ardop"):
            ardop_test(procs, tmp, os.path.abspath(a.ardopcf))
        if a.only in (None, "mercury"):
            mercury_test(procs, tmp, os.path.abspath(a.mercury), shutil.which(a.pat) or os.path.abspath(a.pat))
    finally:
        procs.stop_all()
        if a.keep:
            shutil.copytree(tmp, a.keep, dirs_exist_ok=True, ignore=shutil.ignore_patterns("*.sock", "air-*"))
        shutil.rmtree(tmp, ignore_errors=True)
    print("MODEMS OK")


if __name__ == "__main__":
    main()
