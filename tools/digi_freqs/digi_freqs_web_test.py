#!/usr/bin/env python3
"""Tests of the web configurator's side of the keyboard modes' frequencies (#205) on a PC. See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/digi_freqs/digi_freqs_web_test.py [-v]

1. The Digital modes page lists every app type of src/cfg/digital_modes.h, and the web server takes those and no
   other (freq_lists.DIGITAL_TYPES): a type missing on the page would be saved as FT8. Runs without bottle too.
2. Through bottle's WSGI app, on a params.db made from sql/params.sql: a PSK, an Olivia and a JTTY row of the user's
   added, changed and deleted as the page does it, an unknown type refused, the same frequency twice in one app
   refused and in two apps taken.
3. /api/digital_modes/lists: what a stand-in GUI on the configurator socket answers to {"cmd": "digi_lists"}, passed
   on without its "t"; 503 when no GUI answers or it answers something else.
"""
import inspect
import io
import json
import os
import re
import shutil
import socketserver
import sqlite3
import sys
import tempfile
import threading
import types
import unittest
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + [os.path.join(TOP, "webserver", "src")]
sys.dont_write_bytecode = True
try:
    import cgi  # noqa: F401
except ImportError:     # gone in Python 3.13; bottle 0.12, the radio's, imports it, for forms only
    cgi = types.ModuleType("cgi")

    class FieldStorage:
        def __init__(self, **kw):
            raise NotImplementedError("no cgi module on this Python")
    cgi.FieldStorage = FieldStorage
    sys.modules["cgi"] = cgi
try:
    import bottle  # noqa: F401
    from x6100_webserver import apps, config_pages, settings  # noqa: E402
except ImportError:
    bottle = None
from x6100_webserver import freq_lists  # noqa: E402

HOST = "192.168.1.10"     # an address: one of the radio's own names, by which a change is taken


def header_types():
    """{name: value} of cfg_digital_type_t in src/cfg/digital_modes.h"""
    with open(os.path.join(TOP, "src", "cfg", "digital_modes.h")) as f:
        text = f.read()
    body = re.search(r"typedef enum \{(.*?)\} cfg_digital_type_t;", text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
    out, value = {}, -1
    for item in (i.strip() for i in body.split(",")):
        if not item:
            continue
        name, _, given = item.partition("=")
        value = int(given) if given.strip() else value + 1
        out[name.strip()] = value
    return out


def page_types():
    """{value: name} of the apps map of the Digital modes page"""
    with open(os.path.join(TOP, "webserver", "src", "x6100_webserver", "views", "digital_modes.html")) as f:
        text = f.read()
    body = re.search(r"const apps = \{(.*?)\};", text, re.S).group(1)
    return {int(k): v for k, v in re.findall(r'(\d+):\s*"([^"]*)"', body)}


class Types(unittest.TestCase):
    def test_page_and_server_know_every_type(self):
        enum = header_types()
        self.assertEqual(sorted(enum.values()), list(range(len(enum))), "the types are 0, 1, 2... (params.db)")
        self.assertEqual(sorted(page_types()), sorted(enum.values()), "the page's apps: every type, no other")
        self.assertEqual(list(freq_lists.DIGITAL_TYPES), sorted(enum.values()), "the web server's DIGITAL_TYPES")

    def test_keyboard_modes(self):
        enum, page = header_types(), page_types()
        self.assertEqual([page[enum[n]] for n in ("CFG_DIG_TYPE_PSK", "CFG_DIG_TYPE_OLIVIA", "CFG_DIG_TYPE_JTTY")],
                         ["PSK", "Olivia", "JTTY"])
        self.assertEqual(page[enum["CFG_DIG_TYPE_FT8"]], "FT8")
        self.assertEqual(page[enum["CFG_DIG_TYPE_FT2"]], "FT2")


class DbPlugin:
    """params.db for the routes that take dbcon, committed after the request (bottle-sqlite in __main__)"""
    name, api = "test_db", 2

    def __init__(self):
        self.path = None

    def apply(self, callback, route):
        try:
            takes = "dbcon" in inspect.signature(route.callback).parameters
        except (TypeError, ValueError):
            takes = False
        if not takes:
            return callback

        def wrapper(*a, **kw):
            con = sqlite3.connect(self.path)
            try:
                out = callback(*a, dbcon=con, **kw)
                con.commit()
                return out
            finally:
                con.close()
        return wrapper


DB = DbPlugin()
if bottle:
    apps.app.install(DB)


class Gui(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    """A stand-in for the GUI's configurator socket (src/cfg_socket.cpp): a line in, the answer of `reply` out"""
    daemon_threads = True

    def __init__(self, path, reply):
        self.reply, self.asked = reply, []
        gui = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                gui.asked.append(json.loads(self.rfile.readline()))
                self.wfile.write(gui.reply.encode() + b"\n")
        super().__init__(path, Handler)


@unittest.skipUnless(bottle, "no bottle: give BOTTLE=<a folder with bottle.py>")
class Web(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="x6100-digi-freqs-web.")
        self.addCleanup(shutil.rmtree, self.root)
        for name in ("DB_PATH", "CFG_SOCK"):
            self.addCleanup(setattr, settings, name, getattr(settings, name))
        settings.DB_PATH = DB.path = os.path.join(self.root, "params.db")
        # In Linux's abstract namespace: a path under TMPDIR is too long for a socket's name
        settings.CFG_SOCK = f"\0x6100-digi-freqs-test-{os.getpid()}"
        con = sqlite3.connect(DB.path)
        with open(os.path.join(TOP, "sql", "params.sql")) as f:
            con.executescript(f.read())
        con.commit()
        con.close()

    def req(self, method, path, body=None):
        data = b"" if body is None else json.dumps(body).encode()
        env = {"REQUEST_METHOD": method, "PATH_INFO": path, "HTTP_HOST": HOST,
               "CONTENT_TYPE": "application/json", "CONTENT_LENGTH": str(len(data)), "wsgi.input": io.BytesIO(data)}
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])

        out = b"".join(apps.app(env, start))
        try:
            return got["status"], json.loads(out)
        except ValueError:
            return got["status"], out

    def rows(self, kind):
        code, r = self.req("GET", "/api/digital_modes")
        self.assertEqual(code, 200)
        return [(f["label"], f["freq"], f["mode"]) for f in r if f["type"] == kind]

    def gui(self, reply):
        server = Gui(settings.CFG_SOCK, reply)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def test_own_rows(self):
        """A frequency of the user's for each keyboard mode, as the page sends it (MHz * 1e6, so 7.0385 arrives as
        7038499.999999999), changed and deleted; params.sql has none of theirs"""
        for kind in (6, 7, 8):
            self.assertEqual(self.rows(kind), [])
        code, _ = self.req("PUT", "/api/digital_modes", {"label": "40m US", "freq": 7.07 * 1e6, "mode": 3, "type": 6})
        self.assertEqual(code, 201)
        code, _ = self.req("PUT", "/api/digital_modes", {"label": "40m", "freq": 7.0385 * 1e6, "mode": 3, "type": 7})
        self.assertEqual(code, 201)
        code, _ = self.req("PUT", "/api/digital_modes", {"label": "", "freq": 14.09 * 1e6, "mode": 3, "type": 8})
        self.assertEqual(code, 201)
        self.assertEqual(self.rows(6), [("40m US", 7070000, 3)])
        self.assertEqual(self.rows(7), [("40m", 7038500, 3)])
        self.assertEqual(self.rows(8), [("", 14090000, 3)])
        self.assertEqual(len(self.rows(0)), 11, "FT8's list as it was")

        code, r = self.req("GET", "/api/digital_modes")
        psk = next(f for f in r if f["type"] == 6)
        code, _ = self.req("POST", f"/api/digital_modes/{psk['id']}", {"label": "40m", "freq": 7.071 * 1e6, "mode": 3,
                                                                        "type": 6})
        self.assertEqual(code, 200)
        self.assertEqual(self.rows(6), [("40m", 7071000, 3)])
        code, _ = self.req("DELETE", f"/api/digital_modes/{psk['id']}")
        self.assertEqual(code, 200)
        self.assertEqual(self.rows(6), [], "deleted: the built-in list again")

    def test_refused(self):
        for kind in (9, -1, 99):
            code, r = self.req("PUT", "/api/digital_modes", {"label": "x", "freq": 7070000, "mode": 3, "type": kind})
            self.assertEqual((code, r["msg"]), (400, "Unknown app"), kind)
        code, r = self.req("PUT", "/api/digital_modes", {"label": "x", "freq": 7070000, "mode": 3, "type": "6"})
        self.assertEqual((code, r["msg"]), (400, "Invalid type"))
        self.assertEqual(self.req("PUT", "/api/digital_modes", {"label": "a", "freq": 7070000, "mode": 3,
                                                                 "type": 6})[0], 201)
        code, r = self.req("PUT", "/api/digital_modes", {"label": "b", "freq": 7070000, "mode": 3, "type": 6})
        self.assertEqual((code, r["msg"]), (400, "This frequency is already in the list of that app"))
        self.assertEqual(self.req("PUT", "/api/digital_modes", {"label": "c", "freq": 7070000, "mode": 3,
                                                                 "type": 7})[0], 201, "another app: taken")
        code, r = self.req("GET", "/api/digital_modes")
        psk = next(f for f in r if f["type"] == 6)
        code, r = self.req("POST", f"/api/digital_modes/{psk['id']}", {"label": "a", "freq": 7070000, "mode": 3,
                                                                        "type": 9})
        self.assertEqual((code, r["msg"]), (400, "Unknown app"))
        self.assertEqual(self.rows(6), [("a", 7070000, 3)], "left as it was")

    def test_lists_from_the_gui(self):
        answer = {"t": "digi_lists", "plan": 6, "region": 2,
                  "lists": {"6": [[1838000, 0], [7070000, 0], [14072000, 1]], "7": [[7038500, 0]], "8": []}}
        gui = self.gui(json.dumps(answer))
        code, r = self.req("GET", "/api/digital_modes/lists")
        self.assertEqual(code, 200)
        self.assertEqual(r, {k: v for k, v in answer.items() if k != "t"})
        self.assertEqual(gui.asked, [{"cmd": "digi_lists"}])

    def test_lists_without_the_gui(self):
        code, r = self.req("GET", "/api/digital_modes/lists")
        self.assertEqual((code, r["msg"]), (503, "Radio GUI not reachable"))

    def test_lists_other_answer(self):
        for reply in ('{"t":"err","err":"Unknown command"}', "not json", '{"t":"disp"}'):
            server = Gui(settings.CFG_SOCK, reply)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            try:
                code, r = self.req("GET", "/api/digital_modes/lists")
                self.assertEqual((code, r["msg"]), (503, "Radio GUI not reachable"), reply)
            finally:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    unittest.main()
