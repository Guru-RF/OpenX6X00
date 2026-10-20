/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW keyboard: text typed on a USB or Bluetooth keyboard is keyed in CW.
 *  A line goes out with Enter; lines typed while sending wait in a queue.
 *  Without a keyboard a press of the MFK opens the on-screen one.
 */

#include "dialog_cw_kbd.h"

#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cw.h"
#include "cw_encoder.h"
#include "params/params.h"
#include "events.h"
#include "keyboard.h"
#include "msg.h"
#include "radio.h"
#include "styles.h"
#include "textarea_window.h"
#include "theme.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define TIMER_MS     100
#define LINE_MAX     64
#define HISTORY_MAX  400   /* characters of sent text kept on screen */

#define COLOR_HEAD   theme_hex(TC_text_head)
#define COLOR_DIM    theme_hex(TC_text_dim)
#define COLOR_SENT   theme_hex(TC_text)
#define COLOR_NEXT   theme_hex(TC_accent)

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void send_btn_cb(button_data_t *btn);
static void stop_btn_cb(button_data_t *btn);
static void clear_btn_cb(button_data_t *btn);
static void close_btn_cb(button_data_t *btn);
static void live_btn_cb(button_data_t *btn);

static void msg_btn_cb(button_data_t *btn);
static void enqueue(const char *text);
static void enqueue_raw(const char *text, bool space);

static button_data_t btn_p1    = {.type = BTN_TEXT, .label = "(CW KBD 1:1)", .press = NULL};
static button_data_t btn_send  = {.type = BTN_TEXT, .label = "Send", .press = send_btn_cb};
static button_data_t btn_stop  = {.type = BTN_TEXT, .label = "Stop", .press = stop_btn_cb};
static button_data_t btn_clear = {.type = BTN_TEXT, .label = "Clear", .press = clear_btn_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_btn_cb};

static buttons_page_t page = {{&btn_p1, &btn_send, &btn_stop, &btn_clear, &btn_close}};

/* The last page: Live, each letter keyed as typed (the main screen CW line too) */
static const char *live_label(void) {
    return param_i_get(cfg_cw_live) ? "Live:\nOn" : "Live:\nOff";
}

static button_data_t  btn_pl   = {.type = BTN_TEXT, .press = button_next_page_cb, .next = &page};
static button_data_t  btn_live = {.type = BTN_TEXT_FN, .label_fn = live_label, .press = live_btn_cb};
static buttons_page_t page_live = {{&btn_pl, &btn_live}};

/* The MSG > CW messages (also on the web page CW messages), 4 a page after the first: a press
 * sends one as a typed line */
#define MSGS 8
static char           msgs[MSGS][PARAMS_MSG_CW_LEN];
static char           msg_label[MSGS][16];
static button_data_t  btn_msg[MSGS];
static buttons_page_t page_msg[2];
static button_data_t  btn_pm[2] = {
    {.type = BTN_TEXT, .press = button_next_page_cb},
    {.type = BTN_TEXT, .press = button_next_page_cb},
};

static void msg_btn_cb(button_data_t *btn) {
    enqueue(msgs[btn - btn_msg]);
}

/* Buttons and page labels for the messages there are, then the Live page */
static void msgs_load(void) {
    static char p_label[4][16];
    int         n     = params_msg_cw_list(msgs, MSGS);
    int         np    = (n + 3) / 4;
    int         pages = 2 + np;

    for (int i = 0; i < MSGS; i++) {
        btn_msg[i] = (button_data_t){.type = BTN_TEXT, .label = msg_label[i], .press = msg_btn_cb};
        snprintf(msg_label[i], sizeof(msg_label[i]), "%.10s", msgs[i]);
        page_msg[i / 4].items[1 + i % 4] = i < n ? &btn_msg[i] : NULL;
    }
    snprintf(p_label[0], sizeof(p_label[0]), "(CW KBD 1:%d)", pages);
    btn_p1.label = p_label[0];
    btn_p1.press = button_next_page_cb;
    btn_p1.next  = np ? &page_msg[0] : &page_live;
    for (int k = 0; k < 2; k++) {
        snprintf(p_label[k + 1], sizeof(p_label[k + 1]), "(CW KBD %d:%d)", k + 2, pages);
        btn_pm[k].label   = p_label[k + 1];
        btn_pm[k].next    = k + 1 < np ? &page_msg[k + 1] : &page_live;
        page_msg[k].items[0] = &btn_pm[k];
    }
    snprintf(p_label[3], sizeof(p_label[3]), "(CW KBD %d:%d)", pages, pages);
    btn_pl.label = p_label[3];
}

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
    .audio_cb     = NULL,
    .key_cb       = NULL,
};

dialog_t *dialog_cw_kbd = &dialog;

static lv_obj_t   *head, *head_right, *log_lbl, *input, *state_lbl, *hint;
static lv_timer_t *timer;
static bool        on_screen_kbd;

static char history[HISTORY_MAX + 1]; /* sent */
static char sending[512];             /* being keyed */
static char queue[512];               /* waiting */

static bool cw_mode(void) {
    int32_t m = cparam_i_get(cfg_cur_mode);
    return m == x6100_mode_cw || m == x6100_mode_cwr;
}

static bool busy(void) {
    return cw_encoder_state() != CW_ENCODER_IDLE;
}

static void history_add(const char *text) {
    size_t len = strlen(history), add = strlen(text);

    if (add > HISTORY_MAX) {
        text += add - HISTORY_MAX;
        add = HISTORY_MAX;
    }
    if (len + add > HISTORY_MAX) {     /* drop the oldest */
        memmove(history, history + len + add - HISTORY_MAX, HISTORY_MAX - add);
        len = HISTORY_MAX - add;
    }
    memcpy(history + len, text, add + 1);
}

/* The log: sent text dim, the line being keyed white up to where the key is
 * and yellow after, the queue grey */
static void update_view(void) {
    static char buf[HISTORY_MAX + sizeof(sending) + sizeof(queue) + 64];
    int         sent = busy() ? cw_encoder_sent() : 0;
    int         n    = strlen(sending);

    if (sent > n) {
        sent = n;
    }
    snprintf(buf, sizeof(buf), "#%06x %s##%06x %.*s##%06x %s# #%06x %s#", COLOR_DIM, history, COLOR_SENT, sent,
             sending, COLOR_NEXT, sending + sent, theme_hex(TC_text_faint), queue);
    lv_label_set_text(log_lbl, buf);

    lv_label_set_text_fmt(head, "CW KEYBOARD - #%06x %i WPM#", theme_hex(TC_text), (int)param_i_get(cfg_key_speed));
    lv_label_set_text(head_right, keyboard_ready() ? "keyboard ready" : "no keyboard: MFK press");

    if (!cw_mode()) {
        lv_label_set_text(state_lbl, "NOT CW");
        lv_obj_set_style_bg_color(state_lbl, TC(warn_bg), 0);
        lv_obj_set_style_text_color(state_lbl, TC(warn), 0);
    } else if (busy()) {
        lv_label_set_text(state_lbl, "ON AIR");
        lv_obj_set_style_bg_color(state_lbl, TC(tx_bg), 0);
        lv_obj_set_style_text_color(state_lbl, TC(text), 0);
    } else {
        lv_label_set_text(state_lbl, "READY");
        lv_obj_set_style_bg_color(state_lbl, TC(chip), 0);
        lv_obj_set_style_text_color(state_lbl, lv_color_hex(COLOR_HEAD), 0);
    }
}

/* The finished line goes to the log, the queue goes on air */
static void timer_cb(lv_timer_t *t) {
    (void)t;
    if (!busy()) {
        if (sending[0]) {
            history_add(sending);
            sending[0] = 0;
        }
        if (queue[0] && cw_mode()) {
            snprintf(sending, sizeof(sending), "%s", queue);
            queue[0] = 0;
            cw_encoder_send(sending, false);
        }
    }
    update_view();
}

/* A line to send: upper case, '#' dropped (it would break the colours) */
static void enqueue(const char *text) {
    enqueue_raw(text, true);
}

/* space: a word space after the text (a typed line); without it the Link paddle sends letter by letter */
static void enqueue_raw(const char *text, bool space) {
    size_t len = strlen(queue);

    text = cw_encoder_expand(text);

    if (!cw_mode()) {
        msg_update_text_fmt("Set CW mode to transmit");
        return;
    }
    for (; *text && len < sizeof(queue) - 2; text++) {
        if (*text != '#') {
            queue[len++] = toupper((unsigned char)*text);
        }
    }
    if (space) {
        queue[len++] = ' ';
    }
    queue[len]   = 0;
    timer_cb(NULL);
}

static void send_input(void) {
    const char *text = lv_textarea_get_text(input);

    if (*text) {
        enqueue(text);
        if (cw_mode()) {
            lv_textarea_set_text(input, "");
        }
    }
}

static void stop(void) {
    queue[0] = 0;
    cw_encoder_stop();
    timer_cb(NULL);
}

static void change_speed(int d) {
    int32_t v = param_i_get(cfg_key_speed) + d;

    if (v >= 5 && v <= 60) {
        param_i_set(cfg_key_speed, v);
    }
    update_view();
}

/* On-screen keyboard */

static void osk_end(void) {
    textarea_window_close();
    on_screen_kbd = false;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static bool osk_ok(void) {
    const char *val = textarea_window_get();

    if (val && *val) {
        enqueue(val);
    }
    osk_end();
    return true;
}

static bool osk_cancel(void) {
    osk_end();
    return true;
}

static void osk_open(void) {
    if (on_screen_kbd) {
        return;
    }
    on_screen_kbd = true;
    /* The text area gets the keys only when the dialog is out of the group */
    lv_group_remove_obj(dialog.obj);
    textarea_window_open(osk_ok, osk_cancel);
}

/* Keys: the typed characters, Enter, Esc; arrows and MFK change the speed */
static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ENTER:
        case '\r':
            send_input();
            break;
        case LV_KEY_BACKSPACE:
            if (*lv_textarea_get_text(input)) {
                lv_textarea_del_char(input);
            } else if (param_i_get(cfg_cw_live) && queue[0]) {   /* Live: the last letter not keyed yet */
                queue[strlen(queue) - 1] = 0;
                update_view();
            }
            break;
        case LV_KEY_ESC:
            if (busy() || queue[0]) {
                stop();
            } else {
                dialog_destruct();
            }
            break;
        case LV_KEY_LEFT:
        case LV_KEY_DOWN:
            change_speed(-1);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_UP:
            change_speed(1);
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            if (key >= ' ' && key < 127 && key != '#') {
                char c[2] = {(char)toupper((int)key), 0};

                if (param_i_get(cfg_cw_live)) {   /* keyed at once, letter by letter; a space is the word gap */
                    enqueue_raw(c, false);
                } else {
                    lv_textarea_add_text(input, c);
                }
            }
            break;
    }
}

/* MFK press (the encoder sends a click): send the line, or type one on screen */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();

    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) {
        return;
    }
    if (*lv_textarea_get_text(input)) {
        send_input();
    } else {
        osk_open();
    }
}

static void send_btn_cb(button_data_t *btn) {
    (void)btn;
    if (*lv_textarea_get_text(input)) {
        send_input();
    } else {
        osk_open();
    }
}

static void stop_btn_cb(button_data_t *btn) {
    (void)btn;
    stop();
}

static void clear_btn_cb(button_data_t *btn) {
    (void)btn;
    history[0] = 0;
    lv_textarea_set_text(input, "");
    update_view();
}

static void close_btn_cb(button_data_t *btn) {
    (void)btn;
    dialog_destruct();
}

static void set_placeholder(void) {
    lv_textarea_set_placeholder_text(input, param_i_get(cfg_cw_live) ? "Live: keyed as typed" : "type, Enter sends");
}

static void live_btn_cb(button_data_t *btn) {
    param_i_set(cfg_cw_live, !param_i_get(cfg_cw_live));
    buttons_refresh(btn);
    set_placeholder();
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    msgs_load();
    buttons_load_page(&page);   /* with the page count just found */

    head = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_label_set_recolor(head, true);
    lv_obj_set_pos(head, 16, 10);
    head_right = label(dialog.obj, &sony_20, COLOR_DIM);
    lv_obj_set_width(head_right, 300);
    lv_obj_set_style_text_align(head_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(head_right, 796 - 16 - 300, 10);

    /* Log, newest at the bottom */
    lv_obj_t *box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 776, DIALOG_H - 40 - 96);
    lv_obj_set_pos(box, 10, 40);
    lv_obj_set_style_bg_color(box, TC(panel), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, TC(panel_border), 0);
    lv_obj_set_style_border_width(box, theme_shape.border, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    log_lbl = label(box, &sony_24, COLOR_SENT);
    lv_label_set_recolor(log_lbl, true);
    lv_label_set_long_mode(log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(log_lbl, 776 - 20);
    lv_obj_align(log_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Input line and state */
    input = lv_textarea_create(dialog.obj);
    lv_obj_remove_style(input, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(input, 610, 44);
    lv_obj_set_pos(input, 10, DIALOG_H - 88);
    lv_obj_set_style_bg_color(input, TC(side), 0);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(input, 8, 0);
    lv_obj_set_style_pad_hor(input, 10, 0);
    lv_obj_set_style_pad_top(input, 6, 0);
    lv_obj_set_style_text_font(input, &sony_30, 0);
    lv_obj_set_style_text_color(input, TC(text), 0);
    lv_obj_set_style_bg_color(input, TC(text), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(input, LV_OPA_80, LV_PART_CURSOR);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, LINE_MAX);
    set_placeholder();
    lv_obj_add_state(input, LV_STATE_FOCUSED);   /* shows the cursor; not in the group */
    lv_obj_clear_flag(input, LV_OBJ_FLAG_CLICKABLE);

    state_lbl = label(dialog.obj, &sony_28, COLOR_HEAD);
    lv_obj_set_size(state_lbl, 156, 44);
    lv_obj_set_pos(state_lbl, 630, DIALOG_H - 88);
    lv_obj_set_style_bg_opa(state_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(state_lbl, 8, 0);
    lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(state_lbl, 6, 0);

    hint = label(dialog.obj, &sony_18, COLOR_DIM);
    lv_label_set_text(hint, "Enter: send - Esc: stop - arrows or MFK: speed - MFK press: send or type");
    lv_obj_set_pos(hint, 16, DIALOG_H - 34);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    timer_cb(NULL);
}

static void destruct_cb(void) {
    cw_encoder_stop();
    if (on_screen_kbd) {
        textarea_window_close();
        on_screen_kbd = false;
    }
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    lv_group_remove_obj(dialog.obj);
    sending[0] = queue[0] = 0;
}
