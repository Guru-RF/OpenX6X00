# RS theme for the X6100 GUI, cut from an RS-BA1 screenshot (rsba1.png, 863x673).
# Personal use only: not part of the firmware repo. Output: RS/ (theme.json + PNGs),
# copy it to the SD card as /mnt/themes/RS/.
#
# Each image is a small piece of the screenshot; the firmware stretches its
# middle to the size it needs and keeps the "slice" insets as drawn.
import json, os
from PIL import Image, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = Image.open(os.path.join(HERE, 'rsba1.png')).convert('RGBA')
OUT = os.path.join(HERE, 'RS')
os.makedirs(OUT, exist_ok=True)


def crop(x0, y0, x1, y1):
    """Inclusive box"""
    return SRC.crop((x0, y0, x1 + 1, y1 + 1))


def clean_button(im, cap, col):
    """Text out: every column between the caps becomes a copy of column col
    (a plain one, left of the label); the caps keep the rounded ends."""
    w, h = im.size
    out = im.copy()
    column = im.crop((col, 0, col + 1, h))
    for x in range(cap, w - cap):
        out.paste(column, (x, 0))
    return out


def transparent_outside(im, bg):
    """The screenshot's background around the rounded corners becomes transparent"""
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if abs(r - bg[0]) < 6 and abs(g - bg[1]) < 6 and abs(b - bg[2]) < 6 and (x < 4 or x >= w - 4 or y < 3 or y >= h - 3):
                px[x, y] = (r, g, b, 0)
    return im


images = {}


def save(name, im, slice_):
    im.save(os.path.join(OUT, name + '.png'))
    images[name] = {'file': name + '.png', 'slice': slice_}


# Buttons: the CW (normal) and SSB (selected) mode buttons, 65 x 23, one dark pixel
# around them is part of the screenshot's black panel: keep it as the edge
btn = clean_button(crop(189, 424, 253, 446), 7, 6)     # plain column: x 195
save('btn', btn, [7, 5, 7, 4])
save('btn_active', clean_button(crop(111, 424, 175, 446), 7, 6), [7, 5, 7, 4])   # x 117
save('btn_off', ImageEnhance.Brightness(btn).enhance(0.55), [7, 5, 7, 4])
# Page button: TUNE, on the grey background of the left column
save('btn_page', transparent_outside(clean_button(crop(14, 176, 89, 198), 7, 8), (0x22, 0x22, 0x22)), [7, 5, 7, 4])   # x 22

# LCD: the display frame (black rim, grey line, black inside), contents painted black
lcd = crop(101, 94, 652, 455)
w, h = lcd.size
lcd.paste((0, 0, 0, 255), (4, 4, w - 4, h - 4))
lcd = lcd.resize((w, h))  # keep
corner = 14
small = Image.new('RGBA', (2 * corner + 4, 2 * corner + 4))
for (sx, sy), (dx, dy) in {(0, 0): (0, 0), (w - corner - 2, 0): (corner + 2, 0),
                           (0, h - corner - 2): (0, corner + 2), (w - corner - 2, h - corner - 2): (corner + 2, corner + 2)}.items():
    small.paste(lcd.crop((sx, sy, sx + corner + 2, sy + corner + 2)), (dx, dy))
save('bar', small, [corner, corner, corner, corner])
save('strip', small, [corner, corner, corner, corner])

# Encoder LED: the TUNER button's LED (on), with its glow
led = crop(17, 149, 24, 162)
save('mark', led, [0, 0, 0, 0])
# The control the encoder is on now: the same LED in red (cyan (r, g, b) -> (b, r, r); greys stay grey)
r, g, b, a = led.split()
save('mark_active', Image.merge('RGBA', (b, r, r, a)), [0, 0, 0, 0])

# App windows: the MEMORY panel's body (below its title band)
panel = crop(267, 484, 459, 665)
pw, ph = panel.size
body = panel.copy()
body.paste((0x22, 0x22, 0x22, 255), (4, 4, pw - 4, ph - 4))   # the panel's own grey, no buttons
save('dialog', body, [6, 6, 6, 6])

theme = {
    'base': 'Classic',
    'font': 'arial',
    'radius': 6, 'radius_btn': 5, 'radius_popup': 6, 'border': 1,
    'btn_gradient': True, 'box_gradient': False, 'focus_solid': True,
    # LED on the buttons as on RS-BA1 (3 px in, 8 x 14 on a 23 px button), scaled to the 52 px ones
    'mark': [7, 16, 28],
    'pill_border': 1,       # indicators framed, as P.AMP / ATT / NOTCH
    'mode_box': True,       # the mode in a blue box, as LSB
    'waterfall': ['#000000', '#000050', '#0040c0', '#0ac7ff', '#ffff40', '#ff6000', '#ffffff'],
    'colors': {
        'bg': '#232323',
        'bar': '#000000', 'bar_grad': '#000000', 'bar_border': '#585858',
        'strip': '#000000', 'strip_grad': '#000000', 'strip_border': '#585858',
        'dialog': '#232323', 'dialog_grad': '#232323', 'dialog_border': '#4a4a4a',
        'btn': '#4d4d4d', 'btn_grad': '#020202', 'btn_border': '#6e6e6e', 'btn_text': '#ffffff',
        'btn_active': '#44a4bd', 'btn_active_grad': '#0a6ad8', 'btn_active_border': '#7fd0ff', 'btn_active_text': '#ffffff',
        'btn_off_text': '#6e6e6e', 'btn_page_text': '#e8e8e8',
        'btn_mark': '#0ac7ff', 'btn_mark_assigned': '#0ac7ff',
        'panel': '#000000', 'panel_border': '#585858', 'list': '#0a0a0a', 'row': '#1a1a1a',
        'popup': '#1a1a1a', 'popup_border': '#6e6e6e', 'side': '#141414', 'chip': '#2a2a2a', 'card': '#4a4a4a',
        'sel_bg': '#0583e7', 'sel_text': '#ffffff', 'sel_border': '#7fd0ff', 'focus': '#0583e7', 'focus_text': '#ffffff',
        'focus_bg': '#0583e7', 'edited': '#0ac7ff', 'near_bg': '#193052',
        'vfo': '#0ac7ff', 'vfo_bg': '#193052', 'vfo_line': '#0ac7ff',
        'text': '#ffffff', 'text_value': '#eaffff', 'text_dim': '#8f8f8f', 'text_soft': '#b4b4b4', 'text_mute': '#6e6e6e',
        'text_faint': '#585858', 'text_head': '#ffffff', 'line': '#b4b4b4', 'action': '#0ac7ff', 'accent': '#0ac7ff',
        'accent_soft': '#7fd0ff', 'info': '#0ac7ff', 'track': '#2a2a2a', 'toggle_on': '#0583e7', 'toggle_focus': '#7fd0ff',
        'tx': '#ff3030', 'tx_bg': '#c00000', 'tx_box': '#3a0000', 'ok': '#40e040', 'ok_bar': '#30c040', 'err': '#ff4040',
        'meter_low': '#196fd2', 'meter_high': '#196fd2', 'meter_bar': '#196fd2', 'meter_over': '#ff4040', 'alc': '#0ac7ff',
        'pill_off_text': '#808080', 'pill_border': '#585858', 'mode_dig': '#ffffff',
        'mode_text': '#ffffff', 'mode_bg': '#20529b', 'mode_border': '#afd1ff',
    },
    'images': images,
}
with open(os.path.join(OUT, 'theme.json'), 'w') as f:
    json.dump(theme, f, indent=2)
print('RS written:', sorted(os.listdir(OUT)))
