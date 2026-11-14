/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the CW Keyboard's log (src/dialog_cw_kbd.c, #269): what was sent dim, the line on the key
 *  white up to the key and yellow after, the queue grey, on as many rows as it takes. The real dialog_cw_kbd.c and
 *  recolor_rows.c and the real fonts, put in place by font_select.c's fonts_apply() as at boot, on the PC build's
 *  LVGL: the dialog opened as on the radio, a keyboard's keys sent to it as key_input.c sends them (letters, Å as its
 *  UTF-8 bytes, Enter, Esc), the keyer stood in for, and every letter LVGL draws for the log written down (its draw
 *  context's draw_letter, the step before the pixels): which letter, in which colour.
 *
 *  As reported (#269, Live, a Bluetooth keyboard): once the text sent filled more than one row of the log, the second
 *  row came white and its end showed "#f1f5f9 #e5a823 475569", the colour codes of the runs after it. LVGL 8 starts
 *  every row it draws outside any colour, and wraps the text the same way: a colour run on over a row's end lost its
 *  colour there, its closing '#' then started a colour of its own, and the next run's '#' made that a '#' to show.
 *  The first case draws the log's text as it was made up to v0.3.12 and finds that again, in LVGL as it is.
 *
 *  Each case then checks, in every font a theme or Settings can put there and VISION's sizes:
 *  - every letter drawn is a letter of the text, in its run's colour, and none is missing but whole rows above the
 *    log's box (the oldest, out of sight);
 *  - LVGL lays the label out in exactly the rows the text was cut into: none of them wrapped again.
 *  The cases: the reporter's text keyed in Live; Live while the keyer is on a letter (the next yellow, then white, the
 *  queue grey); a line typed and sent with Enter, keyed up to the middle, over a row's end; a word longer than a row
 *  (cut inside it); Å Ö Ä typed (#265); more than the log keeps (400 characters: the oldest leave the top).
 *  Last recolor_rows() alone, on 150 random texts of up to five colours for each font, at random widths.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lvgl/lvgl.h"
#include "app_window.h"
#include "button_bar.h"
#include "cfg/cfg_api.h"
#include "cw_encoder.h"
#include "dialog_cw_kbd.h"
#include "fonts/font_select.h"
#include "gui_styles.h"
#include "key_input.h"
#include "msg_box.h"
#include "params/params.h"
#include "radio.h"
#include "recolor_rows.h"
#include "text_popup.h"
#include "theme.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 800
#define H 2400      /* the random texts' labels at their narrowest; the dialog sits in the top 480 as on the radio */

/* ---- What dialog_cw_kbd.c calls, stood in for ---- */

/* The theme: the default's colours (RF.Guru, the first of theme.h's columns) */
theme_shape_t theme_shape;

uint32_t theme_hex(theme_color_t c) {
    switch (c) {
#define FIRST(name, a, b, c, d) \
    case TC_##name:             \
        return a;
        THEME_COLORS(FIRST)
#undef FIRST
        default:
            return 0;
    }
}

/* The settings: KEY Speed, Live, the mode (CW) */
static int32_t speed = 16, live = 1, mode = radio_mode_cw;

ParamInt         *cfg_key_speed = (ParamInt *)&speed;
ParamInt         *cfg_cw_live   = (ParamInt *)&live;
ComputedParamInt *cfg_cur_mode  = (ComputedParamInt *)&mode;

int32_t param_i_get(const ParamInt *p) {
    return *(const int32_t *)p;
}

void param_i_set(ParamInt *p, int32_t v) {
    *(int32_t *)p = v;
}

int32_t cparam_i_get(const ComputedParamInt *p) {
    return *(const int32_t *)p;
}

int params_msg_cw_list(char (*out)[PARAMS_MSG_CW_LEN], int max) {
    (void)out;
    (void)max;
    return 0;
}

/* The window: where app_window.c puts it, plain */
lv_obj_t *dialog_init(lv_obj_t *parent) {
    lv_obj_t *win = lv_obj_create(parent);

    lv_obj_remove_style_all(win);
    lv_obj_set_size(win, BOARD_DISP_W, DIALOG_H);
    lv_obj_set_pos(win, 0, DIALOG_Y);
    lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    return win;
}

void dialog_destruct() {
}

void buttons_load_page(buttons_page_t *page) {
    (void)page;
}

void buttons_refresh(button_data_t *data) {
    (void)data;
}

void button_next_page_cb(button_data_t *data) {
    (void)data;
}

lv_group_t *keyboard_group;

bool keyboard_ready() {
    return true;
}

void msg_update_text_fmt(const char *fmt, ...) {
    (void)fmt;
}

uint16_t radio_change_vol(int16_t df) {
    (void)df;
    return 0;
}

/* The on-screen keyboard: not opened here (the keys come from a keyboard) */
lv_obj_t *textarea_window_open(textarea_window_cb_t ok_cb, textarea_window_cb_t cancel_cb) {
    (void)ok_cb;
    (void)cancel_cb;
    return NULL;
}

lv_obj_t *textarea_window_open_w_label(textarea_window_cb_t ok_cb, textarea_window_cb_t cancel_cb, const char *text) {
    (void)ok_cb;
    (void)cancel_cb;
    (void)text;
    return NULL;
}

void textarea_window_close() {
}

const char *textarea_window_get() {
    return "";
}

lv_obj_t *textarea_window_text() {
    return NULL;
}

/* The keyer: done at once (keying_on false), or on a letter of the text until told (true), key_at letters in */
static bool keying_on;
static int  key_at;
static cw_encoder_state_t keyer = CW_ENCODER_IDLE;

unsigned cw_encoder_send(const char *text, bool beacon) {
    (void)text;
    (void)beacon;
    keyer  = keying_on ? CW_ENCODER_SEND : CW_ENCODER_IDLE;
    key_at = 0;
    return 1;
}

void cw_encoder_stop() {
    keyer = CW_ENCODER_IDLE;
}

cw_encoder_state_t cw_encoder_state() {
    return keyer;
}

int cw_encoder_sent() {
    return keyer == CW_ENCODER_IDLE ? 0 : key_at;
}

const char *cw_encoder_expand(const char *text) {
    return text;
}

/* LVGL's clock: moved on by the test, for the dialog's 100 ms timer. With both defined here the library's
 * lv_hal_tick.o is not linked */
static uint32_t now = 1000;

uint32_t lv_tick_get(void) {
    return now;
}

uint32_t lv_tick_elaps(uint32_t prev_tick) {
    return now - prev_tick;
}

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---- The display, and the letters drawn ---- */

typedef struct {
    uint32_t letter;
    uint32_t color;    /* 0xRRGGBB */
} letter_t;

static letter_t drawn[4096];
static int      n_drawn;
static bool     armed, recording;
static void (*sw_letter)(lv_draw_ctx_t *, const lv_draw_label_dsc_t *, const lv_point_t *, uint32_t);

static void record_letter(lv_draw_ctx_t *ctx, const lv_draw_label_dsc_t *dsc, const lv_point_t *pos, uint32_t letter) {
    if (recording && letter > ' ' && n_drawn < (int)(sizeof(drawn) / sizeof(drawn[0]))) {
        drawn[n_drawn++] = (letter_t){letter, lv_color_to32(dsc->color) & 0xffffff};
    }
    sw_letter(ctx, dsc, pos, letter);
}

static void (*sw_init)(lv_disp_drv_t *, lv_draw_ctx_t *);    /* LVGL's own (software drawing) */

static void draw_ctx_init(lv_disp_drv_t *drv, lv_draw_ctx_t *ctx) {
    sw_init(drv, ctx);
    sw_letter        = ctx->draw_letter;
    ctx->draw_letter = record_letter;
}

/* Only the letters of the object being watched: its own drawing, once (draw()) */
static void watch_cb(lv_event_t *e) {
    recording = armed && lv_event_get_code(e) == LV_EVENT_DRAW_MAIN_BEGIN;
}

static void watch(lv_obj_t *obj) {
    lv_obj_add_event_cb(obj, watch_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(obj, watch_cb, LV_EVENT_DRAW_MAIN_END, NULL);
}

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    (void)a;
    (void)px;
    lv_disp_flush_ready(d);
}

static void display(void) {
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t      drv;
    lv_color_t               *px = malloc(W * H * sizeof(lv_color_t));

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, px, NULL, W * H);
    lv_disp_drv_init(&drv);
    drv.hor_res       = W;
    drv.ver_res       = H;
    drv.flush_cb      = flush;
    drv.draw_buf      = &draw_buf;
    sw_init           = drv.draw_ctx_init;
    drv.draw_ctx_init = draw_ctx_init;
    lv_disp_drv_register(&drv);
    lv_disp_set_bg_color(lv_disp_get_default(), lv_color_black());
    lv_disp_set_bg_opa(lv_disp_get_default(), LV_OPA_COVER);
    keyboard_group = lv_group_create();
}

/* obj drawn: the letters it draws written down. What waits to be drawn first, not written down: LVGL draws each
 * area that changed on its own, and a label over two of them twice */
static void draw(lv_obj_t *obj) {
    lv_refr_now(NULL);
    n_drawn = 0;
    lv_obj_invalidate(obj);
    armed = true;
    lv_refr_now(NULL);
    armed = recording = false;
}

/* ---- What should be drawn ---- */

typedef struct {
    letter_t l[4096];
    int      n;
} text_t;

/* The letters of the UTF-8 s (spaces and control characters leave no ink: none), in color */
static void text_add(text_t *t, const char *s, size_t len, uint32_t color) {
    uint32_t i = 0;

    while (i < len) {
        uint32_t c = _lv_txt_encoded_next(s, &i);

        if (c > ' ' && t->n < (int)(sizeof(t->l) / sizeof(t->l[0]))) {
            t->l[t->n++] = (letter_t){c, color};
        }
    }
}

/* The letters drawn as text, to show what went wrong: from the from-th on, "~" after one not in the colour of want's
 * want_from-th on (want NULL: the letters alone) */
static void shown(char *out, size_t size, int from, const text_t *want, int want_from) {
    size_t k = 0;

    for (int i = from; i < n_drawn && k + 8 < size; i++) {
        int  at   = want_from + (i - from);
        bool same = !want || (at < want->n && want->l[at].color == drawn[i].color);

        if (drawn[i].letter < 0x80) {
            out[k++] = (char)drawn[i].letter;
        } else {
            uint32_t u = _lv_txt_unicode_to_encoded(drawn[i].letter);

            for (int b = 0; b < 4 && (u & 0xff); b++, u >>= 8) {
                out[k++] = (char)(u & 0xff);
            }
        }
        if (!same) {
            out[k++] = '~';
        }
    }
    out[k] = 0;
}

/* The letters drawn are want's last ones, each in its colour: those over the top of the box out of sight, none of
 * the rest missing. top: how many of want's are out of sight (-1: the drawn ones are not want's last) */
static int drawn_is(const text_t *want) {
    int top = want->n - n_drawn;

    if (top < 0) {
        return -1;
    }
    for (int i = 0; i < n_drawn; i++) {
        if (drawn[i].letter != want->l[top + i].letter || drawn[i].color != want->l[top + i].color) {
            return -1;
        }
    }
    return top;
}

/* LVGL lays the label's text out in the rows it was cut into: as many as its '\n' and one, no row wrapped again */
static bool rows_kept(lv_obj_t *label, int *rows, int *laid) {
    const char      *s    = lv_label_get_text(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_coord_t       lh   = lv_font_get_line_height(font) + lv_obj_get_style_text_line_space(label, 0);

    *rows = 1;
    for (; *s; s++) {
        *rows += *s == '\n';
    }
    lv_obj_update_layout(label);
    *laid = (lv_obj_get_content_height(label) + lv_obj_get_style_text_line_space(label, 0)) / lh;
    return *laid == *rows;
}

/* ---- The CW Keyboard ---- */

#define DIM   0x94a3b8  /* TC_text_dim: sent */
#define SENT  0xf1f5f9  /* TC_text: keyed of the line on the key */
#define NEXT  0xe5a823  /* TC_accent: the rest of it */
#define FAINT 0x475569  /* TC_text_faint: the queue */

static lv_obj_t *log_lbl;

/* What the dialog keeps of what was sent: up to 400 bytes, the oldest whole letters leave (history_add) */
static char hist[1024];

static void hist_add(const char *s) {
    size_t len;

    strcat(hist, s);
    len = strlen(hist);
    if (len > 400) {
        size_t drop = len - 400;

        while (((unsigned char)hist[drop] & 0xC0) == 0x80) {
            drop++;
        }
        memmove(hist, hist + drop, len - drop + 1);
    }
}

static void open_kbd(uint8_t font, bool vision) {
    button_data_t *clear = dialog_cw_kbd->btn_page->items[3];
    lv_obj_t      *box;

    fonts_apply(font);
    if (vision) {
        fonts_scale(VISION_FONT_STEPS);
    }
    hist[0]   = 0;
    keying_on = false;
    keyer     = CW_ENCODER_IDLE;
    live      = 1;
    dialog_cw_kbd->construct_cb(lv_scr_act());
    box     = lv_obj_get_child(dialog_cw_kbd->obj, 2);    /* after the head's two labels: the log's box */
    log_lbl = lv_obj_get_child(box, 0);
    if (!lv_obj_check_type(log_lbl, &lv_label_class) || strcmp(clear->label, "Clear")) {
        printf("the log's label or the Clear button is not where it was: see construct_cb and page\n");
        exit(1);
    }
    watch(log_lbl);
    clear->press(clear);    /* the log is kept from one time the dialog is open to the next */
}

static void close_kbd(void) {
    dialog_cw_kbd->destruct_cb();
    lv_obj_del(dialog_cw_kbd->obj);
    dialog_cw_kbd->obj = NULL;
}

/* A key as key_input.c gives it: a character, or a letter of the layout as its UTF-8 bytes, the first lowest */
static void key(uint32_t k) {
    lv_event_send(dialog_cw_kbd->obj, LV_EVENT_KEY, &k);
}

static void type(const char *s) {
    while (*s) {
        uint32_t k = 0, n = _lv_txt_encoded_size(s);

        memcpy(&k, s, n);
        key(k);
        s += n;
    }
}

/* The dialog's timer: the line keyed goes to the log, the queue on air */
static void tick(void) {
    now += 150;
    lv_timer_handler();
}

/* A row of the label's text starts after n letters (spaces and the colours' codes not counted) */
static bool row_starts_at(lv_obj_t *label, int n) {
    lv_text_cmd_state_t st    = LV_TEXT_CMD_STATE_WAIT;
    const char         *s     = lv_label_get_text(label);
    uint32_t            i     = 0;
    int                 k     = 0;
    bool                start = true;

    for (;;) {
        uint32_t c;

        if (start && k == n) {
            return true;
        }
        if (!s[i]) {
            return false;
        }
        c     = _lv_txt_encoded_next(s, &i);
        start = c == '\n';
        if (start) {
            st = LV_TEXT_CMD_STATE_WAIT;   /* as LVGL: each row from outside any colour */
        } else if (!_lv_txt_is_cmd(&st, c) && c > ' ') {
            k++;
        }
    }
}

/* The log drawn as want says: every letter, but whole rows of the oldest over the top of the box (out of sight, as
 * the box is only so high); and the rows as cut */
static void log_is(const char *name, const char *what, const text_t *want) {
    char got[2048];
    int  top, rows, laid;
    bool kept, whole;

    draw(log_lbl);
    top   = drawn_is(want);
    whole = top >= 0 && row_starts_at(log_lbl, top);
    shown(got, sizeof(got), n_drawn > 60 ? n_drawn - 60 : 0, want,
          (top < 0 ? want->n - n_drawn : top) + (n_drawn > 60 ? n_drawn - 60 : 0));
    check(whole, "%s: %s: every letter in its colour, %d of %d in whole rows out of sight at the top%s%s", name, what,
          top < 0 ? 0 : top, want->n, whole ? "" : ", drawn: ...", whole ? "" : got);
    kept = rows_kept(log_lbl, &rows, &laid);
    check(kept, "%s: %s: laid out in the %d rows it was cut into (%d)", name, what, rows, laid);
}

/* What was sent, all dim, then the line on the key and the queue */
static void want_log(text_t *t, const char *sending, int at, const char *queue) {
    t->n = 0;
    text_add(t, hist, strlen(hist), DIM);
    text_add(t, sending, at, SENT);
    text_add(t, sending + at, strlen(sending) - at, NEXT);
    text_add(t, queue, strlen(queue), FAINT);
}

static text_t want;

static void kbd_cases(const char *name, uint8_t font, bool vision) {
    static const char reporter[] = "cc,/?oelagerstrom engelskt tangentbord utan svenska tecken sorryb bn n";
    static const char line[]     = "the quick brown fox jumps over the lazy dog 0123456789";
    char              up[256], w90[91];

    open_kbd(font, vision);

    /* As reported: Live, each letter keyed as it is typed and done before the next */
    type(reporter);
    tick();
    snprintf(up, sizeof(up), "%s", reporter);
    for (char *p = up; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    hist_add(up);
    want_log(&want, "", 0, "");
    log_is(name, "the reporter's text keyed in Live", &want);

    /* Live while the keyer is on the first letter: the rest queued */
    keying_on = true;
    type("cq de sa5qed k");
    want_log(&want, "C", 0, "Q DE SA5QED K");
    tick();
    log_is(name, "Live, the C on the key", &want);
    key_at = 1;
    tick();
    want_log(&want, "C", 1, "Q DE SA5QED K");
    log_is(name, "Live, the C keyed", &want);
    key(LV_KEY_ESC);   /* the queue dropped, the C to the log */
    hist_add("C");

    /* A line typed, sent with Enter, keyed half way */
    live = 0;
    type(line);
    key(LV_KEY_ENTER);
    key_at = 20;
    tick();
    snprintf(up, sizeof(up), "%s ", line);
    for (char *p = up; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    want_log(&want, up, 20, "");
    log_is(name, "a line sent with Enter, keyed to the 20th letter", &want);
    keyer = CW_ENCODER_IDLE;
    tick();
    hist_add(up);
    keying_on = false;

    /* A word longer than a row, in Live */
    live = 1;
    memset(w90, 'w', 90);
    w90[90] = 0;
    type(w90);
    tick();
    memset(w90, 'W', 90);
    hist_add(w90);
    want_log(&want, "", 0, "");
    log_is(name, "90 W in one word", &want);

    /* The letters of #265 */
    type(" p\xc3\xa5 r\xc3\xb6\x64 \xc3\xa4lg");
    tick();
    hist_add(" P\xc3\x85 R\xc3\x96\x44 \xc3\x84LG");
    want_log(&want, "", 0, "");
    log_is(name, "P\xc3\x85 R\xc3\x96\x44 \xc3\x84LG typed", &want);

    /* More than the log keeps: the oldest go, the newest at the bottom */
    for (int i = 0; i < 4; i++) {
        type(" the five boxing wizards jump quickly, 73 de sa5qed");
        tick();
        hist_add(" THE FIVE BOXING WIZARDS JUMP QUICKLY, 73 DE SA5QED");
    }
    want_log(&want, "", 0, "");
    log_is(name, "400 characters, the oldest out of sight", &want);

    close_kbd();
}

/* ---- The log's text as it was made up to v0.3.12: one run each, over the end of a row ---- */

static void as_it_was(void) {
    static const char sent[] = "CC,/?OELAGERSTROM ENGELSKT TANGENTBORD UTAN SVENSKA TECKEN SORRYB BN N";
    char              buf[512], got[512];
    lv_obj_t         *l = lv_label_create(lv_scr_act());
    int               top;

    fonts_apply(FONT_DEJAVU);
    lv_obj_set_style_text_font(l, &sony_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(SENT), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 776 - 20);
    watch(l);
    snprintf(buf, sizeof(buf), "#%06x %s##%06x %.*s##%06x %s# #%06x %s#", DIM, sent, SENT, 0, "", NEXT, "", FAINT, "");
    lv_label_set_text(l, buf);
    draw(l);
    want.n = 0;
    text_add(&want, sent, strlen(sent), DIM);
    top = drawn_is(&want);
    shown(got, sizeof(got), 0, NULL, 0);
    check(top != 0 && strstr(got, "N#f1f5f9#e5a823475569"),
          "LVGL as it is, one colour over the end of a row (up to v0.3.12): the codes shown, as reported: %s", got);
    lv_obj_del(l);
}

/* ---- recolor_rows() alone: random texts of up to five colours, at random widths ---- */

static uint32_t seed = 269;

static uint32_t rnd(uint32_t n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) % n;
}

static void random_rows(const char *name, uint8_t font, bool vision) {
    static const char *const sign[] = {"A", "E", "I", "M", "O", "T", "W", "Q", "J", "0", "5", "9", ",", ".", "-",
                                       "/", "?", "=", " ", " ", " ", "\xc3\x85", "\xc3\x84", "\xc3\x96"};
    static const uint32_t    color[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff};
    static const lv_font_t  *fonts[] = {&sony_18, &sony_24, &sony_44};
    static char              text[5][256], buf[4096];
    lv_obj_t                *l = lv_label_create(lv_scr_act());
    int                      bad = 0, first = -1, tall = 0;

    fonts_apply(font);
    if (vision) {
        fonts_scale(VISION_FONT_STEPS);
    }
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    watch(l);
    for (int k = 0; k < 150; k++) {
        const lv_font_t *f = fonts[rnd(3)];
        recolor_run_t    run[5];
        int              n = 1 + rnd(5), rows, laid, top;
        lv_coord_t       w = 60 + rnd(700);

        want.n = 0;
        for (int r = 0; r < n; r++) {
            int len = rnd(70);

            text[r][0] = 0;
            for (int i = 0; i < len; i++) {
                strcat(text[r], sign[rnd(sizeof(sign) / sizeof(sign[0]))]);
            }
            run[r] = (recolor_run_t){text[r], strlen(text[r]), color[r]};
            text_add(&want, text[r], strlen(text[r]), color[r]);
        }
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_width(l, w);
        recolor_rows(buf, sizeof(buf), run, n, f, 0, w);
        lv_label_set_text(l, buf);
        lv_obj_update_layout(l);
        if (lv_obj_get_height(l) > H) {
            tall++;
            continue;
        }
        draw(l);
        top = drawn_is(&want);
        if (top != 0 || !rows_kept(l, &rows, &laid)) {
            bad++;
            if (first < 0) {
                first = k;
                printf("        first wrong: %d px, \"%s\"\n", (int)w, buf);
            }
        }
    }
    check(!bad, "%s: 150 random texts of up to five colours, 60-760 px: every letter in its colour, the rows kept "
          "(%d wrong, %d too tall to draw)", name, bad, tall);
    lv_obj_del(l);
}

int main(void) {
    display();

    as_it_was();

    kbd_cases("the stock font", FONT_SONY, false);
    kbd_cases("Roboto", FONT_ROBOTO, false);
    kbd_cases("DejaVu (the default)", FONT_DEJAVU, false);
    kbd_cases("Classic's", FONT_ARIAL, false);
    kbd_cases("the deck themes' Roboto Condensed", FONT_RCOND, false);
    kbd_cases("1bit's Terminus", FONT_PIXEL, false);
    kbd_cases("Codec's Share Tech Mono", FONT_CODEC, false);
    kbd_cases("DejaVu, VISION's sizes", FONT_DEJAVU, true);
    kbd_cases("the stock font, VISION's sizes", FONT_SONY, true);

    random_rows("the stock font", FONT_SONY, false);
    random_rows("Roboto", FONT_ROBOTO, false);
    random_rows("DejaVu (the default)", FONT_DEJAVU, false);
    random_rows("Classic's", FONT_ARIAL, false);
    random_rows("the deck themes' Roboto Condensed", FONT_RCOND, false);
    random_rows("1bit's Terminus", FONT_PIXEL, false);
    random_rows("Codec's Share Tech Mono", FONT_CODEC, false);
    random_rows("DejaVu, VISION's sizes", FONT_DEJAVU, true);
    random_rows("the stock font, VISION's sizes", FONT_SONY, true);

    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}
