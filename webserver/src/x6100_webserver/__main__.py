import argparse
import logging
import os
import socket
import subprocess
import threading
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


def ensure_cert(cert_dir):
    """Self-signed certificate (10 years) made with the openssl CLI on first start.
    Returns (cert, key) paths, or None when it can't be made."""
    cert, key = os.path.join(cert_dir, "cert.pem"), os.path.join(cert_dir, "key.pem")
    if os.path.isfile(cert) and os.path.isfile(key):
        return cert, key
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
        logging.error("HTTPS: can't create certificate (%s), HTTP only", e)
        for f in (cert + ".tmp", key + ".tmp"):
            try:
                os.remove(f)
            except OSError:
                pass
        return None


def make_https_server(app, host, port, cert, key):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)

    class SSLServer(ThreadingWSGIServer):
        def finish_request(self, request, client_address):
            # handshake in the worker thread, so a stalled client can't block accept()
            request.settimeout(10)
            try:
                request = context.wrap_socket(request, server_side=True)
            except (ssl.SSLError, OSError):
                return   # e.g. the browser rejected the self-signed certificate
            super().finish_request(request, client_address)

    return make_server(host, port, app, SSLServer, RequestHandler)


def start_https(app, host, port, cert_dir):
    if ssl is None:
        logging.error("HTTPS: python ssl module missing, HTTP only")
        return
    files = ensure_cert(cert_dir)
    if not files:
        return
    try:
        server = make_https_server(app, host, port, *files)
    except (ssl.SSLError, OSError) as e:
        logging.error("HTTPS: can't start on port %d (%s), HTTP only", port, e)
        return
    threading.Thread(target=server.serve_forever, daemon=True).start()
    settings.HTTPS_PORT = port


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
    return p.parse_args()


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
    settings.HTTP_PORT = opts.port
    apps.app.run(server=ThreadedServer, host=opts.host, port=opts.port, debug=opts.debug, reloader=opts.debug)


serve()
