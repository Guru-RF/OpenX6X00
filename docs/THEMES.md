# Themes

Every colour of the GUI comes from a **role** (background, button, selected
row, header text, OK, TX...). A theme gives each role a colour, plus a few
shapes, the font and the waterfall palette.

Built in: **RF.Guru** (default: near-black surfaces, gold for what is on, electric blue as the second accent), **Classic** (black, grey keys, IC-7300 style)
**Outdoor** (light, for the sun), **Outdoor-Negative** (Outdoor with the
lightness inverted: dark, the hues kept), **1bit** (black on one white, like a
monochrome LCD) and, not offered in the menu, Legacy. More themes are JSON files
on the SD card in `/mnt/themes/`, one per file: `/mnt/themes/My theme.json`
shows up as *My theme* in Settings -> Display -> INTERFACE -> Theme. Choosing a theme
applies it at once, without restarting the radio app (an open app closes).

On base `1bit` every pixel on the screen is ink or paper: a colour a file gives a
role is drawn as the nearer of the two, a see-through box as a solid one (a faint
one as a 50 % stipple), pictures as dots. `ink` and `paper` set the two colours.

## File format

Every key is optional: what a file does not set comes from its `base`.

```json
{
    "base": "Classic",
    "colors": {
        "bg": "#000000",
        "btn": "#5e5e5e",
        "btn_grad": "#262626",
        "sel_bg": "#1f5fbf",
        "text_head": "#40c8ff"
    },
    "radius": 3,
    "radius_btn": 4,
    "radius_popup": 4,
    "border": 1,
    "btn_gradient": true,
    "box_gradient": false,
    "focus_solid": true,
    "font": "roboto",
    "waterfall": ["#000000", "#000060", "#0040e0", "#00c8ff", "#ffff00", "#ff6000", "#ffffff"],
    "waterfall_steps": 0
}
```

| Key | Meaning |
|---|---|
| `base` | `RF.Guru`, `Classic`, `Legacy`, `Outdoor`, `Outdoor-Negative` or `1bit` (default RF.Guru) |
| `colors` | role -> `#rrggbb`, roles below |
| `radius`, `radius_btn`, `radius_popup` | corners of the boxes (top bar, strip, windows), of the bottom buttons, of menus and panels in the apps |
| `border` | border width of boxes and buttons (3 or more: high contrast boxes) |
| `btn_gradient`, `box_gradient` | vertical gradient `btn` -> `btn_grad` on the buttons, `bar` -> `bar_grad` etc. on the boxes |
| `focus_solid` | the focused control in Settings is filled with `focus` (else half see-through) |
| `font` | `sony`, `roboto`, `dejavu`, `arial` or `pixel` (1bit's); without it the font chosen in Settings |
| `ink`, `paper` | on base `1bit` only: its two colours (`#000000` and `#efece2`), say a green LCD that still has only two |
| `waterfall` | 2 to 16 colours from weak to strong signal |
| `waterfall_steps` | 0 smooth, else that many flat bands |
| `mark` | encoder LED on the buttons: `[x, width, height]`, centred in height; without it the bar on the left edge |
| `pill_border` | border width of the info box items (VFO, mode, AGC, PRE, ATT, ATU), 0 = none |
| `mode_box` | the mode next to the frequency in a box (`mode_bg`, `mode_border`) instead of plain text |
| `btn_gap` | 0 to 4: more space around the bottom buttons, so rounded (pill) buttons show all their corners |
| `rule` | an `accent` line under the top bar and on top of the active button instead of its frame (Holo) |

The encoder LED shows which encoder a button's control is on (`btn_mark`,
`btn_mark_assigned`); the control the encoder adjusts now is `btn_mark_active`
(red in the built-in themes). Pressing a button or turning to another control
moves it.

## Images

A theme can also be a folder, `/mnt/themes/<name>/theme.json` with PNG files
next to it, and draw some surfaces with images instead of colours:

```json
"images": {
    "btn":        {"file": "btn.png",        "slice": [7, 5, 7, 4]},
    "btn_active": {"file": "btn_active.png", "slice": [7, 5, 7, 4]},
    "btn_off":    {"file": "btn_off.png",    "slice": [7, 5, 7, 4]},
    "btn_page":   {"file": "btn_page.png",   "slice": [7, 5, 7, 4]},
    "bar":        {"file": "lcd.png",        "slice": [14, 14, 14, 14]},
    "strip":      {"file": "lcd.png",        "slice": [14, 14, 14, 14]},
    "dialog":     {"file": "panel.png",      "slice": [6, 6, 6, 6]},
    "bg":         {"file": "background.png", "slice": [0, 0, 0, 0]}
}
```

| Image | Surface |
|---|---|
| `bg` | screen background |
| `btn`, `btn_active`, `btn_off`, `btn_page` | bottom buttons: normal, active, disabled, the page button (bottom left; without it, `btn`) |
| `bar` | the top bar boxes (info, S-meter, clock) |
| `strip` | the frequency strip |
| `dialog` | the app windows |
| `mark`, `mark_active` | the encoder LED and the LED of the control the encoder is on now (size from `mark`) |

An image can be small: it is brought to the size of the surface at start.
`slice` is `[left, top, right, bottom]` in pixels: corners and edges that wide
are drawn as they are, the middle is stretched (9-slice), so a 65 x 23 button
becomes a sharp 158 x 52 one. Transparent pixels show what is behind (rounded
corners). Text, lists and the waterfall keep the colours of the roles.

## Roles

The authoritative list, with the value of each built-in theme, is
`THEME_COLORS` in [src/theme.h](../src/theme.h).

- Screen and shared boxes: `bg`; `bar`, `bar_grad`, `bar_border` (top bar); `strip`, `strip_grad`, `strip_border`
  (frequency strip); `dialog`, `dialog_grad`, `dialog_border` (app windows)
- Bottom buttons: `btn`, `btn_grad`, `btn_border`, `btn_text`; `btn_active`, `btn_active_grad`, `btn_active_border`,
  `btn_active_text`; `btn_page`, `btn_page_text`; `btn_off_text`, `btn_off_border` (disabled); `btn_mark`,
  `btn_mark_assigned`, `btn_mark_active`
- Settings: `card`, `focus`, `focus_text`, `edited`, `control_border`, `toggle_on`, `toggle_focus`, `switch_off`, `lock`, `off`
- App content: `panel`, `panel_border`, `list`, `row`, `popup`, `popup_border`, `side`, `chip`, `sel_bg`, `sel_text`,
  `sel_border`, `focus_bg`, `near_bg`, `vfo`, `vfo_bg`, `veil`, `alert_box`, `track`, `frame`
- Text: `text`, `text_value`, `text_dim`, `text_soft`, `text_mute`, `text_faint`, `text_head`, `line`, `action`,
  `accent`, `accent_soft`, `title_text`
- State: `ok`, `ok_bg`, `ok_bar`, `warn`, `warn_bg`, `err`, `err_strong`, `err_bg`, `tx`, `tx_bg`, `info`, `digi`,
  `alert`, `alert_text`, `alert_bg`, `new`
- Main screen: `icon_off`, `mode_dig`, `mode_text`, `mode_bg`, `mode_border`, `pill_off_text`, `pill_border`,
  `meter_weak`, `meter_bar`, `meter_over`, `meter_low`, `meter_high`, `bar_high`, `bar_mid`, `alc`, `tx_box`, `vfo_line`, `spot`,
  `tune_good`, `bp_cw`, `bp_dig`, `bp_ssb`, `bp_bcn`, `bp_out`, `bp_text`, `finder_band`, `finder_cursor`
- Categories: `pill_hp`, `pill_cat`, `pill_cat_text`, `pill_kbd`, `pill_kbd_text` (Bluetooth devices), `age_mid`,
  `age_old` (cluster spots), `row_alert`, `row_alert_text`, `row_heard`, `row_tx`, `row_to_me`, `row_cq`,
  `row_group`, `row_sel_bar` (JS8), `ft8_cq`, `ft8_cq_worked`, `ft8_cq_same`, `ft8_to_me`, `ft8_tx` (FT8)

A file with a wrong colour or an unknown role is still loaded; the error is in
`/tmp/x6100_log.txt`. A file that is not valid JSON falls back to RF.Guru.
