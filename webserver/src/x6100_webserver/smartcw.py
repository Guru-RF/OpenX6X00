"""SmartCW's web page on a port of the radio's own (PORT), for a browser on the radio's network. SmartCW, RF.Guru's
keyer, plugged into the radio's USB port shares a USB network with the radio alone: SmartCW is 192.168.7.1, the
radio's side the interface smartcw0 (rootfs/etc/udev/rules.d/72-smartcw.rules), out of the browser's reach.

Its page asks for /api/events, /config.txt and /api/... by absolute paths, so it is served at the root of a port of
its own, not under a path of the configurator's. Each connection carries one request. Its head is read and checked as
the configurator checks its own (apps.refuse_other_sites), because SmartCW's POSTs send CW on the air, change its
settings and restart it into its bootloader. It goes to SmartCW with Connection: close and the body it declares,
nothing after it, so a second request on the same connection can't pass unchecked. SmartCW's answer comes back as it
comes: its live events (/api/events) are an answer that does not end."""
import html
import logging
import os
import socket
import socketserver
import subprocess
import threading

from . import sites

PORT = 8073
TARGET = ("192.168.7.1", 80)    # SmartCW's address and web server, its defaults
SYS_NET = "/sys/class/net"
IFACE_PREFIX = "smartcw"        # the name udev gives SmartCW's USB network (72-smartcw.rules)
HEAD_MAX = 8192
BODY_MAX = 65536                # SmartCW itself takes 6 KB a request, head and body: its config.txt fits
TIMEOUT = 10                    # s for a request's head and body, as the configurator's
CONNECT_TIMEOUT = 5
READS = ("GET", "HEAD", "OPTIONS")
# Not passed on: the connection's own headers (Connection: close goes in their place), and the radio's cookies (Pat's
# mailbox on port 8080 has one, and a browser sends a name's cookies to every port of it)
DROPPED = {"connection", "keep-alive", "proxy-connection", "upgrade", "te", "trailer", "expect", "cookie"}
# SmartCW lets any site's page read its answers (Access-Control-Allow-Origin: *). Its page needs none of that here, on
# the radio's own port; another site's page would read SmartCW's state and its keying through the radio
DROPPED_BACK = {b"connection", b"keep-alive", b"access-control-allow-origin", b"access-control-allow-methods",
                b"access-control-allow-headers"}
REASONS = {400: "Bad Request", 403: "Forbidden", 413: "Content Too Large", 431: "Request Header Fields Too Large",
           501: "Not Implemented", 502: "Bad Gateway", 503: "Service Unavailable"}


PROFILE = "/usr/lib/NetworkManager/system-connections/smartcw.nmconnection"   # rootfs/, by the update bundle too


class BadRequest(Exception):
    pass


def process_started(name):
    """When the process of that name started (seconds since the epoch, by the clock now), the last one when there are
    more; None when none runs"""
    try:
        with open("/proc/stat") as f:
            boot = next(int(line.split()[1]) for line in f if line.startswith("btime "))
        pids = [p for p in os.listdir("/proc") if p.isdigit()]
    except (OSError, ValueError, StopIteration):
        return None
    ticks = []
    for pid in pids:
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() != name:
                    continue
            with open(f"/proc/{pid}/stat") as f:
                ticks.append(int(f.read().rsplit(")", 1)[1].split()[19]))   # field 22, starttime
        except (OSError, ValueError, IndexError):
            continue
    return boot + max(ticks) / os.sysconf("SC_CLK_TCK") if ticks else None


def follow_profile(reload, profile=PROFILE):
    """NetworkManager reads its profile files at its start only: SmartCW's, newer than the NetworkManager running (an
    update brought it), is read now with reload (apps.NM_RELOAD, ReloadConnections, as the WiFi restore does). Else
    SmartCW's network would wait for the next start of the radio. A clock set after NetworkManager's start can make
    the file look newer than it is: the reload then reads the same files again, which changes nothing. True when it
    asked NetworkManager"""
    started = process_started("NetworkManager")
    try:
        if started is None or os.path.getmtime(profile) <= started:
            return False
    except OSError:
        return False
    try:
        r = subprocess.run(reload, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError) as e:
        logging.error("SmartCW: NetworkManager not told to read the profile (%s)", e)
        return False
    if r.returncode != 0:
        logging.error("SmartCW: NetworkManager not told to read the profile (%s)", r.stderr.strip())
        return False
    logging.warning("SmartCW: NetworkManager told to read its profiles again (SmartCW's came with an update)")
    return True


def smartcw_iface(sys_net=SYS_NET):
    """SmartCW's USB network interface (the first smartcw* there is), None when SmartCW is not plugged in"""
    try:
        names = sorted(n for n in os.listdir(sys_net) if n.startswith(IFACE_PREFIX))
    except OSError:
        return None
    return names[0] if names else None


def bind_to_device(sock, iface):
    """Out through SmartCW's interface only: a home network that is 192.168.7.0/24 too, whose route wins
    (smartcw.nmconnection), can't take the connection to 192.168.7.1"""
    sock.setsockopt(socket.SOL_SOCKET, getattr(socket, "SO_BINDTODEVICE", 25), iface.encode() + b"\0")


class Request:
    """A request's head as the browser sent it: method, target, version, and its header lines (name in lower case,
    value, the line as it came)"""

    def __init__(self, head):
        lines = head[:-4].split(b"\r\n")
        for line in lines:
            if b"\r" in line or b"\n" in line:   # a bare CR or LF: another reader would split the head elsewhere
                raise BadRequest("A line of the request ends in a bare CR or LF")
        word = lines[0].decode("latin-1").split(" ")
        if len(word) != 3 or not word[0].isalpha() or not word[0].isupper() or \
                word[2] not in ("HTTP/1.0", "HTTP/1.1") or not (word[1].startswith("/") or word[1] == "*"):
            raise BadRequest("Not a request line")
        self.method, self.target, self.version = word
        self.headers = []
        for line in lines[1:]:
            name, sep, value = line.decode("latin-1").partition(":")
            if not sep or not name or name != name.strip() or " " in name or "\t" in name:
                raise BadRequest("Not a header line")   # a folded line, or space before the colon
            self.headers.append((name.lower(), value.strip(), line))
        for single in ("host", "content-length"):
            if len(self.all(single)) > 1:
                raise BadRequest(f"More than one {single.title()}")
        length = self.get("content-length", "0")
        if not length.isdigit() or not length.isascii():
            raise BadRequest("Content-Length is not a number")
        self.length = int(length)

    def all(self, name):
        return [v for n, v, _ in self.headers if n == name]

    def get(self, name, default=None):
        values = self.all(name)
        return values[0] if values else default

    def forward(self):
        """The head for SmartCW: the request as it came, one request on the connection"""
        lines = [f"{self.method} {self.target} {self.version}".encode("latin-1")]
        lines += [line for n, _, line in self.headers if n not in DROPPED]
        return b"\r\n".join(lines + [b"Connection: close"]) + b"\r\n\r\n"


def refusal(req, host_ok):
    """(status, title, text) of a refusal by the configurator's rules (apps.refuse_other_sites), None when the request
    may go to SmartCW. host_ok(name) is apps.name_ok: the radio's address or one of its own names"""
    if req.get(sites.FUNNEL_HEADER.lower()) is not None:
        return (403, "Not on the internet",
                "SmartCW's page is not on the internet: open it on the radio's own network or over Tailscale. "
                "Tailscale Funnel only carries the Panel and the Screen page")
    if req.method in READS:
        return None   # reads by any name, as the configurator's pages
    host = sites.host_name(req.get("host"))
    if not host_ok(host):
        return (403, "Not one of the radio's names",
                (f"This page was opened as {host}" if host else "This page was opened without a name") +
                ", which is not one of the radio's own names: by another name a page from another site could send "
                "CW through your browser. Open it by the radio's address or one of its names (System > Network in "
                "the configurator)")
    if req.get("sec-fetch-site") == "cross-site":
        return 403, "Refused", "Cross-site request refused"
    # The configurator's own pages (port 80 or 443, no port in their Origin) pass as they do there, with the same
    # powers; Pat's mailbox on port 8080 does not
    origin = req.get("origin", "")
    if origin and not sites.origin_ok(origin, req.get("host")):   # "null" (a file:// page) too
        return 403, "Refused", "Cross-site request refused"
    return None


def answer(conn, status, title, text):
    """A short page of the radio's own, and the connection's end"""
    title, text = html.escape(title, quote=False), html.escape(text, quote=False)
    body = (f"<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>{title}</title>"
            f"<h1>{title}</h1><p>{text}</p></html>\n").encode()
    head = (f"HTTP/1.1 {status} {REASONS.get(status, 'Error')}\r\nContent-Type: text/html; charset=utf-8\r\n"
            f"Content-Length: {len(body)}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n").encode()
    try:
        conn.sendall(head + body)
    except OSError:
        pass


def read_head(sock, limit=HEAD_MAX):
    """(head up to its blank line, the bytes read after it); head None, and all that was read, when the connection
    ended first or the head is longer than limit"""
    data = b""
    while b"\r\n\r\n" not in data and len(data) <= limit:
        got = sock.recv(limit)
        if not got:
            return None, data
        data += got
    end = data.find(b"\r\n\r\n") + 4
    if not 4 <= end <= limit:
        return None, data
    return data[:end], data[end:]


def answer_head(head):
    """SmartCW's answer head as the browser gets it: Connection: close (its live events say keep-alive, which only
    ends with the connection anyway), and none of its permissions for other sites"""
    lines = head[:-4].split(b"\r\n")
    keep = [lines[0]] + [line for line in lines[1:] if line.partition(b":")[0].strip().lower() not in DROPPED_BACK]
    return b"\r\n".join(keep + [b"Connection: close"]) + b"\r\n\r\n"


def shut(*socks):
    for s in socks:
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


class Proxy:
    """host_ok: the radio's names (apps.name_ok). target, iface and bind are SmartCW's address, how its interface is
    found and how a socket is held to it: the test gives its own"""

    def __init__(self, host_ok, target=TARGET, iface=smartcw_iface, bind=bind_to_device):
        self.host_ok, self.target, self.iface, self.bind = host_ok, target, iface, bind

    def handle(self, conn):
        conn.settimeout(TIMEOUT)
        try:
            head, rest = read_head(conn)
        except OSError:
            return   # nothing in time: a connection a browser opened in advance and never used
        if head is None:
            if len(rest) > HEAD_MAX:
                answer(conn, 431, "Request too large", "The request's head is longer than the radio takes")
            return   # else the browser closed it before the end of the head
        try:
            req = Request(head)
        except BadRequest as e:
            answer(conn, 400, "Bad request", str(e))
            return
        why = refusal(req, self.host_ok)
        if why:
            answer(conn, *why)
            return
        if req.get("transfer-encoding") is not None:
            answer(conn, 501, "Not taken", "SmartCW takes a request with its length (Content-Length) only")
            return
        if req.length > BODY_MAX:
            answer(conn, 413, "Request too large", f"SmartCW takes {BODY_MAX} bytes at most")
            return
        iface = self.iface()
        if not iface:
            answer(conn, 503, "SmartCW is not plugged in",
                   "Plug SmartCW into the radio's USB port, with its USB network on (its setting network, on by "
                   "default). The radio is on SmartCW's network a few seconds later: then load this page again.")
            return
        try:
            if req.get("expect", "").lower() == "100-continue":   # a tool's, never a browser's
                conn.sendall(b"HTTP/1.1 100 Continue\r\n\r\n")
            body = rest[:req.length]
            while len(body) < req.length:
                got = conn.recv(min(65536, req.length - len(body)))
                if not got:
                    return
                body += got
        except OSError:
            return
        up = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.bind(up, iface)
            up.settimeout(CONNECT_TIMEOUT)
            up.connect(self.target)
            up.settimeout(None)
            up.sendall(req.forward() + body)   # and nothing the browser sends after it
        except OSError:
            up.close()
            answer(conn, 502, "SmartCW did not answer",
                   f"SmartCW is plugged in ({iface}), but its web page did not answer at {self.target[0]}. It may "
                   f"still be starting: load this page again in a few seconds. Its address (its setting ip) must "
                   f"be {self.target[0]}, the default.")
            return
        self.relay(conn, up)

    @staticmethod
    def relay(conn, up):
        """SmartCW's answer to the browser as it comes, until either side ends. What the browser sends meanwhile is
        read and dropped: its end is the end of SmartCW's connection too, which SmartCW needs back (it has a few)"""
        conn.settimeout(None)
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # each live event at once
        except OSError:
            pass

        def drain():
            try:
                while conn.recv(65536):
                    pass
            except OSError:
                pass
            shut(up)

        threading.Thread(target=drain, daemon=True).start()
        try:
            head, rest = read_head(up)
            conn.sendall(answer_head(head) + rest if head is not None else rest)   # not HTTP: as it came
            while True:
                data = up.recv(65536)
                if not data:
                    break
                conn.sendall(data)
        except OSError:
            pass
        shut(conn, up)
        up.close()


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def make_server(host, port, proxy):
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            proxy.handle(self.request)

    return Server((host, port), Handler)


def start(host, port, host_ok):
    """The port's server in a thread of its own; False when it can't listen"""
    try:
        server = make_server(host, port, Proxy(host_ok))
    except OSError as e:
        logging.error("SmartCW: can't listen on port %d (%s), no SmartCW page", port, e)
        return False
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return True
