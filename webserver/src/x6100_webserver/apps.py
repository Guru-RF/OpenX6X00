from datetime import datetime
try:
    import zlib
except ImportError:   # images built before python3-zlib was selected
    zlib = None
import base64
import csv
import fcntl
import hashlib
import io
import ipaddress
import json
import os
import re
import secrets
import shutil
import socket
import sqlite3
import subprocess
import tarfile
import tempfile
import threading
import time
import urllib.parse
import zipfile

import bottle

from . import settings

app = bottle.Bottle()   # the configurator pages and their API

# Reads that hand out what the radio keeps secret, taken only by its own names as the changes are: the Files page
# serves all of /mnt (the Wavelog, QRZ, HOTA and WWFF keys, backups/backup.tgz with the SSH host keys and the WiFi
# passwords, the Winlink mail), the Backup download can hold the WiFi passwords, and the Winlink logs your mail's
# subjects and gateways
HOST_ONLY_READS = ("/files/", "/api/backup/download", "/api/winlink/logs")
# Reads sent as a POST (a form with the selection), with no secret in them: by any name, as the GET reads are (the
# QSOs and the pictures are on the pages by any name). The cross-site check still applies
POST_READS = ("/logs/export", "/api/gallery/zip")


def host_refused():
    msg = (f"Open this page by the radio's address, {socket.gethostname()}.local or its Tailscale name: by another "
           "name a page from another site could do the same through your browser")
    if bottle.request.path.startswith("/api/"):   # the pages show the msg of an API's answer
        return bottle.HTTPResponse(json.dumps({"status": "error", "msg": msg}), 403,
                                   {"Content-Type": "application/json"})
    return bottle.HTTPError(403, msg)


def via_funnel():
    """The request came from the internet through Tailscale Funnel: tailscaled sets this header on every one of
    those and drops one a browser sent, so a request from the LAN or the tailnet never has it (#58)"""
    return bottle.request.get_header("Tailscale-Funnel-Request") is not None


def funnel_refused():
    msg = ("The radio's web pages are not on the internet: open them on the radio's own network or over Tailscale. "
           "Tailscale Funnel only carries the Panel and the Screen page (Remote > Tailscale)")
    if bottle.request.path.startswith("/api/"):
        return bottle.HTTPResponse(json.dumps({"status": "error", "msg": msg}), 403,
                                   {"Content-Type": "application/json"})
    return bottle.HTTPError(403, msg)


@app.hook('before_request')
def refuse_other_sites():
    """A request that changes something, sent by a page from another site (a form a browser on the
    LAN was made to submit): refused. The radio's own pages send none of these headers, or their own
    host. Browsers mark the request with Sec-Fetch-Site, older ones only with Origin.
    A site that points a name of its own at the radio (DNS rebinding) sends that name as Host and Origin
    alike and passes this: a change, and a read of HOST_ONLY_READS, needs one of the radio's own names
    (local_host_ok) as well. Before, the Wavelog page sent the key and Files handed it out by any name (audit C08).
    A request from the internet through Tailscale Funnel is refused whatever it asks, also a page: these pages have
    no login, and a funnel made by hand (tailscale funnel 80) would hand anyone the settings, the Files page with
    the keys, Backup restore and the updates. A Funnel name is a *.ts.net name and passed the name check (#58)"""
    req = bottle.request
    if via_funnel():
        raise funnel_refused()
    write = req.method not in ("GET", "HEAD", "OPTIONS")
    change = write and req.path not in POST_READS
    if (change or req.path.startswith(HOST_ONLY_READS)) and not local_host_ok():
        raise host_refused()
    if not write:
        return
    if req.headers.get("Sec-Fetch-Site", "") == "cross-site":
        bottle.abort(403, "Cross-site request refused")
    origin = req.headers.get("Origin", "")
    if origin:   # "null" (a file:// page) has no host and is refused too
        theirs = (urllib.parse.urlsplit(origin).hostname or "").lower()
        ours = (urllib.parse.urlsplit("//" + req.headers.get("Host", "")).hostname or "").lower()
        if not theirs or theirs != ours:
            bottle.abort(403, "Cross-site request refused")


PACKAGE_DIR = os.path.dirname(os.path.abspath(__file__))
bottle.TEMPLATE_PATH.append(os.path.join(PACKAGE_DIR, "views"))


def read_param(name):
    """A text setting of the radio from params.db (callsign, qth), "" when missing."""
    try:
        con = sqlite3.connect(f"file:{settings.DB_PATH}?mode=ro", uri=True)
        try:
            row = con.execute("SELECT val FROM params WHERE name = ?", (name,)).fetchone()
        finally:
            con.close()
    except sqlite3.Error:
        return ""
    return str(row[0]) if row and row[0] else ""


def read_callsign():
    """Callsign from params.db for the page header, "" when missing."""
    return read_param("callsign")


bottle.BaseTemplate.defaults.update(callsign=read_callsign, request=bottle.request)


# DX cluster routes
#
# Same file and format as the radio GUI (src/cluster/client.c): "host=..."
# and "port=..." lines are the server in use, "server=name|host|port" lines
# the saved ones. The GUI reloads the server when the file changes: it is always replaced whole
# (replace_file), so a save within the same 2 s of FAT's mtime is seen by its new inode.
# Filters and alerts are more lines: filter_modes=CW,SSB,DIG,AMFM, filter_bands=40,20,
# filter_activity=POTA,SOTA,WWBOTA,HOTA, max_age=MIN, skip_spotters=A,B,
# filter_spotter_dxcc=PA,ON (spots heard in these countries: the GUI drops the others as they come in),
# alert=PATTERN|bands|mode, alert_popup=0|1, alert_beep=0|1.

CLUSTER_DEFAULT_HOST = "dxfun.com"
# Used while cluster.conf has no server= lines (keep in sync with src/cluster/client.c)
CLUSTER_DEFAULT_SERVERS = [
    ("DXFUN", "dxfun.com", 8000),
    ("VE7CC", "dxc.ve7cc.net", 23),
    ("W3LPL", "dxc.w3lpl.net", 7373),
    ("K1TTT", "k1ttt.net", 7373),
    ("NC7J", "dxc.nc7j.com", 7373),
    ("W1NR", "dxc.w1nr.net", 23),
    ("WA9PIE", "dxc.wa9pie.net", 8000),
    ("K4ZR", "k4zr.no-ip.org", 7300),
    ("DB0SUE", "db0sue.de", 8000),
    ("GB7DJK", "gb7djk.dxcluster.net", 7300),
    ("AE5E", "dxspots.com", 23),
    ("HamQTH", "hamqth.com", 7300),
    ("IZ7AUH", "dx.iz7auh.net", 8000),
    ("ON0DXK", "on0dxk.dyndns.org", 8000),
    ("N2WQ", "dxc.n2wq.com", 7300),
]
CLUSTER_DEFAULT_PORT = 8000
CLUSTER_MAX_SERVERS = 16
CLUSTER_MAX_ALERTS = 50
CLUSTER_MODES = ["CW", "SSB", "DIG", "AMFM"]
CLUSTER_BANDS = [160, 80, 60, 40, 30, 20, 17, 15, 12, 10, 6]
CLUSTER_ACTIVITY = ["POTA", "SOTA", "WWBOTA", "HOTA"]   # the GUI's cluster_act_names
# Written by the radio GUI: pattern|freq_hz|HH:MM of the last spot that matched
CLUSTER_ALERTS_SEEN = "/tmp/cluster_alerts_seen"
# Written by the radio GUI (src/dialog_cluster.c link_write): state|host:port|text, its APP Cluster status line
CLUSTER_LINK = "/tmp/cluster_link"
CLUSTER_LINK_SETTLE_S = 6   # the GUI looks at cluster.conf every 2 s: a change older than this was seen
CLUSTER_LOGIN_RE = re.compile(r"[A-Za-z0-9/-]{1,15}")   # the GUI's cluster_config_login(): else the station's call
# filter_spotter_dxcc: up to 16 prefixes (the GUI's SPOTTER_DXCC_MAX), each standing for its country in cty.dat
CLUSTER_SPOTTER_DXCC_RE = re.compile(r"([A-Z0-9/]{1,6}(,[A-Z0-9/]{1,6}){0,15})?")


def read_cluster_conf():
    """Returns (host, port, servers): the server in use and [(name, host, port)]."""
    host, port, servers = CLUSTER_DEFAULT_HOST, CLUSTER_DEFAULT_PORT, []
    try:
        with open(settings.CLUSTER_CONF, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    for line in lines:
        if line.startswith("host=") and line[5:]:
            host = line[5:]
        elif line.startswith("port="):
            m = re.match(r"\s*[+-]?\d+", line[5:])  # atoi()
            if m and 0 < int(m.group()) < 65536:
                port = int(m.group())
        elif line.startswith("server="):
            parts = line[7:].split("|")
            if len(parts) == 3 and parts[0] and parts[1] and parts[2].isdigit() and 0 < int(parts[2]) < 65536:
                servers.append((parts[0], parts[1], int(parts[2])))
    if not servers:
        servers = list(CLUSTER_DEFAULT_SERVERS)
    if (host, port) not in [(h, p) for _, h, p in servers]:
        servers.append((host, host, port))
    return host, port, servers


def read_cluster_options():
    """Filters and alerts of cluster.conf (same rules as the GUI: missing = no filter)."""
    opts = {"modes": list(CLUSTER_MODES), "bands": [], "activity": [], "max_age": 0, "skip": "", "spotter_dxcc": "", "alerts": [],
            "popup": False, "beep": False, "enabled": True, "login": ""}
    try:
        with open(settings.CLUSTER_CONF, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    for line in lines:
        if line.startswith("filter_modes="):
            modes = [m.strip().upper() for m in line[13:].split(",")]
            modes = [m for m in CLUSTER_MODES if m in modes]
            opts["modes"] = modes or list(CLUSTER_MODES)
        elif line.startswith("filter_bands="):
            bands = {int(b) for b in re.findall(r"\d+", line[13:])}
            opts["bands"] = [b for b in CLUSTER_BANDS if b in bands]
        elif line.startswith("filter_activity="):
            act = [a.strip().upper() for a in line[16:].split(",")]
            opts["activity"] = [a for a in CLUSTER_ACTIVITY if a in act]
        elif line.startswith("max_age="):
            m = re.match(r"\s*[+-]?\d+", line[8:])  # atoi()
            opts["max_age"] = int(m.group()) if m and 0 < int(m.group()) < 1440 else 0
        elif line.startswith("skip_spotters="):
            opts["skip"] = line[14:]
        elif line.startswith("filter_spotter_dxcc="):
            opts["spotter_dxcc"] = line[20:]
        elif line.startswith("alert_popup="):
            opts["popup"] = line[12:].strip() not in ("", "0")
        elif line.startswith("alert_beep="):
            opts["beep"] = line[11:].strip() not in ("", "0")
        elif line.startswith("enabled="):   # the GUI: atoi() != 0, missing = on
            m = re.match(r"\s*[+-]?\d+", line[8:])
            opts["enabled"] = not m or int(m.group()) != 0
        elif line.startswith("login="):
            opts["login"] = line[6:].strip() if CLUSTER_LOGIN_RE.fullmatch(line[6:].strip()) else ""
        elif line.startswith("alert=") or line.startswith("alert_off="):   # alert_off: kept, not used
            on = line.startswith("alert=")
            parts = line[6 if on else 10:].split("|")
            if len(parts) == 3 and parts[0].strip() and len(opts["alerts"]) < CLUSTER_MAX_ALERTS:
                opts["alerts"].append({"pattern": parts[0].strip(), "bands": parts[1], "mode": parts[2].strip().upper(), "enabled": on})
    return opts


def cluster_option_lines(opts):
    lines = []
    if set(opts["modes"]) != set(CLUSTER_MODES):
        lines.append("filter_modes=" + ",".join(opts["modes"]))
    if opts["bands"] and set(opts["bands"]) != set(CLUSTER_BANDS):
        lines.append("filter_bands=" + ",".join(str(b) for b in opts["bands"]))
    if opts["activity"]:
        lines.append("filter_activity=" + ",".join(opts["activity"]))
    if opts["max_age"]:
        lines.append(f"max_age={opts['max_age']}")
    if opts["skip"]:
        lines.append("skip_spotters=" + opts["skip"])
    if opts["spotter_dxcc"]:
        lines.append("filter_spotter_dxcc=" + opts["spotter_dxcc"])
    for a in opts["alerts"]:
        lines.append(f"{'alert' if a.get('enabled', True) else 'alert_off'}={a['pattern']}|{a['bands']}|{a['mode']}")
    lines.append(f"alert_popup={int(opts['popup'])}")
    lines.append(f"alert_beep={int(opts['beep'])}")
    lines.append(f"enabled={int(opts['enabled'])}")
    if opts["login"]:
        lines.append("login=" + opts["login"])
    return lines


# One configuration file change at a time: two requests at once read the same list, wrote the
# same temporary name and one of the edits was lost (audit A12). Held around read, change and replace
config_lock = threading.RLock()   # cluster_update() holds it around write_cluster_conf()


def replace_file(path, write, mode=0o644, **kw):
    """The file written whole through a unique temporary name next to it, then put in place in one step."""
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=os.path.basename(path) + ".", suffix=".tmp")
    try:
        os.chmod(tmp, mode)
        with os.fdopen(fd, "w", **kw) as f:
            write(f)
            f.flush()
            os.fsync(f.fileno())   # /mnt is FAT on the SD card, as the GUI does
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def write_cluster_conf(host, port, servers, opts=None):
    def write(f):
        f.write(f"host={host}\nport={port}\n")
        for name, h, p in servers:
            f.write(f"server={name}|{h}|{p}\n")
        for line in cluster_option_lines(opts):
            f.write(line + "\n")
    with config_lock:
        if opts is None:
            opts = read_cluster_options()  # keep the filters and alerts
        replace_file(settings.CLUSTER_CONF, write)


def cluster_alerts_seen():
    seen = {}
    try:
        with open(CLUSTER_ALERTS_SEEN, errors="replace") as f:
            for line in f.read().splitlines():
                parts = line.split("|")
                if len(parts) == 3 and parts[1].isdigit():
                    seen[parts[0]] = f"{int(parts[1]) / 1000:.1f} kHz {parts[2]}Z"
    except OSError:
        pass
    return seen


def cluster_state():
    host, port, servers = read_cluster_conf()
    opts = read_cluster_options()
    seen = cluster_alerts_seen()
    for a in opts["alerts"]:
        a["seen"] = seen.get(a["pattern"], "")
    return {"host": host, "port": port,
            "servers": [{"name": n, "host": h, "port": p} for n, h, p in servers],
            "filters": {"modes": opts["modes"], "bands": opts["bands"], "activity": opts["activity"], "max_age": opts["max_age"], "skip": opts["skip"],
                        "spotter_dxcc": opts["spotter_dxcc"]},
            "alerts": opts["alerts"], "popup": opts["popup"], "beep": opts["beep"],
            "enabled": opts["enabled"], "login": opts["login"],
            "options": {"modes": CLUSTER_MODES, "bands": CLUSTER_BANDS, "activity": CLUSTER_ACTIVITY}}  # what this firmware has


def cluster_error(status, msg):
    bottle.response.status = status
    return {"status": "error", "msg": msg}


def cluster_save(host, port, servers, opts=None):
    try:
        write_cluster_conf(host, port, servers, opts)
    except OSError as e:
        return cluster_error(500, f"Can't save: {e}")
    return cluster_state()


def cluster_update(change):
    """Reads the cluster configuration, lets change(host, port, servers, opts) alter servers and
    opts in place and writes it back, all under config_lock: two requests that read the same
    list and each wrote its own kept one of the two changes (audit B13). change() returns an
    error response, (host, port) to switch to, or None."""
    with config_lock:
        host, port, servers = read_cluster_conf()
        opts = read_cluster_options()
        r = change(host, port, servers, opts)
        if isinstance(r, dict):
            return r
        if isinstance(r, tuple):
            host, port = r
        return cluster_save(host, port, servers, opts)


# TCI server routes
# Same file and format as the radio GUI (src/tci_server.c): enabled=0|1, port=
TCI_DEFAULTS = {"enabled": "1", "port": "50001", "trust": "operate"}


def read_tci_conf():
    conf = dict(TCI_DEFAULTS)
    try:
        with open(settings.TCI_CONF, errors="replace") as f:
            for line in f:
                key, sep, val = line.rstrip("\r\n").partition("=")
                if sep and key in conf:
                    conf[key] = val
    except OSError:
        pass
    try:
        port = int(conf["port"])
    except ValueError:
        port = int(TCI_DEFAULTS["port"])
    return {"enabled": conf["enabled"] == "1", "port": port, "trust": "view" if conf["trust"] == "view" else "operate"}


@app.route('/tci')
def tci():
    return bottle.template('tci')


# Browser remote (src/remote_server.c): the page is views/remote.html, the radio serves the screen
# and takes the keys on its own WebSocket port; the conf file is shared like the TCI one
REMOTE_DEFAULTS = {"enabled": "1", "port": "50002"}


def read_remote_conf():
    conf = dict(REMOTE_DEFAULTS)
    try:
        with open(settings.REMOTE_CONF, errors="replace") as f:
            for line in f:
                key, sep, val = line.rstrip("\r\n").partition("=")
                if sep and key in conf:
                    conf[key] = val
    except OSError:
        pass
    try:
        port = int(conf["port"])
    except ValueError:
        port = int(REMOTE_DEFAULTS["port"])
    return {"enabled": conf["enabled"] == "1", "port": port}


def write_remote_conf(enabled, port):
    tmp = settings.REMOTE_CONF + ".tmp"
    with open(tmp, "w") as f:
        f.write("enabled=%d\nport=%d\n" % (1 if enabled else 0, port))
    os.replace(tmp, settings.REMOTE_CONF)


def remote_state():
    r = read_remote_conf()
    t = read_tci_conf()
    r.update({"tci_enabled": t["enabled"], "tci_port": t["port"],
              "wss_port": settings.WSS_REMOTE_PORT, "wss_tci_port": settings.WSS_TCI_PORT})
    if not local_host_ok():   # the radio takes a page's sockets by these names only (src/tci_ws.c): say so
        r["name_refused"] = (f"Open this page by the radio's address, {socket.gethostname()}.local or its Tailscale "
                             "name: by another name the radio does not take its connection, as a page from another "
                             "site could make your browser connect the same way")
    ts = tailscale_status()   # Funnel on (#58): the Panel's address on the internet, for "Use away from home"
    if ts.get("state") == "running" and ts.get("funnel") == "on" and ts.get("public", "").startswith("https://"):
        r["funnel_url"] = ts["public"]
    return r


@app.route('/remote')
def remote():
    return bottle.template('remote')


# The Panel page (views/panel.html): the radio worked natively over the same WebSocket
@app.route('/panel')
def panel():
    return bottle.template('panel')


# Tailscale Funnel (#58): the Panel and the Screen page for the internet, written at the start into a folder in RAM
# (settings.FUNNEL_DIR) with the files they load. tailscaled serves them itself, path by path
# (rootfs/usr/sbin/x6100-tailscale, which lists the same files): no request from the internet reaches this server
FUNNEL_FILES = ("favicon.ico", "css/base.css", "css/siimple-icons.css", "js/jquery-3.7.1.min.js",
                "js/radio_link.js", "js/radio_session.js")


FUNNEL_NAME = re.compile(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+")


def funnel_name():
    """The radio's name on the internet, <name>.<tailnet>.ts.net, as x6100-tailscale had it from tailscaled; "" while
    there is none (Tailscale off, or not up yet at boot)"""
    name = tailscale_status().get("name", "").rstrip(".").lower()
    return name if FUNNEL_NAME.fullmatch(name) else ""


def funnel_render(out_dir, name=""):
    """The two pages with funnel=True (funnel_base.html: no menu, no callsign, nothing of the configurator; their
    sockets at wss://<name>/ws/remote and /ws/tci, which their Content-Security-Policy names when name is known),
    and the files, written whole into a new folder that then takes out_dir's place: a page of an older start never
    mixes with files of this one. A page that loads a file tailscaled does not serve is said in the log"""
    tmp = out_dir + ".new"
    shutil.rmtree(tmp, ignore_errors=True)
    for sub in ("css", "js"):
        os.makedirs(os.path.join(tmp, "static", sub), mode=0o755)
    for view in ("panel", "remote"):
        html = bottle.template(view, funnel=True, funnel_host=name if FUNNEL_NAME.fullmatch(name) else "")
        loads = set(re.findall(r'(?:src|href)="/static/([^"?]+)', html))
        if loads - set(FUNNEL_FILES):
            raise ValueError(f"{view}.html loads {', '.join(sorted(loads - set(FUNNEL_FILES)))}, which Funnel does not "
                             "serve: add it to FUNNEL_FILES here and in x6100-tailscale")
        with open(os.path.join(tmp, view + ".html"), "w") as f:
            f.write(html)
    for name in FUNNEL_FILES:
        shutil.copyfile(os.path.join(PACKAGE_DIR, "static", name), os.path.join(tmp, "static", name))
    for dirpath, _, files in os.walk(tmp):
        os.chmod(dirpath, 0o755)
        for name in files:
            os.chmod(os.path.join(dirpath, name), 0o644)
    old = out_dir + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.isdir(out_dir):
        os.replace(out_dir, old)
    os.replace(tmp, out_dir)
    shutil.rmtree(old, ignore_errors=True)


@app.get('/api/remote')
def remote_get():
    return remote_state()


@app.post('/api/remote')
def remote_set():
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    if not isinstance(data, dict):
        return api_error(400, "Invalid request")
    cur = read_remote_conf()
    enabled = data.get("enabled", cur["enabled"])
    port = data.get("port", cur["port"])
    if not isinstance(enabled, bool) or not isinstance(port, int) or isinstance(port, bool) or not 1024 <= port <= 65535:
        return api_error(400, "Invalid value")
    try:
        write_remote_conf(enabled, port)
    except OSError as e:
        return api_error(500, "Can't save: %s" % e)
    return remote_state()


# Firmware update: x6100-update on the radio does the work (check, download, verify, install,
# rollback); the page shows its status and starts a job in a thread

update_lock = threading.Lock()
update_job = {"running": False, "what": "", "result": ""}
UPDATE_CMD = "/usr/sbin/x6100-update"
UPDATE_MAX_BYTES = 64 * 1024 * 1024


def update_run(args, timeout=1800):
    try:
        r = subprocess.run([UPDATE_CMD] + args, capture_output=True, text=True, timeout=timeout)
        lines = r.stdout.strip().splitlines()
        return lines[-1] if lines else f"error exit code {r.returncode}"
    except (OSError, subprocess.SubprocessError) as e:
        return f"error {e}"


def update_json(args):
    out = update_run(args, timeout=60)
    try:
        return json.loads(out)
    except ValueError:
        return {"error": out[6:] if out.startswith("error ") else out}


def update_job_run(what, args):
    result = update_run(args)
    with update_lock:
        update_job.update(running=False, result=result)
    if what == "install" and len(args) > 1 and os.path.basename(args[1]).startswith("upload-"):
        try:
            os.remove(args[1])      # the uploaded bundle is staged and applied: not needed again
        except OSError:
            pass


def update_reserve(what):
    """The one job at a time, taken before anything is done for it; None, or the 409 response."""
    with update_lock:
        if update_job["running"]:
            return api_error(409, "An update is already running")
        update_job.update(running=True, what=what, result="")
    return None


def update_release():
    with update_lock:
        update_job["running"] = False


def update_launch(args):
    """Runs the reserved job in its thread"""
    threading.Thread(target=update_job_run, args=(update_job["what"], args), daemon=True).start()
    return get_update()


def update_start(what, args):
    return update_reserve(what) or update_launch(args)


@app.route('/update')
def update_page():
    return bottle.template('update')


@app.get('/api/update')
def get_update():
    st = update_json(["status"])
    with update_lock:
        st["job"] = dict(update_job)
    return st


@app.get('/api/update/check')
def check_update():
    return update_json(["check"])


@app.post('/api/update/upgrade')
def start_upgrade():
    return update_start("upgrade", ["upgrade"])


@app.post('/api/update/rollback')
def start_rollback():
    return update_start("rollback", ["rollback"])


@app.post('/api/update/upload')
def upload_update():
    """A bundle file (multipart field "file"): saved on the DATA partition and installed."""
    upload = bottle.request.files.get("file")
    if upload is None:
        return api_error(400, "No file")
    if not upload.raw_filename.endswith(".tar.gz"):
        return api_error(400, "Not an update bundle (.tar.gz)")
    # The job is taken before the file is written, and every upload has a file of its own: a
    # second upload wrote over the one shared path while the accepted job had not opened it yet,
    # was told "already running", and the job installed the second bundle (audit B08)
    err = update_reserve("install")
    if err:
        return err
    dest = "/mnt/update/upload-%s.tar.gz" % secrets.token_hex(4)
    try:
        os.makedirs("/mnt/update", exist_ok=True)
        size = 0
        with open(dest + ".part", "wb") as f:
            while True:
                chunk = upload.file.read(1 << 20)
                if not chunk:
                    break
                size += len(chunk)
                if size > UPDATE_MAX_BYTES:
                    raise ValueError("Larger than 64 MB")
                f.write(chunk)
        os.replace(dest + ".part", dest)
    except BaseException as e:
        update_release()
        try:
            os.remove(dest + ".part")
        except OSError:
            pass
        if isinstance(e, ValueError):
            return api_error(413, str(e))
        raise
    return update_launch(["install", dest])


# Tailscale (rootfs/usr/sbin/x6100-tailscale): its supervisor keeps a status file, the page reads it;
# the switch and Log out run the helper, which only starts or signals the supervisor and returns
TAILSCALE_CMD = "/usr/sbin/x6100-tailscale"
TAILSCALE_STATUS = "/run/tailscale/status"
TAILSCALE_CONF = "/mnt/tailscale.conf"


def tailscale_status():
    st = {"state": "off", "text": "Off"}
    try:
        with open(TAILSCALE_STATUS) as f:
            for line in f:
                k, sep, v = line.rstrip("\n").partition("=")
                if sep:
                    st[k] = v
    except OSError:
        pass
    try:
        with open(TAILSCALE_CONF) as f:
            lines = [line.strip() for line in f]
        st["enabled"] = "enabled=1" in lines
        st["funnel_wanted"] = "funnel=1" in lines   # Funnel (#58): the setting; "funnel" is what the supervisor made of it
    except OSError:
        st["enabled"] = st["funnel_wanted"] = False
    return st


@app.route('/tailscale')
def tailscale_page():
    return bottle.template('tailscale')


def local_host_ok():
    """The page opened by an address or by the radio's own name. A page from another site that points its own
    name at the radio (DNS rebinding) passes the cross-site guard: every change (refuse_other_sites), the Files
    page and the Backup download, Tailscale's login link (which would put the radio on that site's tailnet) and
    the list of SSH keys only by these names. Pat's mailbox on port 8080 takes the same names (tools/pat).
    MagicDNS's short name too, which is the tailnet's name for the radio: x6200-1 while the tailnet still has the
    machine of an earlier card image as x6200. Never a request from the internet through Tailscale Funnel: its
    name is a *.ts.net name too (#58)"""
    if via_funnel():
        return False
    host = (urllib.parse.urlsplit("//" + (bottle.request.get_header("Host") or "")).hostname or "").rstrip(".")
    try:
        ipaddress.ip_address(host)
        return True
    except ValueError:
        pass
    name = socket.gethostname().lower()
    names = {"localhost", name, name + ".local"}
    tailnet = tailscale_status().get("name", "").lower()
    if tailnet:
        names.add(tailnet.split(".")[0])
    return host in names or host.endswith(".ts.net")


@app.get('/api/tailscale')
def get_tailscale():
    st = tailscale_status()
    if not local_host_ok():
        st.pop("url", None)
        st.pop("funnel_url", None)   # the admin console's link that allows Funnel: the login's kind of link
    return st


# The actions, as the helper's arguments: Funnel (#58) only writes its setting, the supervisor follows it
TAILSCALE_ACTIONS = {"on": ["on"], "off": ["off"], "logout": ["logout"], "funnel_on": ["funnel", "on"],
                     "funnel_off": ["funnel", "off"]}


@app.post('/api/tailscale')
def set_tailscale():
    action = (bottle.request.json or {}).get("action")
    if action not in TAILSCALE_ACTIONS:
        return api_error(400, "action: on, off, logout, funnel_on or funnel_off")
    try:
        r = subprocess.run([TAILSCALE_CMD] + TAILSCALE_ACTIONS[action], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        return api_error(500, f"Tailscale: {e}")
    if r.returncode != 0:
        msg = (r.stderr.strip().splitlines() or [f"exit code {r.returncode}"])[-1]
        return api_error(500, "Tailscale: " + (msg[6:] if msg.startswith("error ") else msg))
    return tailscale_status()


# Winlink (rootfs/usr/sbin/x6100-winlink): Pat and the modem (ardopcf, or Mercury's x6x00-mercury) under a
# supervisor that keeps a status file. The page writes the settings (/mnt/winlink.conf), hands a new password to the
# helper on stdin (it goes to the root filesystem, where these pages never read it back) and runs the helper to apply
# them and to turn it on or off
WINLINK_CMD = "/usr/sbin/x6100-winlink"
WINLINK_STATUS = "/run/winlink/status"
WINLINK_RADIO = "/run/winlink/radio"                  # the GUI's (src/winlink.c), while a modem is on its socket
WINLINK_RADIO_FRESH = 15                              # s: older is from a GUI that is gone
WINLINK_CONF = "/mnt/winlink.conf"
WINLINK_PASSWORD = "/var/lib/winlink/password"
WINLINK_PROGRAMS = ("/usr/bin/pat", "/usr/bin/ardopcf")
WINLINK_PORT = 8080                                   # Pat's mailbox
WINLINK_CALL_RE = re.compile(r"[A-Z0-9]{3,12}(-[0-9]{1,2})?")   # as x6100-winlink checks them
WINLINK_LOCATOR_RE = re.compile(r"[A-R]{2}[0-9]{2}([A-X]{2})?", re.I)
WINLINK_BANDWIDTHS = (200, 500, 1000, 2000)           # ARDOP's, the widest Pat may use (as x6100-winlink's)
WINLINK_MODEMS = ("ardop", "mercury")                 # the modem= choice (as x6100-winlink's), ardop the default
WINLINK_MERCURY_BANDWIDTHS = (500, 2300, 2750)        # Mercury's, VARA's three (as x6100-winlink's), 2300 the default
winlink_lock = threading.Lock()                       # one change at a time: Off may take 20 s


def winlink_conf():
    conf = {"enabled": "0", "mycall": "", "locator": "", "bandwidth": "500", "modem": "ardop",
            "mercury_bandwidth": "2300"}
    try:
        with open(WINLINK_CONF) as f:
            for line in f:
                k, sep, v = line.strip().partition("=")
                if sep:
                    conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def winlink_state():
    """The settings, the supervisor's status and the defaults; where a password is set, never the password: on this
    page (password_set), in Pat's own Settings (the supervisor's password=pat, #80) or nowhere. The modem chosen is
    modem_choice: the status's modem is how the one that runs is doing"""
    st = {"state": "off", "text": "Off", "pat": "off", "ardop": "off", "mercury": "off"}
    try:
        with open(WINLINK_STATUS) as f:
            for line in f:
                k, sep, v = line.rstrip("\n").partition("=")
                if sep:
                    st[k] = v
    except OSError:
        pass
    conf = winlink_conf()
    try:
        password_set = os.path.getsize(WINLINK_PASSWORD) > 0
    except OSError:
        password_set = False
    st.update(enabled=conf["enabled"] == "1", mycall=conf["mycall"], locator=conf["locator"],
              bandwidth=int(conf["bandwidth"]) if conf["bandwidth"] in [str(b) for b in WINLINK_BANDWIDTHS] else 500,
              modem_choice=conf["modem"] if conf["modem"] in WINLINK_MODEMS else "ardop",
              mercury_bandwidth=int(conf["mercury_bandwidth"])
              if conf["mercury_bandwidth"] in [str(b) for b in WINLINK_MERCURY_BANDWIDTHS] else 2300,
              password_set=password_set,
              password="page" if password_set else "pat" if st.get("password") == "pat" else "none",
              radio_mycall=max(read_callsign().upper().split("/"), key=len),   # ON6URE of ON6URE/P
              radio_locator=read_param("qth"), installed=all(os.access(p, os.X_OK) for p in WINLINK_PROGRAMS),
              host_ok=local_host_ok(), port=WINLINK_PORT, radio=winlink_radio())
    return st


def winlink_radio():
    """The radio's state as the GUI writes it for Winlink: frequencies, mode, the simplex check, filters, power, the
    app open, the SWR hold, the last refusal and QSY (with how many seconds ago); {} when not written lately"""
    try:
        with open(WINLINK_RADIO) as f:
            if time.time() - os.fstat(f.fileno()).st_mtime > WINLINK_RADIO_FRESH:
                return {}
            radio = dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
    except OSError:
        return {}
    for k, v in radio.items():
        if re.fullmatch(r"-?\d+", v):
            radio[k] = int(v)
        elif re.fullmatch(r"-?\d+\.\d+", v):
            radio[k] = float(v)
    now = int(time.time())
    for k in ("refused", "qsy"):
        at = radio.get(k + "_at")
        radio[k + "_age"] = now - at if isinstance(at, int) and at > 0 else None
    return radio


def winlink_run(args, stdin=None):
    """The helper; its error line, None when it went well"""
    try:
        r = subprocess.run([WINLINK_CMD, *args], input=stdin, capture_output=True, text=True, timeout=40)
    except (OSError, subprocess.SubprocessError) as e:
        return str(e)
    if r.returncode != 0:
        msg = (r.stderr.strip().splitlines() or [f"exit code {r.returncode}"])[-1]
        return msg[6:] if msg.startswith("error ") else msg
    return None


@app.route('/winlink')
def winlink_page():
    return bottle.template('winlink')


@app.get('/api/winlink')
def get_winlink():
    return winlink_state()


@app.get('/api/winlink/logs')
def winlink_logs():
    """Pat's, the modem's and the radio's logs as one text file for a bug report; the helper masks the passwords"""
    try:
        r = subprocess.run([WINLINK_CMD, "logs"], capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        return api_error(500, f"Winlink: {e}")
    if r.returncode != 0:
        msg = (r.stderr.decode(errors="replace").strip().splitlines() or [f"exit code {r.returncode}"])[-1]
        return api_error(500, "Winlink: " + (msg[6:] if msg.startswith("error ") else msg))
    bottle.response.content_type = "text/plain; charset=utf-8"
    bottle.response.set_header("Content-Disposition",
                               f'attachment; filename="winlink-logs-{time.strftime("%Y%m%d-%H%M")}.txt"')
    return r.stdout


@app.post('/api/winlink')
def set_winlink():
    """Any of enabled, mycall, locator, bandwidth, modem_choice (ardop or mercury), mercury_bandwidth and password;
    what is not sent stays. An empty callsign or locator is the radio's own, an empty password keeps the one set"""
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    if not isinstance(data, dict):
        return api_error(400, "Invalid request")
    with winlink_lock:
        conf = winlink_conf()
        new = dict(conf)
        if "enabled" in data:
            if not isinstance(data["enabled"], bool):
                return api_error(400, "enabled: true or false")
            new["enabled"] = "1" if data["enabled"] else "0"
        if "mycall" in data:
            call = data["mycall"].strip().upper() if isinstance(data["mycall"], str) else None
            if call is None or call and not WINLINK_CALL_RE.fullmatch(call):
                return api_error(400, "Callsign: your Winlink account, letters and digits (an SSID like -5 may follow); "
                                      "empty for the radio's")
            new["mycall"] = call
        if "locator" in data:
            loc = data["locator"].strip() if isinstance(data["locator"], str) else None
            if loc is None or loc and not WINLINK_LOCATOR_RE.fullmatch(loc):
                return api_error(400, "Locator: 4 or 6 characters (JO20 or JO20ab); empty for the radio's QTH")
            new["locator"] = loc[:4].upper() + loc[4:].lower()
        if "bandwidth" in data:
            if data["bandwidth"] not in WINLINK_BANDWIDTHS or isinstance(data["bandwidth"], bool):
                return api_error(400, "Bandwidth: 200, 500, 1000 or 2000")
            new["bandwidth"] = str(data["bandwidth"])
        if "modem_choice" in data:
            if data["modem_choice"] not in WINLINK_MODEMS:
                return api_error(400, "Modem: ARDOP or Mercury")
            new["modem"] = data["modem_choice"]
        if "mercury_bandwidth" in data:
            mbw = data["mercury_bandwidth"]
            if mbw not in WINLINK_MERCURY_BANDWIDTHS or isinstance(mbw, bool):
                return api_error(400, "Mercury bandwidth: 500, 2300 or 2750")
            new["mercury_bandwidth"] = str(mbw)
        password = data.get("password", "")
        password = password.strip() if isinstance(password, str) else None
        if password is None or len(password) > 64 or any(ord(c) < 32 or ord(c) == 127 for c in password):
            return api_error(400, "Password: up to 64 characters")
        if password:
            err = winlink_run(["set-password"], password)
            if err:
                return api_error(500, "Winlink: " + err)
        keys = ["enabled", "mycall", "locator", "bandwidth", "modem", "mercury_bandwidth"]
        if new != conf:
            try:
                replace_file(WINLINK_CONF, lambda f: f.write(
                    "".join(f"{k}={new[k]}\n" for k in keys + [k for k in new if k not in keys])))
            except OSError as e:
                return api_error(500, f"Can't save: {e}")
        err = None
        if new["enabled"] == "1":
            if password or any(new[k] != conf[k] for k in keys[1:]):
                err = winlink_run(["apply"])          # Pat restarts with them, and the modem when another is chosen
            err = err or winlink_run(["on"])
        elif conf["enabled"] == "1" or "enabled" in data:
            err = winlink_run(["off"])
        if err:
            return api_error(500, "Winlink: " + err)
    return winlink_state()


# SSH takes keys only (rootfs/etc/ssh/sshd_config): root's keys in /root/.ssh/authorized_keys are listed, added and
# removed here. S04restore_backup_config keeps them on the DATA partition, after each change here and at every
# clean power-off, and puts them back on a new card image
SSH_DIR = "/root/.ssh"
SSH_AUTHORIZED_KEYS = os.path.join(SSH_DIR, "authorized_keys")
SSH_BACKUP = ["/etc/init.d/S04restore_backup_config", "stop"]
SSH_KEY_TYPES = ("ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521", "ssh-rsa")
SSH_EC_CURVES = {"nistp256": (256, 65), "nistp384": (384, 97), "nistp521": (521, 133)}   # bits, point 04|x|y
SSH_RSA_MIN_BITS = 3072
SSH_MAX_KEYS = 32
SSH_TYPE_RE = re.compile(r"[a-z0-9][a-z0-9@.-]{2,63}")
ssh_lock = threading.Lock()


def ssh_blob_fields(blob):
    """The length-prefixed strings an SSH public key is made of, None when it is not made of them"""
    out, i = [], 0
    while i < len(blob):
        n = int.from_bytes(blob[i:i + 4], "big")
        if i + 4 + n > len(blob):
            return None
        out.append(blob[i + 4:i + 4 + n])
        i += 4 + n
    return out


def ssh_key(kind, b64):
    """{type, bits, fingerprint} of a public key as authorized_keys has it (type, base64), None when the base64 is
    not a key of that type. A type this page does not add (sk-, ssh-dss) is listed with bits None"""
    try:
        blob = base64.b64decode(b64, validate=True)
    except ValueError:
        return None
    f = ssh_blob_fields(blob)
    if not f or f[0] != kind.encode():
        return None
    bits = None
    if kind == "ssh-ed25519":
        if len(f) != 2 or len(f[1]) != 32:
            return None
        bits = 256
    elif kind.startswith("ecdsa-sha2-") and kind in SSH_KEY_TYPES:
        bits, point = SSH_EC_CURVES[kind[11:]]
        if len(f) != 3 or f[1] != kind[11:].encode() or len(f[2]) != point or f[2][:1] != b"\x04":
            return None
    elif kind == "ssh-rsa":
        if len(f) != 3:
            return None
        bits = int.from_bytes(f[2], "big").bit_length()
    fingerprint = "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")
    return {"type": kind, "bits": bits, "fingerprint": fingerprint}


def ssh_parse_line(line):
    """The key of an authorized_keys line: {type, bits, fingerprint, comment, options, line (the key without its
    options)}; None for a blank line, a # comment or a line sshd would not take either. Options (from=...,
    command=...) come first, and a quoted value may hold spaces"""
    def key_of(text):
        parts = text.split(None, 2)
        key = ssh_key(parts[0], parts[1]) if len(parts) >= 2 and SSH_TYPE_RE.fullmatch(parts[0]) else None
        if key:
            key.update(comment=parts[2] if len(parts) > 2 else "", line=" ".join(parts))
        return key

    s = line.strip()
    if not s or s.startswith("#"):
        return None
    key = key_of(s)
    if key:
        key["options"] = ""
        return key
    quoted, i = False, 0
    while i < len(s) and (quoted or s[i] not in " \t"):
        if s[i] == "\\" and quoted:
            i += 1
        elif s[i] == '"':
            quoted = not quoted
        i += 1
    key = key_of(s[i:].strip())
    if key:
        key["options"] = s[:i]
    return key


def ssh_new_key(text):
    """(key, line to add) for a pasted public key; ValueError says what is wrong with it"""
    text = text.strip() if isinstance(text, str) else ""
    if not text:
        raise ValueError("Paste a public key: one line, ssh-ed25519 AAAA... you@computer")
    if "PRIVATE KEY" in text:
        raise ValueError("That is a private key, which stays on your computer: paste the line of the .pub file")
    if "\n" in text or "\r" in text:
        raise ValueError("One key at a time, on one line")
    if len(text) > 8192:
        raise ValueError("Too long for a public key")
    parts = text.split(None, 2)
    if parts[0] not in SSH_KEY_TYPES:
        raise ValueError("The line starts with the key type: ssh-ed25519, ecdsa-sha2-nistp256 (384, 521) or ssh-rsa, "
                         "with nothing before it")
    key = ssh_key(parts[0], parts[1]) if len(parts) >= 2 else None
    if not key:
        raise ValueError(f"Not a whole {parts[0]} key: copy the line again")
    if key["type"] == "ssh-rsa" and key["bits"] < SSH_RSA_MIN_BITS:
        raise ValueError(f"An RSA key of {key['bits']} bits: {SSH_RSA_MIN_BITS} or more, or an ed25519 key "
                         "(ssh-keygen -t ed25519)")
    comment = parts[2] if len(parts) > 2 else ""
    if len(comment) > 200 or any(ord(c) < 32 or ord(c) == 127 for c in comment):
        raise ValueError("The comment after the key: up to 200 characters, no tabs or control characters")
    return key, " ".join(parts)


def ssh_read():
    """The lines of root's authorized_keys, [] when it has none"""
    try:
        with open(SSH_AUTHORIZED_KEYS, errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def ssh_state():
    return {"keys": [k for k in map(ssh_parse_line, ssh_read()) if k], "max": SSH_MAX_KEYS,
            "rsa_min_bits": SSH_RSA_MIN_BITS}


def ssh_write(lines):
    """authorized_keys replaced whole (0600, in a 0700 .ssh), then the backup on the DATA partition at once: a
    power cut before the next clean power-off would leave a new card image without the change"""
    os.makedirs(SSH_DIR, mode=0o700, exist_ok=True)
    replace_file(SSH_AUTHORIZED_KEYS, lambda f: f.write("".join(line + "\n" for line in lines)), mode=0o600)
    if os.access(SSH_BACKUP[0], os.X_OK):
        try:
            subprocess.run(SSH_BACKUP, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
        except (OSError, subprocess.SubprocessError):
            pass


def ssh_request():
    """The request's JSON object, or the error to answer with: also for a page opened by another site's name"""
    if not local_host_ok():
        return None, api_error(403, f"Open this page by the radio's address or {socket.gethostname()}.local "
                                    "to see or change the SSH keys")
    try:
        data = bottle.request.json if bottle.request.method != "GET" else {}
    except ValueError:
        data = None
    if not isinstance(data, dict):
        return None, api_error(400, "Invalid request")
    return data, None


@app.route('/ssh')
def ssh_page():
    return bottle.template('ssh')


@app.get('/api/ssh')
def get_ssh():
    _, err = ssh_request()
    if err:
        return err
    try:
        return ssh_state()
    except OSError as e:
        return api_error(500, f"Can't read the keys: {e}")


@app.post('/api/ssh/keys')
def add_ssh_key():
    data, err = ssh_request()
    if err:
        return err
    try:
        key, line = ssh_new_key(data.get("key"))
    except ValueError as e:
        return api_error(400, str(e))
    with ssh_lock:
        try:
            lines = ssh_read()
            have = [k for k in map(ssh_parse_line, lines) if k]
            same = next((k for k in have if k["fingerprint"] == key["fingerprint"]), None)
            if same:
                return api_error(400, "This key is there already" + (f" ({same['comment']})" if same["comment"] else ""))
            if len(have) >= SSH_MAX_KEYS:
                return api_error(400, f"{SSH_MAX_KEYS} keys at most: remove one first")
            ssh_write(lines + [line])
            return ssh_state()
        except OSError as e:
            return api_error(500, f"Can't save the key: {e}")


@app.delete('/api/ssh/keys')
def delete_ssh_key():
    """Every line with that key goes (a key may be there twice, with options)"""
    data, err = ssh_request()
    if err:
        return err
    fingerprint = data.get("fingerprint")
    if not isinstance(fingerprint, str) or not fingerprint.startswith("SHA256:"):
        return api_error(400, "fingerprint: SHA256:...")
    with ssh_lock:
        try:
            lines = ssh_read()
            keep = [line for line in lines if (ssh_parse_line(line) or {}).get("fingerprint") != fingerprint]
            if len(keep) == len(lines):
                return api_error(404, "That key is not there any more: reload the page")
            ssh_write(keep)
            return ssh_state()
        except OSError as e:
            return api_error(500, f"Can't remove the key: {e}")


@app.get('/api/tci')
def get_tci():
    return read_tci_conf()


@app.post('/api/tci')
def set_tci():
    data = bottle.request.json or {}
    port = data.get("port", 50001)
    if isinstance(port, bool) or not isinstance(port, int) or not 1024 <= port <= 65535:
        bottle.response.status = 400
        return {"status": "error", "msg": "Invalid port: 1024-65535"}
    trust = data.get("trust", read_tci_conf()["trust"])   # kept when the page does not send it
    if trust not in ("operate", "view"):
        bottle.response.status = 400
        return {"status": "error", "msg": "trust is operate or view"}
    try:
        tmp = settings.TCI_CONF + ".tmp"
        with open(tmp, "w") as f:
            f.write(f"enabled={1 if data.get('enabled') else 0}\nport={port}\ntrust={trust}\n")
        os.rename(tmp, settings.TCI_CONF)
    except OSError as e:
        bottle.response.status = 500
        return {"status": "error", "msg": f"Can't save: {e}"}
    return read_tci_conf()


# Field Ops routes: x6100-field (rootfs/usr/sbin) does the downloads, same files as the GUI:
# <FIELD_DIR>/refs.tsv, qrz.conf, hota.conf and wwff.conf ("key=..."), logs/*.adi

field_lock = threading.Lock()
field_update = {"running": False, "result": ""}
field_refs_counts = (None, {})   # (mtime, size), {"P": parks, "S": summits, "B": bunkers, "H": HOTA, "F": WWFF, "T": towers, "C": castles, "M": mills, "L": lights}


def field_path(*names):
    return os.path.join(settings.FIELD_DIR, *names)


def key_set(name):
    try:
        with open(field_path(name), errors="replace") as f:
            return any(line.startswith("key=") and line[4:].strip() for line in f)
    except OSError:
        return False


QRZ_PROFILES = 3


def qrz_read():
    """qrz.conf: up to three QRZ Logbook profiles, call1/key1..call3/key3 and active=N (an old single
    key= is profile 1). The keys never go back to the browser, only whether one is set."""
    active, legacy, calls, keys = 1, "", [""] * QRZ_PROFILES, [""] * QRZ_PROFILES
    try:
        with open(field_path("qrz.conf"), errors="replace") as f:
            for line in f:
                k, sep, v = line.strip().partition("=")
                if not sep:
                    continue
                k, v = k.strip(), v.strip()
                if k == "active" and v.isdigit():
                    active = int(v)
                elif k == "key":
                    legacy = v
                elif k[:4] == "call" and k[4:].isdigit() and 1 <= int(k[4:]) <= QRZ_PROFILES:
                    calls[int(k[4:]) - 1] = v
                elif k[:3] == "key" and k[3:].isdigit() and 1 <= int(k[3:]) <= QRZ_PROFILES:
                    keys[int(k[3:]) - 1] = v
    except OSError:
        pass
    if not any(keys) and legacy:
        keys[0] = legacy
    if not 1 <= active <= QRZ_PROFILES:
        active = 1
    return active, calls, keys


def qrz_write(active, calls, keys):
    path = field_path("qrz.conf")
    os.makedirs(settings.FIELD_DIR, exist_ok=True)
    if not any(keys):
        if os.path.exists(path):
            os.remove(path)
        return
    tmp = path + ".tmp"
    with os.fdopen(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        f.write(f"active={active}\n")
        for i in range(QRZ_PROFILES):
            if keys[i]:
                f.write(f"call{i + 1}={calls[i]}\nkey{i + 1}={keys[i]}\n")
    os.rename(tmp, path)


def qrz_state():
    active, calls, keys = qrz_read()
    return {"active": active, "profiles": [{"call": calls[i], "key": bool(keys[i])} for i in range(QRZ_PROFILES)]}


def field_refs():
    global field_refs_counts
    try:
        st = os.stat(field_path("refs.tsv"))
    except OSError:
        return None
    stamp = (st.st_mtime, st.st_size)
    if field_refs_counts[0] != stamp:   # counted once per new file
        counts = {}
        with open(field_path("refs.tsv"), "rb") as f:
            for line in f:
                counts[line[:1]] = counts.get(line[:1], 0) + 1
        field_refs_counts = (stamp, counts)
    c = field_refs_counts[1]
    return {"date": time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime)), "size": fmt_size(st.st_size),
            "parks": c.get(b"P", 0), "summits": c.get(b"S", 0), "bunkers": c.get(b"B", 0), "hota": c.get(b"H", 0),
            "wwff": c.get(b"F", 0), "towers": c.get(b"T", 0), "castles": c.get(b"C", 0), "mills": c.get(b"M", 0),
            "lights": c.get(b"L", 0)}


def field_logs():
    try:
        names = os.listdir(field_path("logs"))
    except OSError:
        return []
    logs = []
    for name in names:
        path = field_path("logs", name)
        if name.endswith(".adi") and os.path.isfile(path):
            st = os.stat(path)
            logs.append({"name": name, "size": fmt_size(st.st_size), "mtime": st.st_mtime,
                         "date": time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime))})
    return sorted(logs, key=lambda x: x["mtime"], reverse=True)


def run_field_update():
    env = dict(os.environ, X6100_FIELD_DIR=settings.FIELD_DIR, X6100_CALL=read_callsign())
    try:
        # the helper gives up after 2700 s (its RUN_LIMIT): a little longer here, to hear why
        r = subprocess.run([settings.FIELD_CMD, "update"], capture_output=True, text=True, timeout=2760, env=env)
        lines = r.stdout.strip().splitlines()
        result = lines[-1] if lines else f"error exit code {r.returncode}"
    except (OSError, subprocess.SubprocessError) as e:
        result = f"error {e}"
    with field_lock:
        field_update.update(running=False, result=result)


@app.route('/field')
def field():
    return bottle.template('field')


@app.get('/api/field')
def get_field():
    with field_lock:
        update = dict(field_update)
    if update["running"]:   # "<list n> <lists> <bytes> <total> <retry> <name>", written by x6100-field
        try:
            with open(field_path(".update.progress")) as f:
                n, lists, done, total, retry, name = f.read().split()
            update["progress"] = f"{name} ({n}/{lists}): {int(done) / 1e6:.1f}" + \
                (" MB, reading the list" if done == total != "0" else f" / {int(total) / 1e6:.1f} MB" if int(total) else " MB") + \
                (f", retry {retry} of 5" if retry != "0" else "")
        except (OSError, ValueError):
            pass
    return {"qrz": qrz_state(), "hota_key": key_set("hota.conf"), "wwff_key": key_set("wwff.conf"),
            "update": update, "refs": field_refs(),
            "logs": field_logs()}


@app.post('/api/field/qrz')
def set_field_qrz():
    """The QRZ Logbook profiles: {"active": N, "profiles": [{"call", "key"}, ...]} ("" key: the one
    stored stays), or {"remove": N}. The radio's log page picks the active one too."""
    body = bottle.request.json or {}
    active, calls, keys = qrz_read()
    if "remove" in body:
        n = body["remove"]
        if not (isinstance(n, int) and 1 <= n <= QRZ_PROFILES):
            return api_error(400, "No such profile")
        calls[n - 1] = keys[n - 1] = ""
    else:
        for i, prof in enumerate((body.get("profiles") or [])[:QRZ_PROFILES]):
            call = str(prof.get("call", "")).strip().upper()
            key = str(prof.get("key", "")).strip()
            if call and not re.fullmatch(r"[A-Z0-9/-]{1,16}", call):
                return api_error(400, f"Profile {i + 1}: the call is letters, digits, / and -")
            if key and not re.fullmatch(r"[!-~]{1,128}", key):
                return api_error(400, f"Profile {i + 1}: a key is 1-128 printable characters, no spaces")
            calls[i] = call
            if key:
                keys[i] = key
        if isinstance(body.get("active"), int):
            active = body["active"]
    if not 1 <= active <= QRZ_PROFILES or not keys[active - 1]:
        active = next((i + 1 for i in range(QRZ_PROFILES) if keys[i]), 1)
    try:
        qrz_write(active, calls, keys)
    except OSError as e:
        return api_error(500, f"Can't save: {e}")
    return get_field()


@app.post('/api/field/<name:re:hota|wwff>')
def set_field_key(name):
    """cqhota.app integration key, WWFF Spotline key: <name>.conf, never shown back. {"key": K} stores
    a new one, {"remove": true} takes it away; an empty key changes nothing, as the QRZ page (U04)."""
    body = bottle.request.json or {}
    key = str(body.get("key", "")).strip()
    remove = body.get("remove") is True
    if key and not re.fullmatch(r"[!-~]{1,128}", key):
        return api_error(400, "Invalid key: 1-128 printable characters, no spaces")
    path = field_path(name + ".conf")
    try:
        os.makedirs(settings.FIELD_DIR, exist_ok=True)
        if key:
            tmp = path + ".tmp"
            with os.fdopen(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
                f.write(f"key={key}\n")
            os.rename(tmp, path)
        elif remove and os.path.exists(path):
            os.remove(path)
    except OSError as e:
        return api_error(500, f"Can't save: {e}")
    return get_field()


@app.post('/api/field/update')
def start_field_update():
    with field_lock:
        if field_update["running"]:
            return api_error(409, "An update is already running")
        field_update.update(running=True, result="")
    threading.Thread(target=run_field_update, daemon=True).start()
    return get_field()


@app.get('/field/logs/<name>')
def field_log_file(name):
    if not name.endswith(".adi"):
        raise bottle.HTTPError(404, "Not found")
    return bottle.static_file(name, root=field_path("logs"), download=True, mimetype="text/plain")


# Wavelog routes: x6100-wavelog (rootfs/usr/sbin) talks to the user's Wavelog; the GUI runs it after each QSO.
# <FIELD_DIR>/wavelog.conf: enabled, url, key, station_id, station_name, verify_tls, since. The key never goes
# back to the browser, only whether one is set; wavelog.state and wavelog.queue are the helper's.

WAVELOG_KEYS = ("enabled", "url", "key", "station_id", "station_name", "verify_tls", "since")
WAVELOG_URL = r"https?://[^\s<>\"'=]{3,200}"
# Wavelog 3.1 and later make two kinds: API keys ("Legacy API Keys (API v1)", wl and 13 hex, cl... from Cloudlog)
# and API tokens ("API Tokens (API v2)", wl2_ and 40 hex). x6100-wavelog speaks API v1 (the key in the request),
# which answers a token "missing or invalid api key", 401: Test connection said only "key refused" (#73)
WAVELOG_V2_TOKEN = ("That is a Wavelog API token (wl2_..., API v2), which the radio can't use yet: on Wavelog's API "
                    "page, under Legacy API Keys (API v1), Create a read & write key, and type that key (wl...)")


def wavelog_read():
    conf = {}
    try:
        with open(field_path("wavelog.conf"), errors="replace") as f:
            for line in f:
                k, sep, v = line.strip().partition("=")
                if sep and k.strip() in WAVELOG_KEYS:
                    conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def wavelog_write(conf):
    path = field_path("wavelog.conf")
    os.makedirs(settings.FIELD_DIR, exist_ok=True)
    tmp = path + ".tmp"
    with os.fdopen(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        for k in WAVELOG_KEYS:
            if conf.get(k, "") != "":
                f.write(f"{k}={conf[k]}\n")
    os.rename(tmp, path)


def wavelog_json(name, default):
    try:
        with open(field_path(name), errors="replace") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def wavelog_state(result=None):
    conf = wavelog_read()
    last = wavelog_json("wavelog.state", {}).get("last") or {}
    since = int(conf.get("since") or 0) if (conf.get("since") or "").isdigit() else 0
    return {"enabled": conf.get("enabled") == "1", "url": conf.get("url", ""), "key": bool(conf.get("key")),
            "station_id": conf.get("station_id", ""), "station_name": conf.get("station_name", ""),
            "verify_tls": conf.get("verify_tls", "1") != "0",
            "since": time.strftime("%Y-%m-%d %H:%M", time.localtime(since)) if since else "",
            "queue": len(wavelog_json("wavelog.queue", [])), "result": result,
            "last": {"result": last.get("result", ""),
                     "time": time.strftime("%Y-%m-%d %H:%M", time.localtime(last["time"])) if last.get("time") else ""}}


def wavelog_run(cmd, form=None, timeout=330):
    """x6100-wavelog's line; form: URL, key and certificate check typed on the page, not saved yet"""
    env = dict(os.environ, X6100_FIELD_DIR=settings.FIELD_DIR)
    if form:
        env.update(X6100_WAVELOG_URL=form["url"], X6100_WAVELOG_VERIFY="1" if form["verify_tls"] else "0")
        if form["key"]:
            env["X6100_WAVELOG_KEY"] = form["key"]
    try:   # the helper gives up after 300 s (its RUN_LIMIT): a little longer here, to hear why
        r = subprocess.run([settings.WAVELOG_CMD, cmd], capture_output=True, text=True, timeout=timeout, env=env)
        lines = r.stdout.strip().splitlines()
        return lines[-1] if lines else f"error exit code {r.returncode}"
    except subprocess.TimeoutExpired:
        # The helper's own timeouts leave out the name lookup: a name the network's DNS never answers holds it
        # past them. The page said "Wavelog: Command '['/usr/sbin/x6100-wavelog', 'test']' timed out after 40
        # seconds" (#73)
        return f"error no answer in {timeout} s: check the URL, and that the radio can reach that address"
    except (OSError, subprocess.SubprocessError) as e:
        return f"error {e}"


def wavelog_origin(url):
    """Scheme, host and port of a URL: the server a key is sent to (x6100-wavelog's origin)"""
    try:
        u = urllib.parse.urlsplit(url.strip())
        scheme = u.scheme.lower()
        return scheme, (u.hostname or "").lower(), u.port or {"http": 80, "https": 443}.get(scheme)
    except ValueError:   # a port that is not one: equal only to the same text
        return url.strip()


def wavelog_form(body, saved=None):
    """URL, key and certificate check of the page's form, checked; None and the reason when wrong. An empty key
    is the saved one, for the saved URL's server only: Test or Save with another URL sent it there (audit C08).
    saved: wavelog.conf as the caller read it, the one its key comes from"""
    url = str(body.get("url", "")).strip()
    key = str(body.get("key", "")).strip()
    if url and not re.fullmatch(WAVELOG_URL, url):
        return None, "The URL starts with http:// or https://, no spaces"
    if key and not re.fullmatch(r"[!-~]{1,128}", key):
        return None, "A key is 1-128 printable characters, no spaces"
    if key.lower().startswith("wl2_"):
        return None, WAVELOG_V2_TOKEN
    saved = wavelog_read() if saved is None else saved
    if url and not key and saved.get("key") and wavelog_origin(url) != wavelog_origin(saved.get("url", "")):
        return None, "Another server: type its API key too (the saved key goes only to the server it was saved with)"
    return {"url": url, "key": key, "verify_tls": body.get("verify_tls", True) is not False}, None


@app.route('/wavelog')
def wavelog_page():
    return bottle.template('wavelog')


@app.get('/api/wavelog')
def get_wavelog():
    return wavelog_state()


wavelog_lock = threading.Lock()


@app.post('/api/wavelog')
def set_wavelog():
    """{"enabled", "url", "key" ("": the one stored stays), "station_id", "station_name", "verify_tls"},
    or {"remove": true}: the key goes and the upload stops. Switched on, QSOs from now on are sent.
    One Save at a time, its URL checked against the wavelog.conf it keeps the key of: with a read for the check
    and one for the key, a Save of another server with its key between them gave this one's URL the old key"""
    with wavelog_lock:
        return wavelog_save(bottle.request.json or {})


def wavelog_save(body):
    """set_wavelog's work, under wavelog_lock"""
    conf = wavelog_read()
    if body.get("remove"):
        conf.pop("key", None)
        conf["enabled"] = "0"
    else:
        form, why = wavelog_form(body, conf)
        if not form:
            return api_error(400, why)
        sid = str(body.get("station_id", "")).strip()
        if sid and not (sid.isdigit() or sid == "auto"):   # auto: the profile with the QSO's station callsign
            return api_error(400, "No such station profile")
        name = "" if sid == "auto" else re.sub(r"[\r\n=]", " ", str(body.get("station_name", "")))[:60].strip()
        enabled = bool(body.get("enabled"))
        key = form["key"] or conf.get("key", "")
        if enabled and not (form["url"] and key and sid):
            return api_error(400, "To switch it on: the URL, an API key and a station profile")
        if enabled and conf.get("enabled") != "1":
            conf["since"] = str(int(time.time()))   # from now on: the log from before is not sent
        conf.update(url=form["url"], station_id=sid, station_name=name, verify_tls="1" if form["verify_tls"] else "0",
                    enabled="1" if enabled else "0")
        if key:
            conf["key"] = key
    try:
        wavelog_write(conf)
    except OSError as e:
        return api_error(500, f"Can't save: {e}")
    if conf.get("enabled") == "1":   # what waits goes now, not at the next QSO
        threading.Thread(target=wavelog_run, args=("sync",), daemon=True).start()
    return wavelog_state()


@app.post('/api/wavelog/stations')
def wavelog_stations():
    form, why = wavelog_form(bottle.request.json or {})
    if not form:
        return api_error(400, why)
    line = wavelog_run("stations", form, timeout=40)
    if not line.startswith("ok "):
        return api_error(502, "Wavelog: " + line.partition(" ")[2])
    try:
        return {"stations": json.loads(line[3:])}
    except ValueError:
        return api_error(502, "Wavelog: an answer the page can't read")


@app.post('/api/wavelog/test')
def wavelog_test():
    form, why = wavelog_form(bottle.request.json or {})
    if not form:
        return api_error(400, why)
    line = wavelog_run("test", form, timeout=40)
    if not line.startswith("ok "):
        return api_error(502, "Wavelog: " + line.partition(" ")[2])
    return {"version": line[3:]}


@app.post('/api/wavelog/sync')
def wavelog_sync():
    return wavelog_state(wavelog_run("sync"))


def field_active_log():
    """The log of the activation running on the radio ("" when none), from the GUI's state file."""
    try:
        with open(field_path("state"), errors="replace") as f:
            kv = dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
    except OSError:
        return ""
    return os.path.basename(kv.get("log", "")) if kv.get("active") == "1" else ""


# A log is handled as bytes from the file to the file: an ADIF length counts bytes, as the radio writes them
# (src/field/flog.c) and x6100-wavelog reads them, "Jürgen" is 7. Decoded first and counted in characters, a
# name with ü was read with the next tag's "<", an edit wrote 6 for it or took that "<" away, and the bytes of a
# log that is not UTF-8 were replaced in every record of the file (audit C10). Values are text only in and out.

def adif_split(data):
    """Header (up to <eoh>) and the parts between <eor>s; the last part is what follows the last record."""
    m = re.search(rb'<eoh>', data, re.I)
    head, body = (data[:m.end()], data[m.end():]) if m else (b"", data)
    return head, re.split(rb'(?i)<eor>', body)


def adif_tag(rec, name):
    """The tag <name:n> in rec and the end of its value, n bytes on (a slice there stops at the record's end)"""
    m = re.search(rb'<%s:(\d+)(?::\w)?>' % name.encode(), rec, re.I)
    return (m, m.end() + int(m.group(1))) if m else (None, 0)


def adif_set(rec, name, value):
    """rec with the field `name` set to value (added before the end when missing)."""
    v = value.encode()
    new = b"<%s:%d>%s" % (name.encode(), len(v), v)
    m, end = adif_tag(rec, name)
    if not m:
        return rec.rstrip() + b" " + new + b" "
    return rec[:m.start()] + new + rec[end:]


def adif_get(rec, name):
    """One field of rec, found by its own tag, as the radio finds it"""
    m, end = adif_tag(rec, name)
    return rec[m.end():end].decode(errors="replace") if m else ""


def adif_del(rec, name):
    """rec without the field `name`."""
    m, end = adif_tag(rec, name)
    return rec if not m else rec[:m.start()] + rec[end:]


SOTA_REF = r'[A-Z0-9]{1,4}/[A-Z]{2}-[0-9]{3}'     # I/LO-123 ([0-9]: \d takes any script's digits)
POTA_REF = r'[A-Z0-9]{1,4}-[0-9]{4,5}'            # IT-1234
# The ADIF SIG of each programme but SOTA (SOTA_REF), by the letter ref_prog gives
REF_SIG = {"P": "POTA", "B": "WWBOTA", "H": "HOTA", "F": "WWFF", "T": "TOTA", "C": "WCA", "M": "MOTA", "L": "ARLHS",
           "G": "TILES"}
TILE = r'[A-R]{2}[0-9]{2}[A-X]{2}'                # a Tiles tile, JO20ab (the references come here in capitals)


def ref_prog(ref):
    """The programme of a reference by its shape, as the radio tells it (src/field/flog.c field_ref_prog): S SOTA,
    P POTA, B WWBOTA, H HOTA, F WWFF, T TOTA, C WCA, M MOTA, L a lighthouse (BEL 002), G a Tiles tile (JO20ab); ""
    when it is none of them"""
    if re.fullmatch(r'X[0-9]{5}', ref):                                # X42018
        return "M"
    if re.fullmatch(TILE, ref.upper()):                                # JO20ab
        return "G"
    if re.fullmatch(r'[A-Z]{3} [0-9]{3,4}[A-Z]?', ref):                # BEL 002, USA 1286, AUS 079H
        return "L"
    if not 5 <= len(ref) <= 15 or "-" not in ref or not re.fullmatch(r'[A-Z0-9/-]+', ref):
        return ""
    pre, num = ref.split("-", 1)
    if ref.startswith("B/"):                                           # B/G-0070
        return "B"
    if "/" in ref:                                                     # I/LO-123
        return "S" if re.fullmatch(SOTA_REF, ref) else ""
    if len(pre) == 2 and num[:1] == "H":                               # RO-H0390
        return "H"
    if len(pre) >= 3 and pre.endswith("FF"):                           # DLFF-0123
        return "F"
    if 3 <= len(pre) <= 5 and pre.endswith("R") and re.fullmatch(r'[0-9]{4}', num):  # OKR-0150
        return "T"
    if 1 <= len(pre) <= 3 and re.fullmatch(r'[0-9]{5}', num) and (num[0] == "0" or pre == "I"):   # DL-01156
        return "C"
    if re.fullmatch(r'[A-Z]{3}', pre) and re.fullmatch(r'[0-9]{3,4}[A-Z]?', num):   # BEL-002 (written BEL 002)
        return "L"
    return "P" if re.fullmatch(POTA_REF, ref) else ""                 # US-1234


def ref_norm(ref):
    """A lighthouse as the radio writes it (src/field/flog.c field_ref_lh): BEL-2, BEL002, ARLHS BEL 002, LH BEL 2 and
    SCO-0144 are BEL 002 and SCO 144; a tile as Tiles writes it (field_ref_tile): JO20AB is JO20ab; any other
    reference as it is"""
    if re.fullmatch(TILE, ref.upper()):
        return ref[:4].upper() + ref[4:].lower()
    m = re.fullmatch(r'(?:(?:ARLHS[ _-]?|LH[ -]))?([A-Z]{3})[ -]?([0-9]{1,4})([A-Z]?)', ref)
    if m and ref_prog(ref) in ("L", ""):
        return f"{m[1]} {int(m[2]):03d}{m[3]}"
    return ref


def set_ref(rec, ref):
    """The other station's reference (its programme by its form, ref_prog); "" removes it. Written as the
    radio writes it (src/field/flog.c): SOTA_REF; SIG and SIG_INFO, with POTA_REF for a park and GRIDSQUARE for a
    tile."""
    if adif_get(rec, "SIG").upper() == "TILES" and adif_get(rec, "GRIDSQUARE").upper() == adif_get(rec, "SIG_INFO").upper():
        rec = adif_del(rec, "GRIDSQUARE")         # the tile they gave goes with it
    for name in ("SOTA_REF", "POTA_REF", "SIG", "SIG_INFO"):
        rec = adif_del(rec, name)
    ref = ref_norm(ref) if ref else ""   # a lighthouse as GMA's list and the radio write it: BEL 002
    prog = ref_prog(ref) if ref else ""
    if prog == "S":
        return adif_set(rec, "SOTA_REF", ref)
    if prog in REF_SIG:
        rec = adif_set(adif_set(rec, "SIG", REF_SIG[prog]), "SIG_INFO", ref)
        if prog == "G":
            rec = adif_set(rec, "GRIDSQUARE", ref)    # tile to tile: where they are
        return adif_set(rec, "POTA_REF", ref) if prog == "P" else rec
    return rec


def rec_id(rec):
    """A QSO's identity on the pages: a digest of its record as it stands in the file. The row
    number alone named another QSO once one before it was deleted in another tab (audit B09)."""
    return hashlib.sha1(rec.strip()).hexdigest()[:12]


def edit_qso(name, n, fn, rid):
    """Applies fn to the record with the identity rid (rec_id), of which n is the row number the
    page knew (0 = the first QSO of the file; records without CALL do not count): fn returns the
    new record, or None to delete it. A rid the file no longer holds is a 409: the page is stale.
    Returns an error response or None."""
    if not name.endswith(".adi") or os.path.basename(name) != name:
        return api_error(400, "Not a log")
    if not rid:
        return api_error(400, "Which QSO: the id is missing (reload the page)")
    path = field_path("logs", name)
    # The radio appends QSOs to the same file and takes this lock on the folder while it does
    # (src/field/flog.c): a QSO logged while the file is rewritten here would otherwise be lost
    # with the copy being replaced (audit A11)
    try:
        lock = os.open(os.path.dirname(path), os.O_RDONLY)
    except OSError:
        return api_error(404, "Not found")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except FileNotFoundError:
            return api_error(404, "Not found")
        head, parts = adif_split(raw)
        recs = [i for i, p in enumerate(parts[:-1]) if re.search(rb'<call:\d+', p, re.I)]
        same = [i for i in recs if rec_id(parts[i]) == rid]
        if len(same) == 1:
            at = same[0]
        elif same and 0 <= n < len(recs) and recs[n] in same:
            at = recs[n]      # identical records: the row the page meant
        else:
            return api_error(409, "This QSO is not in the log as the page shows it: reload the page")
        new = fn(parts[at])
        if new is None:
            del parts[at]
        else:
            parts[at] = new
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(head + b"<eor>".join(parts))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        os.close(lock)   # releases the lock
    return None


@app.delete('/api/field/logs/<name>/<n:int>')
def delete_qso(name, n):
    return edit_qso(name, n, lambda rec: None, bottle.request.query.get("id", "")) or {"status": "OK"}


@app.put('/api/field/logs/<name>/<n:int>')
def put_qso(name, n):
    """JSON {"call", "rst_sent", "rst_rcvd", "ref"?}: the QSO's call, reports and the other station's
    reference, of any programme ("" none; left as it is when missing)."""
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    if not isinstance(data, dict):
        return api_error(400, "Invalid QSO")
    call = str(data.get("call", "")).strip().upper()
    rs, rr = str(data.get("rst_sent", "")).strip(), str(data.get("rst_rcvd", "")).strip()
    rst = r'[+-]?\d{1,3}'     # 59, 599, FT8 dB (-10)
    if not re.fullmatch(r'[A-Z0-9/]{3,15}', call) or not re.fullmatch(rst, rs) or not re.fullmatch(rst, rr):
        return api_error(400, "Invalid QSO")
    ref = data.get("ref")
    if ref is not None:
        ref = str(ref).strip().upper()
        if ref and not ref_prog(ref_norm(ref)):
            return api_error(400, "Invalid reference")

    def change(rec):
        rec = adif_set(adif_set(adif_set(rec, "CALL", call), "RST_SENT", rs), "RST_RCVD", rr)
        return rec if ref is None else set_ref(rec, ref)
    return edit_qso(name, n, change, bottle.request.query.get("id", "")) or {"status": "OK"}


@app.delete('/api/field/logs/<name>')
def delete_field_log(name):
    if not name.endswith(".adi") or os.path.basename(name) != name:
        return api_error(400, "Not a log")
    if name == field_active_log():
        return api_error(409, "This activation is still running: end it on the radio first")
    try:
        os.remove(field_path("logs", name))
    except FileNotFoundError:
        return api_error(404, "Not found")
    return get_field()


# Logs page: every QSO of every log in <FIELD_DIR>/logs (the day's logs YYYYMMDD.adi and the activations),
# to see, filter, edit, delete and export. QSOs are addressed as (file, n) as edit_qso() counts them.

def adif_fields(rec):
    """The fields of an ADIF record, names in lower case, values as text; lengths in bytes (adif_split)."""
    out, i = {}, 0
    for m in re.finditer(rb'<(\w+):(\d+)(?::\w)?>', rec):
        if m.start() < i:
            continue
        n = int(m.group(2))
        out[m.group(1).decode().lower()] = rec[m.end():m.end() + n].decode(errors="replace")
        i = m.end() + n
    return out


def log_records(name):
    """The QSO records of a log, bytes (records without CALL do not count, as in edit_qso)."""
    try:
        with open(field_path("logs", name), "rb") as f:
            data = f.read()
    except OSError:
        return []
    return [p for p in adif_split(data)[1][:-1] if re.search(rb'<call:\d+', p, re.I)]


@app.route('/logs')
def logs_page():
    return bottle.template('logs')


@app.get('/api/logs')
def get_logs():
    qsos = []
    for log in field_logs():
        for n, rec in enumerate(log_records(log["name"])):
            q = adif_fields(rec)
            if q.get("submode"):   # MFSK with SUBMODE FT4, JS8, FT2: the submode names it
                q["mode"] = q["submode"]
            qsos.append(dict(q, file=log["name"], n=n, id=rec_id(rec)))
    return {"qsos": qsos, "active": field_active_log()}


# Editable fields and what they may hold (no < or >: they would break the ADIF tags)
LOG_FIELDS = {"call": r'[A-Z0-9/]{3,15}', "qso_date": r'\d{8}', "time_on": r'\d{4}(\d{2})?',
              "band": r'\d{1,4}(\.\d+)?C?M|', "freq": r'\d{1,4}(\.\d{1,6})?|', "mode": r'[A-Z0-9-]{1,12}|',
              "rst_sent": r'[+-]?\d{1,3}|', "rst_rcvd": r'[+-]?\d{1,3}|', "name": r'[^<>]{0,40}',
              "qth": r'[^<>]{0,40}', "gridsquare": r'([A-R]{2}\d{2}([A-X]{2})?)?', "comment": r'[^<>]{0,80}',
              "sota_ref": r'(%s)?' % SOTA_REF, "pota_ref": r'(%s)?' % POTA_REF,
              "ref": r'[A-Z0-9/ -]{0,15}'}   # the other station's reference, any programme (BEL 002): set_ref
# The modes ADIF files as a SUBMODE of MFSK (the radio logs FT4, JS8 and FT2 that way)
SUBMODE_OF = {"FT4": "MFSK", "JS8": "MFSK", "FT2": "MFSK", "FST4": "MFSK", "FST4W": "MFSK", "Q65": "MFSK", "MSK144": "MFSK"}


def log_unchanged(q, k, v):
    """Whether v, put_log_qso's cleaned value of field k, is what the Logs page showed of the record (its fields q,
    as get_logs gives them). The page sends every column back: one not edited is left as it is, bytes and all.
    Written again, a NAME or COMMENT that is not UTF-8 (a log from elsewhere) went back as U+FFFD, and the time
    lost its seconds (the page shows hh:mm), which made the QSO another one for the Wavelog upload (its qso_key)"""
    if k == "ref":   # logs.html ref(): SOTA_REF, POTA_REF, or SIG_INFO but a park's
        sig_info = "" if q.get("sig", "").upper() == "POTA" else q.get("sig_info", "")
        old = q.get("sota_ref") or q.get("pota_ref") or sig_info
    elif k == "mode":   # the submode where there is one
        old = q.get("submode") or q.get("mode", "")
    else:
        old = q.get(k, "")
    old = old.strip() if k in ("name", "qth", "comment") else old.strip().upper()
    if k == "time_on" and len(v) == 4:   # hh:mm on the page
        old = old[:4]
    return v == old


@app.put('/api/logs/<name>/<n:int>')
def put_log_qso(name, n):
    """JSON {field: value} for any of LOG_FIELDS; an empty value removes the field (CALL and the date stay)."""
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    if not isinstance(data, dict) or not data:
        return api_error(400, "Invalid QSO")
    clean = {}
    for k, v in data.items():
        v = str(v).strip()
        v = v if k in ("name", "qth", "comment") else v.upper()
        if k not in LOG_FIELDS or not re.fullmatch(LOG_FIELDS[k], v) or (k in ("call", "qso_date", "time_on") and not v) \
                or (k in ("ref", "sota_ref", "pota_ref") and v and not ref_prog(ref_norm(v))):
            return api_error(400, f"Invalid {k.upper()}: {v}")
        clean[k] = v
    # The other station's reference: "ref", or the sota_ref / pota_ref pair older pages send (one of
    # them, the other empty), written through set_ref so SIG and SIG_INFO follow
    if "ref" not in clean and ("sota_ref" in clean or "pota_ref" in clean):
        clean["ref"] = clean.get("sota_ref") or clean.get("pota_ref") or ""
    clean.pop("sota_ref", None)
    clean.pop("pota_ref", None)

    def change(rec):
        shown = adif_fields(rec)
        for k, v in clean.items():
            if log_unchanged(shown, k, v):
                continue
            if k == "ref":
                rec = set_ref(rec, v)
            elif k == "mode":
                # The page shows the submode where there is one (JS8, FT4) and sends that name
                # back: written as the MODE/SUBMODE pair ADIF defines, and a mode without a
                # submode takes the old SUBMODE with it (audit B17)
                if v in SUBMODE_OF:
                    rec = adif_set(adif_set(rec, "MODE", SUBMODE_OF[v]), "SUBMODE", v)
                else:
                    rec = adif_del(adif_set(rec, "MODE", v) if v else adif_del(rec, "MODE"), "SUBMODE")
            else:
                rec = adif_set(rec, k.upper(), v) if v else adif_del(rec, k.upper())
        return rec
    return edit_qso(name, n, change, bottle.request.query.get("id", "")) or {"status": "OK"}


@app.post('/logs/export')
def export_logs():
    """Form field sel = "file:id,file:id,...", fmt = adi | csv: those QSOs as one ADIF or CSV download."""
    want = {}
    for item in bottle.request.forms.get("sel", "").split(","):
        name, _, rid = item.rpartition(":")
        if name.endswith(".adi") and os.path.basename(name) == name and re.fullmatch(r'[0-9a-f]{12}', rid):
            want.setdefault(name, set()).add(rid)
    recs = [rec for name in sorted(want) for rec in log_records(name) if rec_id(rec) in want[name]]
    stamp = time.strftime("%Y%m%d-%H%M")
    if bottle.request.forms.get("fmt") == "csv":
        cols = ["qso_date", "time_on", "call", "band", "freq", "mode", "rst_sent", "rst_rcvd", "name", "qth",
                "gridsquare", "sota_ref", "pota_ref", "sig", "sig_info", "my_sota_ref", "my_pota_ref", "my_sig",
                "my_sig_info", "comment"]
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow([c.upper() for c in cols])
        for rec in recs:
            f = adif_fields(rec)
            w.writerow([f.get(c, "") for c in cols])
        body, ext, mime = out.getvalue(), "csv", "text/csv"
    else:
        body = b"OpenX6X00 log export\n<adif_ver:5>3.1.4 <programid:9>OpenX6X00 <eoh>\n" + \
               b"".join(rec.strip() + b" <eor>\n" for rec in recs)   # the records' bytes: their lengths hold
        ext, mime = "adi", "text/plain"
    bottle.response.content_type = mime + "; charset=utf-8"
    bottle.response.set_header("Content-Disposition", f'attachment; filename="x6100-log-{stamp}.{ext}"')
    return body


# The GUI's local configurator socket (src/cfg_socket.cpp): one JSON object per line, one answer line


def cfg_connect(timeout):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(settings.CFG_SOCK)
    except OSError:
        sock.close()
        raise
    return sock


# Memories: MEM button slots (params.db, table `memory`) and named channels
# (channels.json, same format as the radio GUI, src/channels.c)
# Names and enum values are x6100_mode_t / x6100_agc_t from x6100_control
MODES = ["LSB", "LSB-D", "USB", "USB-D", "CW", "CWR", "AM", "NFM", "SAM", "WFM"]
AGCS = ["OFF", "SLOW", "FAST", "AUTO"]
MEM_SLOTS = range(1, 10)  # MEM_HKEY_MAX_ID in src/main_screen.h; 10 is the GUI's backup slot
FREQ_MIN, FREQ_MAX = 100000, 56000000  # the GUI HF range (HF_MIN_FREQ)
CHANNELS_MAX = 100
CHANNEL_NAME_RE = re.compile(r"[ -~]{0,31}")


def parse_entry(data, named):
    """Validated slot/channel dict from request JSON; ValueError with a message."""
    if not isinstance(data, dict):
        raise ValueError("Invalid request")
    freq = data.get("frequency")
    if isinstance(freq, bool) or not isinstance(freq, int) or not FREQ_MIN <= freq <= FREQ_MAX:
        raise ValueError("Frequency must be between 0.5 and 56 MHz")
    if data.get("mode") not in MODES:
        raise ValueError("Invalid mode")
    if data.get("agc") not in AGCS:
        raise ValueError("Invalid AGC")
    for key in ("pre", "att"):
        if not isinstance(data.get(key), bool):
            raise ValueError(f"Invalid {key.upper()}")
    entry = {"frequency": freq, "mode": data["mode"], "agc": data["agc"], "pre": data["pre"], "att": data["att"]}
    if named:
        name = data.get("name")
        if not isinstance(name, str) or not CHANNEL_NAME_RE.fullmatch(name):
            raise ValueError("Invalid name: up to 31 printable ASCII characters")
        entry = {"name": name, **entry}
    return entry


def api_error(status, msg):
    bottle.response.status = status
    return {"status": "error", "msg": msg}


@app.route('/memories')
def memories():
    return bottle.template('memories')


def read_slots(dbcon):
    vals = {}
    for id_, name, val in dbcon.execute("SELECT id, name, val FROM memory"):
        vals.setdefault(id_, {})[name] = val
    slots = []
    for id_ in MEM_SLOTS:
        v = vals.get(id_, {})
        if "vfoa_freq" in v:  # the GUI treats a slot without vfoa_freq as empty
            slots.append({"id": id_, "frequency": v["vfoa_freq"],
                          "mode": MODES[v.get("vfoa_mode", 0) % len(MODES)],
                          "agc": AGCS[v.get("vfoa_agc", 0) % len(AGCS)],
                          "pre": bool(v.get("vfoa_pre", 0)), "att": bool(v.get("vfoa_att", 0))})
        else:
            slots.append({"id": id_, "frequency": None})
    return {"slots": slots}


@app.get('/api/memories')
def get_memories(dbcon):
    return read_slots(dbcon)


@app.put('/api/memories/<id:int>')
def set_memory(dbcon, id):
    if id not in MEM_SLOTS:
        return api_error(404, "No such slot")
    try:
        e = parse_entry(bottle.request.json, False)
    except ValueError as ex:
        return api_error(400, str(ex))
    vals = {"vfoa_freq": e["frequency"], "vfoa_mode": MODES.index(e["mode"]), "vfoa_agc": AGCS.index(e["agc"]),
            "vfoa_pre": int(e["pre"]), "vfoa_att": int(e["att"])}
    try:
        with dbcon:
            dbcon.executemany("INSERT OR REPLACE INTO memory(id, name, val) VALUES(?, ?, ?)",
                              [(id, k, v) for k, v in vals.items()])
    except sqlite3.Error as ex:
        return api_error(500, f"Can't save: {ex}")
    return read_slots(dbcon)


@app.delete('/api/memories/<id:int>')
def clear_memory(dbcon, id):
    if id not in MEM_SLOTS:
        return api_error(404, "No such slot")
    try:
        with dbcon:
            dbcon.execute("DELETE FROM memory WHERE id = ?", (id,))
    except sqlite3.Error as ex:
        return api_error(500, f"Can't save: {ex}")
    return read_slots(dbcon)


@app.route('/cw_messages')
def cw_messages():
    return bottle.template('cw_messages')


# Display: the screen options of Settings > Display, read and set live through the GUI socket
# (disp_get / disp_set, src/cfg_socket.cpp); the GUI checks names and ranges

def display_call(cmd):
    try:
        with cfg_connect(3) as sock, sock.makefile("rb") as f:
            sock.sendall(json.dumps(cmd).encode() + b"\n")
            return json.loads(f.readline() or b"{}")
    except (OSError, ValueError):
        return None


@app.route('/display')
def display():
    return bottle.template('display')


@app.get('/api/display')
def display_get():
    reply = display_call({"cmd": "disp_get"})
    if not reply or reply.get("t") != "disp":
        return api_error(503, "Radio GUI not reachable")
    reply.pop("t", None)
    return reply


@app.post('/api/display')
def display_set():
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    if not isinstance(data, dict) or not isinstance(data.get("name"), str) or len(data["name"]) > 23:
        return api_error(400, "Invalid option")
    cmd = {"cmd": "disp_set", "text": data["name"]}
    if data["name"] == "theme":
        if not isinstance(data.get("theme"), str) or len(data["theme"]) > 31:
            return api_error(400, "Invalid theme")
        cmd["theme"] = data["theme"]
    else:
        val = data.get("val")
        if not isinstance(val, int) or isinstance(val, bool):
            return api_error(400, "Invalid value")
        cmd["val"] = val
    reply = display_call(cmd)
    if reply is None:
        return api_error(503, "Radio GUI not reachable")
    if reply.get("t") != "ok":
        return api_error(409, reply.get("err", "Rejected by the radio"))
    return {"status": "OK"}


# Ethernet: the USB adapter's address, as Settings > Wireless > Ethernet. Read and applied through the
# GUI socket (eth_get / eth_set, src/cfg_socket.cpp); the GUI checks the values as its own page does

ETH_FIELDS = ("ip", "mask", "gw", "dns")


@app.route('/network')
def network():
    return bottle.template('network')


# The network name (rootfs/usr/sbin/x6100-hostname): hostname and mDNS, <name>.local
HOSTNAME_TOOL = "/usr/sbin/x6100-hostname"
HOSTNAME_RE = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9-]{0,30}[A-Za-z0-9])?$")


def hostname_state():
    try:
        name = subprocess.run([HOSTNAME_TOOL, "get"], capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        name = ""
    return {"name": name or socket.gethostname(), "mdns": os.path.exists("/usr/sbin/avahi-daemon")}


@app.get('/api/hostname')
def hostname_get():
    return hostname_state()


@app.post('/api/hostname')
def hostname_set():
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    name = data.get("name") if isinstance(data, dict) else None
    if not isinstance(name, str) or not HOSTNAME_RE.match(name.strip()):
        return api_error(400, "Letters, digits and '-', 1 to 32 characters, not starting or ending with '-'")
    try:
        r = subprocess.run([HOSTNAME_TOOL, "set", name.strip()], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        return api_error(503, "Can't set the name: %s" % e)
    if r.returncode != 0:
        return api_error(409, r.stderr.strip() or "Name not set")
    return hostname_state()


@app.get('/api/network')
def network_get():
    reply = display_call({"cmd": "eth_get"})
    if not reply or reply.get("t") != "eth":
        return api_error(503, "Radio GUI not reachable")
    reply.pop("t", None)
    return reply


@app.post('/api/network')
def network_set():
    try:
        data = bottle.request.json
    except ValueError:
        data = None
    if not isinstance(data, dict) or not isinstance(data.get("dhcp"), bool):
        return api_error(400, "Invalid mode")
    cmd = {"cmd": "eth_set", "dhcp": data["dhcp"]}
    for k in ETH_FIELDS:
        v = data.get(k, "")
        if not isinstance(v, str) or len(v) > 15:
            return api_error(400, "Invalid value")
        cmd[k] = v.strip()
    reply = display_call(cmd)
    if reply is None:
        return api_error(503, "Radio GUI not reachable")
    if reply.get("t") != "ok":
        return api_error(409, reply.get("err", "Rejected by the radio"))
    return {"status": "OK"}


CW_MSG_MAX = 64     # as the radio's editor (textarea_window)


def read_cw_messages(dbcon):
    return {"messages": [{"id": i, "text": t} for i, t in dbcon.execute("SELECT id, val FROM msg_cw ORDER BY id")],
            "max": CW_MSG_MAX}


def cw_text():
    """The text of the request: printable ASCII, 1 to CW_MSG_MAX characters."""
    data = bottle.request.json
    text = str(data.get("text", "")).strip() if isinstance(data, dict) else ""
    if not text or len(text) > CW_MSG_MAX or not all(" " <= c <= "~" for c in text):
        raise ValueError("1 to %d characters, letters, digits and symbols only" % CW_MSG_MAX)
    return text


@app.get('/api/cw_messages')
def get_cw_messages(dbcon):
    return read_cw_messages(dbcon)


@app.post('/api/cw_messages')
def add_cw_message(dbcon):
    try:
        text = cw_text()
        with dbcon:
            dbcon.execute("INSERT INTO msg_cw (val) VALUES (?)", (text,))
    except ValueError as ex:
        return api_error(400, str(ex))
    except sqlite3.Error as ex:
        return api_error(500, f"Can't save: {ex}")
    return read_cw_messages(dbcon)


@app.put('/api/cw_messages/<id:int>')
def set_cw_message(dbcon, id):
    try:
        text = cw_text()
        with dbcon:
            dbcon.execute("UPDATE msg_cw SET val = ? WHERE id = ?", (text, id))
    except ValueError as ex:
        return api_error(400, str(ex))
    except sqlite3.Error as ex:
        return api_error(500, f"Can't save: {ex}")
    return read_cw_messages(dbcon)


@app.delete('/api/cw_messages/<id:int>')
def delete_cw_message(dbcon, id):
    try:
        with dbcon:
            dbcon.execute("DELETE FROM msg_cw WHERE id = ?", (id,))
    except sqlite3.Error as ex:
        return api_error(500, f"Can't save: {ex}")
    return read_cw_messages(dbcon)


# Macros of the PSK31, Olivia and JTTY windows: one "name<TAB>text" line each, as src/psk_macro.c,
# olivia_macro.c and jtty_macro.c read and write them. The GUI reads the file when the window opens.
# (count, name, text): PSK_MACRO_MAX, _NAME_LEN - 1, _TEXT_LEN - 1 and the JTTY ones
MACRO_LIMITS = {"psk": (12, 23, 199), "olivia": (12, 23, 199), "jtty": (8, 23, 80)}


@app.route('/macros')
def macros_page():
    return bottle.template('macros')


def read_macros(mode):
    """The macros the GUI would load: lines without a tab skipped, at most the mode's count."""
    most, name_max, text_max = MACRO_LIMITS[mode]
    items = []
    try:
        with open(settings.MACRO_FILES[mode], encoding="latin-1") as f:
            for line in f:
                name, tab, text = line.rstrip("\r\n").partition("\t")
                if tab and len(items) < most:
                    items.append({"name": name[:name_max], "text": text[:text_max]})
    except FileNotFoundError:
        pass
    return items


def macros_state(mode):
    most, name_max, text_max = MACRO_LIMITS[mode]
    return {"macros": [dict(m, id=i) for i, m in enumerate(read_macros(mode))],
            "max": most, "name_max": name_max, "text_max": text_max}


def write_macros(mode, items):
    replace_file(settings.MACRO_FILES[mode],
                 lambda f: f.write("".join("%s\t%s\n" % (m["name"], m["text"]) for m in items)),
                 encoding="latin-1")


def macro_entry(mode):
    """Name and text of the request: printable ASCII, not empty, within the GUI's lengths."""
    _, name_max, text_max = MACRO_LIMITS[mode]
    data = bottle.request.json
    if not isinstance(data, dict):
        raise ValueError("Invalid request")
    name, text = str(data.get("name", "")).strip(), str(data.get("text", "")).strip()
    for what, value, most in (("Name", name, name_max), ("Text", text, text_max)):
        if not value or len(value) > most or not all(" " <= c <= "~" for c in value):
            raise ValueError("%s: 1 to %d characters, letters, digits and symbols only" % (what, most))
    return {"name": name, "text": text}


def change_macros(mode, fn):
    """Read the file, let fn change the list (ValueError: refused), write it back."""
    if mode not in settings.MACRO_FILES:
        return api_error(404, "No such mode")
    try:
        with config_lock:
            items = read_macros(mode)
            fn(items)
            write_macros(mode, items)
    except ValueError as ex:
        return api_error(400, str(ex))
    except OSError as ex:
        return api_error(500, f"Can't save: {ex}")
    return macros_state(mode)


def macro_index(items, id):
    if not 0 <= id < len(items):
        raise ValueError("That macro is gone, reload the page")
    return id


@app.get('/api/macros/<mode>')
def get_macros(mode):
    if mode not in settings.MACRO_FILES:
        return api_error(404, "No such mode")
    return macros_state(mode)


@app.post('/api/macros/<mode>')
def add_macro(mode):
    def add(items):
        if len(items) >= MACRO_LIMITS[mode][0]:
            raise ValueError("All %d macros are used, delete one first" % MACRO_LIMITS[mode][0])
        items.append(macro_entry(mode))
    return change_macros(mode, add)


@app.put('/api/macros/<mode>/<id:int>')
def set_macro(mode, id):
    def put(items):
        items[macro_index(items, id)] = macro_entry(mode)
    return change_macros(mode, put)


@app.delete('/api/macros/<mode>/<id:int>')
def delete_macro(mode, id):
    return change_macros(mode, lambda items: items.pop(macro_index(items, id)))


@app.post('/api/macros/<mode>/<id:int>/up')
def move_macro_up(mode, id):
    def up(items):
        i = macro_index(items, id)
        if i > 0:
            items[i - 1], items[i] = items[i], items[i - 1]
    return change_macros(mode, up)


# JS8 INFO and STATUS answers: the INFO= and STATUS= lines of js8_texts.txt (src/dialog_js8.c, TEXT_MAX),
# read when the JS8 window opens. The other lines (groups, alerts, spot form) are kept as they are.
JS8_TEXT_MAX = 64


def read_js8_texts():
    texts = {"info": "", "status": ""}
    try:
        with open(settings.JS8_TEXTS_FILE, encoding="latin-1") as f:
            for line in f:
                key, eq, val = line.rstrip("\r\n").partition("=")
                if eq and key in ("INFO", "STATUS"):
                    texts[key.lower()] = val[:JS8_TEXT_MAX]   # a key given twice: the last, as the GUI
    except FileNotFoundError:
        pass
    return dict(texts, max=JS8_TEXT_MAX)


@app.get('/api/js8_texts')
def get_js8_texts():
    return read_js8_texts()


@app.put('/api/js8_texts')
def set_js8_texts():
    data = bottle.request.json
    if not isinstance(data, dict):
        return api_error(400, "Invalid request")
    new = {}
    for key in ("info", "status"):
        val = str(data.get(key, "")).strip()
        if len(val) > JS8_TEXT_MAX or not all(" " <= c <= "~" for c in val):
            return api_error(400, "%s: up to %d characters, letters, digits and symbols only"
                             % (key.upper(), JS8_TEXT_MAX))
        new[key] = val
    path = settings.JS8_TEXTS_FILE
    try:
        try:
            with open(path, encoding="latin-1", newline="") as f:
                keep = [ln for ln in f if ln.partition("=")[0] not in ("INFO", "STATUS")]
        except FileNotFoundError:
            keep = []
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="latin-1", newline="") as f:
            f.write("INFO=%s\nSTATUS=%s\n" % (new["info"], new["status"]) + "".join(keep))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except OSError as ex:
        return api_error(500, f"Can't save: {ex}")
    return read_js8_texts()


def read_channels():
    """Valid channels from channels.json; invalid ones are skipped like the GUI does."""
    try:
        with open(settings.CHANNELS_FILE) as f:
            items = json.load(f)["channels"]
        items = [parse_entry(x, True) for x in items if isinstance(x, dict)] if isinstance(items, list) else []
    except (OSError, ValueError, KeyError, TypeError):
        return []
    return items[:CHANNELS_MAX]


def write_channels(channels):
    def write(f):
        json.dump({"version": 1, "channels": channels}, f, indent="\t")
        f.write("\n")
    replace_file(settings.CHANNELS_FILE, write)


def channels_state():
    return {"channels": read_channels(), "max": CHANNELS_MAX}


def channels_edit(change):
    """Read, change (raises ValueError), write, return the new list."""
    with config_lock:
        channels = read_channels()
        try:
            change(channels)
        except ValueError as ex:
            return api_error(400, str(ex))
        try:
            write_channels(channels)
        except OSError as ex:
            return api_error(500, f"Can't save: {ex}")
    return channels_state()


def check_index(channels, i):
    if not 0 <= i < len(channels):
        raise ValueError("No such channel")


@app.get('/api/channels')
def get_channels():
    return channels_state()


@app.post('/api/channels')
def add_channel():
    def change(channels):
        if len(channels) >= CHANNELS_MAX:
            raise ValueError(f"At most {CHANNELS_MAX} channels")
        channels.append(parse_entry(bottle.request.json, True))
    return channels_edit(change)


@app.put('/api/channels/<i:int>')
def set_channel(i):
    def change(channels):
        check_index(channels, i)
        channels[i] = parse_entry(bottle.request.json, True)
    return channels_edit(change)


@app.delete('/api/channels/<i:int>')
def delete_channel(i):
    def change(channels):
        check_index(channels, i)
        del channels[i]
    return channels_edit(change)


@app.post('/api/channels/<i:int>/move')
def move_channel(i):
    def change(channels):
        check_index(channels, i)
        to = (bottle.request.json or {}).get("to")
        if isinstance(to, bool) or not isinstance(to, int) or not 0 <= to < len(channels):
            raise ValueError("Invalid position")
        channels.insert(to, channels.pop(i))
    return channels_edit(change)


@app.route('/cluster')
def cluster():
    return bottle.template('cluster')


@app.get('/api/cluster')
def get_cluster():
    return cluster_state()


@app.post('/api/cluster/servers')
def add_cluster_server():
    data = bottle.request.json or {}
    name = str(data.get("name", "")).strip()
    host = str(data.get("host", "")).strip()
    if not name or len(name) > 31 or "|" in name or "\r" in name or "\n" in name:
        return cluster_error(400, "Invalid name: 1-31 characters, no '|'")
    # Hosts the GUI accepts (cluster_parse_server): letters, digits, '.', '-'
    if not re.fullmatch(r"[A-Za-z0-9.-]{1,127}", host):
        return cluster_error(400, "Invalid host: use letters, digits, '.' and '-'")
    try:
        port = int(str(data.get("port", "")).strip())
    except ValueError:
        port = 0
    if not 0 < port < 65536:
        return cluster_error(400, "Invalid port: 1-65535")

    def change(cur_host, cur_port, servers, opts):
        if len(servers) >= CLUSTER_MAX_SERVERS:
            return cluster_error(400, f"At most {CLUSTER_MAX_SERVERS} servers")
        if any(h == host and p == port for _, h, p in servers):
            return cluster_error(400, "This server is already in the list")
        servers.append((name, host, port))
    return cluster_update(change)


def cluster_text_ok(text, max_len):
    return len(text) <= max_len and not any(c in text for c in "|\r\n")


@app.post('/api/cluster/filters')
def set_cluster_filters():
    data = bottle.request.json or {}
    modes = data.get("modes", [])
    if not isinstance(modes, list) or any(m not in CLUSTER_MODES for m in modes):
        return cluster_error(400, "Invalid modes")
    if not modes:
        return cluster_error(400, "Select at least one mode")
    try:
        max_age = int(data.get("max_age", 0))
    except (TypeError, ValueError):
        max_age = -1
    if not 0 <= max_age < 1440:
        return cluster_error(400, "Invalid max age")
    skip = ",".join(w.strip() for w in str(data.get("skip", "")).split(",") if w.strip())
    if not cluster_text_ok(skip, 120) or not re.fullmatch(r"[A-Za-z0-9/*#@,-]*", skip):
        return cluster_error(400, "Invalid spotters: letters, digits, '*', '#', '-', '/' separated by commas")
    # "PA, on" or "PA ON" -> "PA,ON"
    spotter_dxcc = ",".join(w.upper() for w in re.split(r"[,\s]+", str(data.get("spotter_dxcc", ""))) if w)
    if not CLUSTER_SPOTTER_DXCC_RE.fullmatch(spotter_dxcc):
        return cluster_error(400, "Heard in: up to 16 prefixes (letters, digits, '/') separated by commas, e.g. PA, ON")
    bands = data.get("bands", [])
    activity = data.get("activity", [])
    if not isinstance(bands, list) or any(not isinstance(b, int) or b not in CLUSTER_BANDS for b in bands):
        return cluster_error(400, "Invalid bands")
    if not isinstance(activity, list) or any(a not in CLUSTER_ACTIVITY for a in activity):
        return cluster_error(400, "Invalid activity")

    def change(host, port, servers, opts):
        opts.update(modes=[m for m in CLUSTER_MODES if m in modes], max_age=max_age, skip=skip, spotter_dxcc=spotter_dxcc,
                    bands=[b for b in CLUSTER_BANDS if b in bands], activity=[a for a in CLUSTER_ACTIVITY if a in activity])
    return cluster_update(change)


def cluster_alert_input(data):
    """(alert, None) from a request, or (None, error response)."""
    pattern = str(data.get("pattern", "")).strip().upper()
    if not pattern or len(pattern) > 31 or not re.fullmatch(r"[A-Z0-9/*]+", pattern):
        return None, cluster_error(400, "Invalid pattern: 1-31 characters, letters, digits, '/' and '*'")
    bands = data.get("bands", [])
    if not isinstance(bands, list):
        return None, cluster_error(400, "Invalid bands")
    try:
        bands = sorted({int(b) for b in bands}, reverse=True)
    except (TypeError, ValueError):
        return None, cluster_error(400, "Invalid bands")
    if any(b not in CLUSTER_BANDS for b in bands):
        return None, cluster_error(400, "Invalid bands")
    mode = str(data.get("mode", "")).strip().upper()
    if mode not in ("", "CW", "SSB", "DIG"):
        return None, cluster_error(400, "Invalid mode")
    return {"pattern": pattern, "bands": ",".join(str(b) for b in bands), "mode": mode,
            "enabled": data.get("enabled", True) is not False}, None


def cluster_alert_dup(alerts, a):
    return any(x["pattern"].upper() == a["pattern"] and x["bands"] == a["bands"] and x["mode"] == a["mode"] for x in alerts)


@app.post('/api/cluster/alerts')
def add_cluster_alert():
    a, err = cluster_alert_input(bottle.request.json or {})
    if err:
        return err

    def change(host, port, servers, opts):
        if len(opts["alerts"]) >= CLUSTER_MAX_ALERTS:
            return cluster_error(400, f"At most {CLUSTER_MAX_ALERTS} alerts")
        if cluster_alert_dup(opts["alerts"], a):
            return cluster_error(400, "This alert is already in the list")
        opts["alerts"].append(a)
    return cluster_update(change)


# Edit an alert, or turn it off and on ("enabled"; an alert off stays in the list as alert_off=)
@app.put('/api/cluster/alerts/<index:int>')
def edit_cluster_alert(index):
    a, err = cluster_alert_input(bottle.request.json or {})
    if err:
        return err

    def change(host, port, servers, opts):
        if not 0 <= index < len(opts["alerts"]):
            return cluster_error(404, "No such alert")
        if cluster_alert_dup(opts["alerts"][:index] + opts["alerts"][index + 1:], a):
            return cluster_error(400, "This alert is already in the list")
        opts["alerts"][index] = a
    return cluster_update(change)


@app.delete('/api/cluster/alerts/<index:int>')
def delete_cluster_alert(index):
    def change(host, port, servers, opts):
        if not 0 <= index < len(opts["alerts"]):
            return cluster_error(404, "No such alert")
        del opts["alerts"][index]
    return cluster_update(change)


@app.post('/api/cluster/alert-options')
def set_cluster_alert_options():
    data = bottle.request.json or {}

    def change(host, port, servers, opts):
        opts.update(popup=bool(data.get("popup")), beep=bool(data.get("beep")))
    return cluster_update(change)


@app.post('/api/cluster/link')
def set_cluster_link():
    """The cluster login on or off, and the call it logs in with (empty: the station's call)"""
    data = bottle.request.json or {}
    login = str(data.get("login", "")).strip().upper()
    if login and not CLUSTER_LOGIN_RE.fullmatch(login):
        return cluster_error(400, "Login: up to 15 letters, digits, / or -, e.g. ON6URE-2")

    def change(host, port, servers, opts):
        opts.update(enabled=bool(data.get("enabled", True)), login=login)
    return cluster_update(change)


@app.get('/api/cluster/link')
def get_cluster_link():
    """What the radio's cluster link is doing, as APP Cluster's status line says it (issue #23). "stale": the
    radio has not taken up what cluster.conf says (another server, the link on or off) a while after the change"""
    try:
        with open(CLUSTER_LINK, errors="replace") as f:
            state, server, text = (f.readline().rstrip("\n").split("|", 2) + ["", ""])[:3]
    except OSError:
        return {"state": "", "server": "", "text": "", "stale": False}   # the GUI has not written it (yet)
    host, port, _ = read_cluster_conf()
    enabled = read_cluster_options()["enabled"]
    try:
        settled = time.time() - os.stat(settings.CLUSTER_CONF).st_mtime > CLUSTER_LINK_SETTLE_S
    except OSError:
        settled = True
    if state == "off":
        behind = enabled
    elif state == "nocall":   # no callsign: the radio holds no link whatever the file says
        behind = False
    else:
        behind = not enabled or server != f"{host}:{port}"
    return {"state": state, "server": server, "text": text, "stale": behind and settled}


@app.post('/api/cluster/use/<index:int>')
def use_cluster_server(index):
    def change(host, port, servers, opts):
        if not 0 <= index < len(servers):
            return cluster_error(404, "No such server")
        return servers[index][1:]
    return cluster_update(change)


@app.delete('/api/cluster/servers/<index:int>')
def delete_cluster_server(index):
    def change(host, port, servers, opts):
        if not 0 <= index < len(servers):
            return cluster_error(404, "No such server")
        if servers[index][1:] == (host, port):
            return cluster_error(400, "This server is in use: choose another one first")
        del servers[index]
    return cluster_update(change)


# Gallery: screen recordings, audio recordings and screenshots

GALLERY_DIRS = {"screenrec": ("screenrec", (".avi", ".mp4")), "rec": ("rec", (".mp3", ".wav")), "shots": ("", (".png",)),
                "sstv": ("sstv", (".png",)), "sstv_tx": ("sstv/tx_pics", (".png",)),
                "wefax": ("wefax", (".png",)), "wefax_tx": ("wefax/tx_pics", (".png",))}
GALLERY_NAME = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._ -]*$')


def gallery_path(kind, name):
    """Real path of a gallery file, 404 for anything else (no separators, no dot dirs)."""
    sub, exts = GALLERY_DIRS.get(kind, (None, ()))
    if sub is None or not GALLERY_NAME.match(name) or not name.lower().endswith(exts):
        raise bottle.HTTPError(404, "Not found")
    root = os.path.realpath(os.path.join(settings.FILEBROWSER_PATH, sub))
    path = os.path.realpath(os.path.join(root, name))
    if os.path.dirname(path) != root or not os.path.isfile(path):
        raise bottle.HTTPError(404, "Not found")
    return root, path


def avi_first_frame(path):
    """First 00dc (JPEG) chunk of an AVI, or None."""
    with open(path, 'rb') as f:
        head = f.read(12)
        if head[:4] != b'RIFF' or head[8:12] != b'AVI ':
            return None
        while True:
            head = f.read(8)
            if len(head) < 8:
                return None
            fourcc, size = head[:4], int.from_bytes(head[4:], 'little')
            if fourcc == b'LIST':
                if f.read(4) != b'movi':    # go into movi, skip hdrl
                    f.seek(size - 4, os.SEEK_CUR)
            elif fourcc == b'00dc':
                return f.read(size)
            else:
                f.seek(size + (size & 1), os.SEEK_CUR)


@app.route('/gallery')
def gallery():
    return bottle.template('gallery')


@app.get('/api/gallery')
def get_gallery():
    items = []
    for kind, (sub, exts) in GALLERY_DIRS.items():
        if bottle.request.query.get("kind", kind) != kind:
            continue
        folder = os.path.join(settings.FILEBROWSER_PATH, sub)
        try:
            names = os.listdir(folder)
        except OSError:
            continue
        for name in names:
            if not GALLERY_NAME.match(name) or not name.lower().endswith(exts):
                continue
            try:
                st = os.stat(os.path.join(folder, name))
            except OSError:
                continue
            items.append({"kind": kind, "name": name, "size": st.st_size, "mtime": int(st.st_mtime)})
    items.sort(key=lambda x: (x["mtime"], x["name"]), reverse=True)   # same second: sstv-X_dn.png above sstv-X.png
    bottle.response.content_type = 'application/json'
    return json.dumps(items)


@app.get('/gallery/file/<kind>/<name>')
def gallery_file(kind, name):
    root, path = gallery_path(kind, name)
    os.sync()
    response = bottle.static_file(name, root=root, download=bool(bottle.request.query.get('dl')))
    response.set_header("Cache-Control", "private, no-cache, no-store")
    return response


@app.get('/gallery/thumb/<kind>/<name>')
def gallery_thumb(kind, name):
    root, path = gallery_path(kind, name)
    if kind in ("shots", "sstv", "sstv_tx", "wefax", "wefax_tx"):
        return bottle.static_file(name, root=root)
    jpeg = avi_first_frame(path) if kind == "screenrec" else None
    if not jpeg:
        raise bottle.HTTPError(404, "No frame")
    bottle.response.content_type = 'image/jpeg'
    bottle.response.set_header("Cache-Control", "private, max-age=3600")
    return jpeg


@app.delete('/api/gallery/<kind>/<name>')
def delete_gallery_file(kind, name):
    root, path = gallery_path(kind, name)
    os.remove(path)
    return {"status": "OK"}


GALLERY_BULK_MAX = 1000


def gallery_selection(kind, names, items):
    """[(kind, name)] from kind + names and/or items ("kind/name"); ValueError when malformed.
    Names themselves are checked by gallery_path."""
    if not isinstance(names, list) or not isinstance(items, list) or not all(isinstance(x, str) for x in names + items):
        raise ValueError("Invalid request")
    if names and kind not in GALLERY_DIRS:
        raise ValueError("Invalid kind")
    if any("/" not in x for x in items):
        raise ValueError("Invalid item")
    pairs = list(dict.fromkeys([(kind, n) for n in names] + [tuple(x.split("/", 1)) for x in items]))
    if not 0 < len(pairs) <= GALLERY_BULK_MAX:
        raise ValueError(f"Select 1 to {GALLERY_BULK_MAX} files")
    return pairs


@app.post('/api/gallery/delete')
def delete_gallery_files():
    """JSON {"kind", "names": [...]} or {"items": ["kind/name", ...]}; results per file."""
    try:
        data = bottle.request.json
        if not isinstance(data, dict):
            raise ValueError("Invalid request")
        pairs = gallery_selection(data.get("kind"), data.get("names", []), data.get("items", []))
    except ValueError as e:
        return api_error(400, str(e))
    deleted, failed = [], []
    for kind, name in pairs:
        try:
            os.remove(gallery_path(kind, name)[1])
            deleted.append({"kind": kind, "name": name})
        except bottle.HTTPError:
            failed.append({"kind": kind, "name": name, "msg": "Not found"})
        except OSError as e:
            failed.append({"kind": kind, "name": name, "msg": e.strerror or str(e)})
    return {"status": "OK", "deleted": deleted, "failed": failed}


class ZipSink:
    """Write-only file for zipfile: no seek, so it writes data descriptors and the
    zip can be sent while it's made (nothing in RAM or on the SD card)."""

    def __init__(self):
        self.parts = []

    def write(self, data):
        self.parts.append(bytes(data))
        return len(data)

    def flush(self):
        pass

    def take(self):
        data, self.parts = b"".join(self.parts), []
        return data


def zip_stream(files):
    """Stored (the files are already compressed) zip of [(name in the zip, path)], in pieces."""
    sink = ZipSink()
    with zipfile.ZipFile(sink, "w") as zf:
        for arcname, path in files:
            info = zipfile.ZipInfo.from_file(path, arcname, strict_timestamps=False)   # radio clock may be 1970
            with open(path, "rb") as src, zf.open(info, "w") as dst:
                for chunk in iter(lambda: src.read(65536), b""):
                    dst.write(chunk)
                    yield sink.take()
    yield sink.take()


@app.route('/api/gallery/zip', method=['GET', 'POST'])
def zip_gallery_files():
    """Fields kind + names (repeated) and/or items ("kind/name", repeated); files gone meanwhile are skipped."""
    params = bottle.request.params
    try:
        pairs = gallery_selection(params.get("kind"), params.getall("names"), params.getall("items"))
    except ValueError as e:
        return api_error(400, str(e))
    files = []
    for kind, name in pairs:
        try:
            files.append((kind, name, gallery_path(kind, name)[1]))
        except bottle.HTTPError:
            pass
    if not files:
        return api_error(404, "Not found")
    one = len({k for k, _, _ in files}) == 1
    os.sync()
    zip_name = f"x6100-{files[0][0] if one else 'all'}-{datetime.now().strftime('%Y%m%d-%H%M%S')}.zip"
    bottle.response.content_type = "application/zip"
    bottle.response.set_header("Content-Disposition", f'attachment; filename="{zip_name}"')
    bottle.response.set_header("Cache-Control", "private, no-cache, no-store")
    return zip_stream([(name if one else f"{kind}/{name}", path) for kind, name, path in files])


SSTV_TX_MAX = 4 * 1024 * 1024


@app.post('/api/sstv/tx_pics')
def upload_sstv_tx_pic():
    """A PNG for the radio's SSTV TX pictures (multipart field "file")"""
    return upload_tx_pic("sstv_tx")


@app.post('/api/wefax/tx_pics')
def upload_wefax_tx_pic():
    """A PNG for the radio's WeFax TX pictures (multipart field "file")"""
    return upload_tx_pic("wefax_tx")


def upload_tx_pic(kind):
    upload = bottle.request.files.get("file")
    if upload is None:
        return api_error(400, "No file")
    name = upload.raw_filename
    if not GALLERY_NAME.match(name) or not name.endswith(".png"):
        return api_error(400, "Invalid file name")
    data = upload.file.read(SSTV_TX_MAX + 1)
    if len(data) > SSTV_TX_MAX:
        return api_error(413, "Larger than 4 MB")
    if not data.startswith(b"\x89PNG"):
        return api_error(400, "Not a PNG")
    folder = os.path.join(settings.FILEBROWSER_PATH, GALLERY_DIRS[kind][0])
    os.makedirs(folder, exist_ok=True)
    if bottle.request.query.get("unique"):   # the gallery page: keep an existing file, name-1.png, name-2.png...
        # ponytail: check then replace, two uploads of the same name at the same moment can still clash
        stem, n = name[:-4], 1
        while os.path.exists(os.path.join(folder, name)):
            name, n = f"{stem}-{n}.png", n + 1
    tmp = os.path.join(folder, "." + name + ".tmp")   # hidden: the radio and the gallery skip it
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, os.path.join(folder, name))
    return {"status": "OK", "name": name}


# Backup / restore routes
# A backup is a tar(.gz) with manifest.json and a fixed set of member names. An uploaded
# archive is never extracted by tarfile: only whitelisted names are copied, to a work dir.

BACKUP_MAX_BYTES = 50 * 1024 * 1024   # upload size and total unpacked size
BACKUP_MAX_MEMBERS = 200
BACKUP_TEXT_MAX = 1024 * 1024         # any member that is not a .db or .mp3
BACKUP_NAME = r"[A-Za-z0-9][A-Za-z0-9 ._-]{0,59}"
BACKUP_DIRS = (("voice", "msg/", re.compile("msg/" + BACKUP_NAME + r"\.mp3")),
               ("wifi", "wifi/", re.compile("wifi/" + BACKUP_NAME + r"\.nmconnection")))
BACKUP_GUI_PARTS = ("settings", "memories", "qso", "cw")   # files the GUI owns: restore with it stopped
TAR_ERRORS = (tarfile.TarError, EOFError, OSError) + ((zlib.error,) if zlib else ())

backup_lock = threading.Lock()


class BackupError(Exception):
    pass


def backup_files():
    """Fixed members: name -> (part, path on the radio). memory_slots.json is made from params.db."""
    return {"params.db": ("settings", settings.DB_PATH),
            "channels.json": ("memories", settings.CHANNELS_FILE),
            "memory_slots.json": ("memories", settings.DB_PATH),
            "cluster.conf": ("cluster", settings.CLUSTER_CONF),
            "tci.conf": ("tci", settings.TCI_CONF),
            "qso_log.db": ("qso", settings.QSO_LOG_DB),
            "cw_trainer.json": ("cw", settings.CW_TRAINER_FILE)}


def backup_member(name):
    """(part, path on the radio) of a member name, None when it isn't a known one."""
    if name in backup_files():
        return backup_files()[name]
    for part, prefix, pattern in BACKUP_DIRS:
        if pattern.fullmatch(name):
            return part, os.path.join(settings.VOICE_DIR if part == "voice" else settings.WIFI_DIR, name[len(prefix):])
    return None


def backup_current():
    """member -> path of everything the radio has now."""
    files = {n: p for n, (_, p) in backup_files().items() if os.path.isfile(p)}
    for part, prefix, pattern in BACKUP_DIRS:
        d = settings.VOICE_DIR if part == "voice" else settings.WIFI_DIR
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        files.update({prefix + n: os.path.join(d, n) for n in names if pattern.fullmatch(prefix + n)})
    return files


def plural(n, word):
    return f"{n} {word}" + ("" if n == 1 else "s")


def fmt_size(n):
    return f"{n / 1048576:.1f} MB" if n >= 1048576 else f"{max(n // 1024, 1)} kB" if n else "0 kB"


def read_text(path):
    with open(path, errors="replace") as f:
        return f.read(BACKUP_TEXT_MAX)


def memory_rows(path):
    """[[id, name, val]] of the MEM slots, from memory_slots.json or from a params.db."""
    if path.endswith(".json"):
        with open(path) as f:
            return json.load(f)["rows"]
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        return [list(r) for r in con.execute("SELECT id, name, val FROM memory")]
    finally:
        con.close()


def describe_backup(files, date=None):
    """part -> one line summary of the members in files (member -> path); date overrides the file time."""
    def when(member):
        return date or datetime.fromtimestamp(os.path.getmtime(files[member])).strftime("%Y-%m-%d %H:%M")

    def count(member, sql):
        con = sqlite3.connect(f"file:{files[member]}?mode=ro", uri=True)
        try:
            return con.execute(sql).fetchone()[0]
        finally:
            con.close()

    out = {}
    try:
        if "params.db" in files:
            out["settings"] = when("params.db")
        if "channels.json" in files or "memory_slots.json" in files:
            slots = len({r[0] for r in memory_rows(files["memory_slots.json"]) if r[1] == "vfoa_freq" and r[0] in MEM_SLOTS}) \
                if "memory_slots.json" in files else 0
            try:
                with open(files["channels.json"]) as f:
                    chans = len(json.load(f)["channels"])
            except (KeyError, OSError, ValueError, TypeError):
                chans = 0
            out["memories"] = f"{plural(slots, 'slot')}, {plural(chans, 'channel')}"
        if "cluster.conf" in files:
            lines = read_text(files["cluster.conf"]).splitlines()
            servers = sum(1 for x in lines if x.startswith("server="))
            alerts = sum(1 for x in lines if x.startswith("alert"))
            out["cluster"] = plural(servers, "server") + (f", {plural(alerts, 'alert')}" if alerts else "")
        if "tci.conf" in files:
            on = "enabled=1" in read_text(files["tci.conf"]).splitlines()
            out["tci"] = "server enabled" if on else "server disabled"
        if "qso_log.db" in files:
            out["qso"] = plural(count("qso_log.db", "SELECT count(*) FROM qso_log"), "QSO")
        voice = [n for n in files if n.startswith("msg/")]
        if voice:
            size = sum(os.path.getsize(files[n]) for n in voice)
            out["voice"] = f"{plural(len(voice), 'message')}, {fmt_size(size)}"
        if "cw_trainer.json" in files:
            out["cw"] = when("cw_trainer.json")
        wifi = [n for n in files if n.startswith("wifi/")]
        if wifi:
            out["wifi"] = plural(len(wifi), "network")
    except (OSError, ValueError, KeyError, TypeError, sqlite3.Error):
        pass   # a part that can't be read just has no summary
    return out


def check_sqlite(path, table):
    try:
        con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        try:
            ok = con.execute("PRAGMA integrity_check").fetchall() == [("ok",)]
            ok = ok and con.execute("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", (table,)).fetchone()
        finally:
            con.close()
    except sqlite3.Error:
        ok = False
    if not ok:
        raise BackupError(f"{os.path.basename(path)} in the file is not a valid database")


def check_member(name, path):
    """Content check of one unpacked member; raises BackupError."""
    try:
        if name == "params.db":
            check_sqlite(path, "params")
        elif name == "qso_log.db":
            check_sqlite(path, "qso_log")
        elif name == "memory_slots.json":
            with open(path) as f:
                rows = json.load(f)["rows"]
            if not isinstance(rows, list) or len(rows) > 1000 or not all(
                    isinstance(r, list) and len(r) == 3 and isinstance(r[0], int) and isinstance(r[1], str) and len(r[1]) <= 32
                    and isinstance(r[2], (int, float)) and not isinstance(r[2], bool) for r in rows):
                raise ValueError
        elif name == "channels.json":
            with open(path) as f:
                if not isinstance(json.load(f)["channels"], list):
                    raise ValueError
        elif name == "cw_trainer.json":
            with open(path) as f:
                json.load(f)
        elif name.endswith((".conf", ".nmconnection")):
            with open(path, "rb") as f:
                data = f.read()
            if b"\0" in data or (name.endswith(".nmconnection") and b"[connection]" not in data):
                raise ValueError
    except (OSError, ValueError, KeyError, TypeError):
        raise BackupError(f"{name} in the file is not valid")


def unpack_backup(archive, stage):
    """Checks a backup and copies its members into stage. Returns the manifest."""
    with open(archive, "rb") as f:
        gz = f.read(2) == b"\x1f\x8b"
    if gz and not zlib:
        raise BackupError("This radio image can't read .tar.gz backups: upload a plain .tar")
    try:
        with tarfile.open(archive, "r:gz" if gz else "r:") as tf:
            seen, total = set(), 0
            for m in tf:
                if len(seen) >= BACKUP_MAX_MEMBERS:
                    raise BackupError("Too many items in the file")
                if m.name != "manifest.json" and not backup_member(m.name):
                    raise BackupError(f"Unexpected item in the file: {m.name[:60]!r}")
                if m.type not in (tarfile.REGTYPE, tarfile.AREGTYPE) or m.name in seen:
                    raise BackupError(f"Unexpected item in the file: {m.name[:60]!r}")
                total += m.size
                if total > BACKUP_MAX_BYTES or (m.size > BACKUP_TEXT_MAX and not m.name.endswith((".db", ".mp3"))):
                    raise BackupError("The file is too large")
                seen.add(m.name)
                dst = os.path.join(stage, m.name)
                os.makedirs(os.path.dirname(dst), mode=0o700, exist_ok=True)
                with tf.extractfile(m) as src, open(dst, "wb") as out:
                    shutil.copyfileobj(src, out)
                    out.flush()
                    os.fsync(out.fileno())
    except TAR_ERRORS as e:
        raise BackupError(f"Not a valid backup file ({e.__class__.__name__})")
    try:
        with open(os.path.join(stage, "manifest.json")) as f:
            manifest = json.load(f)
        if manifest["version"] != 1 or not isinstance(manifest["date"], str):
            raise ValueError
    except (OSError, ValueError, KeyError, TypeError):
        raise BackupError("Not an X6100 backup: manifest.json is missing or has an unknown version")
    for name in seen - {"manifest.json"}:
        check_member(name, os.path.join(stage, name))
    return manifest


def backup_error(status, msg):
    bottle.response.status = status
    return {"status": "error", "msg": msg}


@app.route('/backup')
def backup():
    return bottle.template('backup')


@app.get('/api/backup')
def get_backup():
    return {"parts": describe_backup(backup_current())}


@app.get('/api/backup/download')
def download_backup():
    parts = [p for p in bottle.request.query.get("parts", "").split(",") if p]
    files = {n: p for n, p in backup_current().items() if backup_member(n)[0] in parts}
    if not files:
        return backup_error(400, "Nothing to back up")
    now = datetime.now()
    os.makedirs(settings.BACKUP_WORK, mode=0o700, exist_ok=True)
    out = tempfile.TemporaryFile(dir=settings.BACKUP_WORK)
    try:
        with tempfile.TemporaryDirectory(dir=settings.BACKUP_WORK) as tmp:
            for name, path in list(files.items()):
                copy = os.path.join(tmp, name.replace("/", "_"))
                if name == "memory_slots.json":
                    with open(copy, "w") as f:
                        json.dump({"rows": memory_rows(path)}, f)
                elif name.endswith(".db"):   # consistent copy while the GUI has it open
                    src, dst = sqlite3.connect(f"file:{path}?mode=ro", uri=True), sqlite3.connect(copy)
                    src.backup(dst)
                    src.close()
                    dst.close()
                else:
                    shutil.copyfile(path, copy)
                files[name] = copy
            callsign = read_callsign()
            call = re.sub(r"[^A-Za-z0-9_-]", "-", callsign)
            manifest = {"version": 1, "date": now.strftime("%Y-%m-%d %H:%M"), "callsign": callsign,
                        "parts": describe_backup(files, now.strftime("%Y-%m-%d %H:%M"))}
            with tarfile.open(fileobj=out, mode="w:gz" if zlib else "w") as tf:
                data = json.dumps(manifest, indent=1).encode()
                info = tarfile.TarInfo("manifest.json")
                info.size, info.mtime = len(data), int(now.timestamp())
                tf.addfile(info, io.BytesIO(data))
                for name, copy in files.items():
                    info = tf.gettarinfo(copy, name)
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mode = 0o600
                    with open(copy, "rb") as f:
                        tf.addfile(info, f)
    except (OSError, sqlite3.Error, ValueError) as e:
        out.close()
        return backup_error(500, f"Can't make the backup: {e}")
    out.seek(0, os.SEEK_END)
    size = out.tell()
    out.seek(0)
    ext = "tar.gz" if zlib else "tar"
    name = f"x6100-backup-{call + '-' if call else ''}{now.strftime('%Y-%m-%d')}.{ext}"
    bottle.response.content_type = "application/gzip" if zlib else "application/x-tar"
    bottle.response.set_header("Content-Disposition", f'attachment; filename="{name}"')
    bottle.response.set_header("Content-Length", str(size))
    return out


def backup_stage():
    return os.path.join(settings.BACKUP_WORK, "stage")


@app.post('/api/backup/upload')
def upload_backup():
    length = bottle.request.content_length
    if length <= 0 or length > BACKUP_MAX_BYTES:
        return backup_error(413, "The file is empty or larger than 50 MB")
    if not backup_lock.acquire(blocking=False):
        return backup_error(409, "Another backup operation is running")
    try:
        stage = backup_stage()
        stage_clear()
        os.makedirs(stage, mode=0o700)
        archive = os.path.join(settings.BACKUP_WORK, "upload.tar")
        try:
            with open(archive, "wb") as f:
                left, src = length, bottle.request.environ["wsgi.input"]
                while left > 0:
                    chunk = src.read(min(left, 65536))
                    if not chunk:
                        raise BackupError("Upload interrupted")
                    f.write(chunk)
                    left -= len(chunk)
            manifest = unpack_backup(archive, stage)
        except (BackupError, OSError) as e:
            shutil.rmtree(stage, ignore_errors=True)
            return backup_error(400, str(e))
        finally:
            try:
                os.remove(archive)
            except OSError:
                pass
        in_file = describe_backup(stage_files(), manifest["date"][:32])
        token = stage_token_new()
        return {"date": manifest["date"][:32], "callsign": str(manifest.get("callsign", ""))[:16], "token": token,
                "parts": {p: {"file": s, "now": describe_backup(backup_current()).get(p, "")} for p, s in in_file.items()}}
    finally:
        backup_lock.release()


def stage_token_path():
    return os.path.join(settings.BACKUP_WORK, "stage.token")


def stage_token_new():
    """The identity of the upload just staged: the restore must name it, so a backup uploaded
    from another tab in between is never restored under this one's preview (audit A10)."""
    token = secrets.token_hex(8)
    with open(stage_token_path(), "w") as f:
        f.write(token)
    return token


def stage_token():
    try:
        with open(stage_token_path()) as f:
            return f.read().strip()
    except OSError:
        return ""


def stage_clear():
    shutil.rmtree(backup_stage(), ignore_errors=True)
    try:
        os.remove(stage_token_path())
    except OSError:
        pass


def stage_files():
    """member -> path of what the last upload unpacked."""
    files = {}
    for root, _, names in os.walk(backup_stage()):
        for n in names:
            path = os.path.join(root, n)
            member = os.path.relpath(path, backup_stage()).replace(os.sep, "/")
            if member != "manifest.json":
                files[member] = path
    return files


def install_file(src, dst, mode):
    """Atomic copy: tmp file in the target directory, fsync, rename."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".tmp"
    with open(src, "rb") as i, open(tmp, "wb") as o:
        shutil.copyfileobj(i, o)
        o.flush()
        os.fsync(o.fileno())
    os.chmod(tmp, mode)
    os.replace(tmp, dst)


def save_safety_copy(member, path):
    if not os.path.isfile(path):
        return
    dst = os.path.join(settings.BACKUP_SAFETY, member)
    os.makedirs(os.path.dirname(dst), mode=0o700, exist_ok=True)
    if member.endswith(".db"):
        try:
            src, out = sqlite3.connect(path), sqlite3.connect(dst)
            src.backup(out)
            src.close()
            out.close()
            return
        except sqlite3.Error:
            pass   # not a database (corrupt): plain copy
    shutil.copyfile(path, dst)


def apply_memory_slots(path):
    rows = [r for r in memory_rows(path) if r[0] in MEM_SLOTS]
    con = sqlite3.connect(settings.DB_PATH)
    try:
        with con:
            con.execute("DELETE FROM memory WHERE id BETWEEN ? AND ?", (MEM_SLOTS[0], MEM_SLOTS[-1]))
            con.executemany("INSERT OR REPLACE INTO memory(id, name, val) VALUES(?, ?, ?)", rows)
    finally:
        con.close()


def gui_service(action):
    """The GUI's init script; its exit status (audit A22)."""
    return subprocess.run([settings.GUI_INIT, action], capture_output=True, timeout=60).returncode


def gui_running():
    """The GUI or its start loop is alive: a writer of the settings and the log."""
    for name in ("x6100_gui", "x6100_daemon"):
        if subprocess.run(["pidof", name], capture_output=True).returncode == 0:
            return True
    return False


def gui_stop_for_restore():
    """Stops the GUI and waits for it and its start loop to be gone: a restore over a live
    writer would be undone or corrupt the database. The error text if they stay (audit A22)."""
    rc = gui_service("stop")
    for _ in range(20):
        if not gui_running():
            return None
        time.sleep(0.5)
    gui_service("start")    # whatever stopped comes back; nothing was replaced
    return f"The radio screen did not stop (exit {rc}): nothing was replaced"


@app.post('/api/backup/restore')
def restore_backup():
    body = bottle.request.json or {}
    parts = body.get("parts")
    if not isinstance(parts, list) or not parts:
        return backup_error(400, "Nothing selected")
    if not backup_lock.acquire(blocking=False):
        return backup_error(409, "Another backup operation is running")
    try:
        token = stage_token()
        if not token or str(body.get("token", "")) != token:
            return backup_error(409, "The backup shown is not the one on the radio any more: choose the file again")
        files = stage_files()
        todo = {}   # member -> path on the radio, in the order of params.db first
        for name, path in sorted(files.items(), key=lambda x: x[0] != "params.db"):
            member = backup_member(name)
            if member and member[0] in parts:
                todo[name] = member[1]
        if not todo:
            return backup_error(409, "No uploaded backup: choose the file again")
        gui = any(backup_member(n)[0] in BACKUP_GUI_PARTS for n in todo)
        gui_up = False
        if gui:
            try:
                err = gui_stop_for_restore()
            except (OSError, subprocess.SubprocessError) as e:
                return backup_error(500, f"Can't stop the radio screen: {e}")
            if err:
                return backup_error(500, err)
        failed, restored = {}, []
        try:
            for name, dst in todo.items():
                part = backup_member(name)[0]
                try:
                    if name == "memory_slots.json":
                        if "params.db" not in todo:   # else already saved, and dst is the new file
                            save_safety_copy("params.db", dst)
                        apply_memory_slots(files[name])
                    else:
                        save_safety_copy(name, dst)
                        install_file(files[name], dst, 0o600 if part == "wifi" else 0o644)
                        if name.endswith(".db"):
                            for ext in ("-wal", "-shm"):   # stale journal of the replaced file
                                try:
                                    os.remove(dst + ext)
                                except OSError:
                                    pass
                    if part not in restored:
                        restored.append(part)
                except (OSError, sqlite3.Error, ValueError, KeyError) as e:
                    failed[part] = str(e)
            os.sync()
            if "wifi" in restored:
                try:
                    subprocess.run(["nmcli", "connection", "reload"], capture_output=True, timeout=20)
                except (OSError, subprocess.SubprocessError):
                    pass
        finally:
            if gui:
                try:
                    rc = gui_service("start")
                    gui_up = rc == 0
                    if rc:
                        failed["gui"] = f"The radio screen did not start (exit {rc}): switch the radio off and on"
                except (OSError, subprocess.SubprocessError) as e:
                    failed["gui"] = f"Can't start the radio screen: {e}"
            stage_clear()
        restored = [p for p in restored if p not in failed]
        return {"restored": restored, "failed": failed, "gui_restarted": gui_up}
    finally:
        backup_lock.release()


# The Bands, Digital modes, Time and Files pages, static files: config_pages.py adds their routes to app
from . import config_pages   # noqa: E402,F401  (last: it imports app and api_error from here)
