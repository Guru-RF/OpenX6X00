#!/usr/bin/env python3
"""Host test of rootfs/usr/sbin/x6100-winlink-mail (APP > Mail, #294) against a real Pat.

Pat v1.0.1 with tools/pat/patches, built for this PC, runs its HTTP API on a port of its own, with a config and
a mailbox under a temporary root (X6100_WINLINK_ROOT), as x6100-winlink lays them out on the radio. The helper is
run as the GUI runs it, and its answers checked:

- status: Winlink on, Pat answering, the counts; off: nothing asked of Pat;
- view: status and a list in one; abort with no connection under way;
- send: the checks (no recipient, no subject, no text, a subject too long), then a message into the outbox;
- list and read: the outbox, a message moved to the inbox and marked unread there, read (and so marked read);
- delete;
- gateways: from Pat's cached gateway list (fixtures in the test, no internet), ARDOP only and by band, the one
  connected to last on top;
- session: a connection to a CMS of the test's own over telnet (the only transport the helper takes on a PC
  besides the radio's modems): the CMS sends a secure login challenge, Pat asks for the password, the helper
  passes the question on and the answer back, the CMS checks the reply, the session ends with nothing to
  exchange; and a session Pat cannot start (no ARDOP modem): a failure, not a hang.

    python3 -B winlink_mail_test.py --pat <pat>      (make mail PAT=<pat>)

It prints MAIL OK.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-winlink-mail")
CALL = "N0CALL"
PASSWORD = "FOOBAR"
CHALLENGE = "23753528"
ANSWER = "72768415"     # wl2k-go's own test of the challenge and FOOBAR (fbb/secure_test.go)

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Root:
    def __init__(self, d, port):
        self.d, self.port = d, port
        for p in ("mnt/winlink/pat", "run/winlink", "var/lib/winlink/pat", "var/lib/winlink/state"):
            os.makedirs(os.path.join(d, p), exist_ok=True)
        self.conf(enabled=1)
        cfg = {"mycall": CALL, "secure_login_password": "", "locator": "JO20ab", "http_addr": f"127.0.0.1:{port}",
               "auto_download_size_limit": -1, "service_codes": ["PUBLIC"], "listen": [], "hamlib_rigs": {},
               "ardop": {"addr": f"127.0.0.1:{free_port()}", "arq_bandwidth": {"Forced": False, "Max": 500},
                         "rig": "", "ptt_ctrl": False, "beacon_interval": 0, "cwid_enabled": True},
               "version_reporting_disabled": True}
        with open(os.path.join(d, "var/lib/winlink/pat/config.json"), "w") as f:
            json.dump(cfg, f)
        # Pat's gateway list as cached on the card: Pat reads it and downloads nothing
        gw = {"ServerName": "test", "ErrorCode": 0, "Gateways": [
            {"Callsign": "N0GWA", "BaseCallsign": "N0GWA", "GatewayChannels": [
                {"SupportedModes": "ARDOP 500", "Frequency": 7101500, "Gridsquare": "JO21aa", "ServiceCode": "PUBLIC"},
                {"SupportedModes": "ARDOP 2000", "Frequency": 14105000, "Gridsquare": "JO21aa",
                 "ServiceCode": "PUBLIC"}]},
            {"Callsign": "N0GWB", "BaseCallsign": "N0GWB", "GatewayChannels": [
                {"SupportedModes": "ARDOP 500", "Frequency": 7102500, "Gridsquare": "JN18aa", "ServiceCode": "PUBLIC"},
                {"SupportedModes": "VARA 2750", "Frequency": 7103500, "Gridsquare": "JN18aa",
                 "ServiceCode": "PUBLIC"}]}]}
        with open(os.path.join(d, "mnt/winlink/pat/rmslist.json"), "w") as f:
            json.dump(gw, f)

    def conf(self, **kv):
        with open(os.path.join(self.d, "mnt/winlink.conf"), "w") as f:
            f.write("".join(f"{k}={v}\n" for k, v in kv.items()))

    def env(self):
        return dict(os.environ, X6100_WINLINK_ROOT=self.d, X6100_WINLINK_MAIL_PORT=str(self.port))


def helper(root, *args, stdin=None):
    p = subprocess.run([sys.executable, "-I", HELPER, *args], input=stdin, capture_output=True, text=True,
                       env=root.env(), timeout=90)
    try:
        out = json.loads(p.stdout)
    except ValueError:
        out = None
    return p.returncode, out, p.stderr


def pat_api(root, method, path, body=None, headers=None):
    import http.client
    con = http.client.HTTPConnection("127.0.0.1", root.port, timeout=10)
    con.request(method, path, body=body, headers={"X-Requested-With": "XMLHttpRequest", **(headers or {})})
    r = con.getresponse()
    data = r.read()
    con.close()
    return r.status, data


def fake_cms(srv, said):
    """A CMS over telnet, as wl2k-go's telnet dialer and B2F handshake expect it: login, the banner with a secure
    login challenge, then the client's handshake (its ;PR: reply) and its FF; FQ ends the session"""
    conn, _ = srv.accept()
    conn.settimeout(30)
    rd = conn.makefile("rb")

    def line():
        buf = b""
        while not buf.endswith(b"\r"):
            c = rd.read(1)
            if not c:
                raise EOFError
            buf += c
        return buf[:-1].decode("latin-1")

    try:
        conn.sendall(b"Callsign :\r")
        said["call"] = line()
        conn.sendall(b"Password :\r")
        said["password"] = line()
        conn.sendall(f"[WL2K-5.0-B2FWIHJM$]\r;PQ: {CHALLENGE}\rCMS>\r".encode())
        while True:
            l = line()
            if l.startswith(";PR: "):
                said["pr"] = l[5:]
            if l.startswith("FF") or l.startswith("FC") or l.startswith("F>"):
                said["turn"] = l
                break
        conn.sendall(b"FQ\r")
        time.sleep(0.5)
    except (EOFError, OSError) as e:
        said["error"] = str(e)
    finally:
        conn.close()


def session(root, url, answer=None, timeout=60):
    """The helper's session, as the GUI drives it: its lines read, a prompt answered"""
    p = subprocess.Popen([sys.executable, "-I", HELPER, "session", url], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=root.env())
    events = []
    end = time.time() + timeout
    while time.time() < end:
        l = p.stdout.readline()
        if not l:
            break
        ev = json.loads(l)
        events.append(ev)
        if "prompt" in ev and answer is not None:
            p.stdin.write(json.dumps({"prompt": {"id": ev["prompt"]["id"], "value": answer(ev["prompt"])}}) + "\n")
            p.stdin.flush()
        if "done" in ev or "error" in ev:
            break
    else:
        p.kill()
    p.stdin.close()
    p.wait(timeout=10)
    return p.returncode, events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pat", required=True)
    ap.add_argument("--keep", help="keep the root here")
    a = ap.parse_args()

    d = tempfile.mkdtemp(prefix="wlmail-")
    port = free_port()
    root = Root(d, port)
    lib = os.path.join(d, "var/lib/winlink")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PAT_", "ARDOP_", "XDG_"))
           and k.lower() not in ("http_proxy", "https_proxy", "all_proxy")}
    env.update(XDG_CONFIG_HOME=lib, XDG_DATA_HOME=os.path.join(d, "mnt/winlink"),
               XDG_STATE_HOME=os.path.join(lib, "state"), HOME=d)
    log = open(os.path.join(d, "pat.out"), "w")
    pat = subprocess.Popen([a.pat, "--log", os.path.join(d, "run/winlink/pat-own.log"), "http", "--addr",
                            f"127.0.0.1:{port}"], env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        run(root)
    finally:
        pat.terminate()
        try:
            pat.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pat.kill()
        log.close()
        if a.keep:
            shutil.copytree(d, a.keep, dirs_exist_ok=True)
        shutil.rmtree(d, ignore_errors=True)
    if failures:
        print(f"{len(failures)} FAILED")
        return 1
    print("MAIL OK")
    return 0


def run(root):
    rc, st, _ = helper(root, "status")
    check(rc == 0 and st["enabled"] and st["pat"] and st.get("in") == 0 and st.get("out") == 0
          and st["password"] == "none" and st["transport"] == "ardop", f"status, on and empty: {st}")
    rc, busy, _ = helper(root, "busy")
    check(rc == 0 and busy == {"busy": False}, f"busy, with no connection: {busy}")

    # send: the checks first, then a message into the outbox
    for msg, why in (({"to": "", "subject": "s", "body": "b"}, "To is empty"),
                     ({"to": "N0ONE", "subject": "", "body": "b"}, "the subject is empty"),
                     ({"to": "N0ONE", "subject": "s", "body": "  \n"}, "the message is empty"),
                     ({"to": "N0ONE", "subject": "x" * 129, "body": "b"}, "longer than 128"),
                     ({"to": "N0 ONE\x01", "subject": "s", "body": "b"}, "not an address")):
        rc, out, _ = helper(root, "send", stdin=json.dumps(msg))
        check(rc == 1 and why in out.get("error", ""), f"send refused: {why} ({out})")
    body = "Hello from the radio.\nSecond line, äöü."
    rc, out, _ = helper(root, "send", stdin=json.dumps({"to": "N0ONE, someone@example.com", "cc": "N0TWO",
                                                         "subject": "Field test", "body": body}))
    check(rc == 0 and out.get("ok"), f"send: {out}")

    rc, lst, _ = helper(root, "list", "out")
    check(rc == 0 and len(lst) == 1 and lst[0]["subject"] == "Field test" and lst[0]["from"] == CALL
          and "N0ONE" in lst[0]["to"] and "someone@example.com" in lst[0]["to"], f"list out: {lst}")
    mid = lst[0]["mid"] if lst else "X"
    rc, m, _ = helper(root, "read", "out", mid)
    check(rc == 0 and m["body"].strip() == body and m["cc"] == "N0TWO"
          and m["attachments"] == [], f"read out, its Latin-1 kept for the radio's fonts: {m}")
    # Latin-1 as typed; what is beyond it written out, before Pat would make it "?"
    rc, out, _ = helper(root, "send", stdin=json.dumps({"to": "N0ONE", "subject": "Grüße aus Köln",
                                                         "body": "„Łódź“ – 5 € …"}))
    rc2, lst2, _ = helper(root, "list", "out")
    m2 = [x for x in lst2 if x["subject"].startswith("Gr")] if rc2 == 0 else []
    rc3, r2, _ = helper(root, "read", "out", m2[0]["mid"]) if m2 else (1, {}, "")
    check(rc == 0 and m2 and m2[0]["subject"] == "Grüße aus Köln" and rc3 == 0
          and r2["body"].strip() == '"Lódz" - 5 EUR ...', f"Latin-1 kept, the rest written out: {r2.get('body')!r}")
    rc, out, _ = helper(root, "send", stdin=json.dumps({"to": "N0ONE", "subject": "a\u2013b", "body": "x"}))
    rc2, lst2, _ = helper(root, "list", "out")
    m3 = [x for x in lst2 if x["subject"] == "a-b"] if rc2 == 0 else []
    check(rc == 0 and len(m3) == 1, f"a subject with a dash beyond Latin-1 goes out as a-b: {lst2}")
    for x in m3:
        helper(root, "delete", "out", x["mid"])
    # a broken letter (the first byte of an a with an accent and nothing after it): sent with "?" for it, whatever
    # the locale, not refused as unreadable
    p = subprocess.run([sys.executable, "-I", HELPER, "send"], input=b'{"to": "N0ONE", "subject": "caf\xc3", '
                       b'"body": "x"}', capture_output=True, env=dict(root.env(), LC_ALL="C.UTF-8"), timeout=90)
    rc2, lst2, _ = helper(root, "list", "out")
    m4 = [x for x in lst2 if x["subject"].startswith("caf")] if rc2 == 0 else []
    check(p.returncode == 0 and [x["subject"] for x in m4] == ["caf?"], f"a broken letter sent as ?: {p.stdout} {m4}")
    for x in m4:
        helper(root, "delete", "out", x["mid"])
    if m2:
        helper(root, "delete", "out", m2[0]["mid"])
    rc, v, _ = helper(root, "view", "out")
    check(rc == 0 and v["status"]["out"] == 1 and [x["mid"] for x in v["list"]] == [mid], f"view: {v}")
    rc, out, _ = helper(root, "abort")
    check(rc == 1 and "no connection" in out["error"], f"abort with no connection: {out}")

    # into the inbox (Pat's move, as its web page does), unread there
    st_, _ = pat_api(root, "POST", "/api/mailbox/in", headers={"X-Pat-SourcePath": f"/api/mailbox/out/{mid}"})
    st2, _ = pat_api(root, "POST", f"/api/mailbox/in/{mid}/read", body=json.dumps({"Read": False}),
                     headers={"Content-Type": "application/json"})
    rc, st, _ = helper(root, "status")
    check(st_ == 200 and st2 == 200 and st.get("in") == 1 and st.get("unread") == 1 and st.get("out") == 0,
          f"status, one unread in the inbox: {st}")
    rc, m, _ = helper(root, "read", "in", mid)
    check(rc == 0 and m["subject"] == "Field test", "read in")
    rc, lst, _ = helper(root, "list", "in")
    check(rc == 0 and lst and not lst[0]["unread"], f"read marks it read: {lst}")
    rc, out, _ = helper(root, "read", "in", "../../etc")
    check(rc == 1 and "message ID" in out["error"], "read: a path for an ID refused")
    rc, out, _ = helper(root, "list", "trash")
    check(rc == 1, "list: no such mailbox")
    rc, out, _ = helper(root, "delete", "in", mid)
    rc2, lst, _ = helper(root, "list", "in")
    check(rc == 0 and rc2 == 0 and lst == [], "delete")

    # A message ID with # and $ in it, as a CMS service message had on the bench X6100 ("ON6URE#$XVEA", 19 Nov): it
    # lists, reads (quoted in Pat's URL) and deletes; a path is still refused
    cms = "N0CALL#$XVEA"
    body = b"Your account has no password recovery address.\r\n"
    head = (f"Mid: {cms}\r\nDate: 2026/10/27 19:11\r\nType: Private\r\nFrom: SERVICE\r\nTo: {CALL}\r\n"
            f"Subject: Missing Password Recovery Address\r\nMbo: SMTP\r\nBody: {len(body)}\r\n\r\n").encode()
    with open(os.path.join(root.d, "mnt/winlink/pat/mailbox", CALL, "in", cms + ".b2f"), "wb") as f:
        f.write(head + body)
    rc, lst, _ = helper(root, "list", "in")
    check(rc == 0 and [x["mid"] for x in lst] == [cms], f"list: a CMS message ID with # and $: {lst}")
    rc, m, _ = helper(root, "read", "in", cms)
    check(rc == 0 and m.get("subject") == "Missing Password Recovery Address" and "recovery" in m.get("body", ""),
          f"read: a CMS message ID with # and $: {m}")
    for bad in ("a/b", ".hidden", "../x", "ABCDEFGHIJKLM", "a b"):
        rc, out, _ = helper(root, "read", "in", bad)
        check(rc == 1 and "message ID" in out.get("error", ""), f"read: {bad!r} refused as an ID")
    rc, out, _ = helper(root, "delete", "in", cms)
    rc2, lst, _ = helper(root, "list", "in")
    check(rc == 0 and rc2 == 0 and lst == [], f"delete: a CMS message ID with # and $ ({out})")

    # gateways: ARDOP only, the nearest first (JO20ab: N0GWA in JO21 before N0GWB in JN18), by band
    rc, gws, _ = helper(root, "gateways")
    calls = [(g["call"], g["khz"]) for g in gws] if rc == 0 else []
    check(rc == 0 and calls == [("N0GWA", 7101.5), ("N0GWA", 14105.0), ("N0GWB", 7102.5)]
          and gws[0]["url"].startswith("ardop:///N0GWA?") and gws[0]["dist"] >= 0, f"gateways: {calls}")
    rc, gws40, _ = helper(root, "gateways", "40m")
    check(rc == 0 and [g["call"] for g in gws40] == ["N0GWA", "N0GWB"], f"gateways on 40 m: {gws40}")
    rc, out, _ = helper(root, "gateways", "40m;x")
    check(rc == 1, "gateways: not a band")

    # a session Pat cannot start (no ARDOP modem listens): it fails, and the gateway goes on top next time
    # Pat first asks whether the account is active: with no internet and an empty mailbox it cannot tell
    url = gws40[1]["url"] if rc == 0 or gws40 else "ardop:///N0GWB?freq=7102.5"
    asked = []

    def confirm(p):
        asked.append(p["kind"])
        return "confirmed" if p["kind"] == "pre-account-activation" else ""

    rc, events = session(root, url, answer=confirm, timeout=60)
    done = [e["done"] for e in events if "done" in e]
    check(asked == ["pre-account-activation"], f"the account question passed on: {asked}")
    check(done and not done[0]["ok"], f"session without a modem fails: {done}")
    rc, gws, _ = helper(root, "gateways")
    check(rc == 0 and gws[0]["url"] == url and gws[0]["recent"] and gws[0]["call"] == "N0GWB",
          "the gateway last connected to on top")
    rc, gws20, _ = helper(root, "gateways", "20m")
    check(rc == 0 and [g["call"] for g in gws20] == ["N0GWA"] and not gws20[0]["recent"],
          f"on 20 m the 40 m gateway called lately is not on top: {gws20}")
    rc, out, _ = helper(root, "session", "http://evil/?x")
    check(rc == 1 and "address" in out["error"], "session: not a gateway's address")

    # a session with a CMS of our own: the secure login challenge, Pat's password question answered through
    # the helper, the reply the CMS expects
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    said = {}
    t = threading.Thread(target=fake_cms, args=(srv, said), daemon=True)
    t.start()
    url = f"telnet://{CALL}:CMSTelnet@127.0.0.1:{srv.getsockname()[1]}/wl2k"
    prompts = []

    def answer(p):
        prompts.append(p)
        return PASSWORD

    rc, events = session(root, url, answer=answer)
    t.join(timeout=10)
    srv.close()
    done = [e["done"] for e in events if "done" in e]
    logs = [e["log"] for e in events if "log" in e]
    check(prompts and prompts[0]["kind"] == "password", f"Pat asked for the password: {prompts}")
    check(said.get("call") == CALL and said.get("pr") == ANSWER and said.get("turn", "").startswith("FF"),
          f"the CMS got the right reply: {said}")
    check(done and done[0]["ok"] and done[0]["received"] == 0, f"session done: {done}")
    check(any("Connected to" in l for l in logs), f"Pat's log lines followed: {logs[-5:]}")
    check(not any(l.startswith("Pat ") or "Starting HTTP service" in l for l in logs),
          "the log from before the session not repeated")

    # Mercury and XARA: Pat's VARA HF transport, the gateways that run VARA
    for m, name in (("mercury", "Mercury"), ("xara", "XARA")):
        root.conf(enabled=1, modem=m)
        rc, st, _ = helper(root, "status")
        rc2, gws, _ = helper(root, "gateways")
        check(rc == 0 and st["transport"] == "varahf" and st["modem_name"] == name and st["modem_choice"] == m
              and rc2 == 0
              and [g["call"] for g in gws] == ["N0GWB"] and gws[0]["url"].startswith("varahf:///N0GWB"),
              f"{name}: VARA HF gateways ({gws})")
    root.conf(enabled=1)

    # off: nothing asked
    root.conf(enabled=0)
    rc, st, _ = helper(root, "status")
    check(rc == 0 and not st["enabled"] and not st["pat"] and "in" not in st, f"status, off: {st}")
    # the app's Modem (#322, #323): the modems on this radio, the one chosen, and whether the supervisor runs it
    check(st["modems"] == [] and st["modem_choice"] == "ardop" and st["modem_state"] == "off", f"no modem here: {st}")
    os.makedirs(os.path.join(root.d, "usr/bin"), exist_ok=True)
    for prog in ("ardopcf", "xara"):
        with open(os.path.join(root.d, "usr/bin", prog), "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(os.path.join(root.d, "usr/bin", prog), 0o755)
    root.conf(enabled=0, modem="xara")
    with open(os.path.join(root.d, "run/winlink/status"), "w") as f:
        f.write("state=running\ntext=Running as N0AAA\npat=running\nardop=off\nmercury=off\nxara=running\n"
                "modem=ready\n")
    rc, st, _ = helper(root, "status")
    check(rc == 0 and st["modems"] == [{"id": "ardop", "name": "ARDOP"}, {"id": "xara", "name": "XARA"}]
          and st["modem_choice"] == "xara" and st["modem_state"] == "running" and st["modem"] == "ready",
          f"the modems here, XARA chosen and running: {st}")
    os.remove(os.path.join(root.d, "run/winlink/status"))
    root.conf(enabled=1)


if __name__ == "__main__":
    sys.exit(main())
