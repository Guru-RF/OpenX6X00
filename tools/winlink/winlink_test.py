#!/usr/bin/env python3
"""Tests of the Winlink supervisor (rootfs/usr/sbin/x6100-winlink) on a PC: each test loads it with
X6100_WINLINK_ROOT set to a new temporary directory, so every path it uses is under that one, and calls its
functions. Standard library only:

    python3 -B tools/winlink/winlink_test.py [-v]

The test of Pat's modem switch with the supervisor itself (SwitchWithTheSupervisor) puts stand-ins on Pat's and the
modems' ports, so it runs only in a network namespace of its own, as `make -C tools/winlink test` runs this file:
    unshare -rn sh -c 'ip link set lo up && WINLINK_NETNS=1 python3 -B tools/winlink/winlink_test.py'
"""
import configparser
import contextlib
import fcntl
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
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


WANT = {"mycall": "ON6URE", "locator": "JO20ab", "bandwidth": 500, "modem": "ardop", "mercury_bandwidth": 2300,
        "xara_bandwidth": 2300, "vara_licence": False, "xara_path_ms": ""}


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
        # -4 dB: Mercury 1.9.25 alone is 4 dB louder than 1.9.21 to 1.9.24, the radio's level stays theirs
        self.assertEqual(dict(ini["audio"]), {"tx_gain_db": "-4.0", "tx_delay_ms": "10"})
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
        modems = {"ardop": Running(alive=False), "mercury": Running(), "xara": Running(alive=False)}
        self.assertEqual(self.wl.modem_states(modems, "ardop"),
                         {"ardop": "stopped", "mercury": "running", "xara": "off"})
        modems["mercury"] = Running(alive=False)
        self.assertEqual(self.wl.modem_states(modems, "mercury"), {"ardop": "off", "mercury": "stopped", "xara": "off"})
        modems["xara"] = Running()
        self.assertEqual(self.wl.modem_states(modems, "xara"), {"ardop": "off", "mercury": "off", "xara": "running"})

    def test_turned_on_keeps_the_choice(self):
        self.conf(enabled=0, mycall="ON6URE", modem="mercury", mercury_bandwidth=500)
        self.wl.set_enabled(True)
        self.assertEqual(read(self.wl.CONF), "enabled=1\nmycall=ON6URE\nlocator=\nbandwidth=500\nmodem=mercury\n"
                                             "mercury_bandwidth=500\nxara_bandwidth=2300\nvara_licence=0\n")


class Xara(Root):
    """modem=xara, xara_bandwidth and vara_licence: Pat's varahf, XARA's settings, its start and its callsign"""

    def test_defaults(self):
        self.conf(enabled=1, mycall="ON6URE", modem="xara")
        want = self.wl.wanted()
        self.assertEqual((want["modem"], want["xara_bandwidth"], want["vara_licence"], want["xara_path_ms"]),
                         ("xara", 2300, False, ""))
        for bw, licence, path in (("2000", "yes", "25,50"), ("", "true", "1,2,3,4"), ("500MAX", "2", "25,50,1001")):
            self.conf(enabled=1, mycall="ON6URE", modem="xara", xara_bandwidth=bw, vara_licence=licence,
                      xara_path_ms=path)
            want = self.wl.wanted()
            self.assertEqual((want["xara_bandwidth"], want["vara_licence"], want["xara_path_ms"]), (2300, False, ""))
        self.conf(enabled=1, mycall="ON6URE", modem="xara", xara_bandwidth=500, vara_licence=1, xara_path_ms="30,60,90")
        want = self.wl.wanted()
        self.assertEqual((want["xara_bandwidth"], want["vara_licence"], want["xara_path_ms"]), (500, True, "30,60,90"))

    def test_varahf_takes_the_chosen_modems_bandwidth(self):
        for modem, bw in (("xara", 500), ("xara", 2750), ("mercury", 2300)):
            self.wl.write_config(dict(WANT, modem=modem, xara_bandwidth=bw, mercury_bandwidth=2300), "")
            self.assertEqual(self.pat_config()["varahf"], {"addr": "127.0.0.1:8300", "bandwidth": bw, "rig": "x6x00",
                                                           "ptt_ctrl": False})

    def conf_lines(self, want):
        return [ln for ln in self.wl.xara_conf(want).splitlines() if not ln.startswith("#")]

    def test_xara_conf_free_by_default(self):
        sock = f"{self.root}/run/ardop.sock"
        self.assertEqual(self.conf_lines(dict(WANT, modem="xara")),
                         ["call = ON6URE", "hf = 2300", "audio = x6x00", f"capture = {sock}", f"playback = {sock}",
                          "radio", "ptt = x6x00", "ptt-tail-ms = 20", "port = 8300", "kiss-port = off",
                          "drive-db = 0"])

    def test_xara_conf_registered_only_with_the_licence_ticked(self):
        lines = self.conf_lines(dict(WANT, modem="xara", xara_bandwidth=500, vara_licence=True))
        self.assertEqual(lines[1], "hf = 500")
        self.assertEqual(lines[-1], "registered")
        self.assertNotIn("registered", self.conf_lines(dict(WANT, modem="xara")))
        lines = self.conf_lines(dict(WANT, modem="xara", xara_path_ms="30,60,90"))
        self.assertEqual(lines[-1], "x6x00-path-ms = 30,60,90")

    def fake(self):
        os.makedirs(os.path.dirname(self.wl.XARA), exist_ok=True)
        with open(self.wl.XARA, "w") as f:
            f.write('#!/bin/sh\necho "$@" > args\necho "${XARA_UI_TOKEN-none} ${STATE_DIRECTORY-none} '
                    '${XDG_CONFIG_HOME-none} $HOME" > env\n')
        os.chmod(self.wl.XARA, 0o755)

    def test_xara_started(self):
        """xara --config with its settings, no XARA_* or STATE_DIRECTORY from outside (saved web UI settings would
        override ours), HOME in RAM"""
        self.fake()
        for k, v in (("XARA_UI_TOKEN", "from-outside"), ("STATE_DIRECTORY", "/var/lib/xara"),
                     ("XDG_CONFIG_HOME", "/root/.config")):
            os.environ[k] = v
            self.addCleanup(os.environ.pop, k, None)
        xara = self.wl.Program("XARA (xara)", self.wl.XARA_LOG)
        self.wl.start_xara(xara, dict(WANT, modem="xara", vara_licence=True))
        xara.proc.wait(10)
        self.assertEqual(read(os.path.join(self.wl.RUN, "args")).split(), ["--config", self.wl.XARA_CONF])
        self.assertEqual(read(os.path.join(self.wl.RUN, "env")).split(), ["none", "none", "none", self.wl.RUN])
        self.assertEqual(read(self.wl.XARA_CONF), self.wl.xara_conf(dict(WANT, modem="xara", vara_licence=True)))
        self.assertEqual(xara.settings, read(self.wl.XARA_CONF))     # started again when it would differ

    def test_a_callsign_xara_takes(self):
        for call in ("ON6URE", "ON6URE-5", "ON6URE-05", "ON6URE-19", "K1A", "PA0ABCD-1"):
            self.assertEqual(self.wl.xara_refused(dict(WANT, mycall=call)), "", call)
        self.assertIn("XARA needs your callsign", self.wl.xara_refused(dict(WANT, mycall="")))
        for call in ("PA0ABCDE", "ON6URE-0", "ON6URE-00", "ON6URE-20", "ON6URE-99", "N0CALL", "N0CALL-5"):
            self.assertIn(f"XARA does not take {call} as a callsign", self.wl.xara_refused(dict(WANT, mycall=call)))
        self.fake()
        xara = self.wl.Program("XARA (xara)", self.wl.XARA_LOG)
        with self.assertRaises(ValueError):                         # never started without one
            self.wl.start_xara(xara, dict(WANT, modem="xara", mycall=""))
        self.assertIsNone(xara.proc)

    def test_status_names_xara(self):
        self.wl.report(Running(), Running(), dict(WANT, modem="xara"), {"modem": "waiting"})
        st = read(self.wl.STATUS)
        self.assertIn("text=Running as ON6URE; XARA waits for the radio's audio (the GUI)\n", st)
        self.assertIn("ardop=off\nmercury=off\nxara=running\n", st)
        refused = self.wl.xara_refused(dict(WANT, mycall=""))
        self.wl.report(Running(), Running(alive=False), dict(WANT, modem="xara", mycall=""), {"modem": "off"}, refused)
        st = read(self.wl.STATUS)
        self.assertIn("state=error\ntext=XARA needs your callsign: set it on the Winlink page", st)
        self.assertIn("xara=stopped\n", st)

    def test_status_without_a_supervisor(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.wl.cmd_status()
        self.assertIn("mercury=off\nxara=off\npassword=none\n", out.getvalue())

    def test_logs_have_xaras(self):
        with open(self.wl.XARA_LOG, "w") as f:
            f.write("xara: connected to the GUI's socket /run/ardop.sock\n")
        with open(self.wl.XARA_CONF, "w") as f:
            f.write(self.wl.xara_conf(dict(WANT, modem="xara")))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.wl.cmd_logs()
        self.assertIn("==== XARA (xara): /run/winlink/xara.log\nxara: connected to the GUI's socket", out.getvalue())
        self.assertIn("==== XARA's settings: /run/winlink/xara.conf\n", out.getvalue())


ALL3 = ["PUBLIC", "XARA", "MERCURY"]


class ServiceCodes(Root):
    """Pat's service_codes (#324): PUBLIC, XARA and MERCURY always, whichever modem is chosen, in a new config.json
    and in one from before, the user's own codes kept"""

    def codes(self):
        self.wl.write_config(self.wl.wanted(), "")
        return self.pat_config()["service_codes"]

    def test_all_three_with_every_modem(self):
        self.conf(enabled=1, mycall="ON6URE")
        self.assertEqual(self.codes(), ALL3)                         # nothing chosen yet
        for modem, last in (("xara", "xara"), ("mercury", "mercury"), ("ardop", "xara"), ("ardop", "mercury")):
            self.conf(enabled=1, mycall="ON6URE", modem=modem, varahf_modem=last)
            self.assertEqual(self.codes(), ALL3, (modem, last))
            self.assertEqual(self.wl.wanted()["varahf_modem"], last, (modem, last))

    def test_config_from_before_brought_up(self):
        """An existing config.json with PUBLIC alone, as every one up to v0.3.13: the modems' codes added, the rest
        of the file as it was"""
        os.makedirs(os.path.dirname(self.wl.PAT_CONFIG))
        with open(self.wl.PAT_CONFIG, "w") as f:
            json.dump(dict(self.wl.PAT_DEFAULTS, motd=["mine"]), f)
        self.conf(enabled=1, mycall="ON6URE", modem="xara")
        self.assertEqual(self.codes(), ALL3)
        self.assertEqual(self.pat_config()["motd"], ["mine"])
        self.assertFalse(self.wl.write_config(self.wl.wanted(), ""))   # and no change the next time

    def test_own_codes_kept(self):
        """Codes typed on Pat's Settings page stay, in their order, behind ours; ours come back when they were taken
        out, and no code twice"""
        self.conf(enabled=1, mycall="ON6URE", modem="xara")
        self.codes()
        for had, modem, now in ((["EMCOMM", "PUBLIC", "MARS"], "xara", ALL3 + ["EMCOMM", "MARS"]),
                                (["EMCOMM"], "mercury", ALL3 + ["EMCOMM"]),
                                (["PUBLIC", "MERCURY", "XARA", "EMCOMM", "EMCOMM"], "ardop", ALL3 + ["EMCOMM"]),
                                ("PUBLIC", "xara", ALL3),                        # not a list: ours
                                ([], "mercury", ALL3),
                                (["public", " emcomm ", 7, ""], "xara", ALL3 + ["emcomm"])):
            cfg = self.pat_config()
            cfg["service_codes"] = had
            with open(self.wl.PAT_CONFIG, "w") as f:
                json.dump(cfg, f)
            self.conf(enabled=1, mycall="ON6URE", modem=modem)
            self.assertEqual(self.codes(), now, (had, modem))


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

    def test_cleared(self):
        """The page's Clear: neither the page's password nor Pat's own is left, the rest of Pat's config is, and
        the status says none, with nothing running to write it again"""
        self.conf(enabled=0, mycall="ON6URE")
        self.wl.write_config(self.wl.wanted(), "")
        cfg = self.pat_config()
        cfg["secure_login_password"] = "typed-in-pat"
        with open(self.wl.PAT_CONFIG, "w") as f:
            json.dump(cfg, f)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("from-the-page\n")
        self.wl.say("off", "Off")
        self.assertEqual(self.status()["password"], "page")
        self.assertEqual(self.wl.cmd_clear_password(), 0)
        self.assertFalse(os.path.exists(self.wl.PASSWORD))
        self.assertEqual(self.pat_config()["secure_login_password"], "")
        self.assertEqual(self.pat_config()["mycall"], "ON6URE")
        self.assertEqual(self.wl.password_where(), "none")
        self.assertEqual(self.status(), {"state": "off", "text": "Off", "pat": "off", "ardop": "off",
                                         "mercury": "off", "xara": "off", "password": "none"})
        self.assertEqual(self.wl.cmd_clear_password(), 0)     # again, with nothing to clear
        os.remove(self.wl.STATUS)
        self.assertEqual(self.wl.cmd_clear_password(), 0)     # no status file: none made
        self.assertFalse(os.path.exists(self.wl.STATUS))

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
        for path in (self.wl.PAT_OWN_LOG, self.wl.ARDOP_LOG, self.wl.MERCURY_LOG, self.wl.XARA_LOG, self.wl.EVENT_LOG):
            open(path, "w").close()
        self.watch = self.wl.Watch()

    def add(self, path, text):
        with open(path, "a") as f:
            f.write(text if text.endswith("\n") else text + "\n")

    def poll(self, pat="", ardop="", events=(), mercury="", kind="ardop", xara=""):
        if pat:
            self.add(self.wl.PAT_OWN_LOG, pat)
        if ardop:
            self.add(self.wl.ARDOP_LOG, ardop)
        if mercury:
            self.add(self.wl.MERCURY_LOG, mercury)
        if xara:
            self.add(self.wl.XARA_LOG, xara)
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
        k = self.failed(self.TIMEOUT, refused="high SWR on 20m, the hold ends in 4 min")
        self.assertEqual(k["last_text"], "No answer from HB9MM: the radio did not transmit, high SWR on 20m, the hold "
                                         "ends in 4 min")
        self.assertIn("Check the antenna", k["last_next"])
        self.assertIn("another band keys at once", k["last_next"])

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
        """Mercury 1.9.25's log (its lines begin with the date since 1.9.22), up to x6x00-mercury on the socket, then
        the rest; Pat's up to its call, then the rest"""
        log = fixture("pc-n0aaa-n0bbb-mercury.mercury.log").splitlines(keepends=True)
        self.assertIn("[engine] Mercury 1.9.25 (git 33efac4e)", log[1])
        on = next(i for i, l in enumerate(log) if l.startswith("x6x00-mercury: connected to ")) + 1
        k = self.poll(mercury="".join(log[:on]), kind="mercury")
        self.assertEqual(k["modem"], "ready")                         # x6x00-mercury on the socket
        pat = fixture("pc-n0aaa-n0bbb-mercury.pat.log").splitlines(keepends=True)
        self.assertIn("VARA modem (Mercury 1.9.25) initialized", "".join(pat))    # our VERSION line, as Pat took it
        call = next(i for i, l in enumerate(pat) if "Connecting to N0BBB (varahf)" in l) + 1
        k = self.poll(pat="".join(pat[:call]), mercury="".join(log[on:]), kind="mercury")
        self.assertEqual((k["last_result"], k["last_gateway"], k["last_text"]), ("dialing", "N0BBB", "Calling N0BBB"))
        self.assertEqual(k["modem"], "waiting")                       # x6x00-mercury: stopping
        k = self.poll(pat="".join(pat[call:]), events=fixture("pc-n0aaa-n0bbb-mercury.events.log").splitlines(),
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
        ring = ("2026-11-22 20:01:02.345 [+9.000s] [WRN] [modem-rx] decoder ring full (mode=23): dropped 160 samples "
                "so far")
        self.assertEqual(self.poll(mercury=ring, kind="mercury")["modem"], "behind")
        self.assertEqual(len(self.notes()), 1)
        self.assertIn("OpenX6X00: Mercury could not keep up with the receiver", self.notes()[0])
        self.poll(mercury=ring.replace("160", "10400"), kind="mercury")
        self.assertEqual(len(self.notes()), 1)                         # once in MERCURY_BEHIND
        self.watch.behind -= self.wl.MERCURY_BEHIND
        self.assertEqual(self.poll(kind="mercury")["modem"], "ready")
        self.watch.modem_started()
        self.assertEqual(self.poll(kind="mercury")["modem"], "waiting")

    def test_backlog_behind(self):
        """Its capture backlog thrown away undecoded: the receiver fell behind before the decoders"""
        self.poll(mercury="x6x00-mercury: connected to /run/ardop.sock", kind="mercury")
        open(self.wl.PAT_OWN_LOG, "a").close()
        flushed = ("20:01:02.345 [+9.000s] [WRN] [modem-rx] RX backlog exceeded ~30 s cap; flushing stale audio to "
                   "catch up")
        self.assertEqual(self.poll(mercury=flushed, kind="mercury")["modem"], "behind")
        self.assertEqual(len(self.notes()), 1)

    def test_no_answer(self):
        k = self.poll(pat="2026/10/29 20:10:00 QSY varahf: 7101.5\n"
                          "2026/10/29 20:10:03 Connecting to KO0OOO (varahf)...\n"
                          "2026/10/29 20:11:40 Unable to establish connection to remote: connect timeout",
                      ardop=ESTABLISHED.format("KO0OOO", 2000), kind="mercury")    # not ardopcf's, ignored
        self.assertEqual(k["last_result"], "failed")
        self.assertEqual((k["last_text"], k["last_bw"]), ("No answer from KO0OOO on 7.101.500", ""))
        self.assertIn("needs Mercury 1.9.25: 1.9.24 and older do not connect to it, and a VARA gateway from Pat's "
                      "list does not answer it", k["last_next"])

    OLDER = ("2026-11-22 20:10:{} [+70.000s] [WRN] [arq] ACCEPT for ON6URE from Mercury 1.9.24 or older (protocol "
             "minor 1): the versions do not connect, ignored")

    def test_answer_from_an_older_mercury(self):
        """Mercury 1.9.20 to 1.9.24 still answers a 1.9.25 call; 1.9.25 ignores the answer and says so in its log
        (datalink_arq/arq.c), and Pat's call times out: not "no answer", the other station's version"""
        k = self.poll(pat="2026/11/22 20:10:00 QSY varahf: 7101.5\n"
                          "2026/11/22 20:10:03 Connecting to KO0OOO (varahf)...", kind="mercury")
        self.assertEqual(k["last_result"], "dialing")
        k = self.poll(mercury=self.OLDER.format("12.500"), kind="mercury")
        self.assertEqual((k["last_result"], k["last_text"]), ("dialing", "Calling KO0OOO on 7.101.500"))
        k = self.poll(mercury=self.OLDER.format("40.100"),             # its answer to the next CALL
                      pat="2026/11/22 20:11:40 Unable to establish connection to remote: connect timeout",
                      kind="mercury")
        self.assertEqual(k["last_result"], "failed")
        self.assertEqual(k["last_text"], "KO0OOO answered on 7.101.500 with Mercury 1.9.24 or older, which does not "
                                         "connect to this radio's")
        self.assertIn("1.9.25 connects only to 1.9.25: the other station has to update its Mercury", k["last_next"])
        self.assertIn("KO0OOO answered on 7.101.500 with Mercury 1.9.24 or older", self.notes()[-1])
        # the next call, to a station nobody hears: no answer, as before
        k = self.poll(pat="2026/11/22 20:20:03 Connecting to KO0OOO (varahf)...\n"
                          "2026/11/22 20:21:40 Unable to establish connection to remote: connect timeout",
                      kind="mercury")
        self.assertEqual(k["last_text"], "No answer from KO0OOO")

    def test_answer_from_an_older_mercury_in_one_poll(self):
        """Pat's timeout read before Mercury's line, in the same poll"""
        k = self.poll(pat="2026/11/22 20:10:03 Connecting to KO0OOO (varahf)...\n"
                          "2026/11/22 20:11:40 Unable to establish connection to remote: connect timeout",
                      mercury=self.OLDER.format("12.500"), kind="mercury")
        self.assertIn("with Mercury 1.9.24 or older", k["last_text"])

    def test_an_older_mercury_calling_us_is_not_our_call(self):
        """Its CALL for us, which 1.9.25 does not answer, while we call another station: that call is not judged by it"""
        k = self.poll(pat="2026/11/22 20:10:03 Connecting to KO0OOO (varahf)...\n"
                          "2026/11/22 20:11:40 Unable to establish connection to remote: connect timeout",
                      mercury="2026-11-22 20:10:12.500 [+70.000s] [WRN] [arq] CALL for ON6URE from Mercury 1.9.24 or "
                              "older (protocol minor 1): the versions do not connect, not answered", kind="mercury")
        self.assertEqual(k["last_text"], "No answer from KO0OOO")

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
        self.assertEqual(k["last_next"], "Choose ARDOP in Pat's Connect dialog, or Mercury or XARA as the modem on "
                                         "the Winlink page")

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


class XaraOutcomes(Logs):
    """With XARA: Pat's varahf lines, our patch's socket lines and the VARA host protocol XARA echoes (the PC test's
    logs, tools/winlink: Pat A to Pat B over XARA, the free speed)"""

    def test_pc_p2p_delivered_at_the_free_speed(self):
        log = fixture("pc-n0aaa-n0bbb-xara.xara.log").splitlines(keepends=True)
        k = self.poll(xara="".join(log[:6]), kind="xara")
        self.assertEqual(k["modem"], "ready")                         # on the GUI's socket
        self.assertNotIn("speed", k)                                  # no REGISTERED line yet: Pat's MYCALL first
        pat = fixture("pc-n0aaa-n0bbb-xara.pat.log").splitlines(keepends=True)
        k = self.poll(pat="".join(pat[:3]), xara="".join(log[6:60]), kind="xara")
        self.assertEqual((k["last_result"], k["last_gateway"], k["last_bw"], k["last_speed"]),
                         ("connected", "N0BBB", 2300, "free"))
        self.assertEqual(k["last_text"], "Connected to N0BBB at 2300 Hz at VARA's free speed, exchanging mail")
        self.assertEqual(k["speed"], "free")
        k = self.poll(pat="".join(pat[3:]), xara="".join(log[60:]),
                      events=fixture("pc-n0aaa-n0bbb-xara.events.log").splitlines(), kind="xara")
        self.assertEqual(k["last_result"], "delivered")
        self.assertEqual(k["last_text"], "Done with N0BBB at 2300 Hz at VARA's free speed: 1 sent, 0 received")
        self.assertEqual([n.split(" ", 2)[2] for n in self.notes()], ["OpenX6X00: " + k["last_text"]])

    def test_full_speed_from_xaras_own_line(self):
        """The licence ticked: XARA says REGISTERED with our call after Pat's MYCALL, and the radio, the caller,
        sets the bit on its link; also when the link came up before that line"""
        for order in ("registered first", "connected first"):
            self.setUp()
            reg, con = "     5.310 < REGISTERED N0AAA", "     8.160 < CONNECTED N0AAA N0BBB 2300"
            lines = [reg, con] if order == "registered first" else [con, reg]
            k = self.poll(pat="2026/11/04 13:41:00 Connecting to N0BBB (varahf)...\n"
                              "2026/11/04 13:41:08 Connected to N0BBB (vara)",
                          xara="xara: connected to the GUI's socket /run/ardop.sock\n" + "\n".join(lines), kind="xara")
            self.assertEqual((k["speed"], k["last_speed"]), ("full", "full"), order)
            self.assertEqual(k["last_text"], "Connected to N0BBB at 2300 Hz at full speed, exchanging mail")

    def test_link_lines_are_the_callees(self):
        k = self.poll(pat="2026/11/04 13:41:00 Connecting to N0BBB (varahf)...\n",
                      xara="     7.590 < CONNECTED N0BBB N0AAA 500\n     7.590 < LINK UNREGISTERED", kind="xara")
        self.assertEqual((k["last_bw"], k["last_speed"]), (500, "free"))
        k = self.poll(pat="2026/11/04 13:42:00 Connecting to N0CCC (varahf)...\n",
                      xara="     9.000 < CONNECTED N0DDD N0EEE 2300", kind="xara")     # another link's: left out
        self.assertEqual((k["last_gateway"], k["last_bw"], k["last_speed"]), ("N0CCC", "", ""))

    def test_mercurys_lines_are_not_xaras(self):
        """Mercury and XARA share Pat's varahf: the hints are the chosen modem's"""
        k = self.poll(pat="2026/11/04 13:10:00 QSY varahf: 7101.5\n"
                          "2026/11/04 13:10:03 Connecting to YO4CIL (varahf)...\n"
                          "2026/11/04 13:10:58 Unable to establish connection to remote: connect timeout", kind="xara")
        self.assertEqual((k["last_result"], k["last_text"]), ("failed", "No answer from YO4CIL on 7.101.500"))
        self.assertNotIn("Mercury", k["last_next"])
        self.assertIn("call the gateway at the bandwidth Pat's VARA HF list gives it", k["last_next"])
        k = self.poll(pat="2026/11/04 13:20:03 Connecting to N0BBB (varahf)...\n"
                          "2026/11/04 13:20:12 Connected to N0BBB (vara)\n"
                          "2026/11/04 13:21:00 Exchange failed: EOF", kind="xara")
        self.assertIn("on a weak path choose 500 Hz for XARA on the Winlink page", k["last_next"])
        k = self.poll(pat="2026/11/04 13:22:03 Connecting to N0BBB (varahf)...\n"
                          "2026/11/04 13:22:03 Unable to establish connection to remote: bandwidth 1000 not supported",
                      kind="xara")
        self.assertEqual(k["last_text"], "XARA has no 1000 Hz: the connection asked for it")
        self.assertEqual(k["last_next"], "Choose 500, 2300 or 2750 Hz for the connection, or none: XARA then takes "
                                         "the Winlink page's")

    def test_the_other_transport_and_not_ready(self):
        k = self.poll(pat="2026/11/04 13:10:00 ARDOP TNC initialization failed: dial tcp 127.0.0.1:8515: "
                          "connect: connection refused", kind="xara")
        self.assertEqual(k["last_text"], "Pat called through ARDOP, but the radio runs XARA")
        self.assertEqual(k["last_next"], "Choose VARA HF in Pat's Connect dialog (XARA speaks it), or ARDOP as the "
                                         "modem on the Winlink page")
        k = self.poll(pat="2026/11/04 13:11:00 vara initialization failed: couldn't connect to VARA command port: "
                          "dial tcp 127.0.0.1:8300: connect: connection refused", kind="xara")
        self.assertEqual(k["last_text"], "XARA was not ready")

    def test_modem_follows_xaras_socket_lines(self):
        self.assertEqual(self.poll(kind="xara")["modem"], "waiting")
        self.assertEqual(self.poll(xara="xara: the GUI's socket /run/ardop.sock is not there yet (No such file or "
                                        "directory (os error 2)), trying again every second", kind="xara")["modem"],
                         "waiting")
        self.assertEqual(self.poll(xara="xara: connected to the GUI's socket /run/ardop.sock", kind="xara")["modem"],
                         "ready")
        self.assertEqual(self.poll(xara="xara: the GUI's socket closed: the GUI went away", kind="xara")["modem"],
                         "waiting")
        # ARDOP chosen after XARA: XARA's last lines come after ardopcf is on the socket
        self.watch.modem_started()
        k = self.poll(ardop="Audio socket /run/ardop.sock connected",
                      xara="xara: the GUI's socket closed: the GUI went away")
        self.assertEqual(k["modem"], "ready")
        self.watch.modem_started()
        k = self.poll(ardop="Audio socket /run/ardop.sock closed: the other end went away",
                      xara="xara: connected to the GUI's socket /run/ardop.sock", kind="xara")
        self.assertEqual(k["modem"], "ready")

    def test_behind(self):
        self.poll(xara="xara: connected to the GUI's socket /run/ardop.sock", kind="xara")
        behind = "xara: audio: 120 ms of capture waiting to be read (xara is falling behind real time: CPU overload?)"
        # one block that took long (XARA making its frame to send) leaves some 120 ms, the next doubling 240: drained
        self.assertEqual(self.poll(xara=behind, kind="xara")["modem"], "ready")
        self.assertEqual(self.poll(xara=behind.replace("120", "240"), kind="xara")["modem"], "ready")
        self.assertEqual(len(self.notes()), 0, "not the processor short")
        behind = behind.replace("120", "480")
        self.assertEqual(self.poll(xara=behind, kind="xara")["modem"], "behind")
        self.assertEqual(len(self.notes()), 1)
        self.assertIn("OpenX6X00: XARA could not keep up with the receiver", self.notes()[0])
        self.poll(xara=behind.replace("480", "960"), kind="xara")
        self.assertEqual(len(self.notes()), 1, "once in ten minutes")
        self.watch.behind -= self.wl.MERCURY_BEHIND
        self.assertEqual(self.poll(kind="xara")["modem"], "ready")


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

    def radio(self, app="", refused="", seq=0, qsy=0, qsy_why="", qsy_seq=0, swr=0, where="", here=None):
        """here None: a GUI before #147, without the swr_hold_where and swr_hold_here lines"""
        with open(self.wl.RADIO, "w") as f:
            f.write(RADIO.format(app=app, refused=refused, seq=seq, qsy=qsy, qsy_why=qsy_why, qsy_seq=qsy_seq,
                                 swr=swr))
            if here is not None:
                f.write(f"swr_hold_where={where}\nswr_hold_here={here}\n")

    def test_swr_hold_said_as_it_begins(self):
        self.radio(swr=120, where="on 20m", here=1)                        # from before this supervisor
        self.poll()
        self.radio(swr=0, here=0)
        self.poll()
        self.assertEqual(self.notes(), [])
        self.radio(swr=299, where="on 20m", here=1, refused="high SWR on 20m, the hold ends in 5 min", seq=1)
        self.poll()
        self.assertRegex(self.notes()[0], r"OpenX6X00: high SWR stopped a Winlink transmission on 20m: no TX there "
                                          r"for 5 minutes, until \d\d:\d\d; another band transmits. Check the "
                                          r"antenna and the tuner$")
        self.assertTrue(self.notes()[1].endswith("no TX, high SWR on 20m, the hold ends in 5 min. Check the "
                                                 "antenna and the tuner: after a high SWR stopped a Winlink "
                                                 "transmission the radio keys there again only after 5 minutes; "
                                                 "another band keys at once"))
        self.radio(swr=250, where="on 20m", here=0, refused="high SWR on 20m, the hold ends in 5 min", seq=1)
        self.poll()
        self.assertEqual(len(self.notes()), 2)                              # on 40m now: the same hold
        self.radio(swr=300, where="on 40m", here=1, refused="high SWR on 20m, the hold ends in 5 min", seq=1)
        self.poll()
        self.assertEqual(len(self.notes()), 3)                              # a trip on 40m: the hold moved there
        self.assertRegex(self.notes()[2], r"OpenX6X00: high SWR stopped a Winlink transmission on 40m: no TX there "
                                          r"for 5 minutes, until \d\d:\d\d; another band transmits")
        self.radio(swr=290, where="on 40m", here=1, refused="high SWR on 20m, the hold ends in 5 min", seq=1)
        self.poll()
        self.assertEqual(len(self.notes()), 3)

    def test_swr_hold_of_a_gui_before_147(self):
        self.radio(swr=0)
        self.poll()
        self.radio(swr=299, refused="high SWR, the hold ends in 5 min", seq=1)
        self.poll()
        self.assertRegex(self.notes()[0], r"OpenX6X00: high SWR stopped a Winlink transmission: no TX for 5 "
                                          r"minutes, until \d\d:\d\d. Check the antenna and the tuner$")

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


# ---- Pat's own choice of the modem (#296): x6100-winlink switch ------------------------------------------------

def status_of(kind, state="running", modem="ready", pat="running", text="Running as N0AAA"):
    """A status file as the supervisor's report() writes it with that modem chosen"""
    st = {"state": state, "text": text, "pat": pat, **{m: "off" for m in ("ardop", "mercury", "xara")},
          kind: "running" if state == "running" else "stopped", "modem": modem}
    return "".join(f"{k}={v}\n" for k, v in st.items())


# A stand-in supervisor for the command run as Pat runs it: the lock held with its pid in it, a poke taken
SUPERVISOR_LOCK = """import fcntl, os, signal, sys, time
signal.signal(signal.SIGUSR1, lambda *_: None)
f = open(sys.argv[1], "a+")
fcntl.flock(f, fcntl.LOCK_EX)
f.truncate(0)
f.write(f"{os.getpid()}\\n")
f.flush()
print("holding", flush=True)
time.sleep(60)
"""


class Switch(Root):
    """switch_modem() with a stand-in for the supervisor: a poke makes it answer, as the real one does, with the
    status of the modem now chosen"""

    def setUp(self):
        super().setUp()
        for p in (self.wl.ARDOPCF, self.wl.MERCURY, self.wl.XARA):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f:
                f.write("#!/bin/sh\n")
            os.chmod(p, 0o755)
        open(self.wl.PAT_OWN_LOG, "w").close()               # Pat runs: its log is there
        self.wl.SWITCH_LOOK, self.wl.SWITCH_WAIT, self.wl.SWITCH_SETTLE = 0.02, 1, 1
        self.wl.supervisor_pid = lambda: 4242
        self.wl.poke = self.poked
        self.listening = self.wl.listening
        self.wl.listening = lambda port: port in self.ports
        self.ports = {8515, 8300}
        self.pokes = []
        self.then = "ready"                                  # what the stand-in does: ready, fails, waits, page

    def poked(self):
        """The supervisor: the switch file is there before the setting, and names the modem it says"""
        conf = self.wl.read_conf()
        self.pokes.append((conf.get("modem"), self.wl.read_kv(self.wl.SWITCH).get("modem")))
        kind = self.wl.conf_modem(conf)
        if self.then == "ready":
            self.status(kind)
        elif self.then == "fails":
            self.status(kind, state="error", modem="off",
                        text="XARA (xara) stopped (code 1: no sound card); starting it again in 5 s")
        elif self.then == "waits":
            self.status(kind, modem="waiting", text="Running as N0AAA; XARA waits for the radio's audio (the GUI)")
        elif self.then == "page":                            # the page chose Mercury meanwhile
            self.conf(**dict(conf, modem="mercury"))
        return True

    def status(self, kind, **kw):
        with open(self.wl.STATUS, "w") as f:
            f.write(status_of(kind, **kw))

    def notes(self):
        return [ln.split(" OpenX6X00: ", 1)[1] for ln in read(self.wl.PAT_OWN_LOG).splitlines()
                if " OpenX6X00: " in ln]

    def test_varahf_while_ardop_runs_switches_to_xara(self):
        """None of Mercury and XARA chosen before: XARA, at its own bandwidth"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop", mercury_bandwidth=500, xara_bandwidth=2750)
        self.status("ardop")
        self.assertEqual(self.wl.switch_modem("varahf"), (True, "2750"))
        self.assertEqual(self.pokes, [("xara", "xara")])
        conf = self.wl.read_conf()
        self.assertEqual((conf["modem"], conf["varahf_modem"]), ("xara", "xara"))
        self.assertEqual(self.notes(), [
            "switching the modem from ARDOP to XARA for this connection through VARA HF",
            "XARA is ready and the connection goes ahead; the modem stays XARA, as the Winlink page "
            "(Operating > Winlink) now shows"])
        self.assertFalse(os.path.exists(self.wl.SWITCH))      # gone once over

    def test_ardop_while_xara_runs_switches_to_ardop(self):
        self.conf(enabled=1, mycall="N0AAA", modem="xara")
        self.status("xara")
        self.assertEqual(self.wl.switch_modem("ardop"), (True, ""))
        conf = self.wl.read_conf()
        self.assertEqual((conf["modem"], conf["varahf_modem"]), ("ardop", "xara"))
        self.assertEqual(self.notes()[0], "switching the modem from XARA to ARDOP for this connection through ARDOP")

    def test_mercury_remembered(self):
        """Mercury the last of the two: VARA HF switches back to it, at Mercury's bandwidth"""
        self.conf(enabled=1, mycall="N0AAA", modem="mercury", mercury_bandwidth=500, xara_bandwidth=2300)
        self.status("mercury")
        self.assertEqual(self.wl.switch_modem("ardop"), (True, ""))
        self.assertEqual(self.wl.read_conf()["varahf_modem"], "mercury")
        self.assertEqual(self.wl.switch_modem("varahf"), (True, "500"))
        self.assertEqual(self.wl.read_conf()["modem"], "mercury")
        # As the Winlink page leaves it (varahf_modem= of its own), with ARDOP chosen there
        self.conf(enabled=1, mycall="N0AAA", modem="ardop", varahf_modem="mercury")
        self.status("ardop")
        self.assertEqual(self.wl.switch_modem("varahf"), (True, "2300"))
        self.assertEqual(self.wl.read_conf()["modem"], "mercury")

    def test_mercury_when_xara_is_not_there(self):
        os.remove(self.wl.XARA)
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        self.assertEqual(self.wl.switch_modem("varahf"), (True, "2300"))
        self.assertEqual(self.wl.read_conf()["modem"], "mercury")

    def test_no_switch_when_the_right_modem_runs(self):
        for kind, via, bw in (("ardop", "ardop", ""), ("mercury", "varahf", "500"), ("xara", "varahf", "2750")):
            self.conf(enabled=1, mycall="N0AAA", modem=kind, mercury_bandwidth=500, xara_bandwidth=2750)
            before = read(self.wl.CONF)
            self.status(kind)
            self.assertEqual(self.wl.switch_modem(via), (True, bw), kind)
            self.assertEqual(read(self.wl.CONF), before)
        self.assertEqual((self.pokes, self.notes()), ([], []))

    def test_only_the_modem_changes(self):
        """The bandwidths, the VARA HF licence tick and what else is in the file stay as they were"""
        self.conf(enabled=1, mycall="N0AAA", locator="JO20ab", bandwidth=200, modem="ardop", mercury_bandwidth=500,
                  xara_bandwidth=2750, vara_licence=1, xara_path_ms="20,30,40")
        self.status("ardop")
        self.assertEqual(self.wl.switch_modem("varahf"), (True, "2750"))
        self.assertEqual(read(self.wl.CONF), "enabled=1\nmycall=N0AAA\nlocator=JO20ab\nbandwidth=200\nmodem=xara\n"
                                             "mercury_bandwidth=500\nxara_bandwidth=2750\nvara_licence=1\n"
                                             "xara_path_ms=20,30,40\nvarahf_modem=xara\n")

    def test_the_modem_fails_to_start(self):
        """A clear line in Pat's log, nothing dialed; the setting stays the new modem's, which the page shows with
        its error, and the supervisor tries it again"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        self.then = "fails"
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertEqual(self.notes()[-1], "no connection through VARA HF: XARA did not start (XARA (xara) stopped "
                         "(code 1: no sound card); starting it again in 5 s); Pat did not call. The Winlink page "
                         "(Operating > Winlink) says how XARA is doing: connect again once it says the modem is "
                         "ready, or choose another modem there")
        self.assertEqual(self.wl.read_conf()["modem"], "xara")
        self.assertFalse(os.path.exists(self.wl.SWITCH))

    def test_not_ready_in_time(self):
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        self.then = "waits"
        start = time.time()
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertLess(time.time() - start, 3)
        self.assertTrue(self.notes()[-1].startswith("no connection through VARA HF: XARA was not ready within 1 s "
                                                    "(Running as N0AAA; XARA waits for the radio's audio (the GUI))"))

    def test_the_tnc_port_open_first(self):
        """On the radio's audio is not enough: Pat reaches the modem on its TNC port"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        self.ports = set()
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertIn("XARA was not ready within 1 s", self.notes()[-1])

    def test_xara_refused_before_switching(self):
        """No callsign XARA takes: said, and the modem left as it is"""
        self.conf(enabled=1, mycall="", modem="ardop")
        self.status("ardop")
        before = read(self.wl.CONF)
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertEqual((read(self.wl.CONF), self.pokes), (before, []))
        self.assertEqual(self.notes(), ["no connection through VARA HF: XARA needs your callsign: set it on the "
                                        "Winlink page (Operating > Winlink), or the radio's in Settings > General; "
                                        "Pat did not call"])

    def test_a_change_of_the_pages_is_finished_first(self):
        """Mercury just chosen on the page, the supervisor still on ARDOP: the switch waits for it, then switches"""
        self.conf(enabled=1, mycall="N0AAA", modem="mercury")
        self.status("ardop")
        timer = threading.Timer(0.3, self.status, ("mercury",))
        timer.start()
        self.addCleanup(timer.cancel)
        start = time.time()
        self.assertEqual(self.wl.switch_modem("ardop"), (True, ""))
        self.assertGreater(time.time() - start, 0.25)
        self.assertEqual(self.pokes, [("ardop", "ardop")])
        # Pat restarting with the page's change: not settled either, and not for ever
        self.conf(enabled=1, mycall="N0AAA", modem="xara")
        self.status("ardop", pat="starting", text="Restarting Pat with the new settings...")
        self.assertEqual(self.wl.switch_modem("ardop"), (False, ""))
        self.assertEqual(self.notes()[-1], "no connection through ARDOP: the radio is still busy with the Winlink "
                                           "page's last change (Restarting Pat with the new settings...); Pat did "
                                           "not call. Connect again once that page says the modem is ready")
        self.assertEqual(self.wl.read_conf()["modem"], "xara")

    def test_the_page_changed_it_meanwhile(self):
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        self.then = "page"
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertEqual(self.notes()[-1], "no connection through VARA HF: the modem was changed on the Winlink page "
                                           "meanwhile, to Mercury; Pat did not call")
        self.assertEqual(self.wl.read_conf()["modem"], "mercury")

    def test_no_supervisor(self):
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        self.wl.supervisor_pid = lambda: 0
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertTrue(self.notes()[-1].startswith("no connection through VARA HF: Winlink's supervisor does not run"))
        self.assertEqual(self.wl.read_conf()["modem"], "ardop")

    def test_other_transports_refused(self):
        """Our Pat asks for ardop and varahf only; anything else is refused here, the settings untouched"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        before = read(self.wl.CONF)
        for args in (["telnet"], ["varafm"], [], ["ardop", "varahf"]):
            with contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertEqual(self.wl.main(["switch", *args]), 1, args)
            self.assertIn("usage: x6100-winlink switch ardop|varahf", err.getvalue())
        self.assertEqual((read(self.wl.CONF), self.pokes, self.notes()), (before, [], []))

    def test_one_at_a_time(self):
        """A second switch waits for the first one"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        lock = open(self.wl.SWITCH_LOCK, "a")
        self.addCleanup(lock.close)
        fcntl.flock(lock, fcntl.LOCK_EX)                     # the first one
        out = io.StringIO()
        t = threading.Thread(target=lambda: self.__dict__.update(code=self.wl.cmd_switch(["varahf"])))
        with contextlib.redirect_stdout(out):
            t.start()
            t.join(0.3)
            self.assertTrue(t.is_alive())
            self.assertEqual(self.pokes, [])
            fcntl.flock(lock, fcntl.LOCK_UN)
            t.join(5)
        self.assertEqual((self.code, out.getvalue()), (0, "bw=2300\n"))

    def test_as_pat_runs_it(self):
        """The command line, as our Pat runs it: no switch needed here, so no supervisor either"""
        for modem, via, out in (("xara", "varahf", "bw=2750\n"), ("ardop", "ardop", "")):
            self.conf(enabled=1, mycall="N0AAA", modem=modem, xara_bandwidth=2750)
            r = subprocess.run([sys.executable, "-I", SUPERVISOR, "switch", via], capture_output=True, text=True,
                               env=dict(os.environ, X6100_WINLINK_ROOT=self.root), timeout=30)
            self.assertEqual((r.returncode, r.stdout, r.stderr), (0, out, ""), modem)

    def test_abort_before_anything_changed(self):
        """Pat's Abort (SIGTERM) while the switch waits for the page's last change: nothing changed, nothing said
        (Pat says it)"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop", pat="starting", text="Restarting Pat with the new settings...")
        before = read(self.wl.CONF)
        timer = threading.Timer(0.1, self.wl._given_up.append, (1,))
        timer.start()
        self.addCleanup(timer.cancel)
        start = time.time()
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertLess(time.time() - start, 0.6)
        self.assertEqual((read(self.wl.CONF), self.pokes, self.notes()), (before, [], []))
        self.assertFalse(os.path.exists(self.wl.SWITCH))

    def test_abort_during_the_switch(self):
        """Pat's Abort while the new modem starts: the switch stops waiting, the modem goes on to the new one (the
        setting and the switch file stay, for a supervisor that has not read them yet)"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        self.then = "waits"
        timer = threading.Timer(0.2, self.wl._given_up.append, (1,))
        timer.start()
        self.addCleanup(timer.cancel)
        start = time.time()
        self.assertEqual(self.wl.switch_modem("varahf"), (False, ""))
        self.assertLess(time.time() - start, 0.8)
        self.assertEqual(self.wl.read_conf()["modem"], "xara")
        self.assertEqual(self.notes(), [
            "switching the modem from ARDOP to XARA for this connection through VARA HF",
            "Abort: the switch to XARA goes on without that connection, and the modem stays XARA, as the Winlink "
            "page (Operating > Winlink) now shows"])
        now = (dict(WANT, modem="xara"), "hash")
        self.assertTrue(self.wl.pats_switch((dict(WANT, modem="ardop"), "hash"), now))

    def test_abort_as_pat_sends_it(self):
        """The command as our Pat runs it, and SIGTERM as Pat sends it on its Abort: exit status 1 at once, with the
        stand-in supervisor never making the new modem ready"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        self.status("ardop")
        holder = subprocess.Popen([sys.executable, "-I", "-c", SUPERVISOR_LOCK, self.wl.LOCK], stdout=subprocess.PIPE,
                                  text=True)
        self.addCleanup(holder.wait, 10)
        self.addCleanup(holder.kill)
        self.assertEqual(holder.stdout.readline(), "holding\n")
        sw = subprocess.Popen([sys.executable, "-I", SUPERVISOR, "switch", "varahf"], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, env=dict(os.environ, X6100_WINLINK_ROOT=self.root))
        self.addCleanup(sw.kill)
        end = time.time() + 20
        while not any(n.startswith("switching the modem") for n in self.notes()) and time.time() < end:
            time.sleep(0.05)
        time.sleep(0.2)                                      # past the setting and the poke, in its wait
        sw.terminate()
        out, err = sw.communicate(timeout=3)
        self.assertEqual((sw.returncode, out, err), (1, "", ""))
        self.assertEqual(self.wl.read_conf()["modem"], "xara")
        self.assertTrue(self.notes()[-1].startswith("Abort: the switch to XARA goes on without that connection"))

    def test_listening(self):
        """/proc/net/tcp read as it is: a port listened on, and one that is not"""
        s = socket.socket()
        self.addCleanup(s.close)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        self.assertFalse(self.listening(port))
        s.listen(1)
        self.assertTrue(self.listening(port))


class ModemChoice(Switch):
    """`x6100-winlink modem`, APP > Mail's Modem (#322, #323): the choice made as the Winlink page makes it"""

    def run_cmd(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = self.wl.cmd_modem(list(args))
        return rc, json.loads(out.getvalue())

    def test_says_the_choice_and_the_modems_here(self):
        self.conf(enabled=1, mycall="N0AAA", modem="mercury")
        self.assertEqual(self.run_cmd(), (0, {"modem": "mercury", "name": "Mercury",
                                              "modems": ["ardop", "mercury", "xara"], "changed": False}))
        os.remove(self.wl.XARA)
        self.assertEqual(self.run_cmd()[1]["modems"], ["ardop", "mercury"])
        self.assertEqual(self.pokes, [])

    def test_chosen_as_the_page_does(self):
        """The setting, the last of Mercury and XARA, Pat's config with that modem's bandwidth and service code, and
        the supervisor poked with no switch file: it restarts Pat, as for the page's Save"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop", bandwidth=1000, xara_bandwidth=2750, own_key="kept")
        rc, out = self.run_cmd("xara")
        self.assertEqual((rc, out["modem"], out["name"], out["changed"]), (0, "xara", "XARA", True))
        conf = self.wl.read_conf()
        self.assertEqual((conf["modem"], conf["varahf_modem"], conf["bandwidth"], conf["own_key"]),
                         ("xara", "xara", "1000", "kept"))
        self.assertEqual(self.pokes, [("xara", None)])
        self.assertEqual(self.pat_config()["varahf"]["bandwidth"], 2750)
        self.assertEqual(self.pat_config()["service_codes"], ALL3)
        self.assertEqual(self.run_cmd("mercury")[1]["modem"], "mercury")
        self.assertEqual(self.wl.read_conf()["varahf_modem"], "mercury")
        self.assertEqual(self.pat_config()["service_codes"], ALL3)
        self.assertEqual(self.run_cmd("ardop")[1]["modem"], "ardop")
        self.assertEqual(self.wl.read_conf()["varahf_modem"], "mercury")      # the last of the two stays
        self.assertEqual(len(self.pokes), 3)
        # a switch file an aborted switch of Pat's left behind does not make this one Pat's: Pat restarts
        self.wl.write_kv(self.wl.SWITCH, {"modem": "xara", "at": f"{time.time():.3f}"})
        self.assertEqual(self.run_cmd("xara")[1]["changed"], True)
        self.assertEqual(self.pokes[-1], ("xara", None))

    def test_the_same_one_changes_nothing(self):
        self.conf(enabled=1, mycall="N0AAA", modem="xara")
        before = read(self.wl.CONF)
        self.assertEqual(self.run_cmd("xara"), (0, {"modem": "xara", "name": "XARA",
                                                    "modems": ["ardop", "mercury", "xara"], "changed": False}))
        self.assertEqual((read(self.wl.CONF), self.pokes), (before, []))

    def test_while_winlink_is_off(self):
        """Kept for when Winlink is turned on: nothing to restart, Pat's config not written"""
        self.conf(enabled=0, mycall="N0AAA", modem="ardop")
        self.assertEqual(self.run_cmd("mercury")[1]["changed"], True)
        self.assertEqual(self.wl.read_conf()["modem"], "mercury")
        self.assertEqual(self.pokes, [])
        self.assertFalse(os.path.exists(self.wl.PAT_CONFIG))

    def test_refused(self):
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        os.remove(self.wl.MERCURY)
        self.assertEqual(self.run_cmd("mercury"), (1, {"error": "Mercury is not on this radio"}))
        self.assertEqual(self.run_cmd("pactor")[0], 1)
        self.assertEqual(self.run_cmd("xara", "ardop")[0], 1)
        self.conf(enabled=1, mycall="TOOLONGCALL", modem="ardop")
        rc, out = self.run_cmd("xara")
        self.assertEqual(rc, 1)
        self.assertIn("XARA does not take TOOLONGCALL as a callsign", out["error"])
        self.assertEqual((self.wl.read_conf()["modem"], self.pokes), ("ardop", []))

    def test_not_during_pats_own_switch(self):
        """Pat's switch for a connection holds the switch lock: the choice waits for it to be over"""
        self.conf(enabled=1, mycall="N0AAA", modem="ardop")
        with open(self.wl.SWITCH_LOCK, "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            rc, out = self.run_cmd("xara")
        self.assertEqual((rc, out), (1, {"error": "Pat is switching the modem for a connection: after it"}))
        self.assertEqual(self.wl.read_conf()["modem"], "ardop")
        self.assertEqual(self.run_cmd("xara")[0], 0)

    def test_as_a_command(self):
        """As the GUI runs it: one JSON object on stdout, exit status 1 with the reason when it is not chosen"""
        self.conf(enabled=0, mycall="N0AAA", modem="ardop")
        env = dict(os.environ, X6100_WINLINK_ROOT=self.root)
        r = subprocess.run([sys.executable, "-B", SUPERVISOR, "modem", "xara"], env=env, capture_output=True,
                           text=True, timeout=20)
        self.assertEqual((r.returncode, json.loads(r.stdout)["modem"], r.stderr), (0, "xara", ""))
        os.remove(self.wl.MERCURY)
        r = subprocess.run([sys.executable, "-B", SUPERVISOR, "modem", "mercury"], env=env, capture_output=True,
                           text=True, timeout=20)
        self.assertEqual((r.returncode, json.loads(r.stdout)), (1, {"error": "Mercury is not on this radio"}))


class MailBusy(Root):
    """`x6100-winlink-mail busy`, asked before APP > Mail's Connect moves the radio with its first press (#322): a
    connection under way, whoever began it, as Pat (a stand-in for its /api/status here) and the switch lock say"""

    def setUp(self):
        super().setUp()
        import http.server
        test = self
        self.pat = {"connected": False, "dialing": False}

        class Pat(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = json.dumps(test.pat).encode() if self.path == "/api/status" else b"{}"
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.srv = http.server.HTTPServer(("127.0.0.1", 0), Pat)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        os.environ["X6100_WINLINK_MAIL_PORT"] = str(self.srv.server_address[1])
        self.addCleanup(os.environ.pop, "X6100_WINLINK_MAIL_PORT", None)
        loader = importlib.machinery.SourceFileLoader("x6100_winlink_mail", SUPERVISOR + "-mail")
        self.mail = importlib.util.module_from_spec(importlib.util.spec_from_loader("x6100_winlink_mail", loader))
        loader.exec_module(self.mail)

    def test_idle(self):
        self.assertEqual(self.mail.cmd_busy(), {"busy": False})

    def test_a_connection_from_anywhere(self):
        """Pat's own word: one begun in the web mailbox shows as one begun on the radio"""
        self.pat = {"connected": False, "dialing": True}
        self.assertEqual(self.mail.cmd_busy(), {"busy": True, "why": "Pat is calling a gateway"})
        self.pat = {"connected": True, "dialing": False}
        self.assertEqual(self.mail.cmd_busy(), {"busy": True, "why": "a connection is under way"})

    def test_pats_modem_switch_before_it_dials(self):
        """Our Pat waits for the other modem before its QSY and says neither connected nor dialing meanwhile:
        `x6100-winlink switch` holds its lock then"""
        with open(self.wl.SWITCH_LOCK, "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.assertEqual(self.mail.cmd_busy(),
                             {"busy": True, "why": "the modem is being switched for a connection"})
        self.assertEqual(self.mail.cmd_busy(), {"busy": False})

    def test_pat_not_answering(self):
        """No answer is no "not busy": an error, and the app moves nothing"""
        self.srv.shutdown()
        self.srv.server_close()
        with self.assertRaises(self.mail.Fail):
            self.mail.cmd_busy()

    def test_as_the_app_runs_it(self):
        self.pat = {"connected": True, "dialing": False}
        env = dict(os.environ, X6100_WINLINK_ROOT=self.root)
        r = subprocess.run([sys.executable, "-I", SUPERVISOR + "-mail", "busy"], env=env, capture_output=True,
                           text=True, timeout=20)
        self.assertEqual((r.returncode, json.loads(r.stdout)), (0, {"busy": True, "why": "a connection is under way"}))


class SwitchSupervisor(Root):
    """The supervisor's side: it goes on with Pat through Pat's own switch, and restarts it for the page's"""

    def setUp(self):
        super().setUp()
        self.want = dict(WANT, modem="ardop")
        self.now = (self.want, "hash")

    def test_pats_switch(self):
        self.wl.write_kv(self.wl.SWITCH, {"modem": "xara", "at": time.time()})
        new = (dict(self.want, modem="xara"), "hash")
        self.assertTrue(self.wl.pats_switch(self.now, new))
        self.assertFalse(self.wl.pats_switch(self.now, (dict(self.want, modem="mercury"), "hash")))  # not that one
        self.assertFalse(self.wl.pats_switch(self.now, (dict(self.want, modem="xara", xara_bandwidth=500), "hash")))
        self.assertFalse(self.wl.pats_switch(self.now, (dict(self.want, modem="xara"), "new password")))
        self.assertFalse(self.wl.pats_switch(None, new))
        self.wl.write_kv(self.wl.SWITCH, {"modem": "xara", "at": time.time() - 600})   # left behind
        self.assertFalse(self.wl.pats_switch(self.now, new))
        os.remove(self.wl.SWITCH)
        self.assertFalse(self.wl.pats_switch(self.now, new))

    def test_pat_told_where_the_switch_is(self):
        os.makedirs(os.path.dirname(self.wl.PAT), exist_ok=True)
        with open(self.wl.PAT, "w") as f:
            f.write(f'#!/bin/sh\necho "${self.wl.SWITCH_ENV}" > "$2.switch"\n')
        os.chmod(self.wl.PAT, 0o755)
        os.environ[self.wl.SWITCH_ENV] = "/elsewhere"
        self.addCleanup(os.environ.pop, self.wl.SWITCH_ENV, None)
        pat = self.wl.Program("Pat", self.wl.PAT_LOG)
        self.wl.start_pat(pat, dict(WANT))
        pat.proc.wait(10)
        self.assertEqual(read(self.wl.PAT_OWN_LOG + ".switch").strip(), os.path.abspath(SUPERVISOR))


class SwitchOutcome(Logs):
    def test_the_switch_failed(self):
        """Our line when the switch did not come about is the last connection, as it says, not said again"""
        line = ("2026/11/18 10:00:00 OpenX6X00: no connection through VARA HF: XARA did not start (XARA (xara) "
                "stopped (code 1)); Pat did not call. The Winlink page (Operating > Winlink) says how XARA is doing")
        k = self.poll(pat=line)
        self.assertEqual(k["last_result"], "failed")
        self.assertEqual(k["last_text"], "XARA did not start (XARA (xara) stopped (code 1)); Pat did not call. The "
                                         "Winlink page (Operating > Winlink) says how XARA is doing")
        self.assertEqual(k["last_next"], "")
        self.assertEqual(len(self.notes()), 1)              # its own line only
        k = self.poll(pat="2026/11/18 10:00:00 OpenX6X00: switching the modem from ARDOP to XARA for this "
                          "connection through VARA HF\n2026/11/18 10:00:04 OpenX6X00: XARA is ready and the "
                          "connection goes ahead\n2026/11/18 10:00:05 Connecting to N0BBB (varahf)...")
        self.assertEqual((k["last_result"], k["last_gateway"]), ("dialing", "N0BBB"))

    REFUSED = ("2026/11/19 11:41:53 OpenX6X00: no connection through ARDOP: one through VARA HF is under way, and the "
               "radio runs one modem at a time")

    def test_a_refusal_during_a_dial_then_its_outcome(self):
        """The bench's order (19 Nov): an ARDOP connection refused while the VARA HF one waits for a clear channel is
        shown while it is the latest, and the VARA HF one's outcome replaces it"""
        k = self.poll(pat="2026/11/19 11:40:13 VARA modem (VARA HF v4.9.0) initialized\n"
                          "2026/11/19 11:40:13 Connecting to N0CALL (varahf)...\n"
                          "2026/11/19 11:40:13 Waiting for clear channel...", kind="xara")
        self.assertEqual((k["last_result"], k["last_gateway"]), ("dialing", "N0CALL"))
        k = self.poll(pat="2026/11/19 11:41:43 Got abort signal while dialing varahf, cancelling...\n" + self.REFUSED,
                      kind="xara")
        self.assertEqual((k["last_result"], k["last_gateway"]), ("failed", ""))
        self.assertEqual(k["last_text"], "one through VARA HF is under way, and the radio runs one modem at a time")
        self.assertEqual(self.notes(), [self.REFUSED])       # Pat's own line only
        k = self.poll(pat="2026/11/19 11:45:13 Unable to establish connection to remote: aborted while waiting for "
                          "clear channel", kind="xara")
        self.assertEqual((k["last_result"], k["last_gateway"]), ("failed", "N0CALL"))
        self.assertEqual(k["last_text"], "The frequency stayed busy, or Abort was chosen while Pat waited for it to "
                                         "clear")
        self.assertEqual(k["last_next"], "Try again later, another gateway or another band")
        self.assertEqual(len(self.notes()), 2)               # its outcome said once
        self.assertTrue(self.notes()[-1].endswith(" OpenX6X00: " + k["last_text"] + ". " + k["last_next"]))
        # The next connection, as before
        k = self.poll(pat="2026/11/19 11:45:26 Connecting to N0CALL (ardop)...")
        self.assertEqual((k["last_result"], k["last_text"]), ("dialing", "Calling N0CALL"))
        k = self.poll(pat="2026/11/19 11:46:04 Unable to establish connection to remote: Connect timeout")
        self.assertEqual((k["last_result"], k["last_text"]), ("failed", "No answer from N0CALL"))

    def test_a_refusal_during_a_dial_that_connects(self):
        """The ARDOP connection under way connects after the refusal, and goes on to the end: its lines and
        ardopcf's are its own"""
        self.poll(pat="2026/11/19 11:40:13 Connecting to N0BBB (ardop)...")
        k = self.poll(pat="2026/11/19 11:40:20 OpenX6X00: no connection through VARA HF: one through ARDOP is under "
                          "way, and the radio runs one modem at a time", ardop=ESTABLISHED.format("N0BBB", 500))
        self.assertEqual((k["last_result"], k["last_bw"]), ("failed", ""))
        k = self.poll(pat="2026/11/19 11:40:21 Connected to N0BBB (ardop)\nRemote accepted ABC123")
        self.assertEqual((k["last_result"], k["last_gateway"], k["last_bw"]), ("connected", "N0BBB", 500))
        k = self.poll(pat="2026/11/19 11:41:00 Disconnected.")
        self.assertEqual(k["last_text"], "Done with N0BBB at 500 Hz: 1 sent, 0 received")

    def test_a_refusal_during_a_dial_with_no_answer(self):
        """ardopcf's word for the call under way counts for it, not for the refusal shown meanwhile"""
        self.poll(pat="2026/11/19 11:40:10 QSY ardop: 7101.5\n2026/11/19 11:40:13 Connecting to N0BBB (ardop)...")
        self.poll(pat="2026/11/19 11:40:20 OpenX6X00: no connection through VARA HF: one through ARDOP is under way, "
                      "and the radio runs one modem at a time")
        k = self.poll(pat="2026/11/19 11:41:00 Unable to establish connection to remote: Connect timeout",
                      ardop="[STATUS: ARQ CONNECTION REJECTED BY N0BBB, REMOTE STATION BUSY.]")
        self.assertEqual((k["last_gateway"], k["last_freq"]), ("N0BBB", 7101500))
        self.assertEqual(k["last_text"], "N0BBB is busy with another station")

    def test_a_refusal_and_pat_restarted(self):
        """Pat restarted with the dial under way behind a refusal: that dial ends there, shown"""
        self.poll(pat="2026/11/19 11:40:13 Connecting to N0BBB (ardop)...\n" + self.REFUSED)
        self.watch.pat_started()
        open(self.wl.PAT_OWN_LOG, "w").close()
        k = self.poll()
        self.assertEqual((k["last_result"], k["last_text"]), ("failed", "Pat stopped during the connection to N0BBB"))

    def test_a_refusal_alone_stays(self):
        """With nothing under way, a refusal is the last connection until the next one"""
        self.poll(pat="2026/11/19 11:40:13 Connecting to N0BBB (ardop)...\n"
                      "2026/11/19 11:41:00 Unable to establish connection to remote: Connect timeout")
        k = self.poll(pat=self.REFUSED)
        self.assertEqual(k["last_text"], "one through VARA HF is under way, and the radio runs one modem at a time")
        k = self.poll(pat="2026/11/19 11:42:00 Waiting for clear channel...")
        self.assertEqual(k["last_text"], "one through VARA HF is under way, and the radio runs one modem at a time")

    def test_the_old_modems_radio_file_left_out(self):
        """Just after a switch the GUI's file may still say connected for the modem that stopped"""
        with open(self.wl.RADIO, "w") as f:
            f.write(RADIO.format(app="", refused="", seq=0, qsy=0, qsy_why="", qsy_seq=0, swr=0))
        self.watch.modem_started()
        old = time.time() - 2
        os.utime(self.wl.RADIO, (old, old))
        self.assertEqual(self.poll(kind="xara")["modem"], "waiting")
        self.assertEqual(self.poll(kind="xara", xara="xara: connected to the GUI's socket /run/ardop.sock")["modem"],
                         "ready")
        os.utime(self.wl.RADIO, None)                        # written again since: the GUI's word
        self.assertEqual(self.poll(kind="xara")["modem"], "ready")


FAKE_PAT = """#!{py}
# A stand-in for Pat: its log made, its start counted, its mailbox's /api/status on 8080
import http.server, json, os, sys
log = sys.argv[sys.argv.index("--log") + 1]
open(log, "a").close()
with open(os.path.join(os.path.dirname(log), "pat-starts"), "a") as f:
    f.write(f"{{os.getpid()}}\\n")
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps({{"connected": False, "dialing": False, "http_clients": []}}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass
http.server.HTTPServer.allow_reuse_address = True
http.server.HTTPServer(("127.0.0.1", 8080), H).serve_forever()
"""

FAKE_MODEM = """#!{py}
# A stand-in for a modem: its TNC port open, its line that it is on the GUI's socket, until SIGTERM
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", {port}))
s.listen(1)
print("{said}", flush=True)
while True:
    time.sleep(1)
"""


@unittest.skipUnless(os.environ.get("WINLINK_NETNS") == "1",
                     "needs a network namespace of its own (ports 8080, 8300, 8515): make -C tools/winlink test")
class SwitchWithTheSupervisor(Root):
    """The supervisor itself (x6100-winlink run) with stand-ins for Pat and the three modems, and Pat's switch as
    Pat runs it: the modem swapped, Pat not restarted for it, the page's own change restarting Pat as before"""

    def setUp(self):
        super().setUp()
        progs = {self.wl.ARDOPCF: (8515, "Audio socket {sock} connected"),
                 self.wl.MERCURY: (8300, "x6x00-mercury: connected to {sock}"),
                 self.wl.XARA: (8300, "xara: connected to the GUI's socket {sock}")}
        for path, (port, said) in progs.items():
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(FAKE_MODEM.format(py=sys.executable, port=port, said=said.format(sock=self.wl.ARDOP_SOCK)))
            os.chmod(path, 0o755)
        with open(self.wl.PAT, "w") as f:
            f.write(FAKE_PAT.format(py=sys.executable))
        os.chmod(self.wl.PAT, 0o755)
        self.conf(enabled=1, mycall="N0AAA", modem="ardop", mercury_bandwidth=500, xara_bandwidth=2300)
        self.sup = subprocess.Popen([sys.executable, "-I", SUPERVISOR, "run"],
                                    env=dict(os.environ, X6100_WINLINK_ROOT=self.root),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop)

    def stop(self):
        self.sup.terminate()
        self.sup.wait(30)

    def until(self, what, test, seconds=20):
        end = time.time() + seconds
        while time.time() < end:
            if test():
                return
            time.sleep(0.1)
        self.fail(f"{what}: status {self.wl.read_kv(self.wl.STATUS)}")

    def starts(self):
        return read(os.path.join(self.wl.RUN, "pat-starts")).split()

    def test_switched_and_back_pat_running_on(self):
        self.until("ARDOP ready", lambda: self.wl.modem_ready(self.wl.read_kv(self.wl.STATUS), "ardop")
                   and self.wl.read_kv(self.wl.STATUS).get("pat") == "running")
        self.assertEqual(self.wl.switch_modem("varahf"), (True, "2300"))
        st = self.wl.read_kv(self.wl.STATUS)
        self.assertEqual((st["ardop"], st["xara"], st["modem"]), ("off", "running", "ready"))
        self.assertTrue(self.wl.listening(8300) and not self.wl.listening(8515))
        self.assertEqual(self.wl.read_conf()["modem"], "xara")
        self.assertEqual(self.wl.switch_modem("ardop"), (True, ""))
        self.assertEqual(self.wl.read_kv(self.wl.STATUS)["ardop"], "running")
        self.assertEqual(len(self.starts()), 1)              # Pat went on through both
        log = read(self.wl.PAT_OWN_LOG)
        self.assertIn("OpenX6X00: XARA is ready and the connection goes ahead", log)
        self.assertIn("OpenX6X00: ARDOP is ready and the connection goes ahead", log)
        # The page's own choice, as before: Pat restarts with it
        self.conf(enabled=1, mycall="N0AAA", modem="mercury", mercury_bandwidth=500, xara_bandwidth=2300)
        self.wl.poke()
        self.until("Mercury ready after the page's choice",
                   lambda: self.wl.modem_ready(self.wl.read_kv(self.wl.STATUS), "mercury")
                   and len(self.starts()) == 2)
        # And as our Pat runs it, the right modem running: at once
        r = subprocess.run([sys.executable, "-I", SUPERVISOR, "switch", "varahf"], capture_output=True, text=True,
                           env=dict(os.environ, X6100_WINLINK_ROOT=self.root), timeout=30)
        self.assertEqual((r.returncode, r.stdout), (0, "bw=500\n"), r.stderr)


if __name__ == "__main__":
    unittest.main()
