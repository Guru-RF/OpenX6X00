#!/usr/bin/env python3
"""Tests of the Winlink supervisor (rootfs/usr/sbin/x6100-winlink) on a PC: each test loads it with
X6100_WINLINK_ROOT set to a new temporary directory, so every path it uses is under that one, and calls its
functions. Standard library only:

    python3 -B tools/winlink/winlink_test.py [-v]
"""
import configparser
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SUPERVISOR = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-winlink")
FIXTURES = os.path.join(HERE, "fixtures")
sys.dont_write_bytecode = True


def load(root):
    """The supervisor as a module, its paths under root"""
    os.environ["X6100_WINLINK_ROOT"] = root
    loader = importlib.machinery.SourceFileLoader("x6100_winlink", SUPERVISOR)
    spec = importlib.util.spec_from_loader("x6100_winlink", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def fixture(name):
    return read(os.path.join(FIXTURES, name))


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class Root(unittest.TestCase):
    """A supervisor of its own in an empty root, with /mnt and /run/winlink there"""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-winlink.")
        self.addCleanup(shutil.rmtree, self.root)
        self.wl = load(self.root)
        os.makedirs(os.path.join(self.root, "mnt"))
        os.makedirs(self.wl.RUN)

    def conf(self, **kw):
        with open(self.wl.CONF, "w") as f:
            f.write("".join(f"{k}={v}\n" for k, v in kw.items()))

    def pat_config(self):
        with open(self.wl.PAT_CONFIG) as f:
            return json.load(f)


class Bandwidth(Root):
    def test_each_bandwidth_reaches_pat(self):
        for bw in (200, 500, 1000, 2000):
            self.conf(enabled=1, mycall="ON6URE", bandwidth=bw)
            want = self.wl.wanted()
            self.assertEqual(want["bandwidth"], bw)
            self.wl.write_config(want, "")
            self.assertEqual(self.pat_config()["ardop"]["arq_bandwidth"], {"Forced": False, "Max": bw})

    def test_other_values_are_500(self):
        for bw in ("", "300", "2000MAX", "5000"):
            self.conf(enabled=1, mycall="ON6URE", bandwidth=bw)
            self.assertEqual(self.wl.wanted()["bandwidth"], 500, bw)


WANT = {"mycall": "ON6URE", "locator": "JO20ab", "bandwidth": 500, "modem": "ardop", "mercury_bandwidth": 2300}


class Modem(Root):
    """modem=ardop|mercury and mercury_bandwidth: Pat's varahf, Mercury's settings and its start"""

    def test_defaults(self):
        self.conf(enabled=1, mycall="ON6URE")
        self.assertEqual((self.wl.wanted()["modem"], self.wl.wanted()["mercury_bandwidth"]), ("ardop", 2300))
        for modem, bw in (("vara", "2000"), ("", "2300MAX"), ("ARDOP", "")):
            self.conf(enabled=1, mycall="ON6URE", modem=modem, mercury_bandwidth=bw)
            self.assertEqual((self.wl.wanted()["modem"], self.wl.wanted()["mercury_bandwidth"]), ("ardop", 2300))

    def test_varahf_reaches_pat(self):
        for bw in (500, 2300, 2750):
            self.conf(enabled=1, mycall="ON6URE", modem="mercury", mercury_bandwidth=bw)
            want = self.wl.wanted()
            self.assertEqual((want["modem"], want["mercury_bandwidth"]), ("mercury", bw))
            self.wl.write_config(want, "")
            cfg = self.pat_config()
            self.assertEqual(cfg["varahf"], {"addr": "127.0.0.1:8300", "bandwidth": bw, "rig": "x6x00",
                                             "ptt_ctrl": False})
            self.assertEqual(cfg["listen"], [])                     # no P2P call answered
            self.assertEqual(cfg["ardop"]["addr"], "127.0.0.1:8515")  # ARDOP's kept for a change back

    def test_varahf_merged(self):
        self.wl.write_config(dict(WANT), "")
        cfg = self.pat_config()
        cfg["varahf"].update(addr="localhost:9999", own_key=1)     # Pat's Settings page, or by hand
        with open(self.wl.PAT_CONFIG, "w") as f:
            json.dump(cfg, f)
        self.wl.write_config(dict(WANT, modem="mercury"), "")
        self.assertEqual(self.pat_config()["varahf"]["addr"], "127.0.0.1:8300")
        self.assertEqual(self.pat_config()["varahf"]["own_key"], 1)

    def ini(self):
        ini = configparser.ConfigParser()
        self.assertEqual(ini.read(self.wl.MERCURY_INI), [self.wl.MERCURY_INI])
        return ini

    def test_mercury_ini(self):
        self.wl.write_mercury_ini(dict(WANT, modem="mercury"))
        ini = self.ini()
        self.assertEqual(dict(ini["main"]), {"ui_enabled": "false", "waterfall_enabled": "false",
                                             "arq_tcp_base_port": "8300", "broadcast_tcp_port": "8100",
                                             "callsign": "ON6URE"})
        self.assertEqual(dict(ini["audio"]), {"tx_gain_db": "0.0", "tx_delay_ms": "10"})
        self.assertEqual(ini["channel"]["busy_detect"], "false")
        self.assertEqual(ini["tnc"]["keepalive_s"], "60")
        self.assertEqual(ini["store"]["enabled"], "false")
        self.wl.write_mercury_ini(dict(WANT, mycall=""))           # no callsign yet: Pat sends it
        self.assertNotIn("callsign", self.ini()["main"])

    def test_mercury_started(self):
        """x6x00-mercury on the GUI's socket with its settings, no MERCURY_* test hook from outside, HOME in RAM"""
        os.makedirs(os.path.dirname(self.wl.MERCURY), exist_ok=True)
        with open(self.wl.MERCURY, "w") as f:
            f.write('#!/bin/sh\necho "$@" > args\necho "${MERCURY_RX_DUMP-none} $HOME" > env\n')
        os.chmod(self.wl.MERCURY, 0o755)
        os.environ["MERCURY_RX_DUMP"] = "/tmp/dump"
        self.addCleanup(os.environ.pop, "MERCURY_RX_DUMP", None)
        mercury = self.wl.Program("Mercury (x6x00-mercury)", self.wl.MERCURY_LOG)
        self.wl.start_mercury(mercury, dict(WANT, modem="mercury"))
        mercury.proc.wait(10)
        self.assertEqual(read(os.path.join(self.wl.RUN, "args")).split(),
                         [f"SOCK:{self.root}/run/ardop.sock", "-C", self.wl.MERCURY_INI])
        self.assertEqual(read(os.path.join(self.wl.RUN, "env")).split(), ["none", self.wl.RUN])
        self.assertTrue(os.path.exists(self.wl.MERCURY_INI))

    def test_status_names_the_modem(self):
        self.wl.report(Running(), Running(), dict(WANT, modem="mercury"), {"modem": "waiting"})
        st = read(self.wl.STATUS)
        self.assertIn("text=Running as ON6URE; Mercury waits for the radio's audio (the GUI)\n", st)
        self.assertIn("ardop=off\nmercury=running\n", st)
        ardop = self.wl.Program("ARDOP (ardopcf)", self.wl.ARDOP_LOG)
        ardop.failed("ARDOP (ardopcf) stopped (code 1)")
        self.wl.report(Running(), ardop, dict(WANT), {"modem": "off"})
        st = read(self.wl.STATUS)
        self.assertIn("text=ARDOP (ardopcf) stopped (code 1); starting it again in 5 s\n", st)
        self.assertIn("ardop=stopped\nmercury=off\n", st)

    def test_modem_states(self):
        modems = {"ardop": Running(alive=False), "mercury": Running()}
        self.assertEqual(self.wl.modem_states(modems, "ardop"), {"ardop": "stopped", "mercury": "running"})
        modems["mercury"] = Running(alive=False)
        self.assertEqual(self.wl.modem_states(modems, "mercury"), {"ardop": "off", "mercury": "stopped"})

    def test_turned_on_keeps_the_choice(self):
        self.conf(enabled=0, mycall="ON6URE", modem="mercury", mercury_bandwidth=500)
        self.wl.set_enabled(True)
        self.assertEqual(read(self.wl.CONF), "enabled=1\nmycall=ON6URE\nlocator=\nbandwidth=500\nmodem=mercury\n"
                                             "mercury_bandwidth=500\n")


class Password(Root):
    def status(self):
        with open(self.wl.STATUS) as f:
            return dict(line.rstrip("\n").split("=", 1) for line in f)

    def test_where_it_is_set(self):
        self.conf(enabled=1, mycall="ON6URE")
        self.assertEqual(self.wl.password_where(), "none")          # no Pat config yet
        self.wl.write_config(self.wl.wanted(), "")
        self.assertEqual(self.wl.password_where(), "none")
        cfg = self.pat_config()
        cfg["secure_login_password"] = "typed-in-pat"                # Pat's own Settings page
        with open(self.wl.PAT_CONFIG, "w") as f:
            json.dump(cfg, f)
        self.assertEqual(self.wl.password_where(), "pat")
        self.wl.write_config(self.wl.wanted(), "")                   # kept while the page has none (#80)
        self.assertEqual(self.wl.password_where(), "pat")
        os.makedirs(self.wl.LIB, exist_ok=True)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("from-the-page\n")
        self.assertEqual(self.wl.password_where(), "page")

    def test_in_the_status_never_the_password(self):
        os.makedirs(self.wl.LIB, exist_ok=True)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("s3cret\n")
        self.wl.say("error", "Pat stopped (code 1: login s3cret refused)")
        st = self.status()
        self.assertEqual(st["password"], "page")
        self.assertNotIn("s3cret", read(self.wl.STATUS))

    def test_broken_pat_config(self):
        os.makedirs(os.path.dirname(self.wl.PAT_CONFIG))
        with open(self.wl.PAT_CONFIG, "w") as f:
            f.write("{not json")
        self.assertEqual(self.wl.password_where(), "none")


class Notes(Root):
    def test_appended_to_pats_log(self):
        self.assertFalse(self.wl.note("before Pat"))                 # Pat empties it as it starts: none before
        self.assertFalse(os.path.exists(self.wl.PAT_OWN_LOG))
        pat = os.open(self.wl.PAT_OWN_LOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_APPEND, 0o666)
        os.write(pat, b"2026/10/29 12:15:54 QSY ardop: 7061.5\n")
        self.assertTrue(self.wl.note("a line\nof ours"))
        os.write(pat, b"2026/10/29 12:15:57 Connecting to DM6TS (ardop)...\n")
        os.close(pat)
        lines = read(self.wl.PAT_OWN_LOG).splitlines()
        self.assertEqual(len(lines), 3)
        self.assertRegex(lines[1], r"^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d OpenX6X00: a line of ours$")
        self.assertTrue(lines[2].endswith("Connecting to DM6TS (ardop)..."))

    def test_never_the_password(self):
        os.makedirs(self.wl.LIB)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("s3cret\n")
        open(self.wl.PAT_OWN_LOG, "w").close()
        self.wl.note("login s3cret refused")
        self.assertNotIn("s3cret", read(self.wl.PAT_OWN_LOG))

    def test_no_password_said_when_pat_answers(self):
        open(self.wl.PAT_OWN_LOG, "w").close()
        self.wl.pat_up({"mycall": "M7SVA"})
        self.assertIn("OpenX6X00: no Winlink password for M7SVA", read(self.wl.PAT_OWN_LOG))
        os.makedirs(self.wl.LIB)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("pw\n")
        open(self.wl.PAT_OWN_LOG, "w").close()
        self.wl.pat_up({"mycall": "M7SVA"})
        self.assertEqual(read(self.wl.PAT_OWN_LOG), "")

    def test_trimmed(self):
        pat = os.open(self.wl.PAT_OWN_LOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_APPEND, 0o666)
        os.write(pat, b"x" * (self.wl.LOG_MAX + 1))
        self.wl.trim_logs()
        os.write(pat, b"next\n")
        os.close(pat)
        self.assertEqual(read(self.wl.PAT_OWN_LOG).encode(), b"next\n")


class Running:
    """A stand-in for a Program that runs"""

    def __init__(self, alive=True, answered=True):
        self._alive, self.answered = alive, answered

    def alive(self):
        return self._alive


ESTABLISHED = "[STATUS: ARQ CONNECTION ESTABLISHED WITH {}, SESSION BW = {} HZ]"


class Logs(Root):
    """A Watch on the logs in the root, Pat's status given"""

    def setUp(self):
        super().setUp()
        self.pat_status = {"connected": False, "dialing": False, "http_clients": []}
        self.wl.pat_status = lambda: self.pat_status
        for path in (self.wl.PAT_OWN_LOG, self.wl.ARDOP_LOG, self.wl.MERCURY_LOG, self.wl.EVENT_LOG):
            open(path, "w").close()
        self.watch = self.wl.Watch()

    def add(self, path, text):
        with open(path, "a") as f:
            f.write(text if text.endswith("\n") else text + "\n")

    def poll(self, pat="", ardop="", events=(), mercury="", kind="ardop"):
        if pat:
            self.add(self.wl.PAT_OWN_LOG, pat)
        if ardop:
            self.add(self.wl.ARDOP_LOG, ardop)
        if mercury:
            self.add(self.wl.MERCURY_LOG, mercury)
        for e in events:
            self.add(self.wl.EVENT_LOG, e if isinstance(e, str) else json.dumps(e))
        return self.watch.poll(Running(), Running(), kind)

    def notes(self):
        return [ln for ln in read(self.wl.PAT_OWN_LOG).splitlines() if " OpenX6X00: " in ln]


class Outcomes(Logs):
    """Pat's log (the real ones from #18 and #80), ardopcf's console and Pat's event log, read at each poll"""

    def test_km4gkd_one_message_sent(self):
        k = self.poll(pat=fixture("km4gkd-w4nwg-80m.pat.log"))
        self.assertEqual(k["last_result"], "delivered")
        self.assertEqual((k["last_gateway"], k["last_freq"]), ("W4NWG", 3587000))
        self.assertEqual(k["last_text"], "Done with W4NWG: 1 sent, 0 received")
        self.assertEqual(k["last_next"], "")
        self.assertEqual(len(self.notes()), 1)
        self.assertTrue(self.notes()[0].endswith("OpenX6X00: Done with W4NWG: 1 sent, 0 received"))
        self.poll()
        self.assertEqual(len(self.notes()), 1)          # said once

    def test_m7sva_step_by_step(self):
        log = fixture("m7sva-dm6ts-40m.pat.log").splitlines(keepends=True)
        k = self.poll(pat="".join(log[:2]), ardop="Audio socket /run/ardop.sock connected")
        self.assertEqual((k["modem"], k["last_result"]), ("ready", "dialing"))
        self.assertEqual(k["last_text"], "Calling DM6TS on 7.061.500")
        k = self.poll(pat="".join(log[2:9]), ardop=ESTABLISHED.format("DM6TS", 200))
        self.assertEqual(k["last_result"], "connected")
        self.assertEqual(k["last_text"], "Connected to DM6TS at 200 Hz, exchanging mail")
        self.assertEqual(self.notes(), [])
        k = self.poll(pat="".join(log[9:]), ardop="[STATUS: ARQ CONNECTION ENDED WITH DM6TS]",
                      events=[{"what": "connect", "success": True, "operation": "connect ardop:///DM6TS?freq=7061.5"},
                              {"what": "exchange", "targetcall": "DM6TS", "success": True,
                               "sent": ["ECTEM7Q3ODBL"], "received": None}])
        self.assertEqual(k["last_result"], "delivered")
        self.assertEqual((k["last_sent"], k["last_received"], k["last_bw"]), (1, 0, 200))
        self.assertEqual(k["last_text"], "Done with DM6TS at 200 Hz: 1 sent, 0 received")

    def test_ea5jtt_password_prompt_timed_out(self):
        k = self.poll(pat=fixture("ea5jtt-hb9mm-20m.pat.log"))
        self.assertEqual(k["last_result"], "failed")
        self.assertEqual((k["last_gateway"], k["last_freq"]), ("HB9MM", 14107700))
        self.assertIn("password prompt timed out", k["last_text"])
        self.assertTrue(k["last_next"].startswith("Set the Winlink password on the radio's Winlink page"))

    def test_ea5jtt_each_connection_noted(self):
        log = fixture("ea5jtt-hb9mm-20m.pat.log").splitlines(keepends=True)
        self.poll(pat="".join(log[:16]))
        self.poll(pat="".join(log[16:]))
        self.assertEqual(len(self.notes()), 2)

    def test_the_bandwidth_capped(self):
        k = self.poll(pat="2026/10/29 12:15:54 QSY ardop: 7061.5\n"
                          "2026/10/29 12:15:54 ARDOP bandwidth 500MAX, the widest set on the radio "
                          "(Operating > Winlink), not the 2000MAX asked\n"
                          "2026/10/29 12:15:57 Connecting to DM6TS (ardop)...\n",
                      ardop=ESTABLISHED.format("DM6TS", 500))
        self.assertEqual((k["last_bw_asked"], k["last_bw_capped"], k["last_bw"]), ("2000MAX", "500MAX", 500))

    def failed(self, pat, ardop="", refused=""):
        self.poll(pat="2026/10/29 09:47:01 QSY ardop: 14107.7\n2026/10/29 09:47:04 Connecting to HB9MM (ardop)...")
        if refused:
            self.watch.outcome.refusal(refused)
        k = self.poll(pat=pat, ardop=ardop)
        self.assertEqual(k["last_result"], "failed")
        self.assertEqual(len(self.notes()), 1)
        return k

    TIMEOUT = "2026/10/29 09:48:10 Unable to establish connection to remote: Connect timeout"

    def test_no_answer(self):
        k = self.failed(self.TIMEOUT, "[STATUS: CONNECT TO HB9MM FAILED]")
        self.assertEqual(k["last_text"], "No answer from HB9MM on 14.107.700")
        self.assertIn("500 or 200 Hz", k["last_next"])

    def test_bandwidth_rejected(self):
        k = self.failed(self.TIMEOUT, "[STATUS: ARQ CONNECTION REJECTED BY HB9MM, INCOMPATIBLE BW.]")
        self.assertIn("refused the bandwidth", k["last_text"])
        self.assertTrue(k["last_next"].startswith("Choose 500 Hz"))

    def test_gateway_busy(self):
        k = self.failed(self.TIMEOUT, "[STATUS: ARQ CONNECTION REJECTED BY HB9MM, REMOTE STATION BUSY.]")
        self.assertEqual(k["last_text"], "HB9MM is busy with another station")

    def test_another_stations_lines_left_out(self):
        k = self.failed(self.TIMEOUT, "[STATUS: ARQ CONNECTION REJECTED BY DM6TS, INCOMPATIBLE BW.]")
        self.assertEqual(k["last_text"], "No answer from HB9MM on 14.107.700")

    def test_busy_channel(self):
        k = self.failed("2026/10/29 09:47:05 Waiting for clear channel...\n"
                        "2026/10/29 09:52:05 Unable to establish connection to remote: aborted while waiting for "
                        "clear channel")
        self.assertIn("stayed busy", k["last_text"])

    def test_secure_login_failed(self):
        k = self.failed("2026/10/29 09:47:23 Connected to HB9MM (ardop)\n;PQ: 76303167\nCMS via HB9MM >\n"
                        "2026/10/29 09:47:40 Exchange failed: [1] Secure login failed - account password does not "
                        "match. - Disconnecting (88.90.2.192)")
        self.assertEqual(k["last_text"], "HB9MM refused the password")
        self.assertIn("capitals", k["last_next"])

    def test_link_dropped(self):
        k = self.failed("2026/10/29 09:47:23 Connected to HB9MM (ardop)\nRemote accepted ABC123\n"
                        "Transmitting [test] [offset 0]\n2026/10/29 09:49:00 Exchange failed: EOF",
                        ESTABLISHED.format("HB9MM", 2000))
        self.assertEqual(k["last_text"], "The link to HB9MM at 2000 Hz dropped (EOF)")
        self.assertIn("500 or 200 Hz", k["last_next"])

    def test_app_holding_the_radio(self):
        k = self.failed(self.TIMEOUT, refused="FT8 is open, close it")
        self.assertEqual(k["last_text"], "No answer from HB9MM: the radio did not transmit, FT8 is open, close it")
        self.assertEqual(k["last_next"], "Close the app on the radio, then connect again")

    def test_swr_hold(self):
        k = self.failed(self.TIMEOUT, refused="high SWR, the hold ends in 4 min")
        self.assertIn("Check the antenna", k["last_next"])

    def test_qsy_refused(self):
        k = self.poll(pat="2026/10/29 09:47:01 QSY ardop: 14107.7\n"
                          "2026/10/29 09:47:01 Unable to QSY: RPRT -9")
        self.assertEqual(k["last_result"], "failed")
        self.assertIn("could not tune", k["last_text"])

    def test_session_and_browsers(self):
        self.pat_status = {"connected": False, "dialing": True, "http_clients": ["192.168.1.30:51234"]}
        k = self.poll()
        self.assertEqual((k["session"], k["browsers"], k["service"]), ("dialing", 1, "running"))
        self.wl.pat_status = lambda: None                # no answer within a second
        self.assertEqual(self.poll()["session"], "unknown")
        self.assertNotIn("last_result", self.poll())

    def test_modem_follows_ardopcf(self):
        self.assertEqual(self.poll()["modem"], "waiting")
        self.assertEqual(self.poll(ardop="Audio socket /run/ardop.sock connected")["modem"], "ready")
        self.assertEqual(self.poll(ardop="Audio socket full, 'R' message dropped")["modem"], "ready")
        self.assertEqual(self.poll(ardop="Audio socket /run/ardop.sock closed: the other end went away")["modem"],
                         "waiting")
        self.assertEqual(self.poll(ardop="Audio socket /run/ardop.sock is not there yet, trying again every "
                                         "second")["modem"], "waiting")

    def test_a_connection_without_a_qsy_has_no_frequency(self):
        self.poll(pat=fixture("km4gkd-w4nwg-80m.pat.log"))
        k = self.poll(pat="2026/10/28 01:10:00 Connecting to W4NWG (ardop)...")
        self.assertEqual((k["last_result"], k["last_freq"], k["last_text"]), ("dialing", "", "Calling W4NWG"))

    def test_pat_restarted_during_a_connection(self):
        self.poll(pat="2026/10/29 09:47:01 QSY ardop: 14107.7\n2026/10/29 09:47:04 Connecting to HB9MM (ardop)...")
        self.watch.pat_started()
        open(self.wl.PAT_OWN_LOG, "w").close()           # Pat empties its log
        k = self.poll()
        self.assertEqual((k["last_result"], k["last_text"]), ("failed", "Pat stopped during the connection to HB9MM"))
        self.assertEqual(self.notes(), [])               # not in Pat's new log

    def test_waits_for_the_audio_until_ardopcf_is_on_it(self):
        open(self.wl.ARDOP_SOCK, "w").close()            # the socket there, ardopcf not on it yet
        self.wl.report(Running(), Running(), {"mycall": "ON6URE"}, {"modem": "waiting"})
        self.assertIn("ARDOP waits for the radio's audio", read(self.wl.STATUS))
        self.wl.report(Running(), Running(), {"mycall": "ON6URE"}, {"modem": "ready"})
        self.assertIn("text=Running as ON6URE\n", read(self.wl.STATUS))


class MercuryOutcomes(Logs):
    """With Mercury: Pat's varahf lines, x6x00-mercury's and Mercury's own (the PC test's logs, tools/winlink)"""

    def test_pc_p2p_delivered(self):
        log = fixture("pc-n0aaa-n0bbb-mercury.mercury.log").splitlines(keepends=True)
        k = self.poll(mercury="".join(log[:27]), kind="mercury")
        self.assertEqual(k["modem"], "ready")                         # x6x00-mercury on the socket
        pat = fixture("pc-n0aaa-n0bbb-mercury.pat.log").splitlines(keepends=True)
        k = self.poll(pat="".join(pat[:2]), mercury="".join(log[27:]), kind="mercury")
        self.assertEqual((k["last_result"], k["last_gateway"], k["last_text"]), ("dialing", "N0BBB", "Calling N0BBB"))
        self.assertEqual(k["modem"], "waiting")                       # x6x00-mercury: stopping
        k = self.poll(pat="".join(pat[2:]), events=fixture("pc-n0aaa-n0bbb-mercury.events.log").splitlines(),
                      kind="mercury")
        self.assertEqual(k["last_result"], "delivered")
        self.assertEqual(k["last_text"], "Done with N0BBB: 1 sent, 0 received")
        self.assertEqual((k["last_bw"], k["last_bw_asked"]), ("", ""))   # Pat does not log Mercury's
        self.assertEqual(len(self.notes()), 1)

    def test_modem_follows_x6x00_mercury(self):
        self.assertEqual(self.poll(kind="mercury")["modem"], "waiting")
        self.assertEqual(self.poll(mercury="x6x00-mercury: /run/ardop.sock is not there yet, trying again every "
                                           "second", kind="mercury")["modem"], "waiting")
        self.assertEqual(self.poll(mercury="x6x00-mercury: connected to /run/ardop.sock", kind="mercury")["modem"],
                         "ready")
        self.assertEqual(self.poll(mercury="x6x00-mercury: the GUI's socket closed: the GUI went away",
                                   kind="mercury")["modem"], "waiting")

    def test_the_modem_that_stopped_left_out(self):
        """ARDOP chosen after Mercury: Mercury's last lines come after ardopcf is on the socket (seen live)"""
        self.poll(mercury="x6x00-mercury: connected to /run/ardop.sock", kind="mercury")
        self.watch.modem_started()
        k = self.poll(ardop="Audio socket /run/ardop.sock connected",
                      mercury="x6x00-mercury: stopping\nx6x00-mercury: the GUI's socket closed: the GUI went away")
        self.assertEqual(k["modem"], "ready")
        self.watch.modem_started()
        k = self.poll(ardop="Audio socket /run/ardop.sock closed: the other end went away",
                      mercury="x6x00-mercury: connected to /run/ardop.sock", kind="mercury")
        self.assertEqual(k["modem"], "ready")

    def test_decoders_behind(self):
        self.poll(mercury="x6x00-mercury: connected to /run/ardop.sock", kind="mercury")
        open(self.wl.PAT_OWN_LOG, "a").close()
        ring = "20:01:02.345 [+9.000s] [WRN] [modem-rx] decoder ring full (mode=23): dropped 160 samples so far"
        self.assertEqual(self.poll(mercury=ring, kind="mercury")["modem"], "behind")
        self.assertEqual(len(self.notes()), 1)
        self.assertIn("OpenX6X00: Mercury could not keep up with the receiver", self.notes()[0])
        self.poll(mercury=ring.replace("160", "10400"), kind="mercury")
        self.assertEqual(len(self.notes()), 1)                         # once in MERCURY_BEHIND
        self.watch.behind -= self.wl.MERCURY_BEHIND
        self.assertEqual(self.poll(kind="mercury")["modem"], "ready")
        self.watch.modem_started()
        self.assertEqual(self.poll(kind="mercury")["modem"], "waiting")

    def test_no_answer(self):
        k = self.poll(pat="2026/10/29 20:10:00 QSY varahf: 7101.5\n"
                          "2026/10/29 20:10:03 Connecting to KO0OOO (varahf)...\n"
                          "2026/10/29 20:11:40 Unable to establish connection to remote: connect timeout",
                      ardop=ESTABLISHED.format("KO0OOO", 2000), kind="mercury")    # not ardopcf's, ignored
        self.assertEqual(k["last_result"], "failed")
        self.assertEqual((k["last_text"], k["last_bw"]), ("No answer from KO0OOO on 7.101.500", ""))
        self.assertIn("Mercury 1.9.20 or later: a VARA gateway from Pat's list does not answer it", k["last_next"])

    def test_link_dropped(self):
        k = self.poll(pat="2026/10/29 20:10:03 Connecting to N0BBB (varahf)...\n"
                          "2026/10/29 20:10:12 Connected to N0BBB (vara)\n"
                          "2026/10/29 20:12:00 Exchange failed: EOF", kind="mercury")
        self.assertEqual(k["last_text"], "The link to N0BBB dropped (EOF)")
        self.assertIn("on a weak path choose 500 Hz for Mercury on the Winlink page", k["last_next"])

    def test_the_other_transport(self):
        k = self.poll(pat="2026/10/29 20:10:00 ARDOP TNC initialization failed: dial tcp 127.0.0.1:8515: "
                          "connect: connection refused", kind="mercury")
        self.assertEqual(k["last_text"], "Pat called through ARDOP, but the radio runs Mercury")
        self.assertEqual(k["last_next"], "Choose VARA HF in Pat's Connect dialog (Mercury speaks it), or ARDOP as "
                                         "the modem on the Winlink page")
        k = self.poll(pat="2026/10/29 20:11:00 vara initialization failed: couldn't connect to VARA command port: "
                          "dial tcp 127.0.0.1:8300: connect: connection refused")
        self.assertEqual(k["last_text"], "Pat called through VARA HF, but the radio runs ARDOP")
        self.assertEqual(k["last_next"], "Choose ARDOP in Pat's Connect dialog, or Mercury as the modem on the "
                                         "Winlink page")

    def test_not_ready(self):
        k = self.poll(pat="2026/10/29 20:11:00 vara initialization failed: couldn't connect to VARA command port: "
                          "dial tcp 127.0.0.1:8300: connect: connection refused", kind="mercury")
        self.assertEqual(k["last_text"], "Mercury was not ready")

    def test_qsy_refused(self):
        k = self.poll(pat="2026/10/29 20:10:00 QSY varahf: 7101.5\n2026/10/29 20:10:00 Unable to QSY: RPRT -9",
                      kind="mercury")
        self.assertIn("could not tune", k["last_text"])

    def test_bandwidth_not_there(self):
        k = self.poll(pat="2026/10/29 20:10:03 Connecting to N0BBB (varahf)...\n"
                          "2026/10/29 20:10:03 Unable to establish connection to remote: bandwidth 1000 not supported",
                      kind="mercury")
        self.assertEqual(k["last_text"], "Mercury has no 1000 Hz: the connection asked for it")


RADIO = """modem=connected
rx=14107700
tx=14107700
mode=USB-D
split=0
shift=0
rit=0
xit=0
simplex=
filter_low=100
filter_high=2900
tx_filter_low=160
tx_filter_high=3000
pwr=5.0
cap=7.0
app={app}
locked=1
on_air=0
swr_hold={swr}
refused={refused}
refused_seq={seq}
refused_at=1793296526
qsy={qsy}
qsy_why={qsy_why}
qsy_seq={qsy_seq}
qsy_at=1793296520
"""


class RadioFile(Logs):
    """The GUI's /run/winlink/radio (src/winlink.c, as the PC build wrote it with FT8 open)"""

    def radio(self, app="", refused="", seq=0, qsy=0, qsy_why="", qsy_seq=0, swr=0):
        with open(self.wl.RADIO, "w") as f:
            f.write(RADIO.format(app=app, refused=refused, seq=seq, qsy=qsy, qsy_why=qsy_why, qsy_seq=qsy_seq,
                                 swr=swr))

    def test_swr_hold_said_as_it_begins(self):
        self.radio(swr=120)                                                 # from before this supervisor
        self.poll()
        self.radio(swr=0)
        self.poll()
        self.assertEqual(self.notes(), [])
        self.radio(swr=299, refused="high SWR, the hold ends in 5 min", seq=1)
        self.poll()
        self.assertRegex(self.notes()[0], r"OpenX6X00: high SWR stopped a Winlink transmission: no TX for 5 "
                                          r"minutes, until \d\d:\d\d. Check the antenna and the tuner$")
        self.assertTrue(self.notes()[1].endswith("no TX, high SWR, the hold ends in 5 min. Check the "
                                                 "antenna and the tuner: after a high SWR stopped a Winlink "
                                                 "transmission the radio keys again only after 5 minutes"))
        self.radio(swr=250, refused="high SWR, the hold ends in 5 min", seq=1)
        self.poll()
        self.assertEqual(len(self.notes()), 2)

    def test_refusals_into_pats_log_once(self):
        self.radio(app="FT8", refused="FT8 is open, close it", seq=3)     # from before this supervisor
        self.assertEqual(self.poll()["modem"], "ready")                    # the GUI's word, no ardopcf line
        self.assertEqual(self.notes(), [])
        self.poll(pat="2026/10/29 09:47:01 QSY ardop: 14107.7\n2026/10/29 09:47:04 Connecting to HB9MM (ardop)...")
        self.radio(app="FT8", refused="FT8 is open, close it", seq=4)
        k = self.poll()
        self.assertEqual(k["last_result"], "dialing")
        self.assertIn("the radio does not transmit: FT8 is open, close it", k["last_text"])
        self.assertEqual(len(self.notes()), 1)
        self.assertTrue(self.notes()[0].endswith(
            "OpenX6X00: no TX, FT8 is open, close it. Close the app on the radio, then connect again"))
        self.radio(app="FT8", refused="FT8 is open, close it", seq=5)     # ardopcf's next connect request
        self.poll()
        self.assertEqual(len(self.notes()), 1)
        k = self.poll(pat="2026/10/29 09:48:10 Unable to establish connection to remote: Connect timeout")
        self.assertEqual(k["last_next"], "Close the app on the radio, then connect again")
        self.assertEqual(len(self.notes()), 2)

    def test_qsy_not_made(self):
        self.radio()
        self.poll()
        self.radio(qsy=7061500, qsy_why="FT8 is open, close it", qsy_seq=1)
        self.poll()
        self.assertTrue(self.notes()[-1].endswith("OpenX6X00: no QSY to 7.061.500 MHz, FT8 is open, close it"))
        self.radio(qsy=7061500, qsy_why="", qsy_seq=2)                      # made
        self.poll()
        self.assertEqual(len(self.notes()), 1)

    def test_stale_file_left_out(self):
        self.radio(app="FT8", refused="FT8 is open, close it", seq=1)
        old = time.time() - 60
        os.utime(self.wl.RADIO, (old, old))
        self.assertEqual(self.wl.read_radio(), {})
        self.assertEqual(self.poll()["modem"], "waiting")


class PatStart(Root):
    def test_pats_log_emptied_before_pat_runs(self):
        """The Watch reads Pat's log from its start once Pat is started: the last run's lines are gone by then"""
        os.makedirs(os.path.dirname(self.wl.PAT), exist_ok=True)
        with open(self.wl.PAT, "w") as f:
            f.write('#!/bin/sh\nwc -c < "$2" > "$2.size"\n')    # $2: the --log file
        os.chmod(self.wl.PAT, 0o755)
        with open(self.wl.PAT_OWN_LOG, "w") as f:
            f.write("2026/10/29 09:47:04 Connecting to HB9MM (ardop)...\n")
        pat = self.wl.Program("Pat", self.wl.PAT_LOG)
        self.wl.start_pat(pat, dict(WANT))
        pat.proc.wait(10)
        self.assertEqual(read(self.wl.PAT_OWN_LOG + ".size").strip(), "0")


class LogDownload(Root):
    """x6100-winlink logs: one text file for a bug report, every password masked"""

    def test_passwords_masked(self):
        os.makedirs(self.wl.LIB)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("PageSecret1\n")
        self.wl.write_config(dict(WANT, mycall="EA5JTT", locator="IM98"), "")
        cfg = self.pat_config()
        cfg.update(secure_login_password="PatSecret22", auxiliary_addresses=["EMCOMM-1:AuxSecret333", "N0CALL"])
        cfg["telnet"]["password"] = "TelnetSecret4"
        with open(self.wl.PAT_CONFIG, "w") as f:
            json.dump(cfg, f)
        with open(self.wl.PAT_OWN_LOG, "w") as f:
            f.write(fixture("ea5jtt-hb9mm-20m.pat.log") + ";PR: 81234567\n"
                    "DO NOT LOSE YOUR PASSWORD: NEWACCT9\n2026/10/29 09:56:00 login PageSecret1 / PatSecret22\n")
        with open(self.wl.EVENT_LOG, "w") as f:
            f.write(json.dumps({"what": "exchange", "auxiliary_addresses": ["EMCOMM-1:AuxSecret333"]}) + "\n")
            f.write(json.dumps({"what": "connect", "operation": "connect telnet://EA5JTT:P2pSecret55@10.0.0.7:8774/"})
                    + "\n")
        with open(self.wl.ARDOP_LOG, "w") as f:
            f.write(ESTABLISHED.format("HB9MM", 2000) + "\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(self.wl.cmd_logs(), 0)
        text = out.getvalue()
        for secret in ("PageSecret1", "PatSecret22", "AuxSecret333", "TelnetSecret4", "NEWACCT9", "81234567",
                       "P2pSecret55"):
            self.assertNotIn(secret, text)
        self.assertIn("telnet://EA5JTT:***@10.0.0.7:8774/", text)
        self.assertIn("DO NOT LOSE YOUR PASSWORD: ***", text)
        self.assertIn(";PR: ***", text)
        self.assertIn("Exchange failed: context deadline exceeded", text)
        self.assertIn("==== ardopcf: /run/winlink/ardopcf.log\n[STATUS: ARQ CONNECTION ESTABLISHED WITH HB9MM", text)
        self.assertIn("==== the radio (the GUI): /run/winlink/radio\n(none)", text)


    # Pat prints a new account's activation message whole, the password in it, above its DO NOT LOSE line
    # (app/app.go, the message as app/account_activation.go has it)
    ACTIVATION = ("\n========== Your New Winlink Account ==========\nA new Winlink account for 'EA5JTT' has been "
                  "activated. The next time you connect to a Winlink server or gateway you will be required to use "
                  "'K1CHN7' as your account password (no quotes).\n\nPlease print and save this message in case you "
                  "forget your password.\n" + "=" * 62 + "\n\n")

    def logs(self, pat_log):
        with open(self.wl.PAT_OWN_LOG, "w") as f:
            f.write(pat_log)
        with open(self.wl.PAT_LOG, "w") as f:
            f.write(pat_log)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(self.wl.cmd_logs(), 0)
        return out.getvalue()

    def test_new_accounts_password_masked_everywhere(self):
        text = self.logs(self.ACTIVATION + "DO NOT LOSE YOUR PASSWORD: K1CHN7\n\n")
        self.assertNotIn("K1CHN7", text)
        self.assertIn("A new Winlink account for 'EA5JTT' has been activated", text)
        self.assertIn("required to use '***' as your account password", text)

    def test_new_accounts_password_masked_when_pat_did_not_find_it(self):
        text = self.logs(self.ACTIVATION)                        # no DO NOT LOSE line: Pat found no password
        self.assertNotIn("K1CHN7", text)
        self.assertIn("for 'EA5JTT' has been", text)              # a quoted word in another sentence stays


class Tails(Root):
    def test_whole_lines_trim_and_restart(self):
        path = os.path.join(self.wl.RUN, "x.log")
        with open(path, "w") as f:
            f.write("from before\n")
        t = self.wl.Tail(path)
        self.assertEqual(t.lines(), [])
        with open(path, "a") as f:
            f.write("one\ntw")
        self.assertEqual(t.lines(), ["one"])
        with open(path, "a") as f:
            f.write("o\n")
        self.assertEqual(t.lines(), ["two"])
        os.truncate(path, 0)
        with open(path, "a") as f:
            f.write("after the trim\n")
        self.assertEqual(t.lines(), ["after the trim"])
        t.again()
        self.assertEqual(t.lines(), ["after the trim"])


if __name__ == "__main__":
    unittest.main()
