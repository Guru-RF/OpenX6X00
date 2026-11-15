"""Wavelog's live link (#245), both ways, each off until it is switched on on the Wavelog page (Operating > Wavelog).
Its routes join the app of apps.py, which imports this module after its own Wavelog routes. The settings are more lines
of <FIELD_DIR>/wavelog.conf: live, radio_name, cat, cat_url, cat_origin, cat_token.

Radio to Wavelog (live=1): what the radio is on, its frequency, mode and power, goes to Wavelog's api/radio, so
Wavelog's QSO form and band map follow the radio. A thread of the web server asks the GUI for it on the configurator
socket (radio_get, src/wavelog_cat.c) twice a second and posts it once it has settled. Here, not in the GUI: the web
server runs all the time, has the saved URL and key and knows the radio's addresses for cat_url, and a post that
waits for a slow Wavelog or a dead network waits in its own thread, never the GUI's. The post is x6100-wavelog's (its
api_url, its key check, its request with the certificate check the page sets), the helper loaded as a module.

Wavelog to radio (cat=1): the posts carry cat_url, <the radio's address>/cat/<token>, and Wavelog's page, on a click
on a spot in its DX cluster or band map, fetches <cat_url>/<Hz>/<mode> (assets/js/cat.js performRadioTuning; the same
again with http and https swapped when that fails). cat_route answers only that: the right token, the Origin of the
owner's Wavelog (CORS for that one origin, Chrome's Private Network Access preflight too), one of the radio's own names
in Host (or the host of the address typed for it), a frequency the radio receives and a mode it has. The tune goes
to the GUI on the same socket (tune), which sets the frequency and the mode and never transmits. The request comes
from Wavelog's page, another site, without the radio's pages' rules: the token in the path is what keeps other pages
and devices from retuning the radio."""

import decimal
import hmac
import ipaddress
import json
import logging
import os
import re
import secrets
import shutil
import socket
import threading
import time
import types
import urllib.parse

import bottle

from . import apps
from . import settings

# When to post, against Wavelog's rate limit for api/radio: none unless its admin sets one (config.php
# api_rate_limits, per API key), the example there 60 a minute for radio and 30 for an endpoint without its own
POLL_S = 0.5          # the GUI asked what the radio is on
SETTLE_S = 1.5        # the same for this long: the knob stopped, the value is posted
MIN_GAP_S = 4.0       # between two posts at most 15 a minute, half the strictest example
HEARTBEAT_S = 60.0    # the same again after a minute: Wavelog takes a radio for gone after its Radio Timeout Warning
                      # (cat_timeout_interval, 1800 s by default, counted in whole minutes)
BACKOFF_S = (10, 20, 40, 80, 120)   # after the 1st, 2nd... failure in a row (no network, a server error)
REFUSED_WAIT_S = 600  # a key or a certificate refused: again after a Save, or after this
RETRY_AFTER_MAX_S = 300   # Wavelog's 429 says when to try again: at most this long
POST_TIMEOUT_S = 6    # a frequency is soon old: the helper's 20 s are for a QSO

# Wavelog's modes for tuning (cat.js validModes, lower case) and the radio's for them. Its band map sends RTTY for
# every digital spot (FT8, PSK31...: radiohelpers.js determineRadioMode), as the radio's own cluster tunes them, USB-D.
# The radio has no FM data mode: pktfm is FM
CAT_MODES = {"lsb": "LSB", "usb": "USB", "cw": "CW", "am": "AM", "fm": "NFM", "rtty": "USB-D", "pkt": "USB-D",
             "dig": "USB-D", "pktusb": "USB-D", "pktlsb": "LSB-D", "pktfm": "NFM"}
# Wavelog has the frequency as MHz times a million in JavaScript: 1.001 MHz comes as 1000999.9999999999
FREQ_RE = re.compile(r"[0-9]{1,10}(\.[0-9]{1,20})?")
TOKEN_RE = re.compile(r"[A-Za-z0-9_-]{16,64}")
ORIGIN_RE = re.compile(r"https?://[^/\s?#@]{1,255}")          # what a browser sends in Origin: no path, no login
BASE_RE = re.compile(r"https?://([A-Za-z0-9.-]{1,253}|\[[0-9A-Fa-f:.]{2,45}\])(:\d{1,5})?")
NAME_RE = re.compile(r"[ -~]{1,32}")    # Wavelog keeps a radio by its name (up to 250 characters)
MODE_RE = re.compile(r"[A-Z0-9-]{1,10}")   # Wavelog takes up to 10
DEFAULT_PORTS = {"http": 80, "https": 443}


def live_on(conf):
    return conf.get("live") == "1" and bool(conf.get("url")) and bool(conf.get("key"))


def cat_on(conf):
    return live_on(conf) and conf.get("cat") == "1" and bool(TOKEN_RE.fullmatch(conf.get("cat_token", "")))


def default_radio_name():
    """The radio's model, X6100 or X6200: Wavelog's radio list and its QSO form show it"""
    return apps.radio_model().upper()


def origin_of(url):
    """scheme://host[:port] of an http(s) URL, the host in lower case and the port only when it is not the scheme's
    own, as a browser writes Origin; "" for anything else"""
    try:
        u = urllib.parse.urlsplit(str(url).strip())
        port = u.port
    except ValueError:
        return ""
    scheme = u.scheme.lower()
    if scheme not in DEFAULT_PORTS or not u.hostname or u.username is not None:
        return ""
    host = f"[{u.hostname.lower()}]" if ":" in u.hostname else u.hostname.lower()
    return f"{scheme}://{host}" + (f":{port}" if port and port != DEFAULT_PORTS[scheme] else "")


def wavelog_origin(conf):
    """The origin of the owner's Wavelog in the browser: the one typed on the page, else the Wavelog URL's"""
    return origin_of(conf.get("cat_origin") or conf.get("url", ""))


def lan_address():
    """The radio's IPv4 address on its network, where its default route leaves from ("" without one): a UDP socket's
    connect only picks the route, nothing is sent (192.0.2.1 is an address for documentation)"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            ip = s.getsockname()[0]
        return ip if not ipaddress.ip_address(ip).is_loopback and ip != "0.0.0.0" else ""
    except (OSError, ValueError):
        return ""


def auto_base():
    """The radio's address for Wavelog's page when none is typed: https while the radio's HTTPS server runs (Wavelog
    swaps to http by itself when https fails), on the radio's Tailscale name while Tailscale runs, else on its address
    on the network (its name.local without one). Not the Tailscale name while Funnel is on: port 443 there is
    tailscaled's, which serves the Panel alone"""
    scheme, port = ("https", settings.HTTPS_PORT) if settings.HTTPS_PORT else ("http", settings.HTTP_PORT)
    ts = apps.tailscale_status()
    ts_name = apps.funnel_name() if ts.get("state") == "running" and ts.get("funnel") not in ("on", "starting") else ""
    host = ts_name or lan_address() or f"{socket.gethostname().lower()}.local"
    return f"{scheme}://{host}" + ("" if port == DEFAULT_PORTS[scheme] else f":{port}")


def cat_base(conf):
    return conf.get("cat_url") or auto_base()


def cat_url(conf):
    """What Wavelog's page fetches, without the frequency and the mode; None while the switch is off"""
    return f"{cat_base(conf)}/cat/{conf['cat_token']}" if cat_on(conf) else None


def radio_state():
    """What the radio is on (src/wavelog_cat.c wavelog_cat_state), None while the GUI doesn't answer"""
    reply = apps.display_call({"cmd": "radio_get"})
    if not isinstance(reply, dict) or reply.get("t") != "radio":
        return None
    try:
        hz, tx_hz, pwr = int(reply["hz"]), int(reply.get("tx_hz") or 0), float(reply.get("pwr") or 0)
    except (KeyError, TypeError, ValueError):
        return None
    if hz <= 0:
        return None
    return {"hz": hz, "tx_hz": tx_hz if tx_hz > 0 else hz, "mode": str(reply.get("mode", "")),
            "log_mode": str(reply.get("log_mode", "")), "pwr": pwr}


def wavelog_mode(st):
    """The mode for Wavelog's QSO form: what the radio's quick QSO log would log now (an app's mode, FT8 or FT4 by the
    dial, CW, AM, FM), voice as its sideband (Wavelog has USB and LSB as SSB's submodes, and its band map and waterfall
    need the sideband), a data mode the radio can't name as USB-D or LSB-D, Wavelog's own names for those (it shows
    them as USB and LSB)"""
    radio, log = st.get("mode", ""), st.get("log_mode", "")
    if log == "SSB":
        mode = "LSB" if radio.startswith("LSB") else "USB"
    else:
        mode = log or radio
    return mode if MODE_RE.fullmatch(mode) else "USB"


def payload(conf, st):
    """api/radio's fields, but the key: frequency is where the radio transmits (ADIF's FREQ), frequency_rx where it
    receives, in split only"""
    body = {"radio": conf.get("radio_name") or default_radio_name(), "frequency": st["tx_hz"],
            "mode": wavelog_mode(st), "power": round(st["pwr"], 1)}
    if st["hz"] != st["tx_hz"]:
        body["frequency_rx"] = st["hz"]
    url = cat_url(conf)
    if url:
        body["cat_url"] = url
    return body


class Feed:
    """When a value goes: once it settled (the same for SETTLE_S), when it is new or HEARTBEAT_S passed since the one
    that went, MIN_GAP_S after the last try, and not before a failure's wait is over. now: a monotonic clock"""

    def __init__(self):
        self.seen = self.sent = None
        self.seen_at = self.sent_at = 0.0
        self.tried_at = float("-inf")
        self.wait_until = 0.0
        self.fails = 0

    def due(self, now, value):
        if value != self.seen:
            self.seen, self.seen_at = value, now
        if now - self.seen_at < SETTLE_S or now < self.wait_until or now - self.tried_at < MIN_GAP_S:
            return False
        return value != self.sent or now - self.sent_at >= HEARTBEAT_S

    def done(self, now, value):
        self.tried_at = now
        self.sent, self.sent_at = value, now
        self.fails, self.wait_until = 0, 0.0

    def failed(self, now, wait=None):
        """wait: how long Wavelog asked for (429), else the next of BACKOFF_S"""
        self.tried_at = now
        self.fails += 1
        if wait is None:
            wait = BACKOFF_S[min(self.fails, len(BACKOFF_S)) - 1]
        self.wait_until = now + wait

    def again(self):
        """Settings saved: the next settled value goes, whatever went or failed before"""
        self.sent, self.wait_until, self.fails = None, 0.0, 0


helper_cache = {}


def helper():
    """x6100-wavelog (rootfs/usr/sbin) as a module, read and compiled here without a .pyc beside it (a .pyc left in
    /usr/sbin would outlive an update of the helper)"""
    path = settings.WAVELOG_CMD if os.path.isabs(settings.WAVELOG_CMD) else shutil.which(settings.WAVELOG_CMD)
    if not path:
        raise OSError(f"{settings.WAVELOG_CMD} not found")
    st = os.stat(path)
    hit = helper_cache.get(path)
    if hit and hit[0] == (st.st_mtime_ns, st.st_size):
        return hit[1]
    with open(path, encoding="utf-8") as f:
        code = compile(f.read(), path, "exec")
    mod = types.ModuleType("x6100_wavelog_helper")
    mod.__file__ = path
    exec(code, mod.__dict__)
    helper_cache[path] = ((st.st_mtime_ns, st.st_size), mod)
    return mod


def post(conf, body):
    """("ok", ""), ("retry", why, wait or None) or ("refused", why): one post to api/radio with the saved URL and key"""
    try:
        h = helper()
    except (OSError, SyntaxError, ValueError) as e:
        return "refused", f"the upload helper could not be loaded ({e})"
    key = conf.get("key", "")
    if key.lower().startswith("wl2_"):
        return "refused", h.V2_TOKEN
    try:
        code, j, text = h.request(h.api_url(conf["url"], "radio"), dict(body, key=key),
                                  conf.get("verify_tls", "1") != "0", timeout=POST_TIMEOUT_S)
    except h.Retry as e:
        return "retry", f"no connection ({e})", None
    except h.Fail as e:   # a certificate refused
        return "refused", str(e)
    except Exception as e:   # anything else the helper's own run would have said as an error (a URL urllib refuses)
        return "retry", h.one_line(e, 60) or type(e).__name__, BACKOFF_S[-1]
    status = str(j.get("status", "")).lower() if isinstance(j, dict) else ""
    if code == 200 and status == "success":
        return "ok", ""
    if not status:   # not Wavelog: a WiFi login page, a proxy's page, a wrong URL
        return "retry", f"not Wavelog's answer (HTTP {code})", None
    why = h.one_line(j.get("reason") or j.get("message") or "", 60)
    if code == 429:
        try:
            wait = int(j.get("retry_after"))
        except (TypeError, ValueError):
            wait = 60
        return "retry", "Wavelog's rate limit", max(1, min(wait, RETRY_AFTER_MAX_S))
    if code in (401, 403):   # the key, or one without write rights
        return "refused", why or f"key refused (HTTP {code})"
    if code in (408,) or code >= 500:
        return "retry", f"server HTTP {code}", None
    return "retry", f"HTTP {code} {why}".strip(), BACKOFF_S[-1]


# The sender: one thread while the switch is on. status is what the page shows
lock = threading.Lock()
sender = None
status = {"state": "off", "text": "", "time": 0}
tuned = {"text": "", "time": 0}


def note(state, text):
    """The page's line; a change of state goes to the log once (no network: one line, not one a post)"""
    with lock:
        was = status["state"]
        status.update(state=state, text=text, time=int(time.time()))
    if state != was and state in ("retry", "refused", "noradio"):
        logging.warning("Wavelog live link: %s", apps.hide_secrets(text))
    elif state != was and state == "ok" and was in ("retry", "refused", "noradio"):
        logging.warning("Wavelog live link: sending again")


def describe(body):
    hz = body["frequency"]
    mhz = f"{hz // 1000000}.{hz // 1000 % 1000:03d}.{hz % 1000:03d}"
    return f"{mhz} MHz {body['mode']}, {body['power']:g} W" + (" (split)" if "frequency_rx" in body else "")


def start():
    """The sender, while the switch is on: at the web server's start and after a Save that turned it on"""
    global sender
    with lock:
        if sender and sender.is_alive():
            return
        if not live_on(apps.wavelog_read()):
            status.update(state="off", text="", time=int(time.time()))
            return
        sender = threading.Thread(target=run, name="wavelog-live", daemon=True)
        sender.start()


def stopping():
    """The switch is off (or the key gone): the thread ends, under the lock start() takes, so a Save that turns it on
    meanwhile finds no thread and starts one"""
    global sender
    with lock:
        if live_on(apps.wavelog_read()):
            return False
        sender = None
        status.update(state="off", text="", time=int(time.time()))
        return True


def conf_seen(conf):
    return json.dumps(sorted(conf.items()))


def run():
    feed, seen = Feed(), None
    while True:
        conf = apps.wavelog_read()
        if not live_on(conf):
            if stopping():
                return
            continue   # switched on again meanwhile: read it again
        if conf_seen(conf) != seen:
            if seen is not None:
                feed.again()
            seen = conf_seen(conf)
        try:
            step(feed, conf)
        except Exception as e:   # never the end of the thread while the switch is on
            feed.failed(time.monotonic())
            note("retry", f"Not sent: {type(e).__name__} {e}")
        time.sleep(POLL_S)


def step(feed, conf):
    """One look at the radio, and a post when one is due"""
    st = radio_state()
    if st is None:
        if status["state"] != "noradio":
            note("noradio", "The radio's screen program doesn't answer: waiting for it")
        return
    body = payload(conf, st)
    value = json.dumps(body, sort_keys=True)
    now = time.monotonic()
    if not feed.due(now, value):
        return
    res = post(conf, body)
    now = time.monotonic()
    if res[0] == "ok":
        feed.done(now, value)
        note("ok", f"{describe(body)}, sent {time.strftime('%H:%M:%S')}")
    elif res[0] == "retry":
        feed.failed(now, res[2])
        if status["state"] != "retry":   # the first failure says when it began, the next ones nothing new
            note("retry", f"Not sent since {time.strftime('%H:%M')}: {res[1]}. The radio tries again")
    else:
        feed.failed(now, REFUSED_WAIT_S)
        note("refused", f"Wavelog refused it: {res[1]}. Fix it, then Save")


def page_state(conf):
    """For the Wavelog page (apps.wavelog_state): the settings, the addresses used, the line. Never the token"""
    with lock:
        st, tn = dict(status), dict(tuned)
    if not live_on(conf):
        st = {"state": "off", "text": "", "time": 0}
    return {"on": conf.get("live") == "1", "radio_name": conf.get("radio_name", ""),
            "radio_name_default": default_radio_name(), "cat": conf.get("cat") == "1",
            "cat_url": conf.get("cat_url", ""), "cat_url_auto": auto_base(), "cat_origin": conf.get("cat_origin", ""),
            "cat_origin_default": origin_of(conf.get("url", "")), "state": st["state"],
            "text": apps.hide_secrets(st["text"]),
            "tuned": tn["text"] if cat_on(conf) and tn["time"] > time.time() - 3600 else ""}


@apps.app.post('/api/wavelog/live')
def set_live():
    """{"live", "radio_name" ("": the model), "cat", "cat_url" ("": automatic), "cat_origin" ("": the Wavelog URL's),
    "new_token"}: the live link's settings. The Wavelog URL and key are the upload's, saved above them on the page"""
    body = bottle.request.json
    if not isinstance(body, dict):
        return apps.api_error(400, "Invalid request")
    name = str(body.get("radio_name", "")).strip()
    if name and not NAME_RE.fullmatch(name):
        return apps.api_error(400, "The radio's name: up to 32 letters, digits, spaces and signs (ASCII)")
    base = str(body.get("cat_url", "")).strip().rstrip("/")
    if base and not BASE_RE.fullmatch(base):
        return apps.api_error(400, "The radio's address for Wavelog: http:// or https://, a name or an address, a port "
                                   "if it is not the usual one, and nothing after it")
    origin = str(body.get("cat_origin", "")).strip()
    if origin and not origin_of(origin):
        return apps.api_error(400, "Wavelog in the browser: the address you open Wavelog with, http:// or https://")
    live, cat = bool(body.get("live")), bool(body.get("cat"))
    with apps.wavelog_lock:
        conf = apps.wavelog_read()
        if live and not (conf.get("url") and conf.get("key")):
            return apps.api_error(400, "Save the Wavelog URL and an API key above first")
        if live and conf.get("key", "").lower().startswith("wl2_"):
            return apps.api_error(400, apps.WAVELOG_V2_TOKEN)
        if cat and not live:
            return apps.api_error(400, "Tune from Wavelog needs Show the radio in Wavelog on: Wavelog learns the "
                                       "radio's address from it")
        conf.update(live="1" if live else "0", cat="1" if cat else "0", radio_name=name, cat_url=base,
                    cat_origin=origin_of(origin) if origin else "")
        if body.get("new_token") or not cat:
            conf.pop("cat_token", None)   # a new one when it is on again: the old address stops working
        if cat and not TOKEN_RE.fullmatch(conf.get("cat_token", "")):
            conf["cat_token"] = secrets.token_urlsafe(18)
        try:
            apps.wavelog_write(conf)
        except OSError as e:
            return apps.api_error(500, f"Can't save: {e}")
    start()
    return apps.wavelog_state()


def typed_host_ok(conf):
    """The host of the radio's address typed on the page (a name a router forwards to the radio): the owner's own
    choice, for this route alone, never through Funnel. The token and the Origin still have to be right"""
    typed = urllib.parse.urlsplit(conf.get("cat_url", "")).hostname if conf.get("cat_url") else None
    return bool(typed) and not apps.via_funnel() and apps.request_host() == typed.lower().rstrip(".")


def answer(code, text, headers=None):
    hdr = {"Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store", "Vary": "Origin"}
    hdr.update(headers or {})
    return bottle.HTTPResponse(text + "\n" if text else "", code, hdr)


@apps.app.route('/cat/<token>/<freq>/<mode>', method=["GET", "OPTIONS"])
def cat_route(token, freq, mode):
    """<cat_url>/<Hz>/<mode>, fetched by Wavelog's page on a click on a spot: the radio tunes there. In this order:
    the switch and the token (404 for anything else: the route says nothing to whoever doesn't know it), one of the
    radio's own names in Host (local_host_ok, as for a change on its pages) or the host of the address typed for it
    on the page, the Origin of the owner's Wavelog (403
    without CORS for any other, and for none: a page of the radio's own site sends none, a program on the network
    anything). Chrome's preflight for a public page reaching a private address (Private Network Access: OPTIONS with
    Access-Control-Request-Private-Network) is answered for that origin alone, with GET and no other method or header.
    Then the frequency, inside the radio's receive range, and the mode, one of Wavelog's (CAT_MODES); the GUI checks
    them again and refuses while the radio transmits (409). Only the GUI's tune command is sent: frequency and mode"""
    req = bottle.request
    conf = apps.wavelog_read()
    mine = conf.get("cat_token", "")
    if not (cat_on(conf) and hmac.compare_digest(token.encode(), mine.encode())):
        return answer(404, "Not found")
    if not (apps.local_host_ok() or typed_host_ok(conf)):
        return answer(403, "Not one of the radio's own names")
    origin = req.get_header("Origin") or ""
    allowed = wavelog_origin(conf)
    if not (allowed and ORIGIN_RE.fullmatch(origin) and origin_of(origin) == allowed):
        return answer(403, "Only the page of the Wavelog set on the radio tunes it")
    cors = {"Access-Control-Allow-Origin": origin}
    if req.method == "OPTIONS":
        if (req.get_header("Access-Control-Request-Method") or "GET") != "GET" or \
                req.get_header("Access-Control-Request-Headers"):
            return answer(403, "A plain GET only")
        cors.update({"Access-Control-Allow-Methods": "GET", "Access-Control-Max-Age": "600"})
        if (req.get_header("Access-Control-Request-Private-Network") or "").strip().lower() == "true":
            cors["Access-Control-Allow-Private-Network"] = "true"
        return answer(204, "", cors)
    if req.method != "GET":   # HEAD, which bottle sends to a GET route
        return answer(405, "GET only", dict(cors, Allow="GET, OPTIONS"))
    if not FREQ_RE.fullmatch(freq):
        return answer(400, "The frequency: Hz", cors)
    hz = int(decimal.Decimal(freq).to_integral_value(decimal.ROUND_HALF_UP))
    ranges = apps.tune_ranges()
    if not any(lo <= hz <= hi for lo, hi in ranges):
        return answer(400, "Outside the radio's receive range: " +
                      " or ".join(f"{lo / 1e6:g}-{hi / 1e6:g}" for lo, hi in ranges) + " MHz", cors)
    ours = CAT_MODES.get(mode.lower())
    if not ours:
        return answer(400, "No such mode: " + ", ".join(CAT_MODES), cors)
    reply = apps.display_call({"cmd": "tune", "hz": hz, "mode": ours})
    if not isinstance(reply, dict) or reply.get("t") not in ("ok", "err"):
        return answer(503, "The radio's screen program doesn't answer", cors)
    if reply["t"] == "err":
        return answer(409, str(reply.get("err", ""))[:80], cors)
    with lock:
        tuned.update(text=f"{hz / 1e6:.4f} MHz {ours}, {time.strftime('%H:%M:%S')}", time=int(time.time()))
    return answer(200, f"OK {hz} {ours}", cors)
