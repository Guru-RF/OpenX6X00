"""The web page GhostNet (/ghostnet): the same files as the JS8 app's GhostNet (src/ghostnet.c). The nets' schedule
(GhostNet version 1.5, /mnt/ghostnet_nets.txt or the image's /usr/share/x6100/ghostnet_nets.txt), the settings in
/mnt/ghostnet.conf (the radio reads them again by itself when they change), and the messages the JS8 app wrote to
/mnt/ghostnet_log.txt. Times in UTC and in the radio's own zone, which is the GUI's (/mnt/tz.conf, a POSIX TZ
string), not the web server's /etc/localtime. No bottle here: apps.py has the routes.

SPDX-License-Identifier: LGPL-2.1-or-later
After the GhostNet page of the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico)."""
import calendar
import re
import sqlite3
import time

from . import settings
from .iso3_web import ISO3

DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
OWN = "OWN"
MODES = ("JS8", "RTTY", "LSB", "USB")
# The keys of ghostnet.conf in the order src/ghostnet.c writes them, with its defaults
DEFAULTS = {"remind": False, "remind_min": 10, "nets": [], "own_on": False, "own_name": "Own net", "own_day": 4,
            "own_time": "21:00", "own_min": 30, "own_khz": 7107, "own_mode": "JS8", "dept": "", "region": "",
            "translate_rx": False, "translate_tx": False}


def country_iso2():
    """The radio's country, ISO 3166-1 alpha-2 in capitals, for the regional group: the setting "country" of params.db
    (Settings > General > STATION > Country, or the first-start guide's), as ghostnet_country_iso2() in
    src/ghostnet_gui.c reads it; "" while none is set or params.db cannot be read"""
    try:
        con = sqlite3.connect(f"file:{settings.DB_PATH}?mode=ro", uri=True)
        try:
            row = con.execute("SELECT val FROM params WHERE name = ?", ("country",)).fetchone()
        finally:
            con.close()
    except sqlite3.Error:
        return ""
    cc = str(row[0]).strip().upper() if row and row[0] else ""
    return cc if re.fullmatch(r"[A-Z]{2}", cc) else ""


# ---- The radio's time zone: the POSIX TZ string of /mnt/tz.conf ----

TZ_PART = r"([A-Za-z]{3,}|<[^>]+>)([+-]?\d{1,2}(?::\d{2}){0,2})"
TZ_RULE = r"M(\d{1,2})\.([1-5])\.([0-6])(?:/([+-]?\d{1,3}(?::\d{2}){0,2}))?"
TZ_RE = re.compile(r"^" + TZ_PART + r"(?:([A-Za-z]{3,}|<[^>]+>)([+-]?\d{1,2}(?::\d{2}){0,2})?"
                   r"(?:," + TZ_RULE + r"," + TZ_RULE + r")?)?$")


def _seconds(text, default=0):
    """"-5:30" as seconds, "1" as 3600; default for None"""
    if text is None:
        return default
    sign = -1 if text.startswith("-") else 1
    parts = [int(p) for p in text.lstrip("+-").split(":")] + [0, 0]
    return sign * (parts[0] * 3600 + parts[1] * 60 + parts[2])


class Zone:
    """A POSIX TZ string ("UTC0", "IST-5:30", "CET-1CEST,M3.5.0,M10.5.0/3") as the C library reads it, for the
    zones of the GUI's list (src/clock.c clock_zones): offsets east of UTC in seconds, summer time by M rules"""

    def __init__(self, posix):
        m = TZ_RE.match((posix or "").strip())
        if not m:
            m = TZ_RE.match("UTC0")
        g = m.groups()
        self.std_name, self.std = g[0].strip("<>"), -_seconds(g[1])
        self.dst_name = g[2].strip("<>") if g[2] else None
        self.dst = -_seconds(g[3]) if g[3] else self.std + 3600
        if g[4]:
            self.start = (int(g[4]), int(g[5]), int(g[6]), _seconds(g[7], 7200))
            self.end = (int(g[8]), int(g[9]), int(g[10]), _seconds(g[11], 7200))
        else:   # summer time without rules: the C library's default, the US rules
            self.start, self.end = (3, 2, 0, 7200), (11, 1, 0, 7200)

    @staticmethod
    def _rule_day(year, rule):
        month, week, wday, _ = rule
        first = (calendar.weekday(year, month, 1) + 1) % 7      # 0 Sunday, as POSIX
        day = 1 + (wday - first) % 7 + (week - 1) * 7
        last = calendar.monthrange(year, month)[1]
        while day > last:
            day -= 7
        return calendar.timegm((year, month, day, 0, 0, 0)) + rule[3]

    def is_dst(self, t):
        if not self.dst_name:
            return False
        year = time.gmtime(t + self.std).tm_year
        start = self._rule_day(year, self.start) - self.std     # in standard time
        end = self._rule_day(year, self.end) - self.dst         # in summer time
        return start <= t < end if start < end else not end <= t < start

    def offset(self, t):
        return self.dst if self.is_dst(t) else self.std

    def name(self, t):
        return self.dst_name if self.is_dst(t) else self.std_name

    def local(self, t):
        """time.struct_time of t in this zone"""
        return time.gmtime(t + self.offset(t))

    def utc_of(self, y, mo, d, hh, mm):
        """The UTC time of a local date and time (days past the month's end roll over), as mktime with tm_isdst -1"""
        naive = calendar.timegm((y, mo, 1, hh, mm, 0)) + (d - 1) * 86400
        t = naive - self.std
        return naive - self.offset(t)


def radio_zone():
    try:
        with open(settings.TZ_CONF, errors="replace") as f:
            return Zone(f.readline())
    except OSError:
        return Zone("UTC0")


# ---- The settings ----

def _int(value, low, high, default):
    try:
        n = int(str(value).strip())
    except ValueError:
        return default
    return n if low <= n <= high else default


def parse_conf(text):
    """ghostnet.conf as src/ghostnet.c reads it: a value out of range is its default"""
    c = dict(DEFAULTS, nets=[])
    for line in text.splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        key, _, v = line.partition("=")
        if key in ("remind", "own_on", "translate_rx", "translate_tx"):
            c[key] = _int(v, -2**31, 2**31 - 1, 0) != 0
        elif key == "remind_min":
            c[key] = _int(v, 1, 120, 10)
        elif key == "nets":
            c[key] = [n for n in v.split(",") if n]
        elif key == "own_name":
            c[key] = v[:31] or c[key]
        elif key == "own_day":
            c[key] = _int(v, 0, 6, 4)
        elif key == "own_time":
            m = re.fullmatch(r"\s*(\d{1,2}):(\d{1,2})\s*", v)
            if m and int(m.group(1)) <= 23 and int(m.group(2)) <= 59:
                c[key] = "%02d:%02d" % (int(m.group(1)), int(m.group(2)))
        elif key == "own_min":
            c[key] = _int(v, 5, 240, 30)
        elif key == "own_khz":
            c[key] = _int(v, 1800, 54000, 7107)
        elif key == "own_mode":
            if v in MODES:
                c[key] = v
        elif key == "dept":
            c[key] = re.sub(r"[^A-Za-z0-9]", "", v).upper()[:3]
        elif key == "region":
            c[key] = v[:15]
    return c


def read_conf():
    try:
        with open(settings.GHOSTNET_CONF, encoding="utf-8", errors="replace") as f:
            return parse_conf(f.read())
    except OSError:
        return dict(DEFAULTS, nets=[])


def conf_text(c):
    """The file as src/ghostnet.c writes it"""
    b = lambda v: "1" if v else "0"   # noqa: E731
    return ("# GhostNet: the JS8 app's GhostNet popup and the web page GhostNet write this file\n"
            f"remind={b(c['remind'])}\nremind_min={c['remind_min']}\nnets={','.join(c['nets'])}\n"
            f"own_on={b(c['own_on'])}\nown_name={c['own_name']}\nown_day={c['own_day']}\nown_time={c['own_time']}\n"
            f"own_min={c['own_min']}\nown_khz={c['own_khz']}\nown_mode={c['own_mode']}\n"
            f"dept={c['dept']}\nregion={c['region']}\n"
            f"translate_rx={b(c['translate_rx'])}\ntranslate_tx={b(c['translate_tx'])}\n")


def change_conf(c, new, net_ids):
    """The page's changes (a dict of some keys) into a copy of c: (conf, None), or (None, what is wrong)"""
    c = dict(c, nets=list(c["nets"]))
    for key in ("remind", "own_on", "translate_rx", "translate_tx"):
        if key in new:
            if not isinstance(new[key], bool):
                return None, f"{key} is true or false"
            c[key] = new[key]

    def number(key, low, high, what):
        v = new[key]
        if isinstance(v, bool) or not isinstance(v, (int, str)) or not re.fullmatch(r"\s*\d+\s*", str(v)):
            raise ValueError(what)
        n = int(v)
        if not low <= n <= high:
            raise ValueError(what)
        return n

    try:
        if "remind_min" in new:
            c["remind_min"] = number("remind_min", 1, 120, "Reminder: 1 to 120 minutes before")
        if "own_day" in new:
            c["own_day"] = number("own_day", 0, 6, "The day is 0 (Sunday) to 6 (Saturday)")
        if "own_min" in new:
            c["own_min"] = number("own_min", 5, 240, "Length: 5 to 240 minutes")
        if "own_khz" in new:
            v = str(new["own_khz"]).strip().replace(",", ".")
            if isinstance(new["own_khz"], bool) or not re.fullmatch(r"\d+(\.\d+)?", v):
                raise ValueError("Frequency in kHz, 1800 to 54000")
            k = float(v)
            k = round(k * 1000) if k < 100 else round(k)    # 7.107 as MHz
            if not 1800 <= k <= 54000:
                raise ValueError("Frequency in kHz, 1800 to 54000")
            c["own_khz"] = int(k)
    except ValueError as e:
        return None, str(e)
    if "nets" in new:
        nets = new["nets"]
        if not isinstance(nets, list) or not all(isinstance(n, str) for n in nets):
            return None, "nets is a list of the nets' ids"
        unknown = [n for n in nets if n not in net_ids and n != OWN]
        if unknown:
            return None, "No such net: " + ", ".join(unknown)
        c["nets"] = [n for i, n in enumerate(nets) if n not in nets[:i]]
        if len(",".join(c["nets"])) > 95:
            return None, "Too many nets"
    if "own_time" in new:
        m = re.fullmatch(r"\s*([01]?\d|2[0-3]):([0-5]\d)\s*", str(new["own_time"]))
        if not m:
            return None, "The time like 21:00"
        c["own_time"] = "%02d:%s" % (int(m.group(1)), m.group(2))
    if "own_mode" in new:
        if new["own_mode"] not in MODES:
            return None, "The mode is JS8, RTTY, LSB or USB"
        c["own_mode"] = new["own_mode"]
    if "own_name" in new:
        name = " ".join(re.sub(r"[\x00-\x1f\x7f]", " ", str(new["own_name"])).split())
        if len(name.encode("utf-8")) > 31:
            return None, "The name: at most 31 characters"
        c["own_name"] = name or DEFAULTS["own_name"]
    if "dept" in new:
        dept = str(new["dept"]).strip().upper()
        if not re.fullmatch(r"[A-Z0-9]{0,3}", dept):
            return None, "The department: up to 3 letters or digits"
        c["dept"] = dept
    if "region" in new:
        r = str(new["region"]).strip().upper().lstrip("@")
        if r and not re.fullmatch(r"GN[A-Z0-9]{3,12}", r):
            return None, "The regional group is @GN and the rest, like @GNURYMO"
        c["region"] = "@" + r if r else ""
    return c, None


def region_tag(conf, iso2=None):
    """(the regional group, how it was made): the one typed, else @GN + the country's alpha-3 + the department"""
    if conf["region"]:
        return conf["region"], "typed here"
    a3 = ISO3.get((country_iso2() if iso2 is None else iso2 or "").upper()[:2])
    if not a3:
        return "", "the radio's country is not known: type the group"
    return "@GN" + a3 + conf["dept"], "the radio's country" + (" and the department" if conf["dept"] else "")


# ---- The schedule ----

def parse_schedule(text):
    """({id: name}, [windows]) as src/ghostnet.c reads ghostnet_nets.txt; a bad line is left out"""
    nets, segs = {}, []
    for line in text.splitlines():
        line = re.split(r"[#\r\n]", line, maxsplit=1)[0]
        first = line.split(None, 1)
        if not first:
            continue
        f = line.split(None, 2 if first[0] == "net" else 6)   # the rest of the line: the name or the description
        if f[0] == "net":
            if len(f) >= 2 and len(nets) < 16:
                nets[f[1][:7]] = (f[2].strip() if len(f) > 2 and f[2].strip() else f[1])[:39]
            continue
        if len(f) < 6 or len(segs) >= 48:
            continue
        names = [d.lower() for d in DAYS]
        day = names.index(f[1].lower()) if f[1].lower() in names else -1
        m = re.fullmatch(r"(\d+):(\d+)", f[2])
        if day < 0 or not m or int(m.group(1)) > 23 or int(m.group(2)) > 59 or not f[3].isdigit():
            continue
        minutes = int(f[3])
        if not 1 <= minutes <= 1440:
            continue
        khz = 0
        if f[4] != "-":
            if not f[4].isdigit() or not 100 <= int(f[4]) <= 450000:
                continue
            khz = int(f[4])
        what = f[6].strip() if len(f) > 6 and f[6].strip() else f[5]
        segs.append({"net": f[0][:7], "wday": day, "hh": int(m.group(1)), "mm": int(m.group(2)), "minutes": minutes,
                     "khz": khz, "mode": f[5][:7], "what": what[:39]})
    return nets, segs


def read_schedule():
    for path in (settings.GHOSTNET_NETS_USER, settings.GHOSTNET_NETS):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                return parse_schedule(f.read())
        except OSError:
            continue
    return {}, []


def next_utc(now, wday, hh, mm, minutes):
    """The start of the window now on, or the next one (src/ghostnet.c ghostnet_next_utc)"""
    t = time.gmtime(now)
    day0 = int(now) - (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec)
    s = day0 + (wday - (t.tm_wday + 1) % 7 + 7) % 7 * 86400 + hh * 3600 + mm * 60     # Python's tm_wday: 0 Monday
    if s + minutes * 60 <= now:
        s += 7 * 86400
    if s - 7 * 86400 + minutes * 60 > now:
        s -= 7 * 86400
    return s


def next_local(now, zone, wday, hh, mm, minutes):
    """The same for a weekday and time of the radio's zone (ghostnet_next_local)"""
    t = zone.local(now)
    ahead = (wday - (t.tm_wday + 1) % 7 + 7) % 7
    for week in (-1, 0, 1):
        s = zone.utc_of(t.tm_year, t.tm_mon, t.tm_mday + ahead + week * 7, hh, mm)
        if s + minutes * 60 > now:
            return s
    return now + 7 * 86400


def when(t, zone):
    """"Thu 18:00 UTC", and the radio's time when its zone is not UTC: "Fri 01:00 UTC (Thu 20:00 EST)" """
    u, loc = time.gmtime(t), zone.local(t)
    text = "%s %02d:%02d UTC" % (DAYS[(u.tm_wday + 1) % 7], u.tm_hour, u.tm_min)
    if zone.offset(t) == 0:
        return text
    day = DAYS[(loc.tm_wday + 1) % 7] + " " if loc.tm_wday != u.tm_wday else ""
    return text + " (%s%02d:%02d %s)" % (day, loc.tm_hour, loc.tm_min, zone.name(t))


def schedule(conf, nets, segs, zone, now=None, days=7):
    """The windows of the next days, the one now on first, with where the reminder comes"""
    now = time.time() if now is None else now
    out = []
    for g in segs:
        s = next_utc(now, g["wday"], g["hh"], g["mm"], g["minutes"])
        if s <= now + days * 86400:
            out.append(dict(g, start=s, name=nets.get(g["net"], g["net"])))
    if conf["own_on"]:
        hh, mm = (int(x) for x in conf["own_time"].split(":"))
        s = next_local(now, zone, conf["own_day"], hh, mm, conf["own_min"])
        if s <= now + days * 86400:
            out.append({"net": OWN, "name": conf["own_name"], "start": s, "minutes": conf["own_min"],
                        "khz": conf["own_khz"], "mode": conf["own_mode"], "what": conf["own_mode"]})
    out.sort(key=lambda w: w["start"])
    rows = []
    for w in out:
        follows = any(o is not w and o["net"] == w["net"] and o["start"] + o["minutes"] * 60 == w["start"] for o in out)
        reminded = w["net"] in conf["nets"]
        rows.append({"start": int(w["start"]), "when": when(w["start"], zone), "net": w["net"], "name": w["name"],
                     "minutes": w["minutes"], "khz": w["khz"], "mode": w["mode"], "what": w["what"],
                     "now": w["start"] <= now < w["start"] + w["minutes"] * 60, "reminded": reminded,
                     # where the radio's reminder comes: the first JS8 window of a net chosen, or the own net
                     "reminder": conf["remind"] and reminded and (w["net"] == OWN or (w["mode"] == "JS8" and not follows))})
    return rows


# ---- The messages the JS8 app wrote ----

def messages(zone, limit=150):
    """(the newest messages first, with their translations; the stations heard, the last heard first). The lines:
    kind (R heard, X sent, T a translation), epoch, message id, from, to, SNR, dial Hz, text; tab-separated"""
    lines = []
    for path in (settings.GHOSTNET_LOG + ".1", settings.GHOSTNET_LOG):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                lines += f.read().splitlines()
        except OSError:
            pass
    msgs, trans = [], {}
    for line in lines:
        p = line.split("\t")
        if len(p) < 8 or p[0] not in ("R", "X", "T"):
            continue
        try:
            epoch, snr, dial = int(p[1]), int(p[5]), int(p[6])
        except ValueError:
            continue
        kind, mid, frm, to, text = p[0], p[2], p[3], p[4], "\t".join(p[7:])
        if kind == "T":
            trans[(mid, frm)] = text
            continue
        loc = zone.local(epoch)
        msgs.append({"kind": kind, "time": epoch, "local": "%02d-%02d %02d:%02d" % (loc.tm_mon, loc.tm_mday,
                     loc.tm_hour, loc.tm_min), "id": mid, "from": frm, "to": to, "snr": snr, "dial": dial,
                     "text": text, "flash": to.upper() == "@GSTFLASH" or bool(re.search(r"(?<![A-Z0-9])@GSTFLASH(?![A-Z0-9/])",
                                                                                        text.upper()))})
    for m in msgs:
        m["translation"] = trans.get((m["id"], m["from"]), "") if m["id"] != "0" else ""
    stations = {}
    for m in msgs:
        if m["kind"] != "R" or not m["from"]:
            continue
        s = stations.setdefault(m["from"], {"call": m["from"], "count": 0, "groups": []})
        s["count"] += 1
        s["last"], s["local"], s["snr"], s["dial"] = m["time"], m["local"], m["snr"], m["dial"]
        if m["to"].startswith("@") and m["to"] not in s["groups"]:
            s["groups"].append(m["to"])
    return msgs[-limit:][::-1], sorted(stations.values(), key=lambda s: -s["last"])[:100]


def state(conf=None, now=None):
    """What the page shows"""
    conf = read_conf() if conf is None else conf
    zone = radio_zone()
    nets, segs = read_schedule()
    tag, why = region_tag(conf)
    msgs, stations = messages(zone)
    t = time.time() if now is None else now
    return {"conf": conf, "nets": [{"id": k, "name": v} for k, v in nets.items()], "region": tag, "region_why": why,
            "schedule": schedule(conf, nets, segs, zone, t), "messages": msgs, "stations": stations,
            "zone": zone.name(t), "utc_offset": zone.offset(t)}
