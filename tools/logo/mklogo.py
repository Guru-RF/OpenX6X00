"""OpenX6X00 logo: an open ring crossed by a sine trace, and the wordmark.
Usage: python3 tools/logo/mklogo.py assets/fonts boot
  -> boot/logo.png (800x480 boot logo), boot/kernel_logo.png (the plain background), boot/mark.png and
     boot/mark_dark.png (the mark alone, 512x512). bootanim/bootanim.c has the timing for this layout."""
import math, sys, os
from PIL import Image, ImageDraw, ImageFont

fontdir, outdir = sys.argv[1], sys.argv[2]
BG      = (8, 9, 12)        # near-black
GOLD    = (229, 168, 35)
GOLD_LT = (242, 216, 134)
BLUE    = (0, 130, 252)
WHITE   = (241, 245, 249)
MUTED   = (148, 163, 184)
SS = 4   # supersampling

def mark(size, bg=None):
    """The mark alone: an open ring, gap at the top right, a sine trace through it."""
    s = size * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0) if bg is None else bg + (255,))
    d = ImageDraw.Draw(im)
    cx = cy = s / 2
    r = s * 0.36
    w = s * 0.075
    # the ring, open between 300 and 20 degrees (PIL: 0 = 3 o'clock, clockwise)
    d.arc([cx - r, cy - r, cx + r, cy + r], start=28, end=300, fill=GOLD, width=int(w))
    # round the two ends
    for a in (28, 300):
        x = cx + r * math.cos(math.radians(a)); y = cy + r * math.sin(math.radians(a))
        d.ellipse([x - w/2, y - w/2, x + w/2, y + w/2], fill=GOLD)
    # the sine trace: enters through the ring on the left, one period across, leaves through the
    # gap rising to the top right. Stamped as circles: a polyline shows its joints at this width.
    x0, x1 = cx - r * 1.08, cx + r * 1.36
    amp = r * 0.40
    tw = w * 0.74
    n = 900
    pts = []
    for i in range(n + 1):
        t = i / n
        x = x0 + (x1 - x0) * t
        y = cy - amp * math.sin(t * 2 * math.pi * 1.25 - math.pi * 0.55) * (0.45 + 0.55 * t)
        pts.append((x, y))
    for x, y in pts:
        d.ellipse([x - tw/2, y - tw/2, x + tw/2, y + tw/2], fill=BLUE)
    x, y = pts[-1]
    rr = w * 0.62
    d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=BLUE)
    return im.resize((size, size), Image.LANCZOS)

def text_width(font, s):
    b = font.getbbox(s); return b[2] - b[0], b[3] - b[1], b

def boot(W=800, H=480):
    im = Image.new("RGB", (W, H), BG)
    m = mark(190)
    im.paste(m, (W // 2 - m.width // 2, 48), m)
    d = ImageDraw.Draw(im)
    bold = ImageFont.truetype(os.path.join(fontdir, "Inter-Bold.ttf"), 78)
    med  = ImageFont.truetype(os.path.join(fontdir, "Inter-Medium.ttf"), 24)
    # the wordmark: "Open" white, "X6X00" gold
    a, b = "Open", "X6X00"
    wa, ha, ba = text_width(bold, a); wb, hb, bb = text_width(bold, b)
    gap = 6
    x = W // 2 - (wa + gap + wb) // 2
    y = 258
    d.text((x - ba[0], y), a, font=bold, fill=WHITE)
    d.text((x + wa + gap - bb[0], y), b, font=bold, fill=GOLD)
    sub = "OPEN FIRMWARE FOR THE XIEGU X6100"
    ws, hs, bs = text_width(med, sub)
    # letter-spaced subtitle
    sp = 5
    total = sum(med.getlength(c) for c in sub) + sp * (len(sub) - 1)
    xx = W / 2 - total / 2
    for c in sub:
        d.text((xx, 372), c, font=med, fill=MUTED)
        xx += med.getlength(c) + sp
    # a thin gold rule under the wordmark
    d.rectangle([W // 2 - 60, 352, W // 2 + 60, 354], fill=GOLD)
    return im

os.makedirs(outdir, exist_ok=True)
boot().save(os.path.join(outdir, "logo.png"))
Image.new("RGB", (800, 480), BG).save(os.path.join(outdir, "kernel_logo.png"))
mark(512).save(os.path.join(outdir, "mark.png"))
mark(512, bg=BG).save(os.path.join(outdir, "mark_dark.png"))
print("ok")
