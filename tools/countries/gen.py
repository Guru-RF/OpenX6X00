#!/usr/bin/env python3
"""Makes src/countries_data.inc, the countries the first-start guide and Settings > General offer (src/countries.h):
each one's ISO 3166-1 code, its IARU region, the POSIX time zone and the position of its place in the time zone
database (the capital or the largest city), and its name in English and in each language of po/ (sorted as that
language sorts).

  python3 -I tools/countries/gen.py

The sources, read on this PC:
  - the time zone database (tzdata, /usr/share/zoneinfo: iso3166.tab, zone.tab and the zone files; public domain):
    the codes, the zones and their places;
  - the Unicode CLDR (Unicode License V3, its notice in src/LICENSE-Unicode-CLDR.txt) through Node.js's ICU
    (Intl.DisplayNames, Intl.Collator): the names of the countries in each language and their order. CLDR's data comes
    with Node.js; no package to install.
The IARU region is ours (below): the Radio Regulations' regions, country by country.

A letter our fonts lack is left out of a name, or the name is written without its accents (the Latin languages:
Latin-1, as tools/fonts/mklatin.sh); a Chinese name with a character beyond GB2312 (tools/fonts/mkcjk.sh) is given
in English. Run it again when tzdata, CLDR or po/ change.

After the CX8ANM edition of OpenX6X00 (condoruy, tools/countries/gen.py there), which took the names from Babel.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import json
import pathlib
import subprocess
import sys
import unicodedata

ROOT = pathlib.Path(__file__).resolve().parents[2]
ZI = pathlib.Path('/usr/share/zoneinfo')
OUT = ROOT / 'src' / 'countries_data.inc'

# The languages: English first, then each po/<code>.po; the BCP 47 tag CLDR knows it by
LANGS = [''] + sorted(p.stem for p in (ROOT / 'po').glob('*.po'))
TAG = {'': 'en', 'zh_CN': 'zh-Hans-CN'}

# The IARU region: the Radio Regulations' (Article 5, 5.3 to 5.9), the country as a whole. Mongolia, Central Asia,
# the Caucasus and Türkiye are in Region 1 (5.5), Iran and Afghanistan in Region 3; the rest of the world not listed
# here is Region 1
R2 = set('AG AI AR AW BB BL BM BO BQ BR BS BZ CA CL CO CR CU CW DM DO EC FK GD GF GL GP GS GT GY HN HT '
         'JM KN KY LC MF MQ MS MX NI PA PE PM PR PY SR SV SX TC TT US UY VC VE VG VI'.split())
R3 = set('AF AS AU BD BN BT CC CK CN CX FJ FM GU HK ID IN IO IR JP KH KI KP KR LA LK MH MM MO MP MV MY '
         'NC NF NP NR NU NZ PF PG PH PK PN PW SB SG TH TK TL TO TV TW UM VN VU WF WS'.split())
SKIP = {'AQ', 'BV', 'HM', 'TF'}     # no one lives there for good

# A country with several zones: the one of its capital or its largest city
PRIMARY = {'RU': 'Europe/Moscow', 'US': 'America/New_York', 'CA': 'America/Toronto', 'BR': 'America/Sao_Paulo',
           'AU': 'Australia/Sydney', 'MX': 'America/Mexico_City', 'AR': 'America/Argentina/Buenos_Aires',
           'CL': 'America/Santiago', 'ES': 'Europe/Madrid', 'PT': 'Europe/Lisbon', 'KZ': 'Asia/Almaty',
           'CN': 'Asia/Shanghai', 'ID': 'Asia/Jakarta', 'EC': 'America/Guayaquil', 'DE': 'Europe/Berlin',
           'UA': 'Europe/Kyiv', 'MN': 'Asia/Ulaanbaatar', 'CD': 'Africa/Kinshasa', 'NZ': 'Pacific/Auckland',
           'FM': 'Pacific/Pohnpei', 'KI': 'Pacific/Tarawa', 'PF': 'Pacific/Tahiti', 'MY': 'Asia/Kuala_Lumpur',
           'UZ': 'Asia/Tashkent', 'GL': 'America/Nuuk', 'PS': 'Asia/Gaza', 'CY': 'Asia/Nicosia'}

# What the GUI's fonts draw: ASCII, and behind it Latin-1 with the few marks tools/fonts/mklatin.sh adds
LATIN = (set(chr(c) for c in range(0x20, 0x7F)) | set(chr(c) for c in range(0xA0, 0x100))
         | set('Œœ–—‘’‚“”„…€'))
FOLD = {'ß': 'ss', 'Ł': 'L', 'ł': 'l', 'ı': 'i', 'Đ': 'D', 'đ': 'd', 'Ħ': 'H', 'ħ': 'h'}

NODE = r"""
const req = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const out = {};
if (req.what === 'names') {
    for (const [lang, tag] of Object.entries(req.tags)) {
        const dn = new Intl.DisplayNames([tag], {type: 'region', fallback: 'none'});
        out[lang] = Object.fromEntries(req.codes.map(cc => [cc, dn.of(cc) || '']));
    }
    out.versions = process.versions;
} else {
    for (const [lang, tag] of Object.entries(req.tags)) {
        const col = new Intl.Collator([tag], {sensitivity: 'base'});
        const names = req.names[lang];
        out[lang] = names.map((_, i) => i).sort((a, b) => col.compare(names[a], names[b]) || a - b);
    }
}
process.stdout.write(JSON.stringify(out));
"""


def node(req):
    try:
        res = subprocess.run(['node', '-e', NODE], input=json.dumps(req), capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f'gen.py: Node.js with its ICU data is needed for the CLDR names ({e})')
    return json.loads(res.stdout)


def latin_safe(s):
    s = s.replace('’', "'").replace('‘', "'")
    out = []
    for ch in s:
        if ch in LATIN:
            out.append(ch)
        elif ch in FOLD:
            out.append(FOLD[ch])
        else:
            base = ''.join(c for c in unicodedata.normalize('NFKD', ch) if not unicodedata.combining(c))
            out.append(base if base and all(c in LATIN for c in base) else '')
    return ''.join(out)


def gb2312_safe(s):
    try:
        for ch in s:
            if ord(ch) > 0x7E:
                ch.encode('gb2312')
        return True
    except UnicodeEncodeError:
        return False


def posix_tz(zone):
    """The POSIX TZ string at the end of a zone file (TZif v2+): the rule from the last transition on"""
    return (ZI / zone).read_bytes().rstrip(b'\n').rsplit(b'\n', 1)[-1].decode()


def coord(s):
    """+DDMM, +DDMMSS, +DDDMM or +DDDMMSS (zone.tab) in degrees"""
    sign = -1 if s[0] == '-' else 1
    s = s[1:]
    deg = 2 if len(s) in (4, 6) else 3
    d, m, sec = int(s[:deg]), int(s[deg:deg + 2]), int(s[deg + 2:] or 0)
    return sign * (d + m / 60 + sec / 3600)


def main():
    zones = {}
    for line in (ZI / 'zone.tab').read_text().splitlines():
        if line.startswith('#'):
            continue
        cc, co, zone = line.split('\t')[:3]
        if cc in zones and PRIMARY.get(cc) != zone:
            continue
        i = max(co.rfind('+'), co.rfind('-'))
        zones[cc] = (zone, coord(co[:i]), coord(co[i:]))

    codes = sorted(l.split('\t')[0] for l in (ZI / 'iso3166.tab').read_text().splitlines()
                   if l and not l.startswith('#'))
    codes = [cc for cc in codes if cc not in SKIP]
    tags = {l: TAG.get(l, l.replace('_', '-')) for l in LANGS}
    got = node({'what': 'names', 'tags': tags, 'codes': codes})

    rows, english, dropped = [], 0, set()
    for cc in codes:
        en = latin_safe(got[''][cc]) or cc
        names = []
        for l in LANGS:
            n = got[l][cc]
            if l == 'zh_CN':
                ok = n and gb2312_safe(n)
                english += not ok
                n = n if ok else en
            else:
                dropped.update(ch for ch in n if ch not in LATIN and ch not in '‘’')
                n = latin_safe(n) or en
            names.append(n)
        region = 2 if cc in R2 else 3 if cc in R3 else 1
        tz, lat, lon = '', 0.0, 0.0
        if cc in zones:
            zone, lat, lon = zones[cc]
            tz = posix_tz(zone)
        rows.append((cc, region, tz, lat, lon, names))

    order = node({'what': 'order', 'tags': tags, 'names': {l: [r[5][i] for r in rows] for i, l in enumerate(LANGS)}})

    def c(s):
        return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

    v = got['versions']
    out = ['/* Made by tools/countries/gen.py: not edited by hand. tzdata ' + tzdata_version()
           + ' (public domain); the names and their order from the Unicode CLDR ' + v.get('cldr', '?')
           + ' (Unicode License V3: src/LICENSE-Unicode-CLDR.txt) as ICU ' + v.get('icu', '?') + ' has them */',
           f'#define COUNTRIES_N {len(rows)}',
           f'#define COUNTRY_LANGS {len(LANGS)}',
           'static const char *const country_langs[COUNTRY_LANGS] = {' + ', '.join(c(l) for l in LANGS) + '};',
           'static const country_t country_tab[COUNTRIES_N] = {']
    for cc, region, tz, lat, lon, _ in rows:
        out.append(f'    {{"{cc}", {region}, {c(tz)}, {lat:.2f}f, {lon:.2f}f}},')
    out.append('};')
    out.append('/* The names, in the order of country_langs */')
    out.append('static const char *const country_names[COUNTRIES_N][COUNTRY_LANGS] = {')
    for r in rows:
        out.append('    {' + ', '.join(c(n) for n in r[5]) + '},')
    out.append('};')
    out.append('/* The countries in each language\'s alphabetical order, by their place in country_tab */')
    out.append('static const uint16_t country_order[COUNTRY_LANGS][COUNTRIES_N] = {')
    for l in LANGS:
        out.append('    {' + ', '.join(map(str, order[l])) + '},')
    out.append('};')
    OUT.write_text('\n'.join(out) + '\n', encoding='utf-8')
    print(f'{len(rows)} countries ({sum(r[1] == 2 for r in rows)} in Region 2, {sum(r[1] == 3 for r in rows)} in '
          f'Region 3, {sum(not r[2] for r in rows)} without a zone), {len(LANGS)} languages; {english} Chinese names '
          f'in English; letters written without accents or left out: {"".join(sorted(dropped)) or "none"}')


def tzdata_version():
    try:
        for line in (ZI / 'tzdata.zi').read_text().splitlines()[:3]:
            if line.startswith('# version '):
                return line.split()[-1]
    except OSError:
        pass
    return '?'


if __name__ == '__main__':
    main()
