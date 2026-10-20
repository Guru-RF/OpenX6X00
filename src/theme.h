/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Themes: every colour of the GUI is a role, a theme gives each role its
 *  value plus the shapes (corners, borders, gradients), the font and the
 *  waterfall palette. Four themes are built in; more are JSON files in
 *  /mnt/themes/ (docs/THEMES.md), chosen in Settings and applied live.
 *
 *  Code never writes a colour: TC(role) for an lv_color_t, theme_hex(TC_role)
 *  for 0xRRGGBB (e.g. "#%06x text#" recolor strings).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* role, RF.Guru, Legacy, Outdoor, Classic (IC-7300 style) */
#define THEME_COLORS(X) \
    /* screen and the shared boxes (styles.c) */ \
    X(bg,                0x08090c, 0x0040a0, 0xffffff, 0x000000) /* screen background */ \
    X(bar,               0x13161d, 0x2a5aa0, 0xf2f2f2, 0x0e0e0e) /* top bar boxes: info, S-meter, clock (RF.Guru: the surface of the strip and the apps) */ \
    X(bar_grad,          0x0c0e13, 0x103060, 0xf2f2f2, 0x0e0e0e) \
    X(bar_border,        0x262b36, 0x8aa8d8, 0x000000, 0x4c4c4c) \
    X(strip,             0x13161d, 0x1a4078, 0xffffff, 0x000000) /* frequency strip */ \
    X(strip_grad,        0x0c0e13, 0x0c2448, 0xffffff, 0x000000) \
    X(strip_border,      0x262b36, 0x8aa8d8, 0x000000, 0x4c4c4c) \
    X(dialog,            0x13161d, 0x2a5aa0, 0xffffff, 0x101010) /* app windows */ \
    X(dialog_grad,       0x0c0e13, 0x103060, 0xffffff, 0x000000) \
    X(dialog_border,     0x262b36, 0x8aa8d8, 0x000000, 0x5a5a5a) \
    X(btn,               0x1b1f29, 0x0b1622, 0xe6e6e6, 0x5e5e5e) /* bottom buttons */ \
    X(btn_grad,          0x13161d, 0x0b1622, 0xe6e6e6, 0x262626) \
    X(btn_border,        0x262b36, 0x99aaaa, 0x000000, 0x8a8a8a) \
    X(btn_text,          0xf1f5f9, 0xffffff, 0x000000, 0xffffff) \
    X(btn_active,        0x2a2410, 0x2a220c, 0xffd400, 0x2f72d6) \
    X(btn_active_grad,   0x1e1a0c, 0x2a220c, 0xffd400, 0x123e86) \
    X(btn_active_border, 0xe5a823, 0xffd24a, 0x000000, 0x9fd2ff) \
    X(btn_active_text,   0xf2d886, 0xffffff, 0x000000, 0xffffff) \
    X(btn_page,          0x13161d, 0x16232f, 0xc8c8c8, 0x2a2a2a) /* the page button, bottom left */ \
    X(btn_page_text,     0xcbd5e1, 0xccddee, 0x000000, 0xb4b4b4) \
    X(btn_off_text,     0x64748b, 0x55606c, 0x777777, 0x6a6a6a) /* disabled */ \
    X(btn_off_border,    0x1b1f29, 0x2a323c, 0x777777, 0x3a3a3a) \
    X(btn_mark,          0x4f8bf5, 0x66ff66, 0x008a00, 0xffa000) /* encoder mark on a button */ \
    X(btn_mark_assigned, 0x0082fc, 0x80ff80, 0x50b050, 0xffc060) \
    X(btn_mark_active,   0xf87171, 0xff4040, 0xff0000, 0xff3030) /* the control the encoder is on now */ \
    X(card,              0x262b36, 0x506070, 0x000000, 0x4c4c4c) /* Settings cards: border */ \
    X(focus,             0xe5a823, 0xffffff, 0x0040d0, 0x2f72d6) /* focused control in Settings */ \
    X(focus_text,        0x0c0e13, 0x000000, 0xffffff, 0xffffff) \
    X(edited,            0x0082fc, 0xffffff, 0xff7000, 0xffa000) /* control being edited */ \
    \
    /* app content */ \
    X(panel,             0x08090c, 0x000000, 0xffffff, 0x000000) /* content panels, lists, tables */ \
    X(panel_border,      0x262b36, 0x445566, 0x000000, 0x4c4c4c) \
    X(list,              0x0c0e13, 0x18222c, 0xffffff, 0x0a0a0a) /* JS8 lists (slate: the family of dialog and side) */ \
    X(row,               0x1b1f29, 0x2a3846, 0xe4e4e4, 0x1c1c1c) /* alternate / header rows */ \
    X(popup,             0x13161d, 0x0b1622, 0xffffff, 0x141414) /* menus and popups */ \
    X(popup_border,      0x262b36, 0x99aaaa, 0x000000, 0x8a8a8a) \
    X(side,              0x0c0e13, 0x121a22, 0xf2f2f2, 0x0e0e0e) /* side boxes in apps */ \
    X(chip,              0x1b1f29, 0x1f2a35, 0xe4e4e4, 0x262626) /* state labels, small boxes */ \
    X(sel_bg,            0x3a2e0c, 0x5a4818, 0xffd400, 0x1f5fbf) /* selected row */ \
    X(sel_text,          0xf2d886, 0xffd24a, 0x000000, 0xffffff) \
    X(sel_border,        0xe5a823, 0xffffff, 0x000000, 0x9fd2ff) \
    X(focus_bg,          0x0a1a33, 0x1830a0, 0xffd400, 0x1f5fbf) /* JS8 focused item */ \
    X(near_bg,           0x2a2410, 0x3a2f12, 0xfff1a8, 0x10283c) /* cluster: spot near the VFO */ \
    X(vfo,               0x4f8bf5, 0x7cc4ff, 0x0040ff, 0x40c8ff) /* VFO marker, SSB */ \
    X(vfo_bg,            0x0a1a33, 0x0a1f3a, 0xdde6ff, 0x00203c) \
    \
    /* text */ \
    X(text,              0xf1f5f9, 0xffffff, 0x000000, 0xffffff) \
    X(text_value,        0xf1f5f9, 0xd0e0f0, 0x000000, 0xe6e6e6) /* values, frequencies */ \
    X(text_dim,          0x94a3b8, 0x90a0b0, 0x3a3a3a, 0xa0a0a0) /* hints, notes, secondary */ \
    X(text_soft,         0xcbd5e1, 0xbbbbbb, 0x4a4a4a, 0xb4b4b4) /* inactive knob / lock */ \
    X(text_mute,         0x64748b, 0x808080, 0x5a5a5a, 0x787878) /* greyed items */ \
    X(text_faint,        0x475569, 0x607080, 0x6a6a6a, 0x5a5a5a) /* queued, idle */ \
    X(text_head,         0xe5a823, 0x90f0d0, 0x0030a0, 0x40c8ff) /* app headers and titles */ \
    X(line,              0x94a3b8, 0xaaaaaa, 0x303030, 0xa0a0a0) /* scales, axis lines */ \
    X(action,            0xecc34a, 0xffc040, 0xb04a00, 0xffa000) /* back / close / cancel items */ \
    \
    /* state */ \
    X(ok,                0x34d399, 0x88ff88, 0x007a00, 0x40e040) \
    X(ok_bg,             0x0f2e24, 0x1f4a24, 0xd6f5d6, 0x0c3010) \
    X(ok_bar,            0x34d399, 0x40c060, 0x00a000, 0x30c040) /* progress bars */ \
    X(warn,              0xfbbf24, 0xffb040, 0x9a5a00, 0xffa000) /* also CW */ \
    X(warn_bg,           0x3a2a08, 0x4a3a10, 0xfff0c0, 0x3a2800) \
    X(err,               0xf87171, 0xff6060, 0xd00000, 0xff4040) \
    X(err_strong,        0xef4444, 0xff0000, 0xff0000, 0xff2020) \
    X(err_bg,            0x3a1414, 0x4a1515, 0xffd6d6, 0x400808) \
    X(tx,                0xf87171, 0xff4040, 0xd00000, 0xff3030) /* TX bars and text */ \
    X(tx_bg,             0xb91c1c, 0xb01e1e, 0xff0000, 0xd01010) /* ON AIR */ \
    X(info,              0x4f8bf5, 0x55ccff, 0x0058c0, 0x40c8ff) \
    X(digi,              0xc084fc, 0xc890ff, 0x7000c0, 0xc080ff) /* digital modes */ \
    X(alert,             0xf472b6, 0xff4fc0, 0xc00088, 0xff40c0) /* cluster alerts */ \
    X(alert_text,        0xf9a8d4, 0xff9be0, 0x9a006e, 0xff90e0) \
    X(alert_bg,          0x5a1a3c, 0x7a1f5c, 0xffd0f0, 0x5a1040) \
    X(new,               0x34d399, 0x60ff60, 0x007700, 0x40e040) /* fresh spots */ \
    \
    /* accents and small surfaces */ \
    X(accent,            0xe5a823, 0xffd24a, 0x886600, 0xffa000) /* highlighted text: app name, next char, icons */ \
    X(accent_soft,       0xf2d886, 0xffe080, 0x5f4800, 0xffc060) /* unread messages */ \
    X(veil,              0x13161d, 0x1c2a33, 0xccdae3, 0x101010) /* behind a popup */ \
    X(alert_box,         0x2a1020, 0x2a0f22, 0xf0d5e8, 0x2a0818) /* cluster alert popup */ \
    X(track,             0x262b36, 0x304050, 0xafbfcf, 0x303030) /* progress bar track */ \
    X(frame,             0x0c0e13, 0x182028, 0xd7dfe7, 0x0a0a0a) /* WeFax / channels frame */ \
    X(off,               0x475569, 0x555555, 0x8a8a8a, 0x4a4a4a) /* unlit bars, spectrum peak */ \
    X(switch_off,        0xcbd5e1, 0xe0e0e0, 0x9a9a9a, 0xe0e0e0) /* Settings switch, off */ \
    X(icon_off,          0x1b1f29, 0x151515, 0x8c8c8c, 0x1a1a1a) /* top bar icons that are off */ \
    X(lock,              0xe5a823, 0xe0a030, 0xaf7103, 0xffa000) /* secured WiFi padlock */ \
    X(control_border,    0x64748b, 0x8a8a8a, 0x585858, 0x8a8a8a) /* Settings sliders, toggles */ \
    X(toggle_on,         0x0082fc, 0x2196f3, 0x0062b0, 0x2f72d6) /* toggle on, level bars */ \
    X(toggle_focus,      0x4f8bf5, 0x7cc4ff, 0x003662, 0x9fd2ff) \
    \
    /* main screen */ \
    X(vfo_line,          0xff2020, 0xff2020, 0xe00000, 0xff2020) /* VFO line and filter band on spectrum / waterfall */ \
    X(spot,              0x4f8bf5, 0x40e8ff, 0x000000, 0x40e8ff) /* cluster spots on the waterfall */ \
    X(mode_dig,          0xc084fc, 0xff8080, 0x5f0000, 0xff8080) /* digital mode name in the info box */ \
    X(mode_text,         0xe5a823, 0xff6060, 0x770000, 0xff4040) /* mode next to the frequency */ \
    X(mode_bg,           0x2a2410, 0x5a4818, 0xe7d5a5, 0x1f5fbf) /* its box (theme_shape.mode_box) */ \
    X(mode_border,       0xe5a823, 0xffffff, 0x000000, 0x9fd2ff) \
    X(pill_off_text,     0xcbd5e1, 0xffffff, 0x000000, 0xffffff) /* info box: PRE, ATT, ATU when off */ \
    X(pill_border,       0x3a4150, 0x777777, 0x888888, 0x5a5a5a) /* info box items (theme_shape.pill_border) */ \
    X(meter_weak,        0x94a3b8, 0xaaaaaa, 0xc8c8c8, 0xa0a0a0) /* S-meter first bar of meter_gradient */ \
    X(meter_bar,         0xcbd5e1, 0xaaaaaa, 0x404040, 0xa0a0a0) /* S-meter bars up to S9 */ \
    X(meter_over,        0xf1f5f9, 0xffffff, 0x000000, 0xff4040) /* S-meter +20 +40 labels */ \
    X(meter_low,         0x34d399, 0x20c040, 0x3fdf5f, 0x40e040) /* S-meter above S9: from this... */ \
    X(meter_high,        0xf87171, 0xe02020, 0x000000, 0xff3030) /* ...to this; last bar of meter_gradient */ \
    X(bar_high,          0xf87171, 0xcc3333, 0xcc3333, 0xe03030) /* SWR high, mic clipping */ \
    X(bar_mid,           0xfbbf24, 0xcccc33, 0xcccc33, 0xe0e030) /* SWR raised */ \
    X(alc,               0x4f8bf5, 0x66aabb, 0x296c7c, 0x40c8ff) /* ALC bar */ \
    X(tx_box,            0x5a1414, 0x7a1818, 0xe78585, 0x5a0000) /* TX panel */ \
    X(tune_good,         0xecc34a, 0xffff30, 0x9b9b00, 0xffff30) /* CW tune: on the signal */ \
    X(bp_cw,             0x4f8bf5, 0x4aa3ff, 0x004388, 0x4aa3ff) /* band plan segments */ \
    X(bp_dig,            0xe5a823, 0xffd24a, 0x886600, 0xffa000) \
    X(bp_ssb,            0x34d399, 0x60d060, 0x178417, 0x60d060) \
    X(bp_bcn,            0xf87171, 0xff6060, 0x770000, 0xff6060) \
    X(bp_text,           0x08090c, 0x000000, 0xffffff, 0x000000) \
    X(finder_band,       0xf87171, 0xff5050, 0x830000, 0xff5050) /* app waterfalls: passband */ \
    X(finder_cursor,     0x34d399, 0x50ff50, 0x008300, 0x50ff50) \
    \
    /* categories */ \
    X(pill_hp,           0x3a2e0c, 0x4b3c14, 0xebdcb4, 0x3a2800) /* Bluetooth: headphones (text: accent) */ \
    X(pill_cat,          0x2a2040, 0x3a2f5a, 0xb0a5d0, 0x2a2040) \
    X(pill_cat_text,     0xc4b5fd, 0xccbbff, 0x0d0033, 0xccbbff) \
    X(pill_kbd,          0x0a1a33, 0x14404b, 0xb4e0eb, 0x103040) \
    X(pill_kbd_text,     0x4f8bf5, 0x88e0ff, 0x004259, 0x88e0ff) \
    X(age_mid,           0xecc34a, 0xe8e070, 0x797104, 0xe0e060) /* cluster spot age < 10 min */ \
    X(age_old,           0xb45309, 0xc08a4a, 0x956022, 0xb08040) /* < 30 min */ \
    X(row_alert,         0x5b21b6, 0x6a2ca0, 0x9d5fd3, 0x5a2090) /* JS8 rows */ \
    X(row_alert_text,    0xddd6fe, 0xd0a0ff, 0x240047, 0xd0a0ff) \
    X(row_heard,         0x3a2e0c, 0x5a4400, 0xffe9a5, 0x4a3800) \
    X(row_tx,            0x0a1a33, 0x1830a0, 0x5f77e7, 0x1830a0) \
    X(row_to_me,         0x991b1b, 0xb00000, 0xff4f4f, 0xa00000) \
    X(row_cq,            0x14532d, 0x006000, 0x9fff9f, 0x006000) \
    X(row_group,         0x1e3a5f, 0x102a60, 0x9fb9ef, 0x102a60) \
    X(row_sel_bar,       0x34d399, 0x40ff40, 0x008f00, 0x40ff40) \
    X(title_text,        0x0a1a33, 0x1030a0, 0x0030a0, 0x1030a0) /* on a light title bar */ \
    X(ft8_cq,            0x34d399, 0x00dd00, 0x22ff22, 0x00b000) /* FT8 rows: CQ not worked */ \
    X(ft8_cq_worked,     0x1f4a2e, 0x2e5a00, 0xd3ffa5, 0x2e5a00) \
    X(ft8_cq_same,       0x163a24, 0x224400, 0xddffbb, 0x224400) /* worked in this mode */ \
    X(ft8_to_me,         0xef4444, 0xff0000, 0xff0000, 0xd01010) \
    X(ft8_tx,            0x0082fc, 0x0000ff, 0x0000ff, 0x1f5fbf)

typedef enum {
#define THEME_ENUM(name, a, b, c, d) TC_##name,
    THEME_COLORS(THEME_ENUM)
#undef THEME_ENUM
    TC_COUNT
} theme_color_t;

typedef struct {
    char    name[32];
    int16_t radius;         /* top bar, strip, dialogs */
    int16_t radius_btn;     /* bottom buttons */
    int16_t radius_popup;   /* menus, popups, panels in apps */
    int16_t border;         /* border width of the boxes and buttons */
    bool    btn_gradient;   /* btn -> btn_grad (else flat btn) */
    bool    box_gradient;   /* bar/strip/dialog -> *_grad */
    bool    focus_solid;    /* focused control: solid focus colour (else half see-through) */
    bool    meter_gradient; /* S-meter: one even gradient, bar by bar, meter_weak to meter_high */
    bool    mono_cluster;   /* cluster column all in the text colour (Outdoor: colours fade in the sun) */
    uint8_t images;         /* message/panel backgrounds: 0 drawn, 1 dark images, 2 legacy images */
    int8_t  font;           /* FONT_* (fonts/font_select.h), -1 = the Settings choice */
    int16_t mark[3];        /* encoder LED on the buttons: x, width, height (centred); 0 = the bar on the left edge */
    int16_t pill_border;    /* border of the info box items (VFO, mode, AGC, PRE, ATT, ATU), 0 = none */
    bool    mode_box;       /* the mode next to the frequency in a box (sel_bg, sel_border), else plain text */
    int16_t btn_gap;        /* more space around the bottom buttons, 0..4 px (pill shapes show their corners) */
    bool    rule;           /* accent line under the top bar and over the active button (Holo) */
    bool    vision;         /* VISION: big type for weak eyes, a low top bar, a tall frequency strip, the S-meter on a row of its own */
#define VISION_FONT_STEPS 3 /* VISION: every font this many sizes up, apps and Settings too (fonts_scale) */
    bool    deck;           /* Black Metal, Total Black: two rows of state on top, S/SWR/ALC scales beside a big
                             * frequency, the frequency edges in the spectrum, one-line flat buttons */
    uint32_t waterfall[16];     /* palette stops from weak to strong signal */
    uint8_t  waterfall_n;       /* 0 = a built-in table (waterfall_table) */
    uint8_t  waterfall_table;   /* 0 gauss, 1 legacy */
    uint8_t  waterfall_steps;   /* 0 smooth, else that many flat bands */
} theme_shape_t;

extern theme_shape_t theme_shape;

uint32_t theme_hex(theme_color_t c);
#define TC(role) lv_color_hex(theme_hex(TC_##role))

/* Loads the theme by name: a built-in one or /mnt/themes/<name>.json.
 * Unknown names fall back to RF.Guru. Then styles_init() (styles_load() does both). */
void theme_init(const char *name);

/* Names for the Settings list: the built-in themes, then the files */
int theme_list(char names[][32], int max);

const uint32_t *theme_waterfall(void);

/* Filter passband colour preset (Settings > Display): 0xRRGGBB, -1 for 0 (the theme's) */
int32_t theme_filter_color(int32_t preset);

/* 256 waterfall colours from n stops (0xRRGGBB, weak to strong), in `steps` bands (0: smooth) */
void theme_ramp_palette(const uint32_t *stops, int n, int steps, uint32_t *out);

/* Surfaces a theme may draw with an image instead of its colours */
typedef enum {
    TI_bg,          /* screen background */
    TI_btn,         /* bottom buttons: normal, active, disabled, the page button */
    TI_btn_active,
    TI_btn_off,
    TI_btn_page,
    TI_bar,         /* top bar boxes: info, S-meter, clock */
    TI_strip,       /* frequency strip */
    TI_dialog,      /* app windows */
    TI_mark,        /* encoder LED on the buttons (theme_shape.mark) */
    TI_mark_active, /* the LED of the control the encoder is on now */
    TI_COUNT
} theme_image_t;

/* The theme's PNG for that surface brought to w x h: corners and edges as
 * drawn (the "slice" insets), the middle stretched. Kept for later calls of
 * the same size. NULL when the theme has no image there. */
const lv_img_dsc_t *theme_image(theme_image_t img, lv_coord_t w, lv_coord_t h);

/* A PNG file as it is (icons), loaded once and kept; NULL when it can't be read */
const lv_img_dsc_t *theme_png(const char *path);

/* An icon of the icon menus (/usr/share/x6100/icons/<name>_<size>.png, white on nothing) as an
 * image on parent in the given colour; NULL when there is no such file */
lv_obj_t *theme_icon_img(lv_obj_t *parent, const char *name, int size, lv_color_t color);

#ifdef __cplusplus
}
#endif
