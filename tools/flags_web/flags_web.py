#!/usr/bin/env python3
"""The flags of the apps' web pages (FT8, JS8): src/flags.h's list (the radio's own, by cty.dat's primary prefix),
drawn 40 x 30 from flagcdn.com (public domain, flags.h's own source) into one sprite, webserver's
static/img/flags.png, with static/img/flags.json: {"n": columns, "of": {primary prefix: cell}}. Run it when flags.h
changes (tools/apps_web checks they agree); it needs the internet and Pillow. From OlivLoze's fork.

    python3 tools/flags_web/flags_web.py
"""
import io, json, os, re, sys, urllib.request

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
W, H, COLS = 40, 30, 16
FLAGCDN = {"xi": "gb-nir"}           # flags.h's own code for Northern Ireland

src = open(os.path.join(ROOT, "src", "flags.h")).read()
iso = re.findall(r'"([a-z]{2})"', src[src.index("flag_iso"):src.index("flag_of")])
of = {p: int(i) for p, i in re.findall(r'\{"([^"]+)", (\d+)\}', src[src.index("flag_of"):])}

sheet = Image.new("RGB", (COLS * W, (len(iso) + COLS - 1) // COLS * H), (13, 15, 20))
for i, code in enumerate(iso):
    url = "https://flagcdn.com/w40/%s.png" % FLAGCDN.get(code, code)
    with urllib.request.urlopen(url, timeout=30) as r:
        im = Image.open(io.BytesIO(r.read())).convert("RGB")
    im = im.resize((W, H), Image.LANCZOS)        # flagcdn's w40 is 40 wide, its height the flag's own
    sheet.paste(im, (i % COLS * W, i // COLS * H))
    sys.stdout.write("."); sys.stdout.flush()

out = os.path.join(ROOT, "webserver", "src", "x6100_webserver", "static", "img")
sheet.quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(os.path.join(out, "flags.png"), optimize=True)
json.dump({"n": COLS, "of": of}, open(os.path.join(out, "flags.json"), "w"), separators=(",", ":"))
print("\n%d flags, %d prefixes" % (len(iso), len(of)))
