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


if __name__ == "__main__":
    unittest.main()
