#!/usr/bin/env python3
"""Tests of the web configurator's Logs page with the radio's other stores of a QSO (#284), on a PC, through bottle's
WSGI app, in a new temporary directory. An FT8, FT4 or JS8 QSO is in the day's log (or the activation's), which the
page lists, and in qso_log.db, whose calls FT8, FT4 and JS8 mark as worked, and in the app's own ADIF file
(ft_log.adi, js8call_log.adi). A QSO deleted on the page, one by one, all shown at once, or with its whole log on the
Field Ops page, goes from those too; a call, time, band or mode changed there is changed in them. A QSO in
qso_log.db only (an import, or one an older firmware left there) is counted, and forgotten on request. qso_log.db is
made with the GUI's own schema, read from src/qso_log.c, its rows as qso_log_record_save writes them. See the
Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/qso_log/logs_follow_test.py [-v]
"""
import io
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import types
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(TOP, "webserver", "src")]
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


def gui_schema():
    """The CREATE statements of src/qso_log.c's schema[], as the GUI runs them"""
    with open(os.path.join(TOP, "src", "qso_log.c")) as f:
        src = f.read()
    block = re.search(r'static const char \*schema\[\] = \{(.*?)\n\};', src, re.S).group(1)
    block = re.sub(r'/\*.*?\*/', '', block, flags=re.S)
    stmts, cur = [], ""
    for lit, comma in re.findall(r'"((?:[^"\\]|\\.)*)"|(,)', block):   # literals side by side are one string
        if comma:
            stmts, cur = stmts + [cur], ""
        else:
            cur += lit
    return stmts + ([cur] if cur else [])


# The QSOs as the radio writes them into the day's log (src/field/flog.c field_adif_record): two FT8/FT4 ones, which
# ft_log.adi has too, a JS8 one, which js8call_log.adi has, and one by the phone's logger, which neither has
FT8 = (b"<CALL:6>EA5KDD <QSO_DATE:8>20261104 <TIME_ON:6>180259 <BAND:3>40m <FREQ:8>7.074000 <MODE:3>FT8 "
       b"<RST_SENT:3>+02 <RST_RCVD:3>-06 <GRIDSQUARE:4>IN81 <STATION_CALLSIGN:6>ON6URE <OPERATOR:6>ON6URE "
       b"<MY_GRIDSQUARE:4>JO21 <eor>\n")
FT4 = (b"<CALL:6>DL1ABC <QSO_DATE:8>20261104 <TIME_ON:6>180414 <BAND:3>20m <FREQ:9>14.080000 <MODE:4>MFSK "
       b"<SUBMODE:3>FT4 <RST_SENT:3>-12 <RST_RCVD:3>-09 <GRIDSQUARE:4>JO62 <STATION_CALLSIGN:6>ON6URE "
       b"<OPERATOR:6>ON6URE <MY_GRIDSQUARE:4>JO21 <eor>\n")
JS8 = (b"<CALL:5>K1ABC <QSO_DATE:8>20261104 <TIME_ON:6>190000 <BAND:3>40m <FREQ:8>7.078000 <MODE:4>MFSK "
       b"<SUBMODE:3>JS8 <RST_SENT:3>-03 <RST_RCVD:3>+10 <STATION_CALLSIGN:6>ON6URE <OPERATOR:6>ON6URE <eor>\n")
SSB = (b"<CALL:7>F4ABC/P <QSO_DATE:8>20261104 <TIME_ON:6>200000 <BAND:3>20m <FREQ:9>14.200000 <MODE:3>SSB "
       b"<RST_SENT:2>59 <RST_RCVD:2>57 <STATION_CALLSIGN:6>ON6URE <OPERATOR:6>ON6URE <eor>\n")
HEAD = b"X6200 Field Ops log\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <PROGRAMVERSION:6>0.3.12 <eoh>\n"
FT_HEAD = b"X6200 FT8, FT4 and FT2 log\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <PROGRAMVERSION:6>0.3.12 <eoh>\n"
# js8call_log.adi as src/js8/qso_book.cpp writes it: desktop JS8Call's header and field names
JS8_FILE = (b"X6100 JS8 log, desktop JS8Call format\n<adif_ver:5>3.1.4 <programid:9>X6100_JS8 <eoh>\n"
            b"<call:5>K1ABC <mode:4>MFSK <submode:3>JS8 <rst_sent:3>-03 <rst_rcvd:3>+10 <qso_date:8>20261104 "
            b"<time_on:6>190000 <qso_date_off:8>20261104 <time_off:6>190300 <band:3>40m <freq:8>7.078000 "
            b"<station_callsign:6>ON6URE <eor>\n")

# (unix time, call, band, mode, MHz) as the apps save them; 5 FT8, 6 FT4, 8 JS8 (qso_log.h). The last one was
# imported from incoming_log.adi and is in no log on the page
ROWS = [(1793815379, "EA5KDD", 40, 5, 7.074), (1793815454, "DL1ABC", 20, 6, 14.08), (1793818800, "K1ABC", 40, 8, 7.078),
        (1700000000, "JA1ABC", 20, 5, 14.074)]


class Follow(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-logs-follow.")
        self.addCleanup(shutil.rmtree, self.root)
        for name in ("FIELD_DIR", "APP_VERSION", "QSO_LOG_DB", "FT_LOG", "JS8_LOG"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        settings.FIELD_DIR = os.path.join(self.root, "field")
        settings.APP_VERSION = os.path.join(self.root, "app-version")
        settings.QSO_LOG_DB = os.path.join(self.root, "qso_log.db")
        settings.FT_LOG = os.path.join(self.root, "ft_log.adi")
        settings.JS8_LOG = os.path.join(self.root, "js8call_log.adi")
        os.makedirs(os.path.join(settings.FIELD_DIR, "logs"))
        self.day = os.path.join(settings.FIELD_DIR, "logs", "20261104.adi")
        self.write(self.day, HEAD + FT8 + FT4 + JS8 + SSB)
        self.write(settings.FT_LOG, FT_HEAD + FT8 + FT4)
        self.write(settings.JS8_LOG, JS8_FILE)
        con = sqlite3.connect(settings.QSO_LOG_DB)
        for stmt in gui_schema():
            con.execute(stmt)
        with con:
            for t, call, band, mode, mhz in ROWS:   # src/qso_log.c qso_log_record_save
                con.execute("INSERT OR IGNORE INTO qso_log (ts, freq, band, mode, local_callsign, remote_callsign, "
                            "rsts, rstr, local_grid, remote_grid, op_name, canonized_remote_callsign) "
                            "VALUES (datetime(?, 'unixepoch'), ?, ?, ?, 'ON6URE', ?, 0, 0, 'JO21', '', '', ?)",
                            (t, mhz, band, mode, call, apps.canon_call(call)))
        con.close()

    @staticmethod
    def write(path, data):
        with open(path, "wb") as f:
            f.write(data)

    @staticmethod
    def read(path):
        with open(path, "rb") as f:
            return f.read()

    def req(self, method, path, body=b"", ctype="application/json", query=""):
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "QUERY_STRING": query, "HTTP_HOST": "192.168.1.10",
               "CONTENT_TYPE": ctype, "CONTENT_LENGTH": str(len(body)), "wsgi.input": io.BytesIO(body)}
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])

        out = b"".join(apps.app(env, start))
        return got["status"], json.loads(out or b"null")

    def worked(self, call, band, mode):
        """The FT8 list's question (src/qso_log.c qso_log_search_worked): 0 not worked, 1 worked, 2 on this band
        and mode"""
        con = sqlite3.connect(settings.QSO_LOG_DB)
        try:
            rows = con.execute("SELECT DISTINCT band, mode FROM qso_log WHERE canonized_remote_callsign LIKE ?",
                               (apps.canon_call(call),)).fetchall()
        finally:
            con.close()
        return 2 if (band, mode) in rows else 1 if rows else 0

    def logs(self):
        code, r = self.req("GET", "/api/logs")
        self.assertEqual(code, 200)
        return r

    def qso(self, call):
        return next(q for q in self.logs()["qsos"] if q["call"] == call)

    def test_delete_one(self):
        """One QSO deleted: no longer worked, gone from ft_log.adi; the others as they were"""
        q = self.qso("EA5KDD")
        self.assertEqual(self.worked("EA5KDD", 40, 5), 2)
        code, r = self.req("DELETE", f"/api/field/logs/{q['file']}/{q['n']}", query="id=" + q["id"])
        self.assertEqual((code, r), (200, {"status": "OK"}))
        self.assertEqual(self.worked("EA5KDD", 40, 5), 0)
        self.assertEqual(self.worked("DL1ABC", 20, 6), 2)
        self.assertEqual(self.read(settings.FT_LOG), FT_HEAD + FT4)
        self.assertEqual(self.read(settings.JS8_LOG), JS8_FILE)
        self.assertEqual(self.read(self.day), HEAD + FT4 + JS8 + SSB)

    def test_delete_shown(self):
        """Delete shown, every QSO of the page in one request: none worked, the apps' files without a QSO; the
        import stays, counted as in no log"""
        self.assertEqual(self.logs()["worked"], {"total": 4, "extra": 1})
        sel = [{"file": q["file"], "n": q["n"], "id": q["id"]} for q in self.logs()["qsos"]]
        code, r = self.req("POST", "/api/logs/delete", json.dumps({"qsos": sel}).encode())
        self.assertEqual((code, r), (200, {"status": "OK", "deleted": 4, "stale": 0}))
        for call, band, mode in (("EA5KDD", 40, 5), ("DL1ABC", 20, 6), ("K1ABC", 40, 8)):
            self.assertEqual(self.worked(call, band, mode), 0, call)
        self.assertEqual(self.worked("JA1ABC", 20, 5), 2)
        self.assertEqual(self.read(settings.FT_LOG), FT_HEAD)
        self.assertEqual(self.read(settings.JS8_LOG), JS8_FILE[:JS8_FILE.index(b"<eoh>\n") + 6])
        self.assertEqual(self.read(self.day), HEAD)
        r = self.logs()
        self.assertEqual((r["qsos"], r["worked"]), ([], {"total": 1, "extra": 1}))

    def test_stale(self):
        """A QSO changed meanwhile (another tab): counted, the rest deleted"""
        sel = [{"file": q["file"], "n": q["n"], "id": q["id"]} for q in self.logs()["qsos"]]
        sel[0]["id"] = "0" * 12
        code, r = self.req("POST", "/api/logs/delete", json.dumps({"qsos": sel}).encode())
        self.assertEqual((code, r["deleted"], r["stale"]), (200, 3, 1))

    def test_forget(self):
        """Forget: the QSOs in no log out of qso_log.db, the page's QSOs kept"""
        code, r = self.req("POST", "/api/logs/worked/forget")
        self.assertEqual((code, r), (200, {"status": "OK", "forgot": 1}))
        self.assertEqual(self.worked("JA1ABC", 20, 5), 0)
        self.assertEqual(self.worked("EA5KDD", 40, 5), 2)
        self.assertEqual(self.logs()["worked"], {"total": 3, "extra": 0})

    def test_edit(self):
        """A call and a time set on the page: qso_log.db and ft_log.adi follow; a name alone leaves qso_log.db"""
        q = self.qso("EA5KDD")
        code, r = self.req("PUT", f"/api/logs/{q['file']}/{q['n']}",
                           json.dumps({"call": "ea5kde", "time_on": "1803", "name": "Pepe"}).encode(),
                           query="id=" + q["id"])
        self.assertEqual((code, r), (200, {"status": "OK"}))
        self.assertEqual(self.worked("EA5KDD", 40, 5), 0)
        self.assertEqual(self.worked("EA5KDE", 40, 5), 2)
        con = sqlite3.connect(settings.QSO_LOG_DB)
        self.assertEqual(con.execute("SELECT ts, remote_callsign, freq FROM qso_log WHERE band = 40 AND mode = 5")
                         .fetchall(), [("2026-11-04 18:03:00", "EA5KDE", 7.074)])
        con.close()
        want = FT8.replace(b"<CALL:6>EA5KDD", b"<CALL:6>EA5KDE").replace(b"<TIME_ON:6>180259", b"<TIME_ON:4>1803")
        want = want.replace(b"<MY_GRIDSQUARE:4>JO21", b"<MY_GRIDSQUARE:4>JO21 <NAME:4>Pepe")
        self.assertEqual(self.read(settings.FT_LOG), FT_HEAD + want + FT4)
        # deleted after the edit: found by its minute
        q = self.qso("EA5KDE")
        self.assertEqual(self.req("DELETE", f"/api/field/logs/{q['file']}/{q['n']}", query="id=" + q["id"])[0], 200)
        self.assertEqual(self.worked("EA5KDE", 40, 5), 0)
        self.assertEqual(self.read(settings.FT_LOG), FT_HEAD + FT4)

    def test_edit_band_mode(self):
        """Band and mode set on the page: the worked mark goes to that band and mode"""
        q = self.qso("DL1ABC")
        code, _ = self.req("PUT", f"/api/logs/{q['file']}/{q['n']}",
                           json.dumps({"band": "17M", "freq": "18.104000", "mode": "FT8"}).encode(),
                           query="id=" + q["id"])
        self.assertEqual(code, 200)
        self.assertEqual(self.worked("DL1ABC", 20, 6), 1)
        self.assertEqual(self.worked("DL1ABC", 17, 5), 2)

    def test_js8_edit(self):
        """A JS8 QSO's call set on the page: its record in js8call_log.adi too, the rest of it as JS8 wrote it"""
        q = self.qso("K1ABC")
        code, _ = self.req("PUT", f"/api/logs/{q['file']}/{q['n']}", json.dumps({"call": "K1ABD"}).encode(),
                           query="id=" + q["id"])
        self.assertEqual(code, 200)
        self.assertEqual(self.read(settings.JS8_LOG), JS8_FILE.replace(b"<call:5>K1ABC", b"<CALL:5>K1ABD"))
        self.assertEqual(self.worked("K1ABD", 40, 8), 2)

    def test_phone_qso(self):
        """A QSO the phone logged is in neither: deleting it leaves both alone"""
        q = self.qso("F4ABC/P")
        ft, js8 = self.read(settings.FT_LOG), self.read(settings.JS8_LOG)
        self.assertEqual(self.req("DELETE", f"/api/field/logs/{q['file']}/{q['n']}", query="id=" + q["id"])[0], 200)
        self.assertEqual((self.read(settings.FT_LOG), self.read(settings.JS8_LOG)), (ft, js8))
        self.assertEqual(self.logs()["worked"], {"total": 4, "extra": 1})

    def test_delete_log(self):
        """A whole log deleted on the Field Ops page: its QSOs no longer worked, ft_log.adi's other QSOs kept"""
        other = (b"<CALL:6>G4ABCD <QSO_DATE:8>20261105 <TIME_ON:6>090000 <BAND:3>40m <FREQ:8>7.074000 <MODE:3>FT8 "
                 b"<eor>\n")
        self.write(settings.FT_LOG, FT_HEAD + FT8 + FT4 + other)
        code, r = self.req("DELETE", "/api/field/logs/20261104.adi")
        self.assertEqual(code, 200)
        self.assertNotIn("note", r)
        self.assertEqual(self.worked("EA5KDD", 40, 5), 0)
        self.assertEqual(self.worked("K1ABC", 40, 8), 0)
        self.assertEqual(self.read(settings.FT_LOG), FT_HEAD + other)

    def test_no_db(self):
        """No qso_log.db (a PC, a new card): the delete works, and none is made"""
        os.remove(settings.QSO_LOG_DB)
        q = self.qso("EA5KDD")
        self.assertEqual(self.req("DELETE", f"/api/field/logs/{q['file']}/{q['n']}", query="id=" + q["id"]),
                         (200, {"status": "OK"}))
        self.assertFalse(os.path.exists(settings.QSO_LOG_DB))
        self.assertEqual(self.logs()["worked"], {"total": 0, "extra": 0})

    def test_db_locked(self):
        """qso_log.db held by another writer past the wait: the log is deleted all the same, the page told"""
        q = self.qso("EA5KDD")
        con = sqlite3.connect(settings.QSO_LOG_DB, isolation_level=None)
        con.execute("BEGIN EXCLUSIVE")
        timeout = apps.sqlite3.connect
        apps.sqlite3.connect = lambda *a, **k: timeout(*a, **dict(k, timeout=0.1))
        try:
            code, r = self.req("DELETE", f"/api/field/logs/{q['file']}/{q['n']}", query="id=" + q["id"])
        finally:
            apps.sqlite3.connect = timeout
            con.execute("ROLLBACK")
            con.close()
        self.assertEqual(code, 200)
        self.assertIn("qso_log.db", r.get("note", ""))
        self.assertEqual(self.read(settings.FT_LOG), FT_HEAD + FT4)

    def test_canon(self):
        """The base call of a compound one, as src/util.cpp util_canonize_callsign gives it (qso_log_test.c)"""
        for call, want in (("PA/ON6URE/P", "ON6URE"), ("G4ABC/P", "G4ABC"), ("EA5KDD", "EA5KDD"), ("K1A", "K1A"),
                           ("VP2E/K1ABC", "VP2E")):
            self.assertEqual(apps.canon_call(call), want, call)


if __name__ == "__main__":
    unittest.main()
