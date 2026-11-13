/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the radio's question before a TCI client on the network transmits (src/tci_ask.c): the
 *  real tci_ask.c on the PC build's LVGL, an 800 x 480 display, and the radio's three ways in through LVGL's own
 *  input handling: the MFK as an encoder, the front keys and a USB keyboard as a keypad, the touch screen as a
 *  pointer, each in keyboard_group as on the radio (the touch screen in none).
 *
 *  What it checks: the box over the screen with "<who> wants to transmit", No chosen when it opens, the screen woken;
 *  the MFK turning the choice and its press answering; the keyboard's arrows and Enter, Esc as No; a touch on a choice
 *  taking that one and one beside the choices nothing; the VOL knob still setting the volume; one answer per question;
 *  the focus back where it was, and neither the press that answered nor its release reaching what has the focus then;
 *  the focus left alone when something else took it meanwhile; the server's close, its notes after no answer, Always
 *  and no room to keep it; the box closing itself two seconds after its time with no answer; a new question taking
 *  the place of one still open.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lvgl/lvgl.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "key_input.h"
#include "tci_ask.h"
#include "tci_server.h"
#include "theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W   800
#define H   480
#define SEL 0x00ff00    /* the chosen button's background, theme_hex(TC_sel_bg) here */
#define WHO "192.168.1.20 (JTDX)"

/* ---- What tci_ask.c calls, stood in for ---- */

lv_group_t   *keyboard_group;
lv_style_t    popup_style;
theme_shape_t theme_shape;

/* LVGL's clock, moved by the test: the countdown's timer runs when it is a second further. With both defined here
 * the library's lv_hal_tick.o is not linked */
static uint32_t now = 1000;

uint32_t lv_tick_get(void) {
    return now;
}

uint32_t lv_tick_elaps(uint32_t prev_tick) {
    return now - prev_tick;
}

uint32_t theme_hex(theme_color_t c) {
    return c == TC_sel_bg ? SEL : 0x202020;
}

const lv_font_t *font_down(const lv_font_t *f, int steps) {
    (void)steps;
    return f;
}

static int lit;

bool backlight_tick() {
    lit++;
    return false;
}

static int vol;

uint16_t radio_change_vol(int16_t df) {
    vol += df;
    return 0;
}

static char note[256];
static int  notes;

void msg_schedule_text_ms(uint16_t ms, const char *fmt, ...) {
    va_list ap;

    (void)ms;
    va_start(ap, fmt);
    vsnprintf(note, sizeof(note), fmt, ap);
    va_end(ap);
    notes++;
}

static int       answers;
static uint32_t  answer_id;
static tci_ask_t answer;

void tci_server_answer(uint32_t id, tci_ask_t a) {
    answers++;
    answer_id = id;
    answer    = a;
}

/* ---- The checks ---- */

static int cases, good;

static bool check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    return ok;
}

/* ---- The display and the input devices ---- */

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    (void)a;
    (void)px;
    lv_disp_flush_ready(d);
}

static struct {
    int16_t diff;
    bool    down;
} enc;

static struct {
    uint32_t key;
    bool     down;
} pad;

static struct {
    lv_point_t at;
    bool       down;
} touch;

static lv_indev_t *mfk, *keys, *screen;

static void enc_read(lv_indev_drv_t *d, lv_indev_data_t *data) {
    (void)d;
    data->enc_diff = enc.diff;
    data->state    = enc.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    enc.diff       = 0;
}

static void pad_read(lv_indev_drv_t *d, lv_indev_data_t *data) {
    (void)d;
    data->key   = pad.key;
    data->state = pad.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void touch_read(lv_indev_drv_t *d, lv_indev_data_t *data) {
    (void)d;
    data->point = touch.at;
    data->state = touch.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static lv_indev_t *indev(lv_indev_type_t type, void (*read_cb)(lv_indev_drv_t *, lv_indev_data_t *)) {
    lv_indev_drv_t *drv = calloc(1, sizeof(*drv));

    lv_indev_drv_init(drv);
    drv->type    = type;
    drv->read_cb = read_cb;
    return lv_indev_drv_register(drv);
}

static void setup(void) {
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t      drv;
    lv_color_t               *px = malloc(W * 40 * sizeof(lv_color_t));

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, px, NULL, W * 40);
    lv_disp_drv_init(&drv);
    drv.hor_res  = W;
    drv.ver_res  = H;
    drv.flush_cb = flush;
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);

    lv_style_init(&popup_style);
    lv_style_set_bg_opa(&popup_style, LV_OPA_COVER);

    keyboard_group = lv_group_create();
    mfk            = indev(LV_INDEV_TYPE_ENCODER, enc_read);
    keys           = indev(LV_INDEV_TYPE_KEYPAD, pad_read);
    screen         = indev(LV_INDEV_TYPE_POINTER, touch_read);
    lv_indev_set_group(mfk, keyboard_group);
    lv_indev_set_group(keys, keyboard_group);
}

/* One read of the device, as LVGL's timer does it */
static void poll_dev(lv_indev_t *i) {
    lv_indev_read_timer_cb(i->driver->read_timer);
}

/* What LVGL runs between reads: the calls put off with lv_async_call */
static void run(void) {
    lv_timer_handler();
}

static void turn(int16_t diff) {
    enc.diff = diff;
    poll_dev(mfk);
}

/* Each starts with a read of the device at rest, as LVGL's timer reads it between presses: a press that answered
 * the box waits for that release (lv_indev_wait_release) */
static void mfk_press(void) {
    enc.down = false;
    poll_dev(mfk);
    enc.down = true;
    poll_dev(mfk);
    run();
    enc.down = false;
    poll_dev(mfk);
    run();
}

static void key_down(uint32_t key) {
    pad.down = false;
    poll_dev(keys);
    pad.key  = key;
    pad.down = true;
    poll_dev(keys);
}

static void key_up(void) {
    pad.down = false;
    poll_dev(keys);
}

static void key_tap(uint32_t key) {
    key_down(key);
    run();
    key_up();
    run();
}

static void tap_at(lv_obj_t *obj) {
    lv_area_t a;

    lv_obj_update_layout(lv_layer_top());
    lv_obj_get_coords(obj, &a);
    touch.down = false;
    poll_dev(screen);
    touch.at.x = (a.x1 + a.x2) / 2;
    touch.at.y = (a.y1 + a.y2) / 2;
    touch.down = true;
    poll_dev(screen);
    touch.down = false;
    poll_dev(screen);
    run();
}

/* The countdown: the clock a second on, LVGL's timers run */
static void second(void) {
    now += 1000;
    run();
}

/* ---- The box as tci_ask.c builds it: title, text, the row of three choices, the hint ---- */

static lv_obj_t *box(void) {
    return lv_obj_get_child_cnt(lv_layer_top()) ? lv_obj_get_child(lv_layer_top(), 0) : NULL;
}

static const char *text(void) {
    return lv_label_get_text(lv_obj_get_child(box(), 1));
}

static lv_obj_t *choice(int i) {
    return lv_obj_get_child(lv_obj_get_child(box(), 2), i);
}

static lv_coord_t width(void) {
    lv_obj_update_layout(box());
    return lv_obj_get_width(box());
}

static const char *hint(void) {
    return lv_label_get_text(lv_obj_get_child(box(), 3));
}

/* The chosen one: its background the theme's selection colour, -1 with none or more than one */
static int chosen(void) {
    int at = -1;

    for (int i = 0; i < 3; i++) {
        if (lv_obj_get_style_bg_color(choice(i), LV_PART_MAIN).full == lv_color_hex(SEL).full) {
            if (at >= 0) {
                return -1;
            }
            at = i;
        }
    }
    return at;
}

/* What had the focus before: a button on the screen, in the group, in navigation as the main screen's are */
static lv_obj_t *prev;
static int       prev_clicks, prev_keys;

static void prev_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        prev_clicks++;
    } else if (lv_event_get_code(e) == LV_EVENT_KEY) {
        prev_keys++;
    }
}

static void focus_prev(void) {
    lv_group_focus_obj(prev);
    lv_group_set_editing(keyboard_group, false);
    prev_clicks = prev_keys = 0;
}

/* The box gone, the focus back on prev in navigation, prev reached by nothing */
static void closed_back(const char *what) {
    check(!tci_ask_open() && !box(), "%s: the box closed", what);
    check(lv_group_get_focused(keyboard_group) == prev && !lv_group_get_editing(keyboard_group),
          "%s: the focus back where it was, not editing", what);
    check(prev_clicks == 0 && prev_keys == 0, "%s: what has the focus now got no click (%d) and no key (%d)", what,
          prev_clicks, prev_keys);
}

static const char *const names[] = {"?", "Allow once", "Always", "No", "no answer", "gone", "full"};

static void answered(const char *what, int n, uint32_t id, tci_ask_t a) {
    check(answers == n && answer_id == id && answer == a, "%s: answered %s for question %u, once (%d answers, the last %s "
          "for %u)", what, names[a], id, answers, names[answer], answer_id);
}

/* ---- The cases ---- */

static void opens(void) {
    int was = lit;

    focus_prev();
    tci_ask_show(7, WHO, 20);
    check(tci_ask_open() && box(), "open: the box on the top layer");
    check(lit == was + 1, "open: the screen woken (backlight_tick)");
    check(!strcmp(text(), WHO " wants to transmit"), "open: \"%s\"", text());
    check(chosen() == 2, "open: No chosen (%d)", chosen());
    check(strstr(hint(), "No in 20 s") != NULL, "open: \"%s\"", hint());
    check(lv_group_get_focused(keyboard_group) == box() && lv_group_get_editing(keyboard_group),
          "open: the box has the keys, editing (the MFK turns, not moves the focus)");
    check(width() == 600, "open: 600 px wide (%d)", width());
}

static void by_mfk(void) {
    turn(-1);
    check(chosen() == 1, "MFK: one to the left, Always (%d)", chosen());
    turn(-2);
    check(chosen() == 0, "MFK: two more, Allow once, the first (%d)", chosen());
    turn(1);
    turn(-1);
    check(chosen() == 0 && answers == 0 && prev_keys == 0, "MFK: right and back, Allow once, nothing answered yet");
    mfk_press();
    answered("MFK press", 1, 7, TCI_ASK_ONCE);
    closed_back("MFK press");
}

static void by_keyboard(void) {
    focus_prev();
    tci_ask_show(8, WHO, 20);
    key_tap(LV_KEY_RIGHT);
    check(chosen() == 2, "keyboard: Right on No stays No (%d)", chosen());
    key_tap(LV_KEY_LEFT);
    check(chosen() == 1, "keyboard: Left, Always (%d)", chosen());
    key_tap(LV_KEY_UP);
    key_tap(LV_KEY_DOWN);
    check(chosen() == 1, "keyboard: Up and Down, Always again (%d)", chosen());

    /* Enter answers on its press; Esc meanwhile, before the box goes, answers nothing more */
    uint32_t esc = LV_KEY_ESC;

    key_down(LV_KEY_ENTER);
    lv_event_send(box(), LV_EVENT_KEY, &esc);
    run();
    check(!box(), "keyboard: Enter's press closes the box, before its release");
    key_up();
    run();
    answered("keyboard Enter", 2, 8, TCI_ASK_ALWAYS);
    closed_back("keyboard Enter, then its release");

    /* The server's word that it kept it: the note */
    tci_ask_close(8, TCI_ASK_ALWAYS);
    check(strstr(note, WHO " may transmit from now on") != NULL, "Always kept: \"%s\"", note);
    tci_ask_close(8, TCI_ASK_FULL);
    check(strstr(note, WHO " may transmit until it disconnects") != NULL, "Always, no room: \"%s\"", note);

    focus_prev();
    tci_ask_show(9, WHO, 20);
    key_tap(LV_KEY_LEFT);
    key_tap(LV_KEY_LEFT);
    check(chosen() == 0, "keyboard: Left twice, Allow once (%d)", chosen());
    key_tap(LV_KEY_ESC);
    answered("keyboard Esc on Allow once", 3, 9, TCI_ASK_NO);
    closed_back("keyboard Esc");
}

static void by_touch(void) {
    focus_prev();
    tci_ask_show(10, WHO, 20);
    tap_at(lv_obj_get_child(box(), 1));
    check(box() && answers == 3, "touch beside the choices: nothing answered, the box stays");
    tap_at(choice(1));
    answered("touch on Always", 4, 10, TCI_ASK_ALWAYS);
    check(!box() && lv_group_get_focused(keyboard_group) == prev, "touch on Always: closed, the focus back");

    focus_prev();
    tci_ask_show(11, WHO, 20);
    tap_at(choice(0));
    answered("touch on Allow once, No chosen", 5, 11, TCI_ASK_ONCE);
}

static void volume(void) {
    focus_prev();
    tci_ask_show(12, WHO, 20);
    key_tap(KEY_VOL_LEFT_EDIT);
    key_tap(KEY_VOL_LEFT_SELECT);
    key_tap(KEY_VOL_RIGHT_EDIT);
    check(vol == -1 && box() && chosen() == 2 && answers == 5, "VOL: down, down, up: the volume %d, the box open, No "
          "chosen, nothing answered", vol);

    /* The client left: the server closes it, no note, no answer */
    notes = 0;
    tci_ask_close(12, TCI_ASK_GONE);
    check(answers == 5 && notes == 0, "the client gone: closed with no answer and no note");
    closed_back("the client gone");
}

static void timing(void) {
    focus_prev();
    tci_ask_show(13, WHO, 20);
    second();
    second();
    second();
    check(strstr(hint(), "No in 17 s") != NULL, "three seconds on: \"%s\"", hint());
    tci_ask_close(13, TCI_ASK_TIMEOUT);
    check(!strcmp(note, WHO ": no answer, no transmission"), "the server's no answer: the note \"%s\"", note);
    check(answers == 5, "the server's no answer: nothing answered back");
    closed_back("the server's no answer");

    /* Its word lost: the box goes by itself two seconds after its time, as no answer */
    focus_prev();
    tci_ask_show(14, WHO, 2);
    second();
    second();
    second();
    check(box() != NULL && strstr(hint(), "No in 0 s") != NULL, "its time over, the server's word not there: still "
          "open, \"%s\"", hint());
    second();
    answered("two seconds after its time", 6, 14, TCI_ASK_TIMEOUT);
    closed_back("two seconds after its time");
}

static void another(void) {
    focus_prev();
    tci_ask_show(20, "192.168.1.30", 20);
    tci_ask_show(21, WHO, 20);
    check(lv_obj_get_child_cnt(lv_layer_top()) == 1 && !strcmp(text(), WHO " wants to transmit"),
          "a new question: one box, the new one's");
    notes = 0;
    tci_ask_close(20, TCI_ASK_TIMEOUT);
    check(box() && notes == 0, "the old one's close: the new one stays, no note");
    tci_ask_close(21, TCI_ASK_GONE);
    closed_back("the new one closed");

    /* Something took the keys meanwhile (an app opened with its key): they stay there */
    lv_obj_t *app = lv_obj_create(lv_scr_act());

    focus_prev();
    tci_ask_show(22, WHO, 20);
    lv_group_add_obj(keyboard_group, app);
    lv_group_focus_obj(app);
    lv_group_set_editing(keyboard_group, true);
    tci_ask_close(22, TCI_ASK_GONE);
    check(!box() && lv_group_get_focused(keyboard_group) == app && lv_group_get_editing(keyboard_group),
          "an app took the keys meanwhile: they stay with it, editing");
    focus_prev();
    lv_obj_del(app);

    /* VISION: wider */
    theme_shape.vision = true;
    tci_ask_show(23, WHO, 20);
    check(width() == 680, "VISION: 680 px wide (%d)", width());
    tci_ask_close(23, TCI_ASK_GONE);
    theme_shape.vision = false;
}

int main(void) {
    setup();
    prev = lv_btn_create(lv_scr_act());
    lv_obj_add_event_cb(prev, prev_cb, LV_EVENT_ALL, NULL);
    lv_group_add_obj(keyboard_group, prev);

    opens();
    by_mfk();
    by_keyboard();
    by_touch();
    volume();
    timing();
    another();

    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}
