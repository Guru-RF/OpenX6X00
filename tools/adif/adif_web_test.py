#!/usr/bin/env python3
"""Tests of the web configurator's ADIF export (#174: Logs > Export ADIF, /logs/export) on a PC, through bottle's
WSGI app, the logs in a new temporary directory. The header is the one the radio's own logs have
(src/field/flog.c field_adif_header): ADIF 3.1.8, PROGRAMID OpenX6X00 and the firmware's version; the records go
out as the day's log has them, the one for #174's QSO as ft_log.adi has it too (adif_test.c). A band edited on the
Logs page goes in as the radio writes it, 20m. See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/adif/adif_web_test.py [-v]
"""
import io
import json
import os
import shutil
import sys
import tempfile
import types
import unittest
import urllib.parse
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(HERE, "..", "..", "webserver", "src")]
sys.dont_write_bytecode = True
try:
    import cgi  # noqa: F401
except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for multipart forms only
    cgi = types.ModuleType("cgi")

    class FieldStorage:
        def __init__(self, **kw):
            raise NotImplementedError("no cgi module on this Python")
    cgi.FieldStorage = FieldStorage
    sys.modules["cgi"] = cgi
try:
    import bottle  # noqa: F401
except ImportError:
    sys.exit("bottle not found: give BOTTLE=<a folder with bottle.py>, to make or in the environment")
from x6100_webserver import apps, settings  # noqa: E402

# #174's QSO as the radio writes it into the day's log (src/field/flog.c field_adif_record)
REC = (b"<CALL:6>EA5KDD <QSO_DATE:8>20261104 <TIME_ON:6>180259 <BAND:3>40m <FREQ:8>7.074000 <MODE:3>FT8 "
       b"<RST_SENT:3>+02 <RST_RCVD:3>-06 <GRIDSQUARE:4>IN81 <STATION_CALLSIGN:6>EA5JTT <OPERATOR:6>EA5JTT "
       b"<MY_GRIDSQUARE:6>IM99TL <eor>\n")
DAY = b"X6200 Field Ops log\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <PROGRAMVERSION:5>0.3.8 <eoh>\n" + REC


class Export(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-adif-web.")
        self.addCleanup(shutil.rmtree, self.root)
        for name in ("FIELD_DIR", "APP_VERSION"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        settings.FIELD_DIR = os.path.join(self.root, "field")
        settings.APP_VERSION = os.path.join(self.root, "app-version")
        os.makedirs(os.path.join(settings.FIELD_DIR, "logs"))
        self.log = os.path.join(settings.FIELD_DIR, "logs", "20261104.adi")
        with open(self.log, "wb") as f:
            f.write(DAY)
        with open(settings.APP_VERSION, "w") as f:
            f.write("0.3.8\n")

    def req(self, method, path, body=b"", ctype="application/x-www-form-urlencoded", query=""):
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "QUERY_STRING": query, "HTTP_HOST": "192.168.1.10",
               "CONTENT_TYPE": ctype, "CONTENT_LENGTH": str(len(body)), "wsgi.input": io.BytesIO(body)}
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])

        out = b"".join(apps.app(env, start))
        return got["status"], out

    def export(self, fmt="adi"):
        rid = apps.rec_id(apps.log_records("20261104.adi")[0])
        return self.req("POST", "/logs/export", urllib.parse.urlencode({"sel": "20261104.adi:" + rid,
                                                                         "fmt": fmt}).encode())

    def test_export(self):
        code, out = self.export()
        self.assertEqual(code, 200)
        self.assertEqual(out, b"OpenX6X00 log export\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 "
                              b"<PROGRAMVERSION:5>0.3.8 <eoh>\n" + REC)

    def test_export_without_version(self):
        """A PC, or a version file that can't be read: the header without PROGRAMVERSION"""
        os.remove(settings.APP_VERSION)
        code, out = self.export()
        self.assertEqual(code, 200)
        self.assertEqual(out, b"OpenX6X00 log export\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <eoh>\n" + REC)

    def put(self, data):
        rid = apps.rec_id(apps.log_records("20261104.adi")[0])
        return self.req("PUT", "/api/logs/20261104.adi/0", json.dumps(data).encode(), "application/json", "id=" + rid)

    def test_edit_band(self):
        """A band set on the Logs page goes in as the radio writes it, 20m; the page sends each column back, in
        capitals: a band not changed leaves the record as it was"""
        with open(self.log, "rb") as f:
            before = f.read()
        code, _ = self.put({"call": "EA5KDD", "band": "40M", "freq": "7.074000", "time_on": "1802"})
        self.assertEqual(code, 200)
        with open(self.log, "rb") as f:
            self.assertEqual(f.read(), before)
        code, _ = self.put({"band": "20M", "freq": "14.074000"})
        self.assertEqual(code, 200)
        with open(self.log, "rb") as f:
            self.assertEqual(f.read(), before.replace(b"<BAND:3>40m <FREQ:8>7.074000",
                                                      b"<BAND:3>20m <FREQ:9>14.074000"))

    def test_csv(self):
        code, out = self.export("csv")
        self.assertEqual(code, 200)
        self.assertIn(b"20261104,180259,EA5KDD,40m,7.074000,FT8,,+02,-06", out)


if __name__ == "__main__":
    unittest.main()
