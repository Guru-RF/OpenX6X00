/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */
#include <cmath>
#include <cstring>
#include <strings.h>

#include "text_panel.h"
#include "knob_info.h"
#include "util.h"
#include "cfg/settings_manager.h"

extern "C" {
    #include "rtty.h"
    #include "gui_styles.h"
    #include "theme.h"
    #include "scheduler.h"
    #include "radio.h"
    #include "params/params.h"
    #include "cw_parse.h"
    #include "cw_tune_view.h"
}

/* The decoder text over the waterfall (CW decoder, RTTY, NavTex): a label of the last lines,
 * an info line above it (CW WPM), the RTTY scope in the corner */
static lv_obj_t    *obj;
static bool         vision_cw;      /* VISION: CW text shown, its top line holds the speed and the tuning bars */
static lv_obj_t    *info_line;
static lv_anim_t    dim_anim;       /* tuning dims the text */
static char         buf[1024];      /* the text as decoded, '\n' between the lines */
static char        *buf_write = buf;   /* its end, where the next text goes */
static char         shown[6144];    /* buf with the words coloured (label recolour), up to 9 more chars a word */
static uint8_t      max_line_count;
static radio_mode_t prev_mode;      /* CW-R kept as CW: switching between them keeps the text */

static void mode_cb(Subject *subj, void *user_data);
static void dim_on_tuning(Subject *subj, void *user_data);
static void fade_to(void *var, int32_t opa);

/* RTTY tuning scope, bottom right of the panel: mark filter on X, space on Y. Tuned, two
 * thin ellipses cross at right angles; off tune they open and lean, the tones' offset on top */
#define SCOPE_SZ 128

static lv_obj_t   *scope_box;
static lv_obj_t   *scope_cv;
static lv_obj_t   *scope_off;
static lv_color_t  scope_buf[SCOPE_SZ * SCOPE_SZ];

static void scope_tick(lv_timer_t *t) {
    static float x[RTTY_SCOPE_LEN], y[RTTY_SCOPE_LEN];
    float        off;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_flag(scope_box, LV_OBJ_FLAG_HIDDEN))
        return;

    bool tuned = rtty_scope_get(x, y, &off);

    /* Phosphor: the old trace fades out in a few frames */
    for (uint32_t i = 0; i < SCOPE_SZ * SCOPE_SZ; i++) {
        scope_buf[i].ch.red   = scope_buf[i].ch.red * 5 / 8;
        scope_buf[i].ch.green = scope_buf[i].ch.green * 5 / 8;
        scope_buf[i].ch.blue  = scope_buf[i].ch.blue * 5 / 8;
    }

    lv_color_t grid = lv_color_hex(0x2a4038);
    for (int i = 4; i < SCOPE_SZ - 4; i += 3) {
        scope_buf[SCOPE_SZ / 2 * SCOPE_SZ + i] = grid;
        scope_buf[i * SCOPE_SZ + SCOPE_SZ / 2] = grid;
    }

    float peak = 1e-9f;
    for (int i = 0; i < RTTY_SCOPE_LEN; i++)
        peak = fmaxf(peak, fmaxf(fabsf(x[i]), fabsf(y[i])));

    float      k = (SCOPE_SZ / 2 - 3) / peak;
    lv_color_t trace = lv_color_hex(0x70ff90);
    for (int i = 0; i < RTTY_SCOPE_LEN; i++) {
        int px = SCOPE_SZ / 2 + (int)lroundf(x[i] * k);
        int py = SCOPE_SZ / 2 - (int)lroundf(y[i] * k);

        if (px >= 0 && px < SCOPE_SZ && py >= 0 && py < SCOPE_SZ)
            scope_buf[py * SCOPE_SZ + px] = trace;
    }
    lv_obj_invalidate(scope_cv);

    if (!tuned) {
        lv_label_set_text(scope_off, "");
    } else if (fabsf(off) < 10.0f) {
        lv_label_set_text(scope_off, "OK");
        lv_obj_set_style_text_color(scope_off, TC(ok), 0);
    } else {
        lv_label_set_text_fmt(scope_off, "%+d Hz", (int)lroundf(off));
        lv_obj_set_style_text_color(scope_off, TC(accent), 0);
    }
}

static lv_obj_t * scope_label(const char *text, lv_align_t align, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *l = lv_label_create(scope_box);

    lv_obj_set_style_text_font(l, &sony_16, 0);
    lv_obj_set_style_text_color(l, TC(text_head), 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, align, x, y);
    return l;
}

static void scope_create() {
    scope_box = lv_obj_create(obj);
    lv_obj_remove_style_all(scope_box);
    lv_obj_set_size(scope_box, SCOPE_SZ + 4, SCOPE_SZ + 4);
    lv_obj_set_style_bg_color(scope_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scope_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scope_box, TC(panel_border), 0);
    lv_obj_set_style_border_width(scope_box, 1, 0);
    lv_obj_set_style_radius(scope_box, theme_shape.radius_popup, 0);
    lv_obj_set_style_clip_corner(scope_box, true, 0);
    lv_obj_clear_flag(scope_box, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(scope_box, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(scope_box, LV_OBJ_FLAG_HIDDEN);

    for (uint32_t i = 0; i < SCOPE_SZ * SCOPE_SZ; i++)
        scope_buf[i] = lv_color_black();
    scope_cv = lv_canvas_create(scope_box);
    lv_canvas_set_buffer(scope_cv, scope_buf, SCOPE_SZ, SCOPE_SZ, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(scope_cv);

    scope_label("M", LV_ALIGN_RIGHT_MID, -4, -10);
    scope_label("S", LV_ALIGN_TOP_MID, 9, 1);
    scope_off = scope_label("", LV_ALIGN_BOTTOM_MID, 0, -1);

    static lv_timer_t *tick;
    if (!tick) tick = lv_timer_create(scope_tick, 100, NULL);   /* not again on a theme change */
}

/* As many lines as the panel holds in its font (from upstream panel.cpp) */
static void update_line_count() {
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    lv_coord_t       h = lv_obj_get_style_height(obj, 0) - lv_obj_get_style_pad_top(obj, 0)
        - lv_obj_get_style_pad_bottom(obj, 0);

    max_line_count = LV_MAX(1, h / font->line_height);
}

/* One line more than the panel holds: the first goes (as upstream truncate()). An empty last
 * line (the text ended with '\n') is not counted, the next text goes there */
static void drop_first_line() {
    const char *second = strchr(buf, '\n');
    size_t      len = strlen(buf), lines = 1;

    if (!len) return;
    for (const char *p = second; p; p = strchr(p + 1, '\n')) lines++;
    if (buf[len - 1] == '\n') lines--;
    if (lines > max_line_count) {
        memmove(buf, second + 1, buf + len - second);
        buf_write = buf + strlen(buf);
    }
}

#define MAX_WORD_K 65   /* a word for cw_parse.c, with its NUL */

/* CW with Parse on: words split and coloured (cw_parse.c) */
static bool parse_on() {
    radio_mode_t m = (radio_mode_t)cfg_sm.cp_cur_mode.get();

    return cfg_sm.p_cw_parse.get() && (m == radio_mode_cw || m == radio_mode_cwr);
}

/* A word that looks like a callsign (cw_parse.c), not our own call */
static bool is_call(const char *w, size_t len) {
    return cw_parse_is_call(w, len)
        && !(strlen(params.callsign.x) == len && !strncasecmp(w, params.callsign.x, len));
}

/* Noise decodes into call-shaped words too (TT1SAR, YP4F): a call counts only next to DE or when it
 * comes at least twice, as a station sends it ("CQ CQ DE IK1ABC IK1ABC K") */
static bool sent_call(const char *w, size_t len) {
    return is_call(w, len) && cw_parse_confirmed(buf, w, len);
}

/* buf into the label, the calls coloured; with Parse also abbreviations (dim) and RST. A word still
 * being decoded stays plain */
static void show() {
    char   call[8], abbr[8], rst[8];
    char  *o = shown, *end = shown + sizeof(shown) - 16;
    size_t n;
    bool   parse = parse_on();

    snprintf(call, sizeof(call), "%06X", (unsigned)(theme_hex(parse ? TC_accent : TC_text_head) & 0xFFFFFF));
    snprintf(abbr, sizeof(abbr), "%06X", (unsigned)(theme_hex(TC_text_dim) & 0xFFFFFF));
    snprintf(rst, sizeof(rst), "%06X", (unsigned)(theme_hex(TC_text_head) & 0xFFFFFF));
    for (const char *p = buf; *p && o < end; p += n) {
        n = strcspn(p, " \n");
        if (n == 0) {           /* a separator */
            *o++ = *p;
            n = 1;
            continue;
        }
        const char *color = NULL;
        if (p[n] && sent_call(p, n)) {
            color = call;
        } else if (p[n] && parse) {
            cwp_class_t c = cw_parse_class(p, n);
            color = c == CWP_ABBR ? abbr : c == CWP_RST ? rst : NULL;
        }
        if (color && o + n + 10 >= end) color = NULL;
        if (color) o += sprintf(o, "#%s ", color);
        for (size_t i = 0; i < n && o < end; i++)
            if (p[i] != '#') *o++ = p[i];       /* '#' is the recolour mark */
        if (color) *o++ = '#';
    }
    *o = '\0';
    lv_label_set_text_static(obj, shown);
}

/* CW with Parse: the words this text finished, split in place where they ran together
 * ("DEON6UREK" -> "DE ON6URE K"); only the tail, cw_parse.c only ever adds spaces */
static void split_words(char *from) {
    char   raw[MAX_WORD_K], out[MAX_WORD_K * 2 + 1];
    char  *p = from;
    size_t n, m;

    while (p > buf && !strchr(" \n", p[-1])) p--;      /* the word the text went on with */
    while (*p) {
        n = strcspn(p, " \n");
        if (!p[n]) break;                                /* still being decoded */
        if (n == 0) {
            p++;
            continue;
        }
        if (n < MAX_WORD_K) {
            size_t rn = n;

            memcpy(raw, p, n);
            m = cw_parse_split(raw, n, buf, out, sizeof(out));
            if (m > n && (size_t)(buf_write - buf) + m - n < sizeof(buf) - 1) {
                memmove(p + m, p + n, buf_write - (p + n) + 1);
                memcpy(p, out, m);
                buf_write += m - n;
                n = m;
            }
        }
        p += n + 1;
    }
}

const char *panel_last_call(void) {
    static char call[16];
    const char *p = buf;
    size_t      n;

    call[0] = '\0';
    for (; *p; p += n ? n : 1) {
        n = strcspn(p, " \n");
        if (n && p[n] && sent_call(p, n)) snprintf(call, sizeof(call), "%.*s", (int)n, p);
    }
    return call;
}

/* Settings > Display > Decoder text: Large (the stock sony_24) or Compact (sony_18, more lines).
 * Follows the setting live; ported from SP2KMO's upstream c2fbe40c, with his fix 0736fa9c: every
 * line that no longer fits goes, not one */
static void text_size_cb(Subject *subj, void *user_data) {
    size_t len;

    lv_obj_set_style_text_font(obj, cfg_sm.p_digi_text_compact.get() ? &sony_18 : &sony_24, 0);
    update_line_count();
    do {
        len = strlen(buf);
        drop_first_line();
    } while (strlen(buf) != len);
    show();
}

/* The text wider than the panel (its widest line) */
static bool too_wide() {
    lv_point_t size;

    lv_txt_get_size(&size, buf, lv_obj_get_style_text_font(obj, 0), 0, 0, LV_COORD_MAX, 0);
    return size.x > lv_obj_get_width(obj) - 20;
}

/* New decoded text at the end of buf. No bound on its length: the decoders send a few
 * characters at a time and the lines are kept to the panel's */
static void add_text(void *arg) {
    const char *text = (const char *)arg;
    char       *at = buf_write;

    buf_write = stpcpy(at, text);
    if (buf_write - buf >= 2 && !strcmp(buf_write - 2, "\n\n")) {
        buf_write--;            /* one empty line at most: the next text goes over the second '\n' */
    } else if (buf_write - buf >= 2 && too_wide()) {
        char *w = at;           /* the line breaks before the new text */

        while (w > buf && !strchr(" \n", w[-1])) w--;
        if (parse_on() && w > buf && w[-1] == ' ') {
            w[-1] = '\n';       /* the whole word to the next line: it stays one word */
        } else {
            *at = '\n';
            buf_write = stpcpy(at + 1, text);
        }
    }
    if (parse_on()) {
        split_words(at);
        /* split, the line may have got too long: it breaks at its last space */
        if (too_wide()) {
            char *sp = buf_write;

            while (sp > buf && sp[-1] != '\n' && (sp == buf_write || sp[-1] != ' ')) sp--;
            if (sp > buf && sp[-1] == ' ') sp[-1] = '\n';
        }
    }
    drop_first_line();
    show();
}

static void set_info(void *arg) {
    lv_label_set_text(info_line, (const char *)arg);
}

/* The info (CW WPM) hangs above the panel, over the waterfall: LVGL redraws the panel only
 * where its area is dirty, so each waterfall refresh drew over the label and hid it.
 * The extra draw size takes that strip in */
static void info_ext_draw_cb(lv_event_t *e) {
    lv_coord_t *s = (lv_coord_t *)lv_event_get_param(e);

    *s = LV_MAX(*s, -lv_obj_get_style_y(info_line, 0) + lv_font_get_line_height(lv_obj_get_style_text_font(info_line, 0)));
}

lv_obj_t *panel_init(lv_obj_t *parent) {
    /* Hidden until a decoder is on; the info line hangs out of it */
    obj = lv_label_create(parent);
    lv_obj_add_style(obj, &panel_style, 0);
    lv_obj_add_flag(obj, (lv_obj_flag_t)(LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    lv_label_set_recolor(obj, true);    /* the coloured words of show() */
    lv_label_set_text_static(obj, buf);
    scope_create();

    info_line = lv_label_create(obj);
    lv_obj_add_style(info_line, &panel_info_style, 0);
    lv_label_set_text(info_line, "");
    lv_obj_add_event_cb(obj, info_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);

    lv_anim_init(&dim_anim);
    lv_anim_set_var(&dim_anim, obj);
    lv_anim_set_exec_cb(&dim_anim, fade_to);
    lv_anim_set_time(&dim_anim, 200);

    /* Shown by the mode and the CW decoder switch; the font follows Settings */
    prev_mode = (radio_mode_t)cfg_sm.cp_cur_mode.get();
    cfg_sm.cp_cur_mode.subscribe_delayed(mode_cb, NULL);
    cfg_sm.p_cw_decoder.subscribe_delayed_and_notify(mode_cb, NULL);
    cfg_sm.cp_fg_freq.subscribe_delayed(dim_on_tuning, NULL);
    cfg_sm.p_digi_text_compact.subscribe_delayed_and_notify(text_size_cb, NULL);

    lv_obj_refresh_ext_draw_size(obj);
    return obj;
}

/* A taller spectrum: the panel starts under it, shorter (upstream panel_set_height()) */
void panel_keep_below(lv_coord_t y) {
    /* From the style's place: the spectrum may have got shorter (live) */
    lv_obj_remove_local_style_prop(obj, LV_STYLE_Y, 0);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_HEIGHT, 0);
    text_size_cb(NULL, NULL);

    lv_coord_t bottom = lv_obj_get_style_y(obj, 0) + lv_obj_get_style_height(obj, 0);

    /* VISION: always there, also higher (no spectrum): the band plan and the cluster column
     * no longer show in a strip between the scale and the panel */
    if (y > lv_obj_get_style_y(obj, 0) || theme_shape.vision) {
        lv_obj_set_y(obj, y);
        lv_obj_set_height(obj, bottom - y);
        text_size_cb(NULL, NULL);
    }
    if (vision_cw) {
        cw_tune_set_y(lv_obj_get_style_y(obj, 0) + 2);
    }
}

/* From the decoders' threads: copied, added in the LVGL one */
void panel_add_text(const char *text) {
    scheduler_put(add_text, (void *)text, strlen(text) + 1);
}

void panel_set_info(const char *text) {
    scheduler_put(set_info, (void *)text, strlen(text) + 1);
}

void panel_hide(void) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    knobs_display(true);    /* the knob labels are where the panel was */
}

/* A hidden panel keeps its text */
void panel_clear(void) {
    cw_parse_forget();      /* the calls the app confirmed: they go with the text */
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    buf_write = buf;
    *buf = '\0';
    lv_label_set_text_static(obj, buf);
}

/* CW with the decoder on, or SSB with RTTY (and NavTex) running */
void panel_update_visibility(bool clear) {
    radio_mode_t mode = (radio_mode_t)cfg_sm.cp_cur_mode.get();
    bool         cw = mode == radio_mode_cw || mode == radio_mode_cwr;
    bool         ssb = (unsigned)mode <= (unsigned)radio_mode_usb_dig;   /* LSB, LSB-D, USB, USB-D */
    bool         on = cw ? cfg_sm.p_cw_decoder.get() : ssb && rtty_get_state() != RTTY_OFF;

    /* RTTY: the scope, and the text keeps clear of it */
    bool rtty = on && !cw;
    if (rtty) {
        lv_obj_clear_flag(scope_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_right(obj, SCOPE_SZ + 12, 0);
        lv_obj_align(scope_box, LV_ALIGN_BOTTOM_RIGHT, SCOPE_SZ + 6, 0);    /* past the padding, to the edge */
    } else {
        lv_obj_add_flag(scope_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_local_style_prop(obj, LV_STYLE_PAD_RIGHT, 0);
    }

    /* VISION: the CW speed on a line of its own at the top (styles.c), the text and the CW
     * tuning bars (at its left) under it */
    bool was_cw = vision_cw;

    vision_cw = theme_shape.vision && on && !rtty;
    if (theme_shape.vision) {
        lv_obj_set_style_pad_top(obj, rtty ? 10 : 10 + lv_font_get_line_height(&sony_24), 0);   /* its font in styles.c */
        text_size_cb(NULL, NULL);
        cw_tune_set_y(vision_cw ? lv_obj_get_style_y(obj, 0) + 2 : -1);
    } else if (was_cw) {
        cw_tune_set_y(-1);      /* VISION left for another theme */
    }

    if (on && lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        knobs_display(false);
    } else if (!on) {
        panel_hide();
    }
    if (clear) panel_clear();
}

static radio_mode_t cw_as_one(radio_mode_t m) {
    return m == radio_mode_cwr ? radio_mode_cw : m;
}

/* A new mode clears the text, CW to CW-R and back does not */
static void mode_cb(Subject *subj, void *user_data) {
    radio_mode_t mode = cw_as_one((radio_mode_t)cfg_sm.cp_cur_mode.get());
    bool         clear = mode != cw_as_one(prev_mode);

    prev_mode = mode;
    panel_update_visibility(clear);
}

/* The parts' opacities, not the object's: an object opa below COVER makes LVGL
 * draw the whole box in a layer and blend it, every frame of this fade, which
 * runs at each VFO step (tuning stuttered in CW with the decoder on) */
static void fade_to(void *var, int32_t opa) {
    lv_obj_t *o = (lv_obj_t *)var;

    lv_obj_set_style_text_opa(o, opa, 0);
    lv_obj_set_style_bg_img_opa(o, opa, 0);
    lv_obj_set_style_border_opa(o, opa, 0);
    if (!theme_shape.images) {
        lv_obj_set_style_bg_opa(o, opa, 0);   /* a drawn box; with images there is no bg colour */
    }
}

static void fade(lv_opa_t from, lv_opa_t to, uint32_t delay, lv_anim_ready_cb_t then) {
    lv_anim_set_values(&dim_anim, from, to);
    lv_anim_set_delay(&dim_anim, delay);
    lv_anim_set_ready_cb(&dim_anim, then);
    lv_anim_start(&dim_anim);
}

static void fade_back(lv_anim_t *a) {
    fade(LV_OPA_40, LV_OPA_COVER, 200, NULL);
}

/* Tuning: the text fades to 40% (it was decoded on another frequency), and back once the
 * knob rests; each step starts the fade again from where it is */
static void dim_on_tuning(Subject *subj, void *user_data) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_anim_del(obj, fade_to);
    fade(lv_obj_get_style_text_opa(obj, 0), LV_OPA_40, 0, fade_back);
}
