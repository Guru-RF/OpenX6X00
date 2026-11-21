/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - sending in the RTTY app (#312)
 *
 *  RTTY has no window of its own: it decodes on the main screen, into the panel over the waterfall, and its buttons
 *  are pages of the button bar. The first (button_bar.cpp) has the decoder's Rate, Shift, Center and Reverse; these
 *  are the others:
 *
 *    2 Send... (a line typed, Enter sends it), Stop TX, DX call, RST
 *    3 and 4 the six macros (press sends, hold edits), Stop TX next to them
 *    5 Power, Ext tune, Clear, Preset (Ham: 45.45 baud, 170 Hz, Reverse off; Weather: the defaults, 50 baud, 450 Hz,
 *      Reverse on, for the weather stations)
 *
 *  With a USB or Bluetooth keyboard a letter or digit typed on the main screen opens the line with it in, as CW's
 *  line does in CW (cw_field.c), and CapsLock+1 to 6 send the macros. The macros' tokens are the PSK and Olivia
 *  ones: <MYCALL> the station's callsign (Settings), <CALL> the DX call (typed in, or taken from "DE <call>" in what
 *  RTTY decodes), <RST> and <RSTR> the reports.
 *
 *  What is sent goes through rtty.c and tx_player, as the other digital apps send; the panel shows it in the TX
 *  colour as it goes out.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "rtty_send.h"

#include "digi_qso.h"           /* the DX call and the reports the macros fill in */
#include "i18n.h"
#include "key_input.h"          /* keyboard_ready */
#include "msg.h"
#include "panel.h"
#include "psk_macro.h"          /* psk_macro_expand: the keyboard modes' tokens */
#include "rtty.h"
#include "rtty_macro.h"
#include "rtty_tx.h"            /* the presets */
#include "screen_main.h"        /* main_screen_keys_enable */
#include "textarea_window.h"
#include "theme.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LINE_LEN    (RTTY_MACRO_TEXT_LEN - 1)  /* a line typed, as long as a macro */

static bool        input;       /* a text window of ours is open, or the macro editor between its two */
static int         edit_idx = -1;
static char        edit_name[RTTY_MACRO_NAME_LEN];
static lv_timer_t *editor_timer;
static char        macro_labels[RTTY_MACRO_MAX][RTTY_MACRO_NAME_LEN + 2];

static void send_press(button_data_t *b);
static void stop_press(button_data_t *b);
static const char *dx_label(void);
static void dx_press(button_data_t *b);
static void dx_hold(button_data_t *b);
static const char *rst_label(void);
static void rst_press(button_data_t *b);
static void rst_hold(button_data_t *b);
static void clear_press(button_data_t *b);
static const char *preset_label(void);
static void preset_press(button_data_t *b);
static void macro_press(int idx);
static void macro_hold(int idx);

/* ---- the pages ---- */

static buttons_page_t page_3, page_4;

static button_data_t btn_p2 = {.type = BTN_TEXT, .label = "(RTTY 2:5)", .press = button_next_page_cb,
                               .hold = button_prev_page_cb, .next = &page_3, .prev = &buttons_page_rtty};
static button_data_t btn_p3 = {.type = BTN_TEXT, .label = "(RTTY 3:5)", .press = button_next_page_cb,
                               .hold = button_prev_page_cb, .next = &page_4, .prev = &rtty_page_send};
static button_data_t btn_p4 = {.type = BTN_TEXT, .label = "(RTTY 4:5)", .press = button_next_page_cb,
                               .hold = button_prev_page_cb, .next = &rtty_page_power, .prev = &page_3};
static button_data_t btn_p5 = {.type = BTN_TEXT, .label = "(RTTY 5:5)", .press = button_next_page_cb,
                               .hold = button_prev_page_cb, .next = &buttons_page_rtty, .prev = &page_4};

static button_data_t btn_send  = {.type = BTN_TEXT, .label = "Send...", .press = send_press};
static button_data_t btn_stop  = {.type = BTN_TEXT, .label = "Stop\nTX", .press = stop_press};
static button_data_t btn_dx    = {.type = BTN_TEXT_FN, .label_fn = dx_label, .press = dx_press, .hold = dx_hold};
static button_data_t btn_rst   = {.type = BTN_TEXT_FN, .label_fn = rst_label, .press = rst_press, .hold = rst_hold};
static button_data_t btn_clear = {.type = BTN_TEXT, .label = "Clear", .press = clear_press};
/* On page 5, the one with a free place: page 1 (the decoder's) has none, and held F1 there goes to page 5 */
static button_data_t btn_preset = {.type = BTN_TEXT_FN, .label_fn = preset_label, .press = preset_press};

/* button_data_t has no user pointer: a press and a hold for each */
#define MACRO_BTN(n)                                                                                          \
    static void macro##n##_press(button_data_t *b) { (void)b; macro_press(n); }                              \
    static void macro##n##_hold(button_data_t *b) { (void)b; macro_hold(n); }                                \
    static button_data_t btn_macro##n = {.type = BTN_TEXT, .label = macro_labels[n], .press = macro##n##_press, \
                                         .hold = macro##n##_hold};

MACRO_BTN(0)
MACRO_BTN(1)
MACRO_BTN(2)
MACRO_BTN(3)
MACRO_BTN(4)
MACRO_BTN(5)

#undef MACRO_BTN

static button_data_t *const macro_btns[RTTY_MACRO_MAX] = {&btn_macro0, &btn_macro1, &btn_macro2,
                                                          &btn_macro3, &btn_macro4, &btn_macro5};

/* Stop TX on every page a transmission starts from: a press away, wherever the operator is */
buttons_page_t        rtty_page_send = {{&btn_p2, &btn_send, &btn_stop, &btn_dx, &btn_rst}};
static buttons_page_t page_3 = {{&btn_p3, &btn_macro0, &btn_macro1, &btn_macro2, &btn_stop}};
static buttons_page_t page_4 = {{&btn_p4, &btn_macro3, &btn_macro4, &btn_macro5, &btn_stop}};
buttons_page_t        rtty_page_power = {{&btn_p5, &btn_dig_pwr, &btn_ext_tune, &btn_clear, &btn_preset}};

/* A BTN_TEXT_FN without a subject never repaints by itself; one not on the page shown has nothing to repaint */
static void refresh(button_data_t *b) {
    if (b->disp_btn) buttons_refresh(b);
}

static void refresh_macro_labels(void) {
    for (int i = 0; i < RTTY_MACRO_MAX; i++) {
        const rtty_macro_t *m = rtty_macro_get(i);

        snprintf(macro_labels[i], sizeof(macro_labels[i]), "%s", m && m->name[0] ? m->name : "--");
        refresh(macro_btns[i]);
    }
}

/* ---- the text windows ---- */

/* The main screen's keys go to the window while it is open, back to the spectrum after (as cw_field.c's Log QSO) */
static void input_begin(void) {
    input = true;
    main_screen_keys_enable(false);
}

static void input_end(void) {
    input = false;
    main_screen_keys_enable(true);
}

static bool input_cancel(void) {
    input_end();
    return true;
}

/* textarea_window_set() takes a plain pointer: a copy, so a macro is never written through it */
static void set_copy(const char *text) {
    static char buf[RTTY_MACRO_TEXT_LEN];

    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    textarea_window_set(buf);
}

/* ---- sending ---- */

static bool line_ok(void) {
    const char *t = textarea_window_get();

    if (t && *t) rtty_tx_send(t);
    input_end();
    return true;
}

/* The line to send, first (a key typed on the main screen) in it already, 0 none */
static void open_line(uint32_t first) {
    if (input || textarea_window_is_open()) return;
    input_begin();
    textarea_window_open_w_label(line_ok, input_cancel, "RTTY: ");
    lv_textarea_set_max_length(textarea_window_text(), LINE_LEN);   /* the window's own is 64 */
    if (first) textarea_window_type(first);
}

static void send_press(button_data_t *b) {
    (void)b;
    open_line(0);
}

static void stop_press(button_data_t *b) {
    (void)b;
    rtty_tx_stop();
}

static void clear_press(button_data_t *b) {
    (void)b;
    panel_clear();
}

/* ---- the preset ---- */

static int preset_shown = -2;   /* the label's preset, -1 none; watch_cb repaints it when a knob changes it */

/* The preset in use, as PRE shows On or Off: "--" when Rate, Shift and Reverse are neither */
static const char *preset_label(void) {
    static char buf[40];

    preset_shown = rtty_preset_now();
    snprintf(buf, sizeof(buf), _("Preset:\n%s"), preset_shown >= 0 ? _(rtty_presets[preset_shown].name) : "--");
    return buf;
}

/* Ham, or Weather after Ham: the three settings as the knobs set them. The values in the message, the preset's
 * name on the button: the message line has room for one of them in full */
static void preset_press(button_data_t *b) {
    int                  i = rtty_preset_next(rtty_preset_now());
    const rtty_preset_t *p = &rtty_presets[i];

    rtty_preset_set(i);
    msg_update_text_fmt("%g Bd, %i Hz, Reverse %s", p->rate / 100.0, p->shift, p->reverse ? _("On") : _("Off"));
    refresh(b);
}

bool rtty_send_key(uint32_t key) {
    if (rtty_get_state() == RTTY_OFF || key >= 127 || !isalnum((int)key) || !keyboard_ready() || input) {
        return false;
    }
    open_line(key);
    return true;
}

/* ---- the macros ---- */

static void macro_press(int idx) {
    const rtty_macro_t *m = rtty_macro_get(idx);
    char                out[RTTY_MACRO_TEXT_LEN * 3];
    const char         *missing = NULL;

    if (input) return;
    if (!m) {
        msg_update_text_fmt("Empty slot - hold to create a macro");
        return;
    }
    if (!psk_macro_expand(m->text, out, sizeof(out), &missing)) {
        if (missing && !strcmp(missing, "<CALL>")) {
            msg_update_text_fmt("#%06x No DX call - set it on page 2#", theme_hex(TC_err));
        } else if (missing && !strcmp(missing, "<MYCALL>")) {
            msg_update_text_fmt("#%06x No callsign: Settings > General > STATION#", theme_hex(TC_err));
        } else {
            msg_update_text_fmt("#%06x %s is empty#", theme_hex(TC_err), missing ? missing : "?");
        }
        return;
    }
    rtty_tx_send(out);
}

bool rtty_send_macro(int n) {
    if (rtty_get_state() == RTTY_OFF || !rtty_macro_get(n - 1)) return false;
    macro_press(n - 1);
    return true;
}

/* The editor, as PSK's: the name, then the text, both filled in when a macro is there (so it can be renamed). An
 * empty text deletes it */
static bool editor_text_ok(void) {
    const char *t = textarea_window_get();
    bool        empty = !t || !*t;

    if (edit_idx >= 0) {
        if (empty) {
            rtty_macro_delete(edit_idx);
            msg_update_text_fmt("Macro deleted");
        } else {
            rtty_macro_set(edit_idx, edit_name, t);
            msg_update_text_fmt("Macro saved");
        }
    } else if (!empty) {
        if (rtty_macro_add(edit_name, t) < 0) {
            msg_update_text_fmt("#%06x All %d macro slots are used#", theme_hex(TC_err), RTTY_MACRO_MAX);
        } else {
            msg_update_text_fmt("Macro saved");
        }
    }
    if (!rtty_macros_save()) {
        msg_update_text_fmt("#%06x Could not write /mnt/rtty_macros.txt#", theme_hex(TC_err));
    }
    refresh_macro_labels();
    edit_idx = -1;
    input_end();
    return true;
}

static void editor_timer_cb(lv_timer_t *t) {
    const rtty_macro_t *m = rtty_macro_get(edit_idx);

    (void)t;
    editor_timer = NULL;
    if (rtty_get_state() == RTTY_OFF) {     /* the app closed between the two windows */
        input_end();
        return;
    }
    textarea_window_open_w_label(editor_text_ok, input_cancel, "Macro text:");
    lv_textarea_set_max_length(textarea_window_text(), RTTY_MACRO_TEXT_LEN - 1);
    if (m) set_copy(m->text);
}

static bool editor_name_ok(void) {
    const char         *t = textarea_window_get();
    const rtty_macro_t *m = rtty_macro_get(edit_idx);

    if (t && *t) {
        snprintf(edit_name, sizeof(edit_name), "%s", t);
    } else if (m) {
        snprintf(edit_name, sizeof(edit_name), "%s", m->name);
    } else {
        snprintf(edit_name, sizeof(edit_name), "M%d", rtty_macros_count() + 1);
    }
    /* This window gone before the next opens, or two text boxes are in the keyboard group. input stays set */
    if (!editor_timer) {
        editor_timer = lv_timer_create(editor_timer_cb, 50, NULL);
        lv_timer_set_repeat_count(editor_timer, 1);
    }
    return true;
}

static void macro_hold(int idx) {
    const rtty_macro_t *m;

    if (input) return;
    if (idx >= rtty_macros_count()) {
        if (rtty_macros_count() >= RTTY_MACRO_MAX) {
            msg_update_text_fmt("#%06x All %d macro slots are used#", theme_hex(TC_err), RTTY_MACRO_MAX);
            return;
        }
        idx = -1;       /* an empty slot: a new macro, after the others */
    }
    edit_idx = idx;
    m = rtty_macro_get(idx);
    input_begin();
    textarea_window_open_w_label(editor_name_ok, input_cancel, "Macro name:");
    if (m) set_copy(m->name);
}

/* ---- the QSO ---- */

static const char *dx_label(void) {
    static char buf[32];
    const char *c = digi_qso_dx();

    snprintf(buf, sizeof(buf), "DX call\n%s", c && *c ? c : "---");
    return buf;
}

static bool dx_ok(void) {
    const char *t = textarea_window_get();

    if (t && *t) {
        digi_qso_set_dx(t);
        refresh(&btn_dx);
    }
    input_end();
    return true;
}

static void dx_press(button_data_t *b) {
    const char *c = digi_qso_dx();

    (void)b;
    if (input || textarea_window_is_open()) return;
    input_begin();
    textarea_window_open_w_label(dx_ok, input_cancel, "DX call:");
    if (c && *c) set_copy(c);
}

static void dx_hold(button_data_t *b) {
    (void)b;
    digi_qso_clear();
    msg_update_text_fmt("QSO cleared");
    refresh(&btn_dx);
    refresh(&btn_rst);
}

static const char *rst_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "RST snt\n%s", digi_qso_rst_sent());
    return buf;
}

/* The strength digit; readability and tone stay 5 and 9 */
static void rst_press(button_data_t *b) {
    digi_qso_change_rst_sent(1);
    refresh(b);
}

static void rst_hold(button_data_t *b) {
    for (int i = 0; i < 9 && digi_qso_change_rst_sent(0) != 9; i++) {
        digi_qso_change_rst_sent(1);
    }
    refresh(b);
}

/* The DX call taken from what RTTY decodes ("DE <call>", on the DSP thread): onto its button. The preset's label
 * too when a knob turned Rate, Shift or Reverse while page 5 shows */
static void watch_cb(lv_timer_t *t) {
    static char shown[16];

    (void)t;
    if (strcmp(shown, digi_qso_dx()) != 0) {
        snprintf(shown, sizeof(shown), "%s", digi_qso_dx());
        refresh(&btn_dx);
    }
    if (rtty_preset_now() != preset_shown) refresh(&btn_preset);
}

void rtty_send_open(void) {
    static lv_timer_t *watch;

    rtty_macros_load();
    refresh_macro_labels();
    if (!watch) watch = lv_timer_create(watch_cb, 500, NULL);
}
