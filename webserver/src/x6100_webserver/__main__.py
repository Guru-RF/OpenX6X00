import argparse
import logging
import os
import signal
import socket
import subprocess
import threading
import time
import socketserver
from socketserver import ThreadingMixIn
from wsgiref.simple_server import WSGIRequestHandler, WSGIServer, make_server

import bottle
import bottle.ext.sqlite

try:
    import ssl
except ImportError:   # python built without SSL
    ssl = None

from . import apps
from . import settings


class ThreadingWSGIServer(ThreadingMixIn, WSGIServer):
    daemon_threads = True


class RequestHandler(WSGIRequestHandler):
    # Browsers open connections in advance and may never use them: don't
    # let one of them hold a worker for long.
    timeout = 10


class ThreadedServer(bottle.ServerAdapter):
    """One thread per connection: bottle's default wsgiref server serves
    one connection at a time, so an idle browser connection blocked every
    page for up to a minute."""

    def run(self, handler):
        make_server(self.host, self.port, handler, ThreadingWSGIServer, RequestHandler).serve_forever()


def local_ips():
    ips = {"127.0.0.1"}
    try:
        out = subprocess.run(["ip", "-4", "-o", "addr", "show"], capture_output=True, text=True, timeout=5).stdout
        ips.update(line.split()[3].split("/")[0] for line in out.splitlines() if len(line.split()) > 3)
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        ips.update(socket.gethostbyname_ex(socket.gethostname())[2])
    except OSError:
        pass
    return sorted(ips)


# A clock before this was never set: the PCF8563 that lost its time reads 2000, the kernel without it 1970
CLOCK_SET = 1767225600   # 2026-01-01 UTC


def clock_set():
    return time.time() >= CLOCK_SET


def cert_outdated(cert):
    """True when the certificate is not valid by the clock (expired, or not valid yet) or not a certificate at
    all; False when it is valid, or when openssl can't tell"""
    try:
        r = subprocess.run(["openssl", "x509", "-in", cert, "-noout", "-startdate", "-enddate"],
                           capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return False
    if r.returncode != 0:
        return True   # load_cert_chain would refuse it too: HTTP only for good
    dates = dict(line.strip().split("=", 1) for line in r.stdout.splitlines() if "=" in line)
    try:
        start, end = ssl.cert_time_to_seconds(dates["notBefore"]), ssl.cert_time_to_seconds(dates["notAfter"])
    except (KeyError, ValueError):
        return False
    return not start <= time.time() <= end


def ensure_cert(cert_dir):
    """Self-signed certificate (10 years) made with the openssl CLI on first start, and made again at a start
    that finds it expired or not valid yet: one made while the clock was unset (1970, 2000) was expired for good
    once the clock was right. With the clock unset (before CLOCK_SET) the one there is kept: made anew at such a
    clock it would replace a good one at every start without a network, and be wrong again once the clock is
    set; start_https waits for the clock instead. Returns (cert, key) paths, or None when there is none."""
    cert, key = os.path.join(cert_dir, "cert.pem"), os.path.join(cert_dir, "key.pem")
    have = os.path.isfile(cert) and os.path.isfile(key)
    if have and (not clock_set() or not cert_outdated(cert)):
        return cert, key
    if have:
        logging.warning("HTTPS: the certificate is not valid on %s, making a new one", time.strftime("%Y-%m-%d"))
    try:
        os.makedirs(cert_dir, exist_ok=True)
        san = ",".join(["DNS:x6100"] + ["IP:" + ip for ip in local_ips()])
        base = ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650", "-subj", "/CN=x6100",
                "-keyout", key + ".tmp", "-out", cert + ".tmp"]
        r = subprocess.run(base + ["-addext", "subjectAltName=" + san], capture_output=True, text=True, timeout=120)
        if r.returncode != 0:   # openssl without -addext
            r = subprocess.run(base, capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip())
        os.chmod(key + ".tmp", 0o600)
        os.replace(key + ".tmp", key)
        os.replace(cert + ".tmp", cert)
        return cert, key
    except (OSError, RuntimeError, subprocess.SubprocessError) as e:
        logging.error("HTTPS: can't create certificate (%s), %s", e, "keeping the old one" if have else "HTTP only")
        for f in (cert + ".tmp", key + ".tmp"):
            try:
                os.remove(f)
            except OSError:
                pass
        return (cert, key) if have else None


class TlsContext:
    """The certificate the HTTPS server and the wss proxies hand out. load() puts a new one in for the next
    connections; a handshake under way keeps the context it started with"""

    def __init__(self, cert, key):
        self.load(cert, key)

    def load(self, cert, key):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.context = context

    def wrap(self, sock):
        return self.context.wrap_socket(sock, server_side=True)


def renew_when_clock_set(tls, cert_dir, every=30):
    """The clock was unset at the start: once NTP, the GPS or the user has set it, the certificate is checked
    against it (made anew when it is not valid by it), and the servers take it from their next connection"""
    while not clock_set():
        time.sleep(every)
    files = ensure_cert(cert_dir)
    if files:
        try:
            tls.load(*files)
        except (ssl.SSLError, OSError) as e:
            logging.error("HTTPS: can't load the new certificate (%s)", e)


def make_https_server(app, host, port, tls):
    class SSLServer(ThreadingWSGIServer):
        def finish_request(self, request, client_address):
            # handshake in the worker thread, so a stalled client can't block accept()
            request.settimeout(10)
            try:
                request = tls.wrap(request)
            except (ssl.SSLError, OSError):
                return   # e.g. the browser rejected the self-signed certificate
            super().finish_request(request, client_address)

    return make_server(host, port, app, SSLServer, RequestHandler)


class WssProxy(ThreadingMixIn, socketserver.TCPServer):
    """A TLS front door to one of the GUI's plain WebSocket servers (the remote on 50002, TCI on
    50001), for the https Remote page: a secure page may only open wss:// sockets, and the
    browser's microphone is only given to a secure page. Bytes are piped both ways untouched, so
    the GUI still sees the browser's Origin and Host headers and does its own check."""
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, tls, target_port):
        self.tls = tls
        self.target_port = target_port   # a callable: the port to connect to, read when a client comes
        super().__init__(addr, None)

    def finish_request(self, request, client_address):
        request.settimeout(10)
        try:
            tls = self.tls.wrap(request)
        except (ssl.SSLError, OSError):
            return
        try:
            plain = socket.create_connection(("127.0.0.1", self.target_port()), timeout=5)
        except OSError:
            tls.close()
            return
        tls.settimeout(None)
        plain.settimeout(None)

        def pump(src, dst):
            try:
                while True:
                    data = src.recv(65536)
                    if not data:
                        break
                    dst.sendall(data)
            except OSError:
                pass
            for s in (src, dst):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        threading.Thread(target=pump, args=(plain, tls), daemon=True).start()
        pump(tls, plain)
        tls.close()
        plain.close()


def start_wss_proxies(host, tls):
    for name, port, target in (("WSS_REMOTE_PORT", 50003, lambda: apps.read_remote_conf()["port"]),
                               ("WSS_TCI_PORT", 50004, lambda: apps.read_tci_conf()["port"])):
        try:
            server = WssProxy((host, port), tls, target)
        except OSError as e:
            logging.error("wss proxy: can't listen on %d (%s)", port, e)
            continue
        threading.Thread(target=server.serve_forever, daemon=True).start()
        setattr(settings, name, port)


def start_https(app, host, port, cert_dir):
    if ssl is None:
        logging.error("HTTPS: python ssl module missing, HTTP only")
        return
    files = ensure_cert(cert_dir)
    if not files:
        return
    try:
        tls = TlsContext(*files)
        server = make_https_server(app, host, port, tls)
    except (ssl.SSLError, OSError) as e:
        logging.error("HTTPS: can't start on port %d (%s), HTTP only", port, e)
        return
    threading.Thread(target=server.serve_forever, daemon=True).start()
    settings.HTTPS_PORT = port
    start_wss_proxies(host, tls)
    if not clock_set():
        threading.Thread(target=renew_when_clock_set, args=(tls, cert_dir), daemon=True).start()


SSHD_CONFIG, SSHD_PID = "/etc/ssh/sshd_config", "/var/run/sshd.pid"


def follow_sshd_config():
    """sshd started before /etc/ssh/sshd_config last changed (an update brought a new one): told to read it again.
    SIGHUP makes sshd start itself anew with the file; the sessions open stay. Done here because every updater
    restarts the web server after an install, also those from before keys-only SSH, which left sshd alone; at boot
    sshd starts after the file is in place and nothing happens. A file sshd -t refuses is not loaded (the running
    sshd would exit on it). On the radio only: a web server tried out on a PC leaves that PC's sshd alone"""
    try:
        with open("/proc/device-tree/compatible", "rb") as f:
            if b"xiegu," not in f.read():
                return
        if os.path.getmtime(SSHD_CONFIG) <= os.path.getmtime(SSHD_PID):
            return
        with open(SSHD_PID) as f:
            pid = int(f.read().strip())
        with open(f"/proc/{pid}/comm") as f:
            if f.read().strip() != "sshd":
                return
        r = subprocess.run(["/usr/sbin/sshd", "-t"], capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            logging.error("sshd: %s refused (%s), not loaded", SSHD_CONFIG, r.stderr.strip())
            return
        os.kill(pid, signal.SIGHUP)
        logging.warning("sshd: told to read the new %s", SSHD_CONFIG)
    except (OSError, ValueError, subprocess.SubprocessError):
        pass


OLD_SSH_KEYS = "/mnt/ssh/authorized_keys"


def adopt_old_ssh_keys():
    """The X6100 images v0.3.1-v0.3.5 also took SSH keys from /mnt/ssh/authorized_keys, which the DATA partition's
    other writers can reach; keys-only SSH reads root's file alone. Once, at the first start after the update, the keys
    there move into root's file (those sshd took, as they were, a key root has already left out), and the old file is
    renamed .imported, so whoever logged in with a key still does. On the radio only"""
    try:
        with open("/proc/device-tree/compatible", "rb") as f:
            if b"xiegu," not in f.read():
                return
        with open(OLD_SSH_KEYS, errors="replace") as f:
            old = [k for k in map(apps.ssh_parse_line, f.read().splitlines()) if k]
    except OSError:
        return
    try:
        with apps.ssh_lock:
            lines = apps.ssh_read()
            have = {k["fingerprint"] for k in map(apps.ssh_parse_line, lines) if k}
            new = [(k["options"] + " " + k["line"]).strip() for k in old if k["fingerprint"] not in have]
            if new:
                apps.ssh_write(lines + new)
            os.replace(OLD_SSH_KEYS, OLD_SSH_KEYS + ".imported")
        logging.warning("ssh: %d key(s) from %s moved to root's authorized_keys", len(new), OLD_SSH_KEYS)
    except OSError as e:
        logging.error("ssh: %s not moved: %s", OLD_SSH_KEYS, e)


def options():
    p = argparse.ArgumentParser(description="OpenX6X00 web configurator")
    p.add_argument("--db", required=True, help="params.db of the radio GUI")
    p.add_argument("--host", default="localhost", help="address to listen on")
    p.add_argument("--port", type=int, default=8080, help="HTTP port")
    p.add_argument("--debug", action="store_true", help="bottle debug mode, reloads changed sources")
    p.add_argument("--filebrowser-path", default="/mnt", help="root of the Files page")
    p.add_argument("--cluster-conf", help="path to DX cluster config", default=settings.CLUSTER_CONF)
    p.add_argument("--tci-conf", help="path to the TCI server config", default=settings.TCI_CONF)
    p.add_argument("--channels-file", help="path to channels.json", default=settings.CHANNELS_FILE)
    p.add_argument("--https-port", type=int, help="port for HTTPS (0 = off)", default=443)
    p.add_argument("--cert-dir", help="directory of the self-signed cert.pem and key.pem", default="/mnt/https")
    p.add_argument("--funnel-dir", help="where the Panel for Tailscale Funnel is written (empty: not written)",
                   default=settings.FUNNEL_DIR)
    return p.parse_args()


FUNNEL_NAME_POLL = 5     # s between looks at the radio's name on the internet, for the Funnel pages


def write_funnel_pages(out_dir, name):
    """The Panel and the Screen page as tailscaled serves them over Tailscale Funnel (#58, apps.funnel_render), at
    every start: they follow the pages of the version installed. In RAM (/run): written anew at each boot"""
    if not out_dir:
        return
    try:
        apps.funnel_render(out_dir, name)
    except (OSError, ValueError) as e:
        logging.error("Funnel: the pages for %s not written (%s)", out_dir, e)


def follow_funnel_name(out_dir, name):
    """The Funnel pages name the radio's address on the internet in what they may connect to (funnel_base.html):
    tailscaled tells it once Tailscale is up, at boot after this server's start, and it changes with the radio's
    name or the tailnet's. The pages are written anew when it does"""
    while True:
        time.sleep(FUNNEL_NAME_POLL)
        now = apps.funnel_name()
        if now and now != name:
            name = now
            write_funnel_pages(out_dir, name)


def serve():
    opts = options()
    # params.db for the routes that take a "dbcon" argument: a connection per request, committed after it
    apps.app.install(bottle.ext.sqlite.Plugin(dbfile=opts.db, keyword="dbcon"))
    settings.FILEBROWSER_PATH, settings.DB_PATH = opts.filebrowser_path, opts.db
    settings.CLUSTER_CONF = opts.cluster_conf
    settings.TCI_CONF = opts.tci_conf
    settings.CHANNELS_FILE = opts.channels_file
    if opts.https_port and not (opts.debug and "BOTTLE_CHILD" not in os.environ):   # not in the reloader's parent
        start_https(apps.app, opts.host, opts.https_port, opts.cert_dir)
    adopt_old_ssh_keys()
    follow_sshd_config()
    settings.FUNNEL_DIR = opts.funnel_dir
    name = apps.funnel_name()
    write_funnel_pages(opts.funnel_dir, name)
    if opts.funnel_dir and not (opts.debug and "BOTTLE_CHILD" not in os.environ):   # not in the reloader's parent
        threading.Thread(target=follow_funnel_name, args=(opts.funnel_dir, name), daemon=True).start()
    settings.HTTP_PORT = opts.port
    apps.app.run(server=ThreadedServer, host=opts.host, port=opts.port, debug=opts.debug, reloader=opts.debug)


serve()
