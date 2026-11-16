#!/usr/bin/env python3
"""The GUI's language files (po/, src/i18n.h): the texts from the sources, each language brought up to them, checked.

  python3 -I tools/i18n/potool.py extract            the texts of src/ into po/openx6x00.pot
  python3 -I tools/i18n/potool.py update [code ...]  each po/<code>.po (all without a code) brought up to the .pot
  python3 -I tools/i18n/potool.py check [code ...]   the conversions, what is left to translate; 1 on an error
  python3 -I tools/i18n/potool.py new <code> <name>  a new language: po/<code>.po from the .pot ("Translation: <name>")
  python3 -I tools/i18n/potool.py import-tsv <code> <name> <file.tsv>
                                                     a list "English<TAB>translation" (the CX8ANM edition's) as a
                                                     language file, merged into one already there

The English is the key, as in the GUI: a text the sources no longer have goes to the end of the file as obsolete
(#~), unless it is a line of a longer text that is still there (the GUI looks a text up line by line). The texts are
found by their place: what is handed to LVGL (i18n_hook.h's list), to our own row and message helpers, the tables of
labels; what goes to a log, a file, a command or a comparison is left out. A text the GUI shows that is missing: the
PC build with X6100_I18N_MISSING=<file> lists them as they reach the screen (src/i18n.cpp).

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PO = ROOT / 'po'
POT = PO / 'openx6x00.pot'

# ---- the sources ----

SKIP_DIRS = ('src/fonts/', 'src/hw/', 'src/params/', 'src/cfg/')
# the lists of countries and places stay English, as the DXCC list and the spots name them; the protocol files
SKIP_FILES = {'src/i18n.cpp', 'src/i18n.h', 'src/i18n_hook.h', 'src/navtex/dsc.c', 'src/dxcc.c', 'src/beacons.c',
              'src/avi_writer.c', 'src/tci_ws.c', 'src/theme.c', 'src/theme.h',   # the themes' names stay theirs
              'src/adif.c'}   # the log file's field names

# a text handed to one of these is not shown
SKIP_CALLS = re.compile(
    r'^(LV_LOG_\w*|f?printf|fputs|puts|perror|syslog|v?dprintf|fopen|freopen|open|openat|popen|system|getenv|setenv|'
    r'str(n|case|ncase)?cmp|strn?case|strstr|strcasestr|strchr|strrchr|strspn|strcspn|strtok\w*|s?scanf|sqlite3_\w+|'
    r'cJSON_\w+|json_\w+|vnet_send|\w*_json\w*|remote_\w*send\w*|tci_\w*send\w*|send_\w+|reply\w*|log_\w+|\w+_log|\w+_log_\w*|'
    r'base_guard_log|unlink|rename|remove|mkdir|rmdir|access|stat|lstat|opendir|chdir|exec\w*|spawn\w*|run_cmd\w*|'
    r'params_\w+|cfg_\w+|db_\w+|sql\w*|memcmp|strlen|starts_with|ends_with|has_prefix|prefix\w*|regcomp|'
    r'pthread_setname_np|prctl|dlsym|dlopen|ioctl|write_file|read_file|file_\w+|path_\w+|kv_\w+|ini_\w+|'
    r'nm_\w+|nmcli\w*|bt_cmd\w*|wpa_\w+|curl_\w+|http_\w+|url_\w+|mosquitto_\w+|mqtt_\w+|adif_\w+|'
    r'g_\w+|call|call_on|dev_bool|dev_\w+|prop_\w+|theme_\w+|'
    r'assert|static_assert|_Static_assert|defined|__attribute__|alias|section|asm|__asm__)$')
# a text in one of these fields is not shown
SKIP_FIELDS = re.compile(r'^(name|key|db_name|db|path|file|dir|cmd|command|id|tag|json|param|sql|url|host|mime|'
                         r'ext|suffix|prefix|format_key|cfg|section|table|column|field|dev|device|iface|unit_key)$')

LETTERS = re.compile(r'[A-Za-z]{2,}')
CONV = re.compile(r"%(?:\d+\$)?[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|q|j|z|t)?[diouxXeEfFgGaAcspn%]")
UNITS = re.compile(r'\b(Hz|kHz|MHz|GHz|dB|dBm|dBFS|ms|us|W|mW|V|mV|A|mA|wpm|WPM|px|km|mi|s|UTC|bd|baud|min|ppm|'
                   r'SWR|kB|MB|GB|%)\b')


def not_shown(s):
    """A text that is not words for a person: a path, an address, a name of a setting, SQL, a format alone"""
    t = s.strip()
    if not LETTERS.search(CONV.sub('', t)):
        return True
    if re.match(r'^(/|\./|\w+://|www\.)', t) or re.search(r'\.(c|h|cpp|db|json|png|jpg|bmp|wav|txt|so|sh|conf|csv|'
                                                         r'adi|adif|log|bin|pem|ini|xml|dat|po|ttf|otf|py|js|html|css)$', t):
        return True
    if re.match(r'^(SELECT|INSERT|UPDATE|DELETE|CREATE|DROP|ALTER|PRAGMA|BEGIN|COMMIT|REPLACE|WITH)\b', t):
        return True
    if t.startswith(('{', '[', '<', '\\x', '=', '&')) or '":' in t or '</' in t:
        return True
    if re.match(r'^[a-z0-9_.\-]+$', t) and ('_' in t or '.' in t or '-' in t):   # theme_name, x6100.local, wf-mode
        return True
    if re.match(r'^[a-z][a-z0-9]*$', t):            # a word in small letters alone: a key ("zoom", "digu", "sharp")
        return True
    if re.match(r'^[A-Za-z]+[a-z][A-Z]\w*$', t):    # NoInputNoOutput, GetManagedObjects, HamQTH
        return True
    if re.match(r'^[A-Z][a-z]+/[A-Z]', t):          # Europe/London (GMT/BST): a zone's name
        return True
    if re.match(r'^err ', t) or re.match(r'^[a-z]+ (%|\[|no$|ok$)', t):   # the browser's protocol (remote_server.c)
        return True
    for line in t.split('\n'):                      # vfo:0,0,%d;  org.bluez.Device1  (ssv)  AT+BYPASS=1
        line = line.strip(' \t()')                # a page button's "(SETTINGS" "1:3)" is words
        if re.match(r'^[A-Za-z0-9]+>[A-Za-z0-9]+:?$', line):   # a link's direction ("RF>SVX:"): words
            continue
        if LETTERS.search(line) and ' ' not in line and re.search(r'[;=+(){}\[\]<>|\\]|:.|[a-z]\.[a-zA-Z]', line):
            return True
    if re.match(r'^[A-Z0-9_]+=', t) or re.match(r'^-{1,2}[a-z]', t):           # KEY=, --option
        return True
    if re.match(r'^[A-Z0-9]+(_[A-Z0-9]+)+$', t):    # STATION_CALLSIGN, RST_SENT: a log file's field
        return True
    return False


# the radio's abbreviations of four letters and more, which stay as they are (those of two or three do too)
ABBR = {'WSPR', 'RTTY', 'SSTV', 'NAVTEX', 'WEFAX', 'CTCSS', 'QRSS', 'FREEDV', 'RADE', 'JTTY', 'BPSK', 'QPSK', 'MFSK',
        'GNSS', 'HTTP', 'HTTPS', 'MIDI', 'WIFI', 'DXCC', 'ADIF', 'IARU', 'NCDXF', 'POTA', 'SOTA', 'WWFF', 'IOTA', 'TOTA',
        'MOTA', 'XOTA', 'NOTA', 'LOTW', 'EQSL', 'ARDOP', 'VARA', 'FLRIG', 'HAMLIB', 'CWID', 'QRPP', 'WAVELOG', 'QRZCQ',
        'SPLIT', 'ICOM', 'XARA', 'NMEA', 'BAUD', 'DTMF', 'EDID', 'HDMI', 'JSON', 'MQTT', 'IQSWAP', 'OLIVIA', 'CONTESTIA',
        'ARRL', 'RSGB', 'WSJT', 'PSKR', 'SNTP', 'OPENX6X00', 'MENU', 'FUNC', 'PTT'}


def trivial(s):
    """Nothing to translate in it: numbers, units, the radio's abbreviations (USB, CW, AGC, NR, WSPR) on every line"""
    for line in s.split('\n'):
        t = UNITS.sub('', CONV.sub('', line))
        for w in re.findall(r'[A-Za-z][A-Za-z0-9]+', t):
            if not (w.isupper() and (len(w) <= 3 or w in ABBR or re.search(r'\d', w))):
                return False
    return True


def lex(src):
    """The tokens of a C/C++ source: ('s', text, line), ('id', name, line), ('p', char, line); comments and character
    constants dropped; adjacent string literals joined into one (as the compiler does)"""
    toks, i, line, n = [], 0, 1, len(src)
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
        elif c in ' \t\r\f\v\\':
            i += 1
        elif src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            line += src.count('\n', i, j)
            i = j
        elif c == '#' and (not toks or toks[-1][2] != line or src[src.rfind('\n', 0, i) + 1:i].strip() == ''):
            # a preprocessor line: #include <...> and #if's words are not texts; a #define's are
            j = i
            while True:
                k = src.find('\n', j)
                if k < 0:
                    k = n
                    break
                if src[k - 1] != '\\':
                    break
                j = k + 1
            directive = src[i:k]
            if re.match(r'#\s*define\b', directive):
                toks.append(('p', '#define', line))
                i = i + len(re.match(r'#\s*define', directive).group(0))
            else:
                line += directive.count('\n')
                i = k
        elif c == '"' or (c in 'uUL' and src[i + 1:i + 2] == '"') or (src.startswith('u8"', i)):
            start_line = line
            if c != '"':
                i = src.index('"', i)
            j, buf = i + 1, []
            while j < n and src[j] != '"':
                if src[j] == '\\':
                    buf.append(src[j:j + 2])
                    j += 2
                    continue
                if src[j] == '\n':
                    line += 1
                buf.append(src[j])
                j += 1
            text = ''.join(buf)
            if toks and toks[-1][0] == 's':
                toks[-1] = ('s', toks[-1][1] + text, toks[-1][2])
            else:
                toks.append(('s', text, start_line))
            i = j + 1
        elif c == 'R' and src.startswith('R"', i):
            m = re.match(r'R"([^(]*)\(', src[i:])
            end = src.find(')' + m.group(1) + '"', i)
            line += src.count('\n', i, end)
            i = end + len(m.group(1)) + 2
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            i = j + 1
        elif c.isalpha() or c == '_':
            m = re.match(r'[A-Za-z_]\w*', src[i:])
            toks.append(('id', m.group(0), line))
            i += len(m.group(0))
        elif c.isdigit():
            m = re.match(r'[0-9][0-9A-Za-z_.\']*', src[i:])
            toks.append(('p', '0', line))
            i += len(m.group(0))
        else:
            toks.append(('p', c, line))
            i += 1
    return toks


def c_unescape(text):
    """A literal's text as the program sees it, UTF-8; None: bytes that are not text"""
    out, i = bytearray(), 0
    simple = {'n': 10, 't': 9, 'r': 13, '"': 34, '\\': 92, "'": 39, '0': 0, 'a': 7, 'b': 8, 'f': 12, 'v': 11, '?': 63}
    while i < len(text):
        c = text[i]
        if c != '\\':
            out += c.encode('utf-8')
            i += 1
            continue
        e = text[i + 1:i + 2]
        if e == 'x':
            m = re.match(r'[0-9A-Fa-f]{1,2}', text[i + 2:])
            out.append(int(m.group(0), 16))
            i += 2 + len(m.group(0))
        elif e in '01234567' and e:
            m = re.match(r'[0-7]{1,3}', text[i + 1:])
            out.append(int(m.group(0), 8) & 0xFF)
            i += 1 + len(m.group(0))
        elif e in simple:
            out.append(simple[e])
            i += 2
        else:
            out += text[i:i + 2].encode('utf-8')
            i += 2
    try:
        return out.decode('utf-8')
    except UnicodeDecodeError:
        return None


def callee(toks, i):
    """The function whose argument token i is, and the field a designated initializer gives it (.label = "x")"""
    field = None
    if i >= 2 and toks[i - 1][1] == '=' and toks[i - 2][0] == 'id' and i >= 3 and toks[i - 3][1] == '.':
        field = toks[i - 2][1]
    depth = 0
    for k in range(i - 1, max(-1, i - 400), -1):
        t = toks[k][1]
        if toks[k][0] != 'p':
            continue
        if t in ')]}':
            depth += 1
        elif t in '([{':
            if depth == 0:
                if t == '(' and k > 0 and toks[k - 1][0] == 'id':
                    return toks[k - 1][1], field
                if t == '{':
                    return None, field
                if t == '(' and k > 0 and toks[k - 1][1] == ')':   # a cast or a call through a pointer
                    continue
                return None, field
            depth -= 1
        elif t == ';' and depth == 0:
            return None, field
    return None, field


def extract_file(path, rel):
    src = path.read_text(encoding='utf-8', errors='replace')
    toks = lex(src)
    found = []
    for i, (kind, text, line) in enumerate(toks):
        if kind != 's':
            continue
        nxt = toks[i + 1][1] if i + 1 < len(toks) else ''
        prev = toks[i - 1][1] if i else ''
        if prev == 'include' or prev == '#define' or prev in ('extern',):
            continue
        if nxt == '[' or prev in ('==', '!='):
            continue
        fn, field = callee(toks, i)
        if fn and SKIP_CALLS.match(fn):
            continue
        if field and SKIP_FIELDS.match(field):
            continue
        s = c_unescape(text)
        if s is None or not s.strip() or not_shown(s) or trivial(s):
            continue
        found.append((s, f'{rel}', line))
    return found


def all_literals():
    """Every text of the sources, the ones the list leaves out too: a translation of one is kept (a word the code puts
    into a longer text, "heard", "park")"""
    out = set()
    for path, _rel in sources():
        for kind, text, _line in lex(path.read_text(encoding='utf-8', errors='replace')):
            if kind == 's':
                s = c_unescape(text)
                if s and s.strip():
                    out.add(s)
                    out.add(s.strip())
    return out


def sources():
    for p in sorted((ROOT / 'src').rglob('*')):
        rel = p.relative_to(ROOT).as_posix()
        if p.suffix not in ('.c', '.cpp', '.h', '.hpp') or rel in SKIP_FILES or rel.startswith(SKIP_DIRS):
            continue
        yield p, rel

# ---- .po files ----


class Entry:
    def __init__(self, msgid='', msgstr='', comments=None, refs=None, flags=None, obsolete=False):
        self.msgid, self.msgstr = msgid, msgstr
        self.comments = comments or []   # the translator's ("# ...") and ours ("#. ...")
        self.refs = refs or []
        self.flags = flags or []
        self.obsolete = obsolete


def po_escape(s):
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r').replace('\n', '\\n')


def po_unescape(s):
    out, i = [], 0
    m = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            out.append(m.get(s[i + 1], '\\' + s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def po_string(key, s):
    if '\n' in s[:-1]:
        parts = [p + '\n' for p in s.split('\n')]
        if s.endswith('\n'):
            parts = parts[:-1]
        else:
            parts[-1] = parts[-1][:-1]
        return f'{key} ""\n' + ''.join(f'"{po_escape(p)}"\n' for p in parts)
    return f'{key} "{po_escape(s)}"\n'


def read_po(path):
    """The header's text and the entries, in order"""
    entries, e, field, plural = [], Entry(), None, False

    def flush():
        nonlocal e, field, plural
        if field is not None:
            entries.append(e)
        e, field, plural = Entry(), None, False

    for raw in path.read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        obsolete = line.startswith('#~')
        if obsolete:
            line = line[2:].strip()
        if not line:
            flush()
            continue
        if line.startswith('#'):
            if field is not None and field.startswith('msgstr'):
                flush()
            if line.startswith('#:'):
                e.refs += line[2:].split()
            elif line.startswith('#,'):
                e.flags += [f.strip() for f in line[2:].split(',') if f.strip()]
            else:
                e.comments.append(line)
            continue
        if field is not None and field.startswith('msgstr') and line.startswith('msgid'):
            flush()
        e.obsolete = e.obsolete or obsolete
        m = re.match(r'^(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+"(.*)"$', line)
        if m:
            field = m.group(1)
            val = po_unescape(m.group(2))
            if field == 'msgid':
                e.msgid = val
            elif field in ('msgstr', 'msgstr[0]'):
                e.msgstr = val
            continue
        m = re.match(r'^"(.*)"$', line)
        if m and field:
            val = po_unescape(m.group(1))
            if field == 'msgid':
                e.msgid += val
            elif field in ('msgstr', 'msgstr[0]'):
                e.msgstr += val
    flush()
    header = entries.pop(0) if entries and entries[0].msgid == '' else Entry()
    return header, entries


def write_po(path, header, entries):
    out = []
    for c in header.comments:
        out.append(c + '\n')
    if header.flags:
        out.append('#, ' + ', '.join(header.flags) + '\n')
    out.append('msgid ""\n')
    out.append('msgstr ""\n' + ''.join(f'"{po_escape(l)}\\n"\n' for l in header.msgstr.split('\n') if l) + '\n')
    for e in entries:
        pre = '#~ ' if e.obsolete else ''
        for c in e.comments:
            out.append(c + '\n')
        if e.refs and not e.obsolete:
            refs = sorted(set(e.refs), key=e.refs.index)
            line = '#:'
            for r in refs:
                if len(line) + len(r) + 1 > 79:
                    out.append(line + '\n')
                    line = '#:'
                line += ' ' + r
            out.append(line + '\n')
        if e.flags:
            out.append('#, ' + ', '.join(sorted(set(e.flags))) + '\n')
        block = po_string('msgid', e.msgid) + po_string('msgstr', e.msgstr)
        out.append(''.join(pre + l + '\n' for l in block.splitlines()) + '\n')
    path.write_text(''.join(out), encoding='utf-8')


def header_text(name='', status='draft', lang='', font=''):
    lines = ['Project-Id-Version: OpenX6X00', f'Language: {lang}', 'MIME-Version: 1.0',
             'Content-Type: text/plain; charset=UTF-8', 'Content-Transfer-Encoding: 8bit',
             f'X-OpenX6X00-Name: {name}', f'X-OpenX6X00-Status: {status}']
    if font:
        lines.append(f'X-OpenX6X00-Font: {font}')
    return '\n'.join(lines) + '\n'


def convs(s):
    """The printf conversions in order, as src/i18n.cpp compares them: flags and widths aside, "*", "$", the length
    and the letter kept"""
    out, i = [], 0
    while i < len(s):
        if s[i] != '%':
            i += 1
            continue
        j = i + 1
        if j < len(s) and s[j] == '%':
            i = j + 1
            continue
        k = ''
        while j < len(s) and s[j] in "-+ #0'":
            j += 1
        while j < len(s) and (s[j].isdigit() or s[j] in '.*$'):
            k += s[j] if s[j] in '*$' else ''
            j += 1
        while j < len(s) and s[j] in 'hlLqjzt':
            k += s[j]
            j += 1
        out.append(k + (s[j] if j < len(s) else '?'))
        i = j + 1
    return out


# ---- the commands ----


def cmd_extract(_args):
    texts = {}
    for path, rel in sources():
        for s, ref, _line in extract_file(path, rel):
            e = texts.get(s)
            if not e:
                e = texts[s] = Entry(s, refs=[], flags=['c-format'] if CONV.search(s.replace('%%', '')) else [])
            if ref not in e.refs:
                e.refs.append(ref)
    PO.mkdir(exist_ok=True)
    header = Entry('', header_text(), comments=[
        '# The texts of the OpenX6X00 GUI. Made by tools/i18n/potool.py extract: not edited by hand.',
        '# A new language: python3 -I tools/i18n/potool.py new <code> <name>, then po/README.md.'])
    write_po(POT, header, list(texts.values()))
    print(f'{POT.relative_to(ROOT)}: {len(texts)} texts from {len({r for e in texts.values() for r in e.refs})} files')


def lines_of(s):
    """What the GUI looks up of a text line by line (src/i18n.cpp, line_tr()): each line without the spaces, brackets
    and LVGL symbols at its ends, without its colon, a "Label: value" line's label"""
    out = set()
    for line in s.split('\n'):
        t = line.strip(' \t()')
        while t and (0xF000 <= ord(t[0]) <= 0xF8FF or 0xF000 <= ord(t[-1]) <= 0xF8FF):
            t = t[1:] if 0xF000 <= ord(t[0]) <= 0xF8FF else t[:-1]
            t = t.strip(' \t()')
        if t:
            out.add(t)
            out.add(t.rstrip(':'))
            if ': ' in t:
                out.add(t.split(': ', 1)[0])
    return out


def merge(entries, pot_entries):
    """The language's entries brought up to the .pot's texts, in its order; one the .pot lacks kept when it is a line
    of a longer text or a text the sources still have, obsolete else"""
    have = {e.msgid: e for e in entries}
    parts = all_literals()
    for p in pot_entries:
        parts |= lines_of(p.msgid)
    out, used = [], set()
    for p in pot_entries:
        e = have.get(p.msgid)
        n = Entry(p.msgid, e.msgstr if e else '', e.comments if e else [], p.refs,
                  sorted(set(p.flags) | ({'fuzzy'} if e and 'fuzzy' in e.flags else set())))
        out.append(n)
        used.add(p.msgid)
    pot_ids = {p.msgid for p in pot_entries}
    for e in entries:   # a line of a longer text: kept, the GUI finds it line by line
        if e.msgid in used or not e.msgstr:
            continue
        if e.msgid in parts and e.msgid not in pot_ids:
            comments = [c for c in e.comments if not c.startswith('#.')] + ['#. not in the list: a line or a word of a longer text']
            out.append(Entry(e.msgid, e.msgstr, comments, [], [f for f in e.flags if f == 'fuzzy']))
            used.add(e.msgid)
    for e in entries:
        if e.msgid not in used and e.msgstr:
            out.append(Entry(e.msgid, e.msgstr, [c for c in e.comments if not c.startswith('#.')], [], [],
                             obsolete=True))
    return out


def codes(args):
    if args:
        return args
    return sorted(p.stem for p in PO.glob('*.po'))


def cmd_update(args):
    _, pot = read_po(POT)
    for code in codes(args):
        path = PO / f'{code}.po'
        header, entries = read_po(path)
        merged = merge(entries, pot)
        write_po(path, header, merged)
        done = sum(1 for e in merged if e.msgstr and not e.obsolete and e.refs)
        print(f'{path.relative_to(ROOT)}: {done}/{len(pot)} translated, '
              f'{sum(1 for e in merged if e.obsolete)} obsolete')


def cmd_check(args):
    _, pot = read_po(POT)
    bad = 0
    for code in codes(args):
        path = PO / f'{code}.po'
        header, entries = read_po(path)
        fields = dict(l.split(':', 1) for l in header.msgstr.split('\n') if ':' in l)
        name = fields.get('X-OpenX6X00-Name', '').strip()
        if not name:
            print(f'{path.name}: no X-OpenX6X00-Name in the header (the name in the list of languages)')
            bad += 1
        for e in entries:
            if e.obsolete or not e.msgstr:
                continue
            if convs(e.msgid) != convs(e.msgstr):
                print(f'{path.name}: the conversions differ (the GUI leaves it out): {e.msgid!r} -> {e.msgstr!r}')
                bad += 1
            if e.msgid.count('\n') != e.msgstr.count('\n') and '\n' in e.msgid and len(e.msgid) < 40:
                print(f'{path.name}: note: another number of lines (a button label?): {e.msgid!r} -> {e.msgstr!r}')
        ids = {e.msgid for e in entries if e.msgstr and not e.obsolete and 'fuzzy' not in e.flags}
        done = sum(1 for p in pot if p.msgid in ids)
        print(f'{path.name}: {name or "?"}, {done}/{len(pot)} translated ({100 * done // max(1, len(pot))} %)')
    sys.exit(1 if bad else 0)


def cmd_new(args):
    code, name = args[0], ' '.join(args[1:])
    path = PO / f'{code}.po'
    if path.exists():
        sys.exit(f'{path} is there already')
    _, pot = read_po(POT)
    header = Entry('', header_text(name, 'draft', code), comments=[f'# {name} for the OpenX6X00 GUI.'])
    write_po(path, header, merge([], pot))
    print(f'{path.relative_to(ROOT)}: {len(pot)} texts to translate')


def cmd_import_tsv(args):
    code, name, tsv = args[0], args[1], pathlib.Path(args[2])
    path = PO / f'{code}.po'
    _, pot = read_po(POT)
    if path.exists():
        header, entries = read_po(path)
    else:
        header = Entry('', header_text(name, 'draft', code), comments=[f'# {name} for the OpenX6X00 GUI.'])
        entries = []
    note = f'# The first texts from the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico), {tsv.name}.'
    if note not in header.comments:
        header.comments.append(note)
    have = {e.msgid for e in entries if e.msgstr}
    added = 0
    for raw in tsv.read_text(encoding='utf-8').splitlines():
        if not raw.strip() or raw.startswith('#') or '\t' not in raw:
            continue
        en, tr = raw.split('\t', 1)
        en, tr = en.replace('\\n', '\n'), tr.split('\t')[0].replace('\\n', '\n')
        if en in have or not tr.strip():
            continue
        entries.append(Entry(en, tr))
        have.add(en)
        added += 1
    merged = merge(entries, pot)
    write_po(path, header, merged)
    done = sum(1 for e in merged if e.msgstr and not e.obsolete and e.refs)
    print(f'{path.relative_to(ROOT)}: {added} taken from {tsv.name}, {done}/{len(pot)} translated, '
          f'{sum(1 for e in merged if e.msgstr and not e.refs and not e.obsolete)} line parts, '
          f'{sum(1 for e in merged if e.obsolete)} obsolete')


if __name__ == '__main__':
    cmds = {'extract': cmd_extract, 'update': cmd_update, 'check': cmd_check, 'new': cmd_new,
            'import-tsv': cmd_import_tsv}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](sys.argv[2:])
