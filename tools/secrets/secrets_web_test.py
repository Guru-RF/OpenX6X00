#!/usr/bin/env python3
"""The web configurator never sends a stored secret to the browser, on a PC: the web server's own code through
bottle's WSGI app, every path of the radio it uses moved into a new temporary folder first. See the Makefile:

    BOTTLE=<a directory with bottle.py> python3 -I -B tools/secrets/secrets_web_test.py [-v]

The secrets stored: the Wavelog key, three QRZ Logbook keys, the HOTA and WWFF keys, the Winlink page's password and
one typed in Pat's own Settings, two WiFi passwords. On the DATA partition the files that hold them: field/*.conf,
backups/ (backup.tgz, bluetooth.tgz), an uploaded backup waiting in backup_work, the WiFi networks a restore replaced
in backup_before_restore/wifi, and a second name for the Wavelog key's file (a hard link here; on the radio's FAT,
WAVELOG.CONF or the short name WAVELO~1.CON).

1. Every page and every GET answer (each route of the app, with its parameters filled in), none holding a secret.
   A GET route the test does not know fails it: add it to ROUTES.
2. The Files page, followed link by link from /files/: no secret in any page or file, the files that hold them
   listed without a link and refused when asked for by name, the others still downloaded.
3. The answers after a Save with the secret fields empty, and Clear (Wavelog, QRZ, HOTA, WWFF, Winlink).
4. The answers that pass on what a helper or a server said, with stand-in helpers that say the secrets: Wavelog's
   Test, Load and Sync (with the stored key, and with one typed and not saved), the reference list update, Winlink's
   errors, its status and its logs download.
5. The Backup download, which is the owner's own file: without its WiFi part no secret; with it the WiFi passwords,
   as before (said, not changed).

WEBSERVER=<a webserver/src> tests another web server (make v0310: v0.3.10's, which has to fail).
"""
import gzip
import http.server
import inspect
import io
import json
import os
import re
import shutil
import sqlite3
import sys
import tarfile
import tempfile
import threading
import types
import unittest
import urllib.parse
from unittest import mock
from wsgiref.util import setup_testing_defaults

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, "..", "..")
sys.path[:0] = [p for p in (os.environ.get("BOTTLE"),) if p] + \
    [os.environ.get("WEBSERVER") or os.path.join(TOP, "webserver", "src")]
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
except ImportError:
    sys.exit("bottle not found: give BOTTLE=<a folder with bottle.py>, to make or in the environment")
from x6100_webserver import apps, config_pages, settings  # noqa: E402

WAVELOG_KEY = "wl4f1c2e9a7b3d5"
QRZ_KEYS = ("QRZ1-AAAA-1111-AAAA", "QRZ2-BBBB-2222-BBBB", "QRZ3-CCCC-3333-CCCC")
HOTA_KEY = "hota-5e1f-44aa-9c0d"
WWFF_KEY = "wwff-7a2b-11cc-8e4f"
WINLINK_PW = "Wl1nk-page-pw"
PAT_PW = "Pat-own-settings-pw"
WIFI_PSKS = ("home-wifi-psk-1234", "club-wifi-psk-5678")
BT_KEY = "B1E2C3D4E5F60718293A4B5C6D7E8F90"      # a Bluetooth link key in bluetooth.tgz
TYPED_KEY = "wlTYPEDnotsaved1"                   # typed on the Wavelog page, not saved
SECRETS = (WAVELOG_KEY, *QRZ_KEYS, HOTA_KEY, WWFF_KEY, WINLINK_PW, PAT_PW, *WIFI_PSKS, BT_KEY, TYPED_KEY)
HOST = "192.168.1.10"     # an address: one of the radio's own names, by which every page and answer is given

# The GET routes with parameters, filled in; None: asked for elsewhere (the Files page, the Backup download)
ROUTES = {
    "/<view:re:bands|digital_modes|time>": ["/bands", "/digital_modes", "/time"],
    "/static/<filepath:path>": ["/static/css/base.css"],
    "/files/": None, "/files/<filepath:path>": None, "/files/<filepath:path>/": None,
    "/field/logs/<name>": ["/field/logs/2026-11-13-ONFF-0001.adi"],
    "/textlog/<name>": ["/textlog/2026-11-13.txt"],
    "/api/macros/<mode>": ["/api/macros/psk", "/api/macros/olivia", "/api/macros/jtty"],
    "/gallery/file/<kind>/<name>": ["/gallery/file/shots/shot.png"],
    "/gallery/thumb/<kind>/<name>": ["/gallery/thumb/shots/shot.png"],
    "/api/backup/download": None,
}
KEEP = {"PACKAGE_DIR", "STATIC_PATH", "TOP"}     # paths of the web server's own files, not the radio's


class DbPlugin:
    """params.db for the routes that take dbcon (bottle-sqlite in __main__)"""
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
apps.app.install(DB)


def write(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data.encode() if isinstance(data, str) else data)
    os.chmod(path, mode)


def profile(name, psk):
    return (f"[connection]\nid={name}\nuuid=6a1d3c1e-0b7e-4b55-9d0e-2f3a4b5c6d0{len(name) % 10}\ntype=wifi\n\n"
            f"[wifi]\nmode=infrastructure\nssid={name}Net\n\n[wifi-security]\nkey-mgmt=wpa-psk\npsk={psk}\n\n"
            "[ipv4]\nmethod=auto\n")


def tgz(members):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for name, data in members.items():
            data = data.encode()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def stub(path, body):
    """A helper stood in for by a shell script"""
    write(path, "#!/bin/sh\n" + body, 0o755)


def unpacked(data):
    """The bytes of an answer, and those of a gzip or tar(.gz) in it: a secret could be inside"""
    out = [data]
    if data[:2] == b"\x1f\x8b":
        try:
            out.append(gzip.decompress(data))
        except (OSError, EOFError):
            pass
    for d in list(out):
        try:
            with tarfile.open(fileobj=io.BytesIO(d)) as tf:
                out += [tf.extractfile(m).read() for m in tf.getmembers() if m.isfile()]
        except (tarfile.TarError, EOFError, OSError):
            pass
    return b"\n".join(out)


class Secrets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="x6100-secrets.")
        cls.saved = []
        # Every path of the radio, into the test's folder: settings', apps' and config_pages' own
        for mod in (settings, apps, config_pages):
            for name, val in list(vars(mod).items()):
                if name.isupper() and name not in KEEP and isinstance(val, str) and val.startswith("/"):
                    cls.saved.append((mod, name, val))
                    setattr(mod, name, cls.root + val)
                elif name == "MACRO_FILES" and isinstance(val, dict):
                    cls.saved.append((mod, name, val))
                    setattr(mod, name, {k: cls.root + v for k, v in val.items()})
        r = cls.root
        settings.FILEBROWSER_PATH = r + "/mnt"
        settings.DB_PATH = DB.path = r + "/mnt/params.db"
        settings.WAVELOG_CMD = r + "/bin/x6100-wavelog"
        settings.FIELD_CMD = r + "/bin/x6100-field"
        os.makedirs(r + "/mnt")
        con = sqlite3.connect(DB.path)
        with open(os.path.join(TOP, "sql", "params.sql")) as f:
            con.executescript(f.read())
        con.execute("INSERT OR REPLACE INTO params (name, val) VALUES ('callsign', 'ON6URE')")
        con.commit()
        con.close()

        f = settings.FIELD_DIR
        write(f + "/wavelog.conf", f"enabled=0\nurl=https://log.example.org\nkey={WAVELOG_KEY}\nstation_id=1\n", 0o600)
        write(f + "/wavelog.state", json.dumps({"last": {"result": "ok 2 QSOs sent", "time": 1762000000}}))
        write(f + "/qrz.conf", "active=1\n" + "".join(f"call{i + 1}=ON6URE\nkey{i + 1}={k}\n"
                                                       for i, k in enumerate(QRZ_KEYS)), 0o600)
        write(f + "/hota.conf", f"key={HOTA_KEY}\n", 0o600)
        write(f + "/wwff.conf", f"key={WWFF_KEY}\n", 0o600)
        write(f + "/logs/2026-11-13-ONFF-0001.adi", "<call:6>ON4ABC <eor>\n")
        os.link(f + "/wavelog.conf", f + "/copy-of-wavelog.conf")      # another name for the key's file
        write(apps.WINLINK_PASSWORD, WINLINK_PW + "\n", 0o600)
        pat_config = getattr(apps, "WINLINK_PAT_CONFIG", r + "/var/lib/winlink/pat/config.json")   # (v0.3.10: none)
        write(pat_config, json.dumps({"mycall": "ON6URE", "secure_login_password": PAT_PW}), 0o600)
        write(apps.WINLINK_STATUS, f"state=error\ntext=Pat stopped: login {PAT_PW} refused\npat=off\nardop=off\n"
                                   "mercury=off\nxara=off\npassword=page\n")
        write(settings.WIFI_DIR + "/Home.nmconnection", profile("Home", WIFI_PSKS[0]), 0o600)
        write(settings.WIFI_DIR + "/Club.nmconnection", profile("Club", WIFI_PSKS[1]), 0o600)
        m = settings.FILEBROWSER_PATH
        write(m + "/backups/backup.tgz", tgz({"etc/NetworkManager/system-connections/Home.nmconnection":
                                              profile("Home", WIFI_PSKS[0])}))
        write(m + "/backups/bluetooth.tgz", tgz({"var/lib/bluetooth/00:11:22:33:44:55/66:77:88:99:AA:BB/info":
                                                 f"[LinkKey]\nKey={BT_KEY}\n"}))
        write(settings.BACKUP_WORK + "/stage/wifi/Home.nmconnection", profile("Home", WIFI_PSKS[0]))
        write(settings.BACKUP_SAFETY + "/wifi/Club.nmconnection", profile("Club", WIFI_PSKS[1]))
        write(settings.BACKUP_SAFETY + "/channels.json", '{"version": 1, "channels": []}')
        write(m + "/ft_log.adi", "<call:6>ON4ABC <eor>\n")
        write(m + "/shot.png", b"\x89PNG\r\n\x1a\n")
        write(m + "/digi_text/2026-11-13.txt", "12:00 RX CQ ON4ABC\n")
        settings.TEXTLOG_DIR = m + "/digi_text"

        # The helpers, saying the secrets they were given or that they read: the web server is to hide them
        b = r + "/bin"
        cls.calls = b + "/calls"
        stub(settings.WAVELOG_CMD, f'echo "error HTTP 404 /index.php/api/station_info/${{X6100_WAVELOG_KEY:-'
                                   f'{WAVELOG_KEY}}} not found"\n')
        stub(settings.FIELD_CMD, f'echo "error the list server refused {HOTA_KEY} and {WWFF_KEY}"\n')
        stub(apps.WINLINK_CMD, f'echo "$*" >> {cls.calls}\n'
                               f'case "$1" in\n'
                               f'  logs) echo "Pat: login {WINLINK_PW} / {PAT_PW}; wifi {WIFI_PSKS[0]}" ;;\n'
                               f'  set-password) cat > /dev/null ;;\n'
                               f'  on) echo "error login {WINLINK_PW} refused" >&2; exit 1 ;;\n'
                               f'esac\n')

    @classmethod
    def tearDownClass(cls):
        for mod, name, val in reversed(cls.saved):
            setattr(mod, name, val)
        shutil.rmtree(cls.root, ignore_errors=True)

    def req(self, method, path, body=None):
        """(status, answer bytes) of a request by one of the radio's own names"""
        path, _, query = path.partition("?")
        data = b"" if body is None else json.dumps(body).encode()
        env = {"REQUEST_METHOD": method, "PATH_INFO": urllib.parse.unquote(path), "QUERY_STRING": query,
               "HTTP_HOST": HOST, "CONTENT_TYPE": "application/json", "CONTENT_LENGTH": str(len(data)),
               "wsgi.input": io.BytesIO(data)}
        setup_testing_defaults(env)
        got = {}

        def start(status, hdrs, exc=None):
            got["status"] = int(status.split()[0])

        out = apps.app(env, start)
        try:
            data = b"".join(out)
        finally:
            if hasattr(out, "close"):
                out.close()
        return got["status"], data

    def assertNoSecret(self, what, data):
        found = [s for s in SECRETS if s.encode() in unpacked(data)]
        self.assertEqual(found, [], f"{what} holds {found}")

    def get(self, path, status=None):
        st, data = self.req("GET", path)
        if status is not None:
            self.assertEqual(st, status, f"GET {path}: {data[:300]!r}")
        self.assertNoSecret(f"GET {path} ({st})", data)
        return st, data

    def test_every_page_and_answer(self):
        seen = 0
        for route in apps.app.routes:
            if route.method != "GET":
                continue
            if "<" in route.rule or route.rule in ROUTES:
                self.assertIn(route.rule, ROUTES, "a GET route the test does not know: add it to ROUTES")
                paths = ROUTES[route.rule] or []
            else:
                paths = [route.rule]
            for path in paths:
                with self.subTest(path=path):
                    self.get(path)
                    seen += 1
        self.assertGreater(seen, 50)
        for path in ("/wavelog", "/field", "/winlink", "/backup", "/cluster", "/tailscale", "/ssh", "/api/wavelog",
                     "/api/field", "/api/winlink", "/api/backup", "/api/cluster", "/api/tailscale"):
            self.get(path, 200)

    def test_says_only_whether_one_is_set(self):
        st, data = self.get("/api/wavelog", 200)
        self.assertIs(json.loads(data)["key"], True)
        field = json.loads(self.get("/api/field", 200)[1])
        self.assertEqual([p["key"] for p in field["qrz"]["profiles"]], [True, True, True])
        self.assertEqual((field["hota_key"], field["wwff_key"]), (True, True))
        wl = json.loads(self.get("/api/winlink", 200)[1])
        self.assertEqual((wl["password"], wl["password_set"]), ("page", True))
        self.assertEqual(wl["text"], "Pat stopped: login (hidden) refused")

    def test_files_page(self):
        todo, seen, files = ["/files/"], set(), set()
        while todo:
            path = todo.pop()
            if path in seen:
                continue
            seen.add(path)
            st, data = self.get(path, 200)
            if path.endswith("/"):      # the list's links, not the menu's
                block = data.split(b'class="dp-body tbl files">', 1)[1].split(b"</div>", 1)[0]
                for href in re.findall(rb'<a href="([^"]+)"', block):
                    href = href.decode()
                    if href != "..":
                        todo.append(path + href)
            else:
                files.add(path)
        for path in ("/files/ft_log.adi", "/files/shot.png", "/files/backup_before_restore/channels.json",
                     "/files/field/logs/2026-11-13-ONFF-0001.adi"):
            self.assertIn(path, files)       # the others are still downloaded
        for path in ("/files/field/wavelog.conf", "/files/field/qrz.conf", "/files/field/hota.conf",
                     "/files/field/wwff.conf", "/files/field/copy-of-wavelog.conf", "/files/backups/backup.tgz",
                     "/files/backups/bluetooth.tgz", "/files/backup_work/stage/wifi/Home.nmconnection",
                     "/files/backup_before_restore/wifi/Club.nmconnection"):
            self.assertNotIn(path, files)
            self.get(path, 403)              # asked for by name
        st, data = self.get("/files/field/", 200)
        self.assertIn(b"wavelog.conf", data)     # listed, without a link
        self.assertNotIn(b'href="wavelog.conf"', data)

    def test_save_with_the_fields_empty_and_clear(self):
        st, data = self.req("POST", "/api/wavelog", {"url": "https://log.example.org", "key": "", "station_id": "1",
                                                     "verify_tls": True, "enabled": False})
        self.assertEqual(st, 200, data)
        self.assertNoSecret("Wavelog Save", data)
        self.assertIs(json.loads(data)["key"], True)
        st, data = self.req("POST", "/api/field/qrz", {"active": 1, "profiles": [{"call": "ON6URE", "key": ""}] * 3})
        self.assertNoSecret("QRZ Save", data)
        self.assertEqual([p["key"] for p in json.loads(data)["qrz"]["profiles"]], [True, True, True])
        st, data = self.req("POST", "/api/field/hota", {"key": ""})
        self.assertNoSecret("HOTA Save", data)
        self.assertIs(json.loads(data)["hota_key"], True)
        st, data = self.req("POST", "/api/winlink", {"password": "", "mycall": "ON6URE"})
        self.assertEqual(st, 200, data)
        self.assertNoSecret("Winlink Save", data)
        self.assertTrue(os.path.exists(apps.WINLINK_PASSWORD))
        st, data = self.req("POST", "/api/winlink", {"password": "new-one", "clear_password": True})
        self.assertEqual(st, 400, data)
        st, data = self.req("POST", "/api/winlink", {"clear_password": True})
        self.assertEqual(st, 200, data)
        self.assertNoSecret("Winlink Clear", data)
        with open(self.calls) as f:
            self.assertIn("clear-password", f.read().split("\n"))

    def test_what_a_helper_said(self):
        form = {"url": "https://log.example.org", "key": "", "verify_tls": True}
        for path in ("/api/wavelog/test", "/api/wavelog/stations"):
            for key in ("", TYPED_KEY):
                st, data = self.req("POST", path, dict(form, key=key))
                self.assertEqual(st, 502, data)
                self.assertNoSecret(f"{path} with key {key!r}", data)
                self.assertIn(b"station_info/(hidden) not found", data)
        st, data = self.req("POST", "/api/wavelog/sync")
        self.assertNoSecret("Wavelog Sync", data)
        self.assertIn("station_info/(hidden)", json.loads(data)["result"])
        st, data = self.req("POST", "/api/field/update")
        for t in threading.enumerate():
            if t is not threading.main_thread():
                t.join(30)
        st, data = self.get("/api/field", 200)
        self.assertEqual(json.loads(data)["update"]["result"], "error the list server refused (hidden) and (hidden)")
        st, data = self.req("POST", "/api/winlink", {"enabled": True})
        self.assertEqual(st, 500, data)
        self.assertNoSecret("Winlink on, refused", data)
        self.assertIn(b"login (hidden) refused", data)
        st, data = self.get("/api/winlink/logs", 200)
        self.assertIn(b"Pat: login (hidden) / (hidden); wifi (hidden)", data)

    def test_wavelog_helper_itself(self):
        """x6100-wavelog as it is, against a Wavelog stand-in that quotes the request in its errors (the station
        list's path, the version check's body): the helper's own line has the key hidden already"""
        class Echo(http.server.BaseHTTPRequestHandler):
            def answer(self, code, text):
                self.send_response(code)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(text.encode())

            def do_GET(self):
                self.answer(404, self.path)

            def do_POST(self):
                self.answer(500, self.rfile.read(int(self.headers["Content-Length"])).decode())

            def log_message(self, *a):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Echo)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        url = f"http://127.0.0.1:{server.server_port}"
        conf = os.path.join(settings.FIELD_DIR, "wavelog.conf")
        with open(conf) as f:
            saved = f.read()
        self.addCleanup(write, conf, saved, 0o600)
        write(conf, saved.replace("https://log.example.org", url), 0o600)
        self.addCleanup(setattr, settings, "WAVELOG_CMD", settings.WAVELOG_CMD)
        settings.WAVELOG_CMD = os.path.join(TOP, "rootfs", "usr", "sbin", "x6100-wavelog")
        hiding = mock.patch.object(apps, "hide_secrets", lambda text, *also: text, create=True)
        hiding.start()                                    # the web server's own hiding out of the way
        self.addCleanup(hiding.stop)
        for path, said in (("/api/wavelog/stations", "Wavelog: HTTP 404 /index.php/api/station_info/(hidden)"),
                           ("/api/wavelog/test", 'Wavelog: HTTP 500 {"key": "(hidden)"}')):
            for key in ("", TYPED_KEY):
                st, data = self.req("POST", path, {"url": url, "key": key, "verify_tls": True})
                self.assertEqual(st, 502, data)
                self.assertNoSecret(f"{path} with key {key!r}", data)
                self.assertEqual(json.loads(data)["msg"], said)

    def test_backup_download(self):
        st, data = self.get("/api/backup/download?parts=settings,memories,cluster,tci,qso,cw,voice,bootpic", 200)
        st, data = self.req("GET", "/api/backup/download?parts=wifi")
        self.assertEqual(st, 200, data)
        found = [s for s in SECRETS if s.encode() in unpacked(data)]
        self.assertEqual(found, list(WIFI_PSKS))     # the owner's own file: the WiFi passwords in it, as before


if __name__ == "__main__":
    unittest.main()
