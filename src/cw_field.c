/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW from the keyboard on the main screen (see cw_field.h). Lines typed while
 *  sending wait in a queue, as in the CW keyboard app; the field hides when all is sent.
 */

#include "cw_field.h"

#include "band_plan.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cw.h"
#include "cw_encoder.h"
#include "dialog.h"
#include "dialog_field.h"
#include "keyboard.h"
#include "main_screen.h"
#include "mfk.h"
#include "panel.h"
#include "params/params.h"
#include "radio.h"
#include "styles.h"
#include "theme.h"
#include "textarea_window.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define TIMER_MS  100
#define LINE_MAX  64
#define FIELD_W   740
#define FIELD_X   ((800 - FIELD_W) / 2)   /* the arrow to the MFK fits in the left margin */
#define FIELD_H   64
#define WPM_W     150
#define FIELD_FONT sony_44   /* big: read at arm's length while keying */

static lv_obj_t *field, *text_lbl, *wpm_lbl, *mfk_lbl, *arrow;
static cfg_ctrl_t prev_mfk;             /* the MFK's control before the field took it for the speed */
static char      input[LINE_MAX + 1];  /* typed */
static char      sending[512];         /* being keyed */
static char      queue[512];           /* waiting */
static char      done[LINE_MAX + 1];   /* Live: the line keyed so far, until Enter or Esc */

/* CW / CW-R, VFO in a CW segment, no app open: what is queued goes on air */
static bool can_send(void) {
    int32_t m = cparam_i_get(cfg_cur_mode);

    return (m == radio_mode_cw || m == radio_mode_cwr) && band_plan_is_cw(cparam_i_get(cfg_fg_freq)) &&
           !dialog_is_run();
}

/* ... and a keyboard to type on */
static bool ready(void) {
    return can_send() && keyboard_ready();
}

static bool busy(void) {
    return cw_encoder_state() != CW_ENCODER_IDLE;
}

/* Like karaoke: the chars keyed so far (the one on the key included) turn yellow, the rest of the line stays white,
   the queue grey, then what is typed */
static void update_view(void) {
    static char buf[sizeof(sending) + sizeof(queue) + sizeof(done) + LINE_MAX + 64];
    int         sent = busy() ? cw_encoder_sent() + 1 : 0;
    int         n    = strlen(sending);

    if (!input[0] && !sending[0] && !queue[0] && !done[0]) {
        if (!lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(field, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
            mfk_set_ctrl_quiet(prev_mfk);
        }
        return;
    }
    lv_label_set_text_fmt(wpm_lbl, "%i WPM", (int)param_i_get(cfg_key_speed));
    if (sent > n) {
        sent = n;
    }
    snprintf(buf, sizeof(buf), "#%06x %s%.*s##%06x %s##%06x %s#%s_", theme_hex(TC_accent), done, sent, sending,
             theme_hex(TC_text), sending + sent, theme_hex(TC_text_faint), queue, input);
    lv_label_set_text(text_lbl, buf);
    /* A long line shows its end */
    snprintf(buf, sizeof(buf), "%s%s%s%s_", done, sending, queue, input);
    lv_obj_set_style_text_align(text_lbl,
                                lv_txt_get_width(buf, strlen(buf), &FIELD_FONT, 0, LV_TEXT_FLAG_NONE) > FIELD_W - 20 - WPM_W
                                    ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    if (lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(field, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(field);  /* over the CW decoder panel */
        prev_mfk = mfk_ctrl;            /* the MFK sets the keyer speed (the same as KEY > Speed) */
        mfk_set_ctrl_quiet(CTRL_KEY_SPEED);   /* the speed shows in the field */
    }
}

/* The finished line is gone, the queue goes on air; out of the conditions nothing new starts */
static void timer_cb(lv_timer_t *t) {
    (void)t;
    if (!can_send()) {
        queue[0] = 0;
        if (!busy()) {
            input[0] = sending[0] = done[0] = 0;
        }
    }
    if (!busy()) {
        if (param_i_get(cfg_cw_live)) {     /* the keyed letters stay on the line, its end when long */
            size_t d = strlen(done), n = strlen(sending);

            if (n >= LINE_MAX) {
                d = 0;
            } else if (d + n > LINE_MAX) {
                memmove(done, done + d + n - LINE_MAX, LINE_MAX - n);
                d = LINE_MAX - n;
            }
            snprintf(done + d, sizeof(done) - d, "%s", sending + (n > LINE_MAX ? n - LINE_MAX : 0));
        } else {
            done[0] = 0;
        }
        sending[0] = 0;
        if (queue[0]) {
            snprintf(sending, sizeof(sending), "%s", queue);
            queue[0] = 0;
            cw_encoder_send(sending, false);
        }
    }
    update_view();
}

static void add_text(const char *text, bool space);

void cw_field_send_text(const char *text) {
    add_text(text, true);
}

/* space: a word space after the text; without it the Link paddle sends letter by letter */
static void add_text(const char *text, bool space) {
    size_t q = strlen(queue);

    /* '#' would break the colours */
    for (text = cw_encoder_expand(text); *text && q < sizeof(queue) - 2; text++) {
        if (*text != '#') {
            queue[q++] = toupper((unsigned char)*text);
        }
    }
    if (space) {
        queue[q++] = ' ';
    }
    queue[q]   = 0;
}

void cw_field_log_qso(const char *text) {
    char call[32] = "", rs[8] = "", rr[8] = "", line[64];

    if (sscanf(text, "%31s %7s %7s", call, rs, rr) < 1) {
        return;
    }
    snprintf(line, sizeof(line), "%s|%s|%s", call, rs, rr);
    dialog_field_qso_log(line);
}

static bool log_ok(void) {
    cw_field_log_qso(textarea_window_get());
    textarea_window_close();
    main_screen_keys_enable(true);
    return true;
}

static bool log_cancel(void) {
    textarea_window_close();
    main_screen_keys_enable(true);
    return true;
}

void cw_field_log_open(void) {
    main_screen_keys_enable(false);
    textarea_window_open_w_label(log_ok, log_cancel, "Call [sent [rcvd]]: ");
    if (*panel_last_call()) {                   /* the last call the decoder wrote, ready to log */
        char line[24];
        snprintf(line, sizeof(line), "%s ", panel_last_call());
        textarea_window_set(line);
    }
}

bool cw_field_key(uint32_t key) {
    size_t len  = strlen(input);
    bool   fkey = key >= KEYBOARD_F1 && key <= KEYBOARD_F5;   /* MSG > CW messages 1-5 */

    if (!ready()) {
        return false;
    }
    /* Hidden: a letter, digit or F1-F5 opens it, the other keys keep their main screen use */
    if (lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN) && !(key < 127 && isalnum((int)key)) && !fkey) {
        return false;
    }
    if (fkey) {
        char msgs[5][PARAMS_MSG_CW_LEN];
        int  i = key - KEYBOARD_F1;

        if (params_msg_cw_list(msgs, i + 1) <= i) {
            return false;
        }
        cw_field_send_text(msgs[i]);
        timer_cb(NULL);
        return true;
    }
    if (key == KEYBOARD_F10) {     /* the line is the QSO to log, not sent */
        if (input[0]) {
            cw_field_log_qso(input);
            input[0] = 0;
        }
        timer_cb(NULL);
        return true;
    }
    switch (key) {
        case LV_KEY_ENTER:
        case '\r':
            done[0] = 0;    /* Live: a new line */
            if (input[0]) {
                cw_field_send_text(input);
                input[0] = 0;
            }
            break;
        case LV_KEY_BACKSPACE:
            if (len) {
                input[len - 1] = 0;
            } else if (param_i_get(cfg_cw_live) && queue[0]) {   /* Live: the last letter not keyed yet */
                queue[strlen(queue) - 1] = 0;
            }
            break;
        case LV_KEY_ESC:
            input[0] = queue[0] = done[0] = 0;
            if (sending[0]) {
                cw_encoder_stop();
                sending[0] = 0;
            }
            break;
        default:
            /* '#' would break the colours */
            if (key < ' ' || key >= 127 || key == '#') {
                return false;
            }
            if (param_i_get(cfg_cw_live)) {      /* keyed at once, letter by letter; a space is the word gap */
                char c[2] = {(char)key, 0};
                add_text(c, false);
            } else if (len < LINE_MAX) {
                input[len]     = toupper((int)key);
                input[len + 1] = 0;
            }
            break;
    }
    timer_cb(NULL);
    return true;
}

bool cw_field_visible(void) {
    return field && !lv_obj_has_flag(field, LV_OBJ_FLAG_HIDDEN);
}

void cw_field_init(lv_obj_t *parent) {
    bool again = arrow != NULL;   /* a theme change: the old arrow goes, the timer stays */

    if (again) lv_obj_del(arrow);
    field = lv_obj_create(parent);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, FIELD_W, FIELD_H);
    lv_obj_set_pos(field, FIELD_X, 480 - BTN_HEIGHT - FIELD_H - 8);
    lv_obj_set_style_bg_color(field, TC(panel), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_80, 0);
    lv_obj_set_style_border_color(field, TC(panel_border), 0);
    lv_obj_set_style_border_width(field, theme_shape.border, 0);
    lv_obj_set_style_radius(field, 6, 0);
    lv_obj_set_style_shadow_color(field, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(field, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(field, 14, 0);
    lv_obj_set_style_shadow_ofs_y(field, 3, 0);
    lv_obj_set_style_pad_hor(field, 8, 0);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(field, LV_OBJ_FLAG_HIDDEN);

    text_lbl = lv_label_create(field);
    lv_obj_set_width(text_lbl, FIELD_W - 16 - WPM_W);
    lv_obj_set_style_text_font(text_lbl, &FIELD_FONT, 0);
    lv_obj_set_style_text_color(text_lbl, TC(text), 0);
    lv_label_set_recolor(text_lbl, true);
    lv_label_set_long_mode(text_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(text_lbl, LV_ALIGN_LEFT_MID, WPM_W, 0);

    wpm_lbl = lv_label_create(field);
    lv_obj_set_style_text_font(wpm_lbl, &sony_22, 0);
    lv_obj_set_style_text_color(wpm_lbl, TC(accent), 0);
    lv_obj_align(wpm_lbl, LV_ALIGN_LEFT_MID, 58, 0);

    /* While the field is up the MFK turns the speed: say so above the value */
    mfk_lbl = lv_label_create(field);
    lv_obj_set_style_text_font(mfk_lbl, &sony_18, 0);
    lv_obj_set_style_text_color(mfk_lbl, TC(panel), 0);
    lv_obj_set_style_bg_color(mfk_lbl, TC(accent), 0);
    lv_obj_set_style_bg_opa(mfk_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mfk_lbl, 3, 0);
    lv_obj_set_style_pad_hor(mfk_lbl, 4, 0);
    lv_label_set_text(mfk_lbl, "MFK");
    lv_obj_align(mfk_lbl, LV_ALIGN_LEFT_MID, 6, 0);   /* clear of the arrow's start */

    /* An arrow from the tag out of the screen's left edge, toward the MFK below it; on the top layer, over the buttons */
    static lv_point_t shaft[] = {{34, 8}, {5, 36}};
    static lv_point_t head[]  = {{8, 21}, {5, 36}, {20, 34}};
    arrow = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(arrow);
    lv_obj_set_size(arrow, 46, 48);              /* a real size, or hiding it would leave the lines on screen */
    lv_obj_set_pos(arrow, 0, 480 - BTN_HEIGHT - 8 - FIELD_H / 2 - 10);
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {               /* a dark outline under, then the yellow line */
        lv_obj_t *line = lv_line_create(arrow);
        lv_line_set_points(line, i % 2 ? head : shaft, i % 2 ? 3 : 2);
        /* 1bit: an ink arrow in a paper halo (the outline in paper, solid) */
        lv_obj_set_style_line_color(line, i < 2 ? (theme_shape.mono ? TC(panel) : lv_color_black()) : TC(accent), 0);
        lv_obj_set_style_line_opa(line, i < 2 && !theme_shape.mono ? LV_OPA_60 : LV_OPA_COVER, 0);
        lv_obj_set_style_line_width(line, i < 2 ? 9 : 5, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }

    if (!again) lv_timer_create(timer_cb, TIMER_MS, NULL);
}
