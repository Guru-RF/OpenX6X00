#!/usr/bin/env python3
"""The Wavelog upload and CB QSOs (#286): rootfs/usr/sbin/x6100-wavelog's collect() on a log in a new folder.

    python3 -I -B tools/wavelog_cb/wavelog_cb_test.py [-v]

Wavelog is an amateur log with no 11 m band: a QSO with a CB call on either side (26AT101, 1AT1000, 999ZZ/ZZ, in its
CALL, STATION_CALLSIGN or OPERATOR, in either case) stays on the radio; amateur ones go into the queue as before. The
CB calls are those of src/cb.h's cb_callsign() (tools/aerocb tests the same lists there).

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import importlib.machinery
import importlib.util
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-wavelog")


def load(field_dir):
    os.environ["X6100_FIELD_DIR"] = field_dir     # read when the script loads
    loader = importlib.machinery.SourceFileLoader("x6100_wavelog", SCRIPT)
    spec = importlib.util.spec_from_loader("x6100_wavelog", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def record(**fields):
    return "".join(f"<{k.upper()}:{len(v.encode())}>{v}" for k, v in fields.items()) + "<EOR>\n"


def qso(call, me, t, **more):
    return record(call=call, qso_date="20261116", time_on=t, band=more.pop("band", "20m"), mode="FT8",
                  station_callsign=me, **more)


class CbTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="wavelog-cb-")
        os.makedirs(os.path.join(self.dir, "logs"))
        self.w = load(self.dir)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def queued(self, text):
        path = os.path.join(self.dir, "logs", "2026-11-16.adi")
        with open(path, "w") as f:
            f.write("OpenX6X00 log\n<EOH>\n" + text)
        os.utime(path, (0, 0))     # read at once, not again in 2 s (FAT's step)
        state, queue = {}, []
        self.w.collect(state, queue, 0)
        return [q["call"] for q in queue]

    def test_cb_calls(self):
        for call in ("26AT101", "1AT106", "21AT106", "999ZZ999", "1AT1000", "999ZZ/ZZ", "161XZ085", "1a1", "1at106"):
            self.assertTrue(self.w.cb_qso({"call": call}), call)
        for call in ("ON6URE", "2E0ABC", "4X1AB", "9A1AA", "26AT1000", "26AT", "1234AT1", "1AT/A", "PA/ON6URE", ""):
            self.assertFalse(self.w.cb_qso({"call": call}), call)

    def test_cb_qsos_stay(self):
        log = (qso("ON4ABC", "ON6URE", "120000")
               + qso("26AT101", "1AT106", "120100", band="", freq="27.265000")
               + qso("1AT107", "ON6URE", "120200")                 # an amateur call of ours, a CB call
               + qso("ON4ABC", "26AT101", "120300")                # a CB call of ours
               + record(call="DL1ABC", qso_date="20261116", time_on="120400", band="40m", mode="SSB",
                        operator="999zz/zz")                       # OPERATOR, lower case
               + qso("G4ABC", "ON6URE", "120500"))
        self.assertEqual(self.queued(log), ["ON4ABC", "G4ABC"])

    def test_amateur_unchanged(self):
        log = qso("ON4ABC", "ON6URE", "120000") + qso("2E0ABC", "ON6URE/P", "120100")
        self.assertEqual(self.queued(log), ["ON4ABC", "2E0ABC"])


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=sys.argv[:1])
