// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - the FT8/FT4/FT2 message list
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#include "decode_list.h"
#include "../remote_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cfg/cfg_api.h"
#include "../events.h"
#include "../flags.h"
#include "../i18n.h"
#include "../styles.h"
#include "../theme.h"
#include "../voice.h"
#include "../voice_focus.h"

/* The list keeps up to KEEP_ROWS rows; one more and the oldest DROP_ROWS go together, so the
 * whole table is copied up once in a while and not on every decode */
#define KEEP_ROWS   512
#define DROP_ROWS   64
#define EMPTY_TEXT  "Wait sync"

/* What each row of the list shows, row for row (LVGL's cell user data is freed by LVGL itself:
 * not usable for data that lives here). n == 0: only the "Wait sync" row. Room for KEEP_ROWS, then a
 * slot line and its row before the next push drops the oldest */
static struct {
    cell_data_t cd[KEEP_ROWS + 2];
    uint16_t    n;
} list_rows;

/* The time of the last slot line, "" none yet */
static char sep_time[9];

static lv_obj_t             *list;
static table_view_press_cb_t press_cb;
static table_view_actions_t  actions;

/*
 *  "For you": the last messages to this station, one row a caller, pinned in a small table
 *  under the list whatever the Show filter, so they don't scroll away. A caller leaves when we
 *  send it 73 / RR73 or after PIN_TTL_MS without news. The MFK goes down into it from the last
 *  row of the list (pin.sel >= 0), a press answers that caller as a press on the list does.
 */
#define PIN_MAX     3
#define PIN_TTL_MS  120000

static struct {
    lv_obj_t   *obj;
    lv_timer_t *timer;
    cell_data_t cd[PIN_MAX];
    uint32_t    at[PIN_MAX];    /* lv_tick of the caller's last message */
    int         n;
    int         sel;            /* the MFK row in the pin table, -1: in the list */
    uint16_t    list_row;       /* the list's row while the MFK is down here */
    uint16_t    prev_row;       /* the list's selected row after the last key */
    bool        own_key;        /* the key comes from the autoscroll, not the MFK */
} pin = {.sel = -1};

static lv_coord_t list_y, list_h;   /* the list's place with the pin table hidden */

/* A touch on the list: LVGL's table selects the touched row while the finger is down, and no
 * row once it is lifted (lv_table.c, pointer input) */
static struct {
    uint16_t row;       /* the MFK row before the touch, LV_TABLE_CELL_NONE: none */
    bool     last;      /* that was the last row: the list followed the new ones */
    uint16_t finger;    /* the row under the finger as it was lifted */
} touch;

/* The data of row r of the list or of the pin table, NULL for the "Wait sync" row */
static cell_data_t *row_data(lv_obj_t *obj, uint32_t r) {
    if (obj == pin.obj) return r < (uint32_t)pin.n ? &pin.cd[r] : NULL;
    return r < list_rows.n ? &list_rows.cd[r] : NULL;
}

static bool is_slot_row(const cell_data_t *cd) {
    return cd->cell_type == CELL_RX_INFO;
}

/* A decode or our TX: its text starts with its slot's time, "12:00:15 CQ ..." */
static bool has_time(const cell_data_t *cd) {
    return (cd->cell_type == CELL_RX_MSG || cd->cell_type == CELL_RX_CQ || cd->cell_type == CELL_RX_TO_ME ||
            cd->cell_type == CELL_TX_MSG) &&
           strlen(cd->text) > 9 && cd->text[2] == ':' && cd->text[5] == ':' && cd->text[8] == ' ';
}

/* What the list shows of a row: a decode and our TX without the time, which their slot line has */
static const char *list_text(const cell_data_t *cd) {
    return has_time(cd) ? cd->text + 9 : cd->text;
}

/* ---- speech: a row as said, the messages for this station at once, a count after each slot ---- */

static void row_said(const cell_data_t *cd, char *out, size_t size) {
    const char *t = cd->text;
    char        spelled[160];

    /* the list's own line over a slot's rows: just its time */
    if (cd->cell_type == CELL_SLOT_SEP) {
        snprintf(out, size, "Slot %s", t);
        return;
    }
    /* "12:00:15 CQ K1ABC FN42": the slot's time, which its slot line says, not worth saying again */
    if (cd->cell_type != CELL_RX_INFO && cd->cell_type != CELL_START_QSO) {
        t = list_text(cd);
        voice_spell_calls(t, spelled, sizeof(spelled));
    } else {
        snprintf(spelled, sizeof(spelled), "%s", t);
    }
    snprintf(out, size, "%s%s%s%s", cd->cell_type == CELL_TX_MSG ? "Sent, " : "", spelled,
             cd->country[0] ? ", " : "", cd->country);
    if (cd->cell_type == CELL_RX_CQ) {
        size_t n = strlen(out);

        if (cd->worked_type == SEARCH_WORKED_SAME_MODE) {
            snprintf(out + n, size - n, ", worked");
        } else if (cd->worked_type == SEARCH_WORKED_YES) {
            snprintf(out + n, size - n, ", worked on another band or mode");
        } else if (cd->worked_type == SEARCH_WORKED_NO) {
            snprintf(out + n, size - n, ", new");
        }
    }
}

static int heard_cq, heard_other;       /* since the last slot row */

static void speech_on_row(const cell_data_t *cd) {
    char said[220];

    if (!voice_auto_active()) {
        heard_cq = heard_other = 0;
        return;
    }
    switch (cd->cell_type) {
        case CELL_RX_TO_ME:
            row_said(cd, said, sizeof(said));
            voice_say_text_fmt("For you: %s", said);
            heard_other++;
            break;
        case CELL_RX_CQ:
            heard_cq++;
            break;
        case CELL_RX_MSG:
            heard_other++;
            break;
        case CELL_RX_INFO:
            if (heard_cq || heard_other) {
                if (heard_cq && heard_other) {
                    voice_delay_say_text_fmt("%i C Q, %i other", heard_cq, heard_other);
                } else if (heard_cq) {
                    voice_delay_say_text_fmt("%i C Q", heard_cq);
                } else {
                    voice_delay_say_text_fmt("%i decoded", heard_other);
                }
            }
            heard_cq = heard_other = 0;
            break;
        default:
            break;
    }
}

/* ---- the pin table ---- */

/* Shown under the list with its rows, the list shorter by its height */
static void pin_layout(void) {
    if (!pin.obj || !list) return;
    if (!pin.n) {
        lv_obj_add_flag(pin.obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(list, list_h);
        pin.sel = -1;
        return;
    }
    lv_table_set_row_cnt(pin.obj, pin.n);
    for (int i = 0; i < pin.n; i++) lv_table_set_cell_value(pin.obj, i, 0, pin.cd[i].text);
    lv_obj_clear_flag(pin.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(pin.obj);

    lv_coord_t h = lv_obj_get_height(pin.obj);

    lv_obj_set_y(pin.obj, list_y + list_h - h);
    lv_obj_set_height(list, list_h - h - 4);
    if (pin.sel >= pin.n) pin.sel = pin.n - 1;
    lv_obj_invalidate(pin.obj);
}

static void pin_remove(int i) {
    pin.n--;
    memmove(&pin.cd[i], &pin.cd[i + 1], (size_t)(pin.n - i) * sizeof(pin.cd[0]));
    memmove(&pin.at[i], &pin.at[i + 1], (size_t)(pin.n - i) * sizeof(pin.at[0]));
}

static bool ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && !strcmp(s + n - k, tail);
}

/* The stations we sent 73 or RR73 to in the last PIN_TTL_MS: their own last word (a 73
 * or an RR73 that comes after ours) closes the QSO, it is not a new call to pin */
#define CLOSED_MAX  8

static struct {
    char     call[16];
    uint32_t at;
} closed[CLOSED_MAX];
static int closed_next;

static void closed_clear(void) {
    memset(closed, 0, sizeof(closed));
}

static bool closed_with(const char *call) {
    for (int i = 0; i < CLOSED_MAX; i++) {
        if (closed[i].call[0] && !strcmp(closed[i].call, call) && lv_tick_elaps(closed[i].at) <= PIN_TTL_MS) {
            return true;
        }
    }
    return false;
}

/* Our message "[time] DX OURCALL RR73": DX, the first word that is not the time; "<DX> OURCALL RR73" (a
 * non-standard call of ours) too */
static void closed_add(const char *text) {
    char call[16];

    for (const char *w = text; *w; ) {
        size_t n = strcspn(w, " ");
        if (n && n < sizeof(call) && !memchr(w, ':', n)) {
            if (n > 2 && w[0] == '<' && w[n - 1] == '>') {
                w++;
                n -= 2;
            }
            memcpy(call, w, n);
            call[n] = '\0';
            if (strcmp(call, "CQ")) {
                snprintf(closed[closed_next].call, sizeof(closed[0].call), "%s", call);
                closed[closed_next].at = lv_tick_get();
                closed_next = (closed_next + 1) % CLOSED_MAX;
            }
            return;
        }
        w += n;
        w += strspn(w, " ");
    }
}

/* A message to us: its caller's row, newest at the bottom. A final 73 to us, or an RR73 from
 * a station we have closed with, ends the QSO: no row (and an old one goes) */
static void pin_message(const cell_data_t *cd) {
    bool final;

    if (!cd->meta.call_de[0] || !param_i_get(cfg_ft8_foryou)) return;
    final = ends_with(cd->text, " 73") || (ends_with(cd->text, "RR73") && closed_with(cd->meta.call_de));
    for (int i = 0; i < pin.n; i++) {
        if (!strcmp(pin.cd[i].meta.call_de, cd->meta.call_de)) {
            pin_remove(i);
            if (final) pin_layout();
            break;
        }
    }
    if (final) return;
    if (pin.n == PIN_MAX) pin_remove(0);
    pin.cd[pin.n] = *cd;
    pin.at[pin.n] = lv_tick_get();
    pin.n++;
    pin_layout();
}

/* We sent 73 or RR73: the QSO with the callers named in it is over */
static void pin_sent(const char *text) {
    bool gone = false;

    if (!ends_with(text, " 73") && !ends_with(text, "RR73")) return;
    closed_add(text);
    for (int i = pin.n - 1; i >= 0; i--) {
        char word[16], hashed[18];
        snprintf(word, sizeof(word), " %s ", pin.cd[i].meta.call_de);
        snprintf(hashed, sizeof(hashed), " <%s> ", pin.cd[i].meta.call_de);
        if (strstr(text, word) || strstr(text, hashed)) {
            pin_remove(i);
            gone = true;
        }
    }
    if (gone) pin_layout();
}

static void pin_timer_cb(lv_timer_t *t) {
    bool gone = false;

    (void)t;
    for (int i = pin.n - 1; i >= 0; i--) {
        if (lv_tick_elaps(pin.at[i]) > PIN_TTL_MS) {
            pin_remove(i);
            gone = true;
        }
    }
    if (gone) pin_layout();
}

/* ---- the list ---- */

/* Over KEEP_ROWS: the oldest DROP_ROWS rows go, the view and the MFK row stay on the same messages */
static void drop_oldest(void) {
    lv_table_t *t = (lv_table_t *)list;
    lv_coord_t  gone_h = 0;

    if (list_rows.n <= KEEP_ROWS) return;
    for (int i = 0; i < DROP_ROWS; i++) gone_h += t->row_h[i];
    if (t->row_act != LV_TABLE_CELL_NONE) t->row_act = t->row_act > DROP_ROWS ? t->row_act - DROP_ROWS : 0;
    if (touch.row != LV_TABLE_CELL_NONE) touch.row = touch.row > DROP_ROWS ? touch.row - DROP_ROWS : 0;

    list_rows.n -= DROP_ROWS;
    memmove(&list_rows.cd[0], &list_rows.cd[DROP_ROWS], list_rows.n * sizeof(list_rows.cd[0]));
    for (uint16_t r = 0; r < list_rows.n; r++) lv_table_set_cell_value(list, r, 0, list_text(&list_rows.cd[r]));
    lv_table_set_row_cnt(list, list_rows.n);
    lv_obj_scroll_by_bounded(list, 0, gone_h, LV_ANIM_OFF);
}

void table_view_push_ui(const cell_data_t *cd) {
    uint16_t sel_row = 0, sel_col = 0;

    if (!list || !cd) return;
    drop_oldest();

    /* On the last row the list follows the new ones, scrolled back it stays there */
    lv_table_get_selected_cell(list, &sel_row, &sel_col);

    bool      follow = list_rows.n == sel_row + 1;
    uint16_t  r      = list_rows.n;
    uint16_t  was_n  = list_rows.n;
    cell_data_t *last = r ? &list_rows.cd[r - 1] : NULL;

    if (last && is_slot_row(last)) {
        if (is_slot_row(cd)) {      /* the slot row under the last messages just gets the new time */
            memcpy(last->text, cd->text, sizeof(last->text) - 1);
            last->text[sizeof(last->text) - 1] = '\0';
            lv_table_set_cell_value(list, r - 1, 0, last->text);
            return;
        }
        r--;    /* a message takes its place: the slot row only shows that receiving goes on */
    }

    speech_on_row(cd);

    /* A slot's rows under a line with its time (F4OLI's redesign): the rows go without theirs. The times are the
     * slots' starts (ft8_window.c), the same for each row of a slot */
    if (has_time(cd) && strncmp(cd->text, sep_time, 8)) {
        cell_data_t *sep = &list_rows.cd[r];

        memset(sep, 0, sizeof(*sep));
        sep->cell_type = CELL_SLOT_SEP;
        sep->flag      = -1;
        snprintf(sep->text, sizeof(sep->text), "%.8s", cd->text);
        snprintf(sep_time, sizeof(sep_time), "%.8s", cd->text);
        lv_table_set_cell_value(list, r, 0, sep->text);
        r++;
    }

    list_rows.cd[r] = *cd;
    list_rows.cd[r].text[sizeof(cd->text) - 1] = '\0';
    list_rows.n = r + 1;
    lv_table_set_cell_value(list, r, 0, list_text(&list_rows.cd[r]));

    if (cd->cell_type == CELL_RX_TO_ME) pin_message(cd);
    else if (cd->cell_type == CELL_TX_MSG) pin_sent(cd->text);

    if (follow) {       /* down to the new last row: two with a slot line, one when a row took the slot row's place */
        static uint32_t down = LV_KEY_DOWN;
        int             added = (int)list_rows.n - (int)was_n;

        pin.own_key = true;
        for (int k = 0; k < (added > 1 ? added : 1); k++) lv_event_send(list, LV_EVENT_KEY, &down);
        pin.own_key = false;
    }
}

void table_view_add_msg_cb(void *data) {
    table_view_push_ui(data);
    remote_server_ft8_row(data);    /* the browser panel's list */
}

void table_view_reset(void) {
    if (!list) return;
    list_rows.n = 0;
    sep_time[0] = '\0';
    lv_table_set_row_cnt(list, 1);
    lv_table_set_cell_value(list, 0, 0, EMPTY_TEXT);
    pin.n = 0;      /* another band or mode: the callers of the old one are gone */
    closed_clear();
    pin_layout();
}

/* ---- drawing ---- */

/* The row's background from what it holds; the MFK row lighter. After F4OLI's redesign the CQs stand out of the
 * traffic, to answer them fast: plain rows in the text's colour, with a green edge and a tag (draw_extras_cb), the
 * other stations' QSOs grey; to us red, ours blue as before */
static void draw_row_cb(lv_event_t *e) {
    lv_obj_t               *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t     r  = dsc->id / lv_table_get_col_cnt(obj);
    cell_data_t *cd = row_data(obj, r);
    lv_draw_rect_dsc_t  *rect  = dsc->rect_dsc;
    lv_draw_label_dsc_t *label = dsc->label_dsc;

    rect->bg_opa = obj == pin.obj ? LV_OPA_80 : 115;
    if (cd && cd->cell_type == CELL_SLOT_SEP) {     /* its rules: draw_extras_cb */
        label->align   = LV_TEXT_ALIGN_CENTER;
        label->color   = TC(text_mute);
        rect->bg_color = TC(panel);
    } else if (!cd || cd->cell_type == CELL_RX_INFO || cd->cell_type == CELL_START_QSO) {
        label->align  = LV_TEXT_ALIGN_CENTER;
        rect->bg_color = TC(row);
    } else if (cd->cell_type == CELL_RX_CQ) {
        rect->bg_color = TC(panel);
        label->color   = TC(text);
        if (cd->worked_type == SEARCH_WORKED_YES) {     /* on another band or mode */
            label->opa = LV_OPA_80;
        } else if (cd->worked_type == SEARCH_WORKED_SAME_MODE) {
            label->decor = LV_TEXT_DECOR_STRIKETHROUGH;
            label->opa   = LV_OPA_60;
        }
    } else if (cd->cell_type == CELL_RX_MSG) {
        rect->bg_color = TC(panel);
        label->color   = TC(text_mute);
    } else if (cd->cell_type == CELL_RX_TO_ME) {
        rect->bg_color = TC(ft8_to_me);
    } else if (cd->cell_type == CELL_TX_MSG) {
        rect->bg_color = TC(ft8_tx);
    }

    bool     mfk_here;
    uint16_t sel_row, sel_col;

    if (obj == pin.obj) {
        mfk_here = pin.sel == (int)r;
    } else {
        lv_table_get_selected_cell(obj, &sel_row, &sel_col);
        mfk_here = pin.sel < 0 && sel_row == r;
    }
    if (theme_shape.mono) {     /* 1bit: by shape: the MFK's row framed heavy, a new CQ marked, own TX framed */
        rect->border_color = TC(text);
        rect->border_opa = LV_OPA_COVER;
        if (mfk_here) {
            rect->border_side = LV_BORDER_SIDE_FULL;
            rect->border_width = 2;
        } else if (cd && cd->cell_type == CELL_RX_CQ && cd->worked_type == SEARCH_WORKED_NO) {
            rect->border_side = LV_BORDER_SIDE_LEFT;
            rect->border_width = 4;
        } else if (cd && cd->cell_type == CELL_TX_MSG) {
            rect->border_side = LV_BORDER_SIDE_FULL;
            rect->border_width = 1;
        }
    } else if (mfk_here) {
        rect->bg_color = lv_color_lighten(rect->bg_color, 20);
    }
}

static lv_coord_t text_width(const char *text, const lv_draw_label_dsc_t *ld) {
    lv_point_t size;

    lv_txt_get_size(&size, text, ld->font, ld->letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

#define FLAGS_N (sizeof(flag_px) / sizeof(flag_px[0]))

/* Flag k as an LVGL image, its pixels converted on first use */
static const lv_img_dsc_t *flag_img(int k) {
    static lv_img_dsc_t img[FLAGS_N];
    static lv_color_t  *px[FLAGS_N];

    if (k < 0 || k >= (int)FLAGS_N) return NULL;
    if (!px[k]) {
        px[k] = malloc(FLAG_W * FLAG_H * sizeof(lv_color_t));
        if (!px[k]) return NULL;
        for (int i = 0; i < FLAG_W * FLAG_H; i++) {
            const uint8_t *rgb = &flag_px[k][3 * i];
            px[k][i] = lv_color_make(rgb[0], rgb[1], rgb[2]);
        }
        img[k].header.cf = LV_IMG_CF_TRUE_COLOR;
        img[k].header.w  = FLAG_W;
        img[k].header.h  = FLAG_H;
        img[k].data_size = FLAG_W * FLAG_H * sizeof(lv_color_t);
        img[k].data      = (const uint8_t *)px[k];
    }
    return &img[k];
}

/* The sender's flag and country, right-aligned up to `right`: the name when it fits after the
 * message (`text_end`), else its prefix, else the flag only (from SP2KMO's upstream PR #282) */
static void draw_country(lv_obj_draw_part_dsc_t *dsc, const cell_data_t *cd, lv_area_t *area, lv_coord_t right,
                         lv_coord_t text_end) {
    const lv_img_dsc_t *flag = flag_img(cd->flag);

    if (flag && right - FLAG_W >= text_end + 16) {
        lv_draw_img_dsc_t id;
        lv_area_t         fa;
        lv_draw_img_dsc_init(&id);
        fa.x2 = right;
        fa.x1 = right - FLAG_W + 1;
        fa.y1 = (area->y1 + area->y2 - FLAG_H) / 2;
        fa.y2 = fa.y1 + FLAG_H - 1;
        lv_draw_img(dsc->draw_ctx, &id, &fa, flag);
        right -= FLAG_W + 6;
    }
    const char *choice[2] = {cd->country, cd->cty_prefix};
    for (int i = 0; i < 2; i++) {
        if (!choice[i][0]) continue;
        lv_coord_t w = text_width(choice[i], dsc->label_dsc);
        if (right - w < text_end + 16) continue;
        area->x1 = right - w;
        area->x2 = right;
        lv_draw_label(dsc->draw_ctx, dsc->label_dsc, area, choice[i], NULL);
        return;
    }
}

/* A tag after the message (F4OLI's redesign): small, in col, on a box of the theme's chip colour (black boxes
 * hardly read in the light theme); returns where it ends */
static lv_coord_t draw_tag(lv_obj_draw_part_dsc_t *dsc, const lv_area_t *row, lv_coord_t x, const char *text,
                           lv_color_t col) {
    lv_draw_label_dsc_t ld = *dsc->label_dsc;
    lv_draw_rect_dsc_t  box;
    lv_area_t           a;
    lv_coord_t          h = lv_font_get_line_height(&sony_16);

    ld.font  = &sony_16;
    ld.color = col;
    ld.opa   = LV_OPA_COVER;
    ld.decor = LV_TEXT_DECOR_NONE;
    ld.align = LV_TEXT_ALIGN_LEFT;
    lv_draw_rect_dsc_init(&box);
    box.bg_color = TC(chip);
    box.bg_opa   = LV_OPA_COVER;
    box.radius   = 4;
    a.x1 = x + 10;
    a.x2 = a.x1 + text_width(text, &ld) + 11;
    a.y1 = (row->y1 + row->y2 - h) / 2 - 1;
    a.y2 = a.y1 + h + 1;
    lv_draw_rect(dsc->draw_ctx, &box, &a);
    a.x1 += 6;
    a.y1 += 1;
    lv_draw_label(dsc->draw_ctx, &ld, &a, text, NULL);
    return a.x2;
}

/* The slot line: its time in the middle, a rule on each side */
static void draw_sep(lv_obj_draw_part_dsc_t *dsc, const cell_data_t *cd) {
    const lv_area_t   *row = dsc->draw_area;
    lv_coord_t         w   = text_width(cd->text, dsc->label_dsc);
    lv_coord_t         mid = (row->x1 + row->x2) / 2, y = (row->y1 + row->y2) / 2;
    lv_draw_line_dsc_t ln;

    lv_draw_line_dsc_init(&ln);
    ln.color = TC(line);
    ln.opa   = LV_OPA_50;
    ln.width = 1;

    lv_point_t a1 = {row->x1 + 8, y}, a2 = {mid - w / 2 - 12, y};
    lv_point_t b1 = {mid + w / 2 + 12, y}, b2 = {row->x2 - 8, y};

    lv_draw_line(dsc->draw_ctx, &ln, &a1, &a2);
    lv_draw_line(dsc->draw_ctx, &ln, &b1, &b2);
}

/* A POTA tag after the message, in the row's text colour; returns where the tag ends */
static lv_coord_t draw_pota(lv_obj_draw_part_dsc_t *dsc, const lv_area_t *row, lv_coord_t x) {
    lv_area_t          tag = *row;
    lv_draw_rect_dsc_t frame;

    lv_draw_rect_dsc_init(&frame);
    frame.bg_opa       = LV_OPA_TRANSP;
    frame.border_width = 1;
    frame.border_color = dsc->label_dsc->color;
    frame.radius       = 3;
    tag.x1 = x + 10;
    tag.x2 = tag.x1 + text_width("POTA", dsc->label_dsc) + 7;
    lv_draw_rect(dsc->draw_ctx, &frame, &tag);
    tag.x1 += 4;
    tag.x2 -= 3;
    lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &tag, "POTA", NULL);
    return tag.x2 + 3;
}

bool decode_list_miles(void) {
    int32_t info = param_i_get(cfg_ft8_info);

    return info == 1 || info == 4;
}

/* On a received message, right-aligned from the right edge: the SNR, the distance, the country */
static void draw_extras_cb(lv_event_t *e) {
    lv_obj_t               *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t     r  = dsc->id / lv_table_get_col_cnt(obj);
    cell_data_t *cd = row_data(obj, r);

    if (cd && cd->cell_type == CELL_SLOT_SEP) {
        draw_sep(dsc, cd);
        return;
    }
    if (!cd || (cd->cell_type != CELL_RX_MSG && cd->cell_type != CELL_RX_CQ && cd->cell_type != CELL_RX_TO_ME))
        return;
    if (cd->cell_type == CELL_RX_CQ && !theme_shape.mono) {    /* the CQ's green edge (1bit: its left border) */
        lv_draw_rect_dsc_t edge;
        lv_area_t          ea = *dsc->draw_area;

        lv_draw_rect_dsc_init(&edge);
        edge.bg_color = TC(ok);
        edge.bg_opa   = cd->worked_type == SEARCH_WORKED_NO ? LV_OPA_COVER : LV_OPA_40;
        ea.x2         = ea.x1 + 3;
        lv_draw_rect(dsc->draw_ctx, &edge, &ea);
    }

    char      buf[64];
    lv_area_t area;

    dsc->label_dsc->align = LV_TEXT_ALIGN_RIGHT;
    area.y1 = dsc->draw_area->y1 + lv_obj_get_style_pad_top(obj, LV_PART_ITEMS);
    area.y2 = dsc->draw_area->y2 - lv_obj_get_style_pad_bottom(obj, LV_PART_ITEMS);
    area.x2 = dsc->draw_area->x2 - 15;
    area.x1 = area.x2 - 120;
    snprintf(buf, sizeof(buf), "%i dB", cd->meta.local_snr);
    lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);

    lv_coord_t right    = area.x1 - 10;
    lv_coord_t text_end = dsc->draw_area->x1 + lv_obj_get_style_pad_left(obj, LV_PART_ITEMS) +
                          text_width(lv_table_get_cell_value(obj, r, 0), dsc->label_dsc);

    /* NEW: a CQ or a call to us from a country not in the log; CQ, or worked, on the other CQs. Through the language
     * here: libFT8 is built without the GUI's i18n_hook.h, so its LVGL calls do not translate */
    if (cd->new_dxcc && cd->cell_type != CELL_RX_MSG) text_end = draw_tag(dsc, &area, text_end, _("NEW"), TC(warn));
    else if (cd->cell_type == CELL_RX_CQ && cd->worked_type == SEARCH_WORKED_NO)
        text_end = draw_tag(dsc, &area, text_end, _("CQ"), TC(ok));
    else if (cd->cell_type == CELL_RX_CQ) text_end = draw_tag(dsc, &area, text_end, _("worked"), TC(text_mute));
    if (cd->pota) text_end = draw_pota(dsc, &area, text_end);

    /* Settings > FT8/FT4 info: 0 km, 1 mi, 2 country, 3 country + km, 4 country + mi (from SP2KMO's upstream 3074180e) */
    int32_t info = param_i_get(cfg_ft8_info);

    if (info != 2 && cd->dist > 0) {
        bool miles = decode_list_miles();

        area.x2 = area.x1 - 10;
        area.x1 = area.x2 - 200;
        if (miles) snprintf(buf, sizeof(buf), "%i mi", (int)(cd->dist * 0.621371f + 0.5f));
        else snprintf(buf, sizeof(buf), "%i km", cd->dist);
        lv_draw_label(dsc->draw_ctx, dsc->label_dsc, &area, buf, NULL);
        right = area.x2 - text_width(buf, dsc->label_dsc) - 16;
    }
    if (info >= 2) draw_country(dsc, cd, &area, right, text_end);
}

/* ---- keys and presses ---- */

static void pressed_cb(lv_event_t *e) {
    uint16_t r, c;

    if (!press_cb) return;
    if (lv_event_get_target(e) == list && actions.on_press && actions.on_press()) return;  /* the tiles' */
    if (lv_event_get_target(e) == pin.obj) {    /* touched */
        lv_table_get_selected_cell(pin.obj, &r, &c);
        if (r < pin.n) press_cb(&pin.cd[r]);
    } else if (pin.sel >= 0 && pin.sel < pin.n) {    /* the MFK is down in the pin table */
        press_cb(&pin.cd[pin.sel]);
    } else {
        lv_table_get_selected_cell(list, &r, &c);
        press_cb(row_data(list, r));
    }
}

/* After a touch the MFK row is put back, or the next MFK step started again from the first row,
 * the oldest message, and with no row selected the list stopped following the new ones. A tap
 * (it answered its row on the press) leaves the MFK row where it was, on the last row following
 * on; a drag that scrolled the list leaves it on the row under the finger, as the MFK scrolled
 * back would. Before the table's own handler (pre) and after it */
static void touch_cb(lv_event_t *e) {
    lv_table_t *t     = (lv_table_t *)list;
    lv_indev_t *indev = lv_indev_get_act();
    uint16_t    rows  = lv_table_get_row_cnt(list);
    bool        pre   = lv_event_get_user_data(e) == NULL;
    uint16_t    r;

    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;     /* the MFK press keeps its row */
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        touch.row  = t->row_act < rows ? t->row_act : LV_TABLE_CELL_NONE;
        touch.last = touch.row != LV_TABLE_CELL_NONE && touch.row + 1 == rows;
        return;
    }
    if (pre) {
        touch.finger = t->row_act;
        return;
    }
    if (lv_indev_get_scroll_obj(indev)) r = touch.finger;
    else if (touch.last) r = rows - 1;
    else r = touch.row != LV_TABLE_CELL_NONE ? touch.row : touch.finger;
    if (r >= rows) r = rows - 1;
    t->col_act   = 0;
    t->row_act   = r;
    pin.prev_row = r;
    lv_obj_invalidate(list);
}

/* The MFK past the last row of the list goes on into the pin table, up from its first row
 * back into the list. The list moved its own selection already (class first): put it back */
static void pin_follow_key(uint32_t key) {
    lv_table_t *t    = (lv_table_t *)list;
    uint16_t    rows = lv_table_get_row_cnt(list);
    uint16_t    r, c;

    lv_table_get_selected_cell(list, &r, &c);
    if (pin.own_key) {
        pin.prev_row = r;
        return;
    }
    if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
        if (pin.sel >= 0) {
            t->row_act = pin.list_row;
            if (pin.sel < pin.n - 1) pin.sel++;
        } else if (pin.n && rows && pin.prev_row == rows - 1 && r == rows - 1) {
            pin.list_row = r;
            pin.sel      = 0;
        }
    } else if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
        if (pin.sel >= 0) {
            t->row_act = pin.list_row;
            pin.sel--;
        }
    } else {
        return;
    }
    lv_table_get_selected_cell(list, &pin.prev_row, &c);
    lv_obj_invalidate(list);
    if (pin.obj) lv_obj_invalidate(pin.obj);
}

/* Before the table's own handler: the tiles over the hidden list take the MFK's turns, and the list's cursor stays
 * where it was, so that it still follows the new rows when the traffic shows again. Not the list's own steps down */
static void tiles_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (!pin.own_key && actions.on_key && actions.on_key(key)) lv_event_stop_processing(e);
}

static void list_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    pin_follow_key(key);
    if (!pin.own_key && voice_auto_active() &&
        (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        uint16_t     row, col;

        lv_table_get_selected_cell(list, &row, &col);
        cell_data_t *cd = row_data(list, row);

        if (cd) {
            char said[220];

            row_said(cd, said, sizeof(said));
            voice_delay_say_text_fmt("%s, %u of %u", said, (unsigned)row + 1, (unsigned)list_rows.n);
        }
    }
    if (key == LV_KEY_ESC) {
        if (actions.on_close) actions.on_close();
    } else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) {
        if (actions.on_vol_change) actions.on_vol_change(-1);
    } else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) {
        if (actions.on_vol_change) actions.on_vol_change(1);
    }
}

/* ---- the tables ---- */

/* What the list and the pin table share: one column, rows drawn here, no cell borders */
static lv_obj_t *rows_table(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
    lv_obj_t *obj = lv_table_create(parent);

    lv_obj_remove_style(obj, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_add_event_cb(obj, pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, draw_row_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(obj, draw_extras_cb, LV_EVENT_DRAW_PART_END, NULL);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_table_set_col_cnt(obj, 1);
    lv_table_set_col_width(obj, 0, w - 2);
    lv_obj_set_style_border_width(obj, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(obj, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_pad_left(obj, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(obj, 0, LV_PART_ITEMS);
    return obj;
}

void table_view_build(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    list_rows.n = 0;
    sep_time[0] = '\0';
    press_cb    = NULL;
    memset(&actions, 0, sizeof(actions));

    list = rows_table(parent, x, y, w);
    voice_focus_quiet(list);    /* list_key_cb() and speech_on_row() speak it */
    lv_obj_add_event_cb(list, tiles_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(list, list_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(list, touch_cb, LV_EVENT_PRESSED | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(list, touch_cb, LV_EVENT_RELEASED | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(list, touch_cb, LV_EVENT_RELEASED, list);
    touch.row = LV_TABLE_CELL_NONE;
    lv_obj_set_height(list, h);
    lv_obj_set_style_bg_opa(list, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, theme_shape.border, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, TC(text), LV_PART_MAIN);
    lv_obj_set_style_border_opa(list, 128, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, TC(text_soft), LV_PART_ITEMS);
    /* Its own size: the dialog's 36 (taller still with DejaVu) left room for five rows */
    /* Decoder text Compact (Settings > Display, after SP2KMO): more rows */
    bool       compact = param_i_get(cfg_digi_text_compact);
    lv_coord_t pad     = compact ? 2 : 3;

    lv_obj_set_style_text_font(list, compact ? &sony_20 : &sony_26, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(list, pad, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(list, pad, LV_PART_ITEMS);
    lv_table_set_cell_value(list, 0, 0, EMPTY_TEXT);

    /* "For you" under it: the same rows in the Large font, a line of the to-me colour on top */
    list_y   = y;
    list_h   = h;
    pin.n    = 0;
    pin.sel  = -1;
    pin.obj  = rows_table(parent, x, y + h, w);
    lv_obj_set_height(pin.obj, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(pin.obj, 224, LV_PART_MAIN);
    lv_obj_set_style_border_side(pin.obj, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(pin.obj, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(pin.obj, TC(ft8_to_me), LV_PART_MAIN);
    lv_obj_set_style_text_color(pin.obj, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_text_font(pin.obj, &sony_26, LV_PART_ITEMS);   /* always the Large rows: the calls must stand out */
    lv_obj_set_style_pad_top(pin.obj, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(pin.obj, 3, LV_PART_ITEMS);
    lv_obj_clear_flag(pin.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pin.obj, LV_OBJ_FLAG_HIDDEN);
    pin.timer = lv_timer_create(pin_timer_cb, 5000, NULL);
}

void table_view_destroy(void) {
    if (pin.timer) {
        lv_timer_del(pin.timer);
        pin.timer = NULL;
    }
    if (pin.obj) {
        lv_obj_del(pin.obj);
        pin.obj = NULL;
    }
    pin.n   = 0;
    pin.sel = -1;
    if (list) {
        lv_obj_del(list);
        list = NULL;
    }
    list_rows.n = 0;
    press_cb    = NULL;
    memset(&actions, 0, sizeof(actions));
}

/* "For you" switched off: its rows go */
void table_view_pin_clear(void) {
    pin.n = 0;
    pin_layout();
}

lv_obj_t *table_view_obj(void) {
    return list;
}

void table_view_set_press_cb(table_view_press_cb_t cb) {
    press_cb = cb;
}

void table_view_set_actions(const table_view_actions_t *a) {
    if (a) actions = *a;
    else memset(&actions, 0, sizeof(actions));
}

/* Flag k of flags.h as an LVGL image (the NavTex app's DSC list) */
const lv_img_dsc_t *table_view_flag(int k) {
    return flag_img(k);
}

int table_view_flag_of(const char *prefix) {
    for (size_t i = 0; i < sizeof(flag_of) / sizeof(flag_of[0]); i++)
        if (!strcmp(flag_of[i].prefix, prefix)) return flag_of[i].flag;
    return -1;
}
