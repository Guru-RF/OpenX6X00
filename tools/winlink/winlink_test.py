#!/usr/bin/env python3
"""Tests of the Winlink supervisor (rootfs/usr/sbin/x6100-winlink) on a PC: each test loads it with
X6100_WINLINK_ROOT set to a new temporary directory, so every path it uses is under that one, and calls its
functions. Standard library only:

    python3 -B tools/winlink/winlink_test.py [-v]
"""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import sys
import tempfile
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
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as f:
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
        self.assertNotIn("s3cret", open(self.wl.STATUS).read())

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
        lines = open(self.wl.PAT_OWN_LOG).read().splitlines()
        self.assertEqual(len(lines), 3)
        self.assertRegex(lines[1], r"^\d{4}/\d\d/\d\d \d\d:\d\d:\d\d OpenX6X00: a line of ours$")
        self.assertTrue(lines[2].endswith("Connecting to DM6TS (ardop)..."))

    def test_never_the_password(self):
        os.makedirs(self.wl.LIB)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("s3cret\n")
        open(self.wl.PAT_OWN_LOG, "w").close()
        self.wl.note("login s3cret refused")
        self.assertNotIn("s3cret", open(self.wl.PAT_OWN_LOG).read())

    def test_no_password_said_when_pat_answers(self):
        open(self.wl.PAT_OWN_LOG, "w").close()
        self.wl.pat_up({"mycall": "M7SVA"})
        self.assertIn("OpenX6X00: no Winlink password for M7SVA", open(self.wl.PAT_OWN_LOG).read())
        os.makedirs(self.wl.LIB)
        with open(self.wl.PASSWORD, "w") as f:
            f.write("pw\n")
        open(self.wl.PAT_OWN_LOG, "w").close()
        self.wl.pat_up({"mycall": "M7SVA"})
        self.assertEqual(open(self.wl.PAT_OWN_LOG).read(), "")

    def test_trimmed(self):
        pat = os.open(self.wl.PAT_OWN_LOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_APPEND, 0o666)
        os.write(pat, b"x" * (self.wl.LOG_MAX + 1))
        self.wl.trim_logs()
        os.write(pat, b"next\n")
        os.close(pat)
        self.assertEqual(open(self.wl.PAT_OWN_LOG, "rb").read(), b"next\n")


if __name__ == "__main__":
    unittest.main()
