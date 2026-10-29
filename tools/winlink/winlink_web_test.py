#!/usr/bin/env python3
"""Tests of the web configurator's Winlink page and its API (webserver/, /winlink and /api/winlink) on a PC,
through bottle's WSGI app, the settings files in a new temporary directory and the helper (x6100-winlink) stood
in by a list of its calls. Needs bottle (0.13.4 here):

    PYTHONPATH=<a directory with bottle.py> python3 -B tools/winlink/winlink_web_test.py [-v]
"""
import io
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "webserver", "src"))
sys.dont_write_bytecode = True
try:
    import bottle  # noqa: F401
except ImportError:
    sys.exit("bottle is not installed: PYTHONPATH=<a directory with bottle.py> python3 -B " + sys.argv[0])
from x6100_webserver import apps, settings  # noqa: E402


class Web(unittest.TestCase):
    """The page and its API on a radio of their own: params.db with a callsign and QTH, Winlink off"""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-winlink-web.")
        self.addCleanup(shutil.rmtree, self.root)
        db = os.path.join(self.root, "params.db")
        con = sqlite3.connect(db)
        con.execute("CREATE TABLE params (name TEXT PRIMARY KEY ON CONFLICT REPLACE, val INTEGER)")
        con.executemany("INSERT INTO params VALUES (?, ?)", [("callsign", "ON6URE/P"), ("qth", "JO20ab")])
        con.commit()
        con.close()
        self.calls = []
        for k in ("WINLINK_CONF", "WINLINK_STATUS", "WINLINK_PASSWORD", "WINLINK_RADIO", "WINLINK_PROGRAMS",
                  "winlink_run"):
            self.addCleanup(setattr, apps, k, getattr(apps, k))
        self.addCleanup(setattr, settings, "DB_PATH", settings.DB_PATH)
        settings.DB_PATH = db
        apps.WINLINK_CONF = os.path.join(self.root, "winlink.conf")
        apps.WINLINK_STATUS = os.path.join(self.root, "status")
        apps.WINLINK_PASSWORD = os.path.join(self.root, "password")
        apps.WINLINK_RADIO = os.path.join(self.root, "radio")
        apps.WINLINK_PROGRAMS = ()
        apps.winlink_run = lambda args, stdin=None: self.calls.append(args)

    def req(self, method, path, body=None):
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "HTTP_HOST": "192.168.1.10"}
        if body is not None:
            raw = json.dumps(body).encode()
            env.update(CONTENT_TYPE="application/json", CONTENT_LENGTH=str(len(raw)), **{"wsgi.input": io.BytesIO(raw)})
        setup_testing_defaults(env)
        got = {}

        def start(status, headers, exc=None):
            got["status"] = int(status.split()[0])

        out = b"".join(apps.app(env, start)).decode()
        try:
            return got["status"], json.loads(out)
        except ValueError:
            return got["status"], out

    def conf(self):
        with open(apps.WINLINK_CONF) as f:
            return dict(line.rstrip("\n").split("=", 1) for line in f)


class Modem(Web):
    def test_defaults(self):
        code, st = self.req("GET", "/api/winlink")
        self.assertEqual(code, 200)
        self.assertEqual((st["modem_choice"], st["mercury_bandwidth"], st["bandwidth"]), ("ardop", 2300, 500))
        self.assertEqual((st["ardop"], st["mercury"]), ("off", "off"))
        with open(apps.WINLINK_CONF, "w") as f:
            f.write("enabled=0\nmodem=vara\nmercury_bandwidth=2000\n")
        code, st = self.req("GET", "/api/winlink")
        self.assertEqual((st["modem_choice"], st["mercury_bandwidth"]), ("ardop", 2300))

    def test_status_kept_apart_from_the_choice(self):
        """The supervisor's modem= is how the modem that runs is doing; the choice is modem_choice"""
        with open(apps.WINLINK_STATUS, "w") as f:
            f.write("state=running\ntext=Running as ON6URE\npat=running\nardop=off\nmercury=running\nmodem=behind\n")
        with open(apps.WINLINK_CONF, "w") as f:
            f.write("enabled=1\nmodem=mercury\nmercury_bandwidth=500\n")
        st = self.req("GET", "/api/winlink")[1]
        self.assertEqual((st["modem"], st["modem_choice"], st["mercury"], st["mercury_bandwidth"]),
                         ("behind", "mercury", "running", 500))

    def test_chosen_while_off(self):
        code, st = self.req("POST", "/api/winlink", {"modem_choice": "mercury", "mercury_bandwidth": 2750})
        self.assertEqual(code, 200, st)
        self.assertEqual((st["modem_choice"], st["mercury_bandwidth"]), ("mercury", 2750))
        self.assertEqual(self.conf(), {"enabled": "0", "mycall": "", "locator": "", "bandwidth": "500",
                                       "modem": "mercury", "mercury_bandwidth": "2750"})
        self.assertEqual(self.calls, [])                # saved only: nothing runs

    def test_chosen_while_on(self):
        """Pat restarts with it (apply), and the supervisor changes the modem"""
        with open(apps.WINLINK_CONF, "w") as f:
            f.write("enabled=1\nmycall=\nlocator=\nbandwidth=500\n")
        code, st = self.req("POST", "/api/winlink", {"modem_choice": "mercury"})
        self.assertEqual(code, 200, st)
        self.assertEqual(self.calls, [["apply"], ["on"]])
        self.assertEqual(self.conf()["modem"], "mercury")
        self.assertEqual(self.conf()["mercury_bandwidth"], "2300")
        self.calls.clear()
        self.req("POST", "/api/winlink", {"modem_choice": "mercury", "mercury_bandwidth": 2300})
        self.assertEqual(self.calls, [["on"]])          # nothing changed: no restart
        self.req("POST", "/api/winlink", {"mercury_bandwidth": 500})
        self.assertEqual(self.calls, [["on"], ["apply"], ["on"]])
        self.assertEqual(self.conf()["mercury_bandwidth"], "500")

    def test_refused(self):
        for body, msg in (({"modem_choice": "vara"}, "Modem: ARDOP or Mercury"),
                          ({"modem_choice": "Mercury"}, "Modem: ARDOP or Mercury"),
                          ({"modem_choice": 1}, "Modem: ARDOP or Mercury"),
                          ({"modem_choice": None}, "Modem: ARDOP or Mercury"),
                          ({"mercury_bandwidth": 2000}, "Mercury bandwidth: 500, 2300 or 2750"),
                          ({"mercury_bandwidth": "2300"}, "Mercury bandwidth: 500, 2300 or 2750"),
                          ({"mercury_bandwidth": True}, "Mercury bandwidth: 500, 2300 or 2750"),
                          ({"mercury_bandwidth": 2300.5}, "Mercury bandwidth: 500, 2300 or 2750")):
            code, r = self.req("POST", "/api/winlink", body)
            self.assertEqual((code, r.get("msg")), (400, msg), body)
        self.assertFalse(os.path.exists(apps.WINLINK_CONF))
        self.assertEqual(self.calls, [])

    def test_the_page(self):
        code, page = self.req("GET", "/winlink")
        self.assertEqual(code, 200)
        self.assertIn('<select id="modem"><option value="ardop">ARDOP</option>'
                      '<option value="mercury">Mercury (experimental)</option></select>', page)
        self.assertIn('<select id="mercury_bandwidth"><option value="500">500 Hz</option>'
                      '<option value="2300">2300 Hz</option><option value="2750">2750 Hz</option></select>', page)
        self.assertIn('id="row-mercury-bandwidth"', page)
        self.assertIn("modem_choice: $(\"#modem\").val()", page)


class Radio(Web):
    def test_the_cap(self):
        """The GUI's radio file into the API as numbers: Winlink's cap, and whether it is the X6200's battery's"""
        with open(apps.WINLINK_RADIO, "w") as f:
            f.write("modem=connected\nrx=14105000\nmode=USB-D\npwr=5.0\ncap=5.0\ncap_battery=1\napp=\n")
        r = self.req("GET", "/api/winlink")[1]["radio"]
        self.assertEqual((r["pwr"], r["cap"], r["cap_battery"], r["app"]), (5.0, 5.0, 1, ""))
        with open(apps.WINLINK_RADIO, "w") as f:
            f.write("modem=connected\npwr=10.0\ncap=7.0\n")      # a GUI from before cap_battery
        r = self.req("GET", "/api/winlink")[1]["radio"]
        self.assertEqual((r["pwr"], r["cap"], "cap_battery" in r), (10.0, 7.0, False))


class Page(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "needs Node")
    def test_the_page_script(self):
        """The page's own script on the radio file's values, under Node (winlink_page_test.js)"""
        r = subprocess.run(["node", os.path.join(HERE, "winlink_page_test.js")], capture_output=True, text=True,
                           timeout=60)
        self.assertEqual((r.returncode, r.stdout.strip().splitlines()[-1:]), (0, ["PAGE OK"]), r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
