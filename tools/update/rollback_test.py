#!/usr/bin/env python3
"""What the radio says after a version was taken back (rootfs/usr/sbin/x6100-update): the updater's own code, its
state in a folder of its own under build/tmp for each case, the files an update replaces there too. See the Makefile:

    make          (python3 -I -B rollback_test.py [-v])

1. The start loop's step back after MAX_STARTS starts without a minute: last.json keeps the reason and the
   sentence, "notice" gives it once as two short lines for the screen and then nothing, "status" has the sentence in "last".
2. The same for a version whose GUI said it could not use the settings file ("fault settings"): that reason, not
   "did not stay up". "fault" with nothing pending, or with another word, changes nothing.
3. The step back by hand (rollback) and the one of an install cut short by a power cut.
4. last.json as the updaters up to v0.3.10 wrote it (no reason, no sentence): status and notice make the sentence
   from its "why"; a reason of a later updater is said by its "why"; a version not known reads "the version before".
5. "why" is still written, for the web pages of the versions before, which show it.
"""
import io
import json
import os
import runpy
import shutil
import signal
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
UPDATER = os.path.join(HERE, "..", "..", "rootfs", "usr", "sbin", "x6100-update")


class Updater:
    """The updater's functions on a folder of its own: its state, the files a version installs, its version file"""

    def __init__(self, root):
        self.up = runpy.run_path(UPDATER)["apply"].__globals__
        self.root = root
        state, work = os.path.join(root, "state"), os.path.join(root, "work")
        os.makedirs(state)
        os.makedirs(work)
        self.version = os.path.join(root, "app-version")
        self.target = os.path.join(root, "x6100_gui")
        self.restarts = 0

        def restart():
            self.restarts += 1
        self.up.update(WORK=work, STATE=state, PREV=os.path.join(state, "prev"), JOURNAL=os.path.join(state, "journal.json"),
                       PENDING=os.path.join(state, "pending.json"), LAST=os.path.join(state, "last.json"),
                       LOCK=os.path.join(state, "lock"), PROGRESS=os.path.join(work, ".progress"),
                       VERSION_FILE=self.version, restart_apps=restart,
                       TAILSCALE=os.path.join(root, "no-tailscale"), TAILSCALE_STATE=os.path.join(root, "no-state"))

    def install(self, old="0.3.11", new="0.3.12"):
        """new over old, as cmd_install leaves it once the files are in place: pending, not confirmed"""
        with open(self.version, "w") as f:
            f.write(old + "\n")
        with open(self.target, "w") as f:
            f.write(old)
        stage = os.path.join(self.up["WORK"], "stage")
        for path, body in ((self.target, new), (self.version, new + "\n")):
            staged = os.path.join(stage, path.lstrip("/"))
            os.makedirs(os.path.dirname(staged), exist_ok=True)
            with open(staged, "w") as f:
                f.write(body)
        self.up["apply"]({"version": new, "files": [{"path": self.target}, {"path": self.version}]})
        self.up["save"](self.up["LAST"], {"result": "installed", "version": new, "when": 1})

    def run(self, *argv):
        """A command as the GUI and the web server call it: main(), its one line on stdout, its exit code. Each is a
        process of its own on the radio, whose lock goes when it exits: here the lock's file is closed after it"""
        out, fds = io.StringIO(), []
        take_lock = self.up["take_lock"]

        def lock():
            fds.append(take_lock())
            return fds[-1]
        self.up["take_lock"] = lock
        try:
            with redirect_stdout(out):
                rc = self.up["main"](list(argv))
        finally:
            self.up["take_lock"] = take_lock
            signal.alarm(0)         # main's limit on the run
            for fd in fds:
                os.close(fd)
        return rc, out.getvalue().strip()

    def last(self):
        with open(self.up["LAST"]) as f:
            return json.load(f)

    def write_last(self, obj):
        with open(self.up["LAST"], "w") as f:
            json.dump(obj, f)

    def installed(self):
        with open(self.target) as f:
            return f.read()


class RollbackNotice(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="rollback-", dir=os.environ.get("TMPDIR"))
        self.u = Updater(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def notice_once(self, text, screen):
        """notice gives the screen's two short lines, then nothing at the next start; status keeps the sentence"""
        self.assertEqual(self.u.run("notice"), (0, "ok " + screen))
        self.assertEqual(self.u.run("notice"), (0, "ok"))
        rc, out = self.u.run("status")
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out)["last"]["text"], text)

    def test_starts(self):
        u = self.u
        u.install()
        self.assertEqual(u.run("notice"), (0, "ok"), "an install is no step back")
        for n in range(1, u.up["MAX_STARTS"] + 1):
            self.assertEqual(u.run("boot-check"), (0, f"pending 0.3.12 start {n}"))
        self.assertEqual(u.run("boot-check"), (0, "rolled back to 0.3.11"))
        self.assertEqual(u.installed(), "0.3.11", "the files of the version before")
        last = u.last()
        self.assertEqual((last["result"], last["version"], last["to"], last["reason"], last["starts"]),
                         ("rolled back", "0.3.12", "0.3.11", "starts", 3))
        self.assertEqual(last["why"], "the GUI did not stay up (3 starts)", "the old pages' why, as before")
        self.notice_once("Update v0.3.12 taken back to v0.3.11: it did not stay up a minute in 3 starts",
                         "v0.3.12 taken back to v0.3.11\nit did not stay up (3 starts)")
        self.assertEqual(u.run("boot-check"), (0, "ok"), "nothing pending after it")

    def test_settings(self):
        u = self.u
        self.assertEqual(u.run("fault", "settings"), (0, "ok nothing pending"))
        u.install()
        for n in range(1, u.up["MAX_STARTS"] + 1):
            u.run("boot-check")
            self.assertEqual(u.run("fault", "settings"), (0, "ok 0.3.12 could not use the settings file"))
        rc, out = u.run("fault", "other")
        self.assertEqual((rc, out), (1, "error fault takes: settings"))
        self.assertEqual(u.run("boot-check"), (0, "rolled back to 0.3.11"))
        self.assertEqual(u.last()["reason"], "settings")
        self.assertEqual(u.last()["why"], "this version could not use the settings file")
        self.notice_once("Update v0.3.12 taken back to v0.3.11: it could not use the settings file",
                         "v0.3.12 taken back to v0.3.11\nit could not use the settings")

    def test_confirmed(self):
        u = self.u
        u.install()
        u.run("boot-check")
        u.run("fault", "settings")      # a start that could not, then one that could and ran its minute
        u.run("boot-check")
        self.assertEqual(u.run("confirm"), (0, "ok 0.3.12 confirmed"))
        self.assertEqual(u.run("notice"), (0, "ok"))
        for _ in range(u.up["MAX_STARTS"] + 2):
            self.assertEqual(u.run("boot-check"), (0, "ok"))
        self.assertEqual(u.installed(), "0.3.12")
        rc, out = u.run("status")
        self.assertNotIn("text", json.loads(out)["last"], "a confirm says nothing")

    def test_by_hand(self):
        u = self.u
        u.install()
        u.run("boot-check")
        self.assertEqual(u.run("rollback"), (0, "ok back to 0.3.11"))
        self.assertEqual((u.installed(), u.restarts), ("0.3.11", 1))
        self.assertEqual(u.last()["reason"], "hand")
        self.notice_once("Update v0.3.12 taken back to v0.3.11 by hand", "v0.3.12 taken back to v0.3.11\nby hand")

    def test_interrupted(self):
        u = self.u
        u.install()
        journal = u.up["load"](u.up["JOURNAL"])
        journal["state"] = "applying"       # the power went during the swap
        u.up["save"](u.up["JOURNAL"], journal)
        self.assertEqual(u.run("boot-check"), (0, "rolled back: interrupted install"))
        self.assertEqual(u.last()["reason"], "interrupted")
        self.notice_once("Update v0.3.12 taken back to v0.3.11: the install was cut short",
                         "v0.3.12 taken back to v0.3.11\nits install was cut short")

    def test_old_files(self):
        u = self.u
        cases = [
            ({"result": "rolled back", "version": "0.3.10", "to": "0.3.9",
              "why": "the GUI did not stay up (3 starts)", "when": 1},
             "Update v0.3.10 taken back to v0.3.9: it did not stay up a minute in 3 starts",
             "v0.3.10 taken back to v0.3.9\nit did not stay up (3 starts)"),
            ({"result": "rolled back", "version": "0.3.10", "to": "0.3.9", "when": 1},
             "Update v0.3.10 taken back to v0.3.9 by hand", "v0.3.10 taken back to v0.3.9\nby hand"),
            ({"result": "rolled back", "version": "0.3.10", "to": "0.3.9", "why": "the install was interrupted",
              "when": 1},
             "Update v0.3.10 taken back to v0.3.9: the install was cut short",
             "v0.3.10 taken back to v0.3.9\nits install was cut short"),
            ({"result": "rolled back", "version": "0.3.13", "to": "0.3.12", "reason": "battery",
              "why": "the battery ran out during its first minute", "when": 1},
             "Update v0.3.13 taken back to v0.3.12: the battery ran out during its first minute",
             "v0.3.13 taken back to v0.3.12\nthe battery ran out during its first minute"),
            ({"result": "rolled back", "version": "0.3.10", "to": "unknown", "reason": "starts", "starts": 5,
              "when": 1},
             "Update v0.3.10 taken back to the version before: it did not stay up a minute in 5 starts",
             "v0.3.10 taken back to the version before\nit did not stay up (5 starts)"),
            ({"result": "rolled back", "to": "0.3.9", "reason": "hand", "when": 1},
             "The update taken back to v0.3.9 by hand", "Update taken back to v0.3.9\nby hand"),
        ]
        for old, text, screen in cases:
            with self.subTest(old=old):
                u.write_last(old)
                rc, out = u.run("status")
                self.assertEqual(json.loads(out)["last"]["text"], text)
                self.assertNotIn("text", u.last(), "status reads, it writes nothing")
                self.notice_once(text, screen)
        u.write_last({"result": "confirmed", "version": "0.3.10", "when": 1})
        self.assertEqual(u.run("notice"), (0, "ok"))
        u.write_last(["not", "an", "object"])
        self.assertEqual(u.run("notice"), (0, "ok"))
        self.assertIsNone(json.loads(u.run("status")[1])["last"])
        os.remove(u.up["LAST"])
        self.assertEqual(u.run("notice"), (0, "ok"))

    def test_later_text_kept(self):
        """A sentence a later updater wrote is said as it is"""
        u = self.u
        u.write_last({"result": "rolled back", "version": "0.4.0", "to": "0.3.12", "reason": "new",
                      "text": "Update v0.4.0 taken back to v0.3.12: something new", "when": 1})
        self.notice_once("Update v0.4.0 taken back to v0.3.12: something new", "v0.4.0 taken back to v0.3.12\nsomething new")


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
