/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of Settings opening where it was left (src/settings_place.c, #362). The real
 *  settings_place.c on the PC build's LVGL, on pages built here as settings_window.cpp builds a tab: a scrolling
 *  column of cards, each a grid with its title in a head over the columns, the row labels in column 0 (marked as
 *  card_end() marks them), the controls in the keys' group, alone on the grid (a button showing its value) or in a cell
 *  of their own, three on one row (Date), and a list of rows named by their entries (the networks).
 *
 *  Up to v0.3.13 Settings opened on General's first row each time. The place kept at the close is found again on the
 *  page built anew at the next opening:
 *  - each control of the page, the page built again the same: that control;
 *  - a card more above (Display's spectrum rows, turned on), a Volume row in it too: the same rows still found;
 *  - the three fields of one row, and rows of the same label in one card and in two: each its own;
 *  - a list's entry by its name, wherever the list has it now;
 *  - a button showing a value (the callsign), the value changed: still found;
 *  - not found, so the tab opens at its top: the row left out now (no screensaver: no Minutes row), hidden, disabled,
 *    a list entry gone or not listed yet, a label in another language;
 *  - nothing kept from an object that is not the page's (the keyboard, a menu over the page) or from none, and a place
 *    kept empty finds nothing; objects of the group outside the page never counted;
 *  - how far the page was scrolled, kept with it; the row's label and whether the knob can reach a control.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lvgl/lvgl.h"
#include "settings_place.h"

#include <stdio.h>
#include <string.h>

#define W   800
#define H   480

static int fails, checks;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

/* LVGL's clock: still, nothing here waits. With both defined here the library's lv_hal_tick.o is not linked */
uint32_t lv_tick_get(void) {
    return 1000;
}

uint32_t lv_tick_elaps(uint32_t prev_tick) {
    return 1000 - prev_tick;
}

/* ---- The display: nothing drawn, the places only ---- */

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
    (void)a; (void)px;
    lv_disp_flush_ready(d);
}

static void display(void) {
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t      drv;
    static lv_color_t         px[W * 40];

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, px, NULL, W * 40);
    lv_disp_drv_init(&drv);
    drv.hor_res  = W;
    drv.ver_res  = H;
    drv.flush_cb = flush;
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);
}

/* ---- A tab, built as settings_window.cpp builds one ---- */

static lv_group_t *group;
static lv_obj_t   *page, *grid;
static lv_coord_t  cols[] = {LV_GRID_FR(1), 60, 60, 60, LV_GRID_TEMPLATE_LAST};
static lv_coord_t  row_pool[8][16];
static lv_coord_t *rows;
static int         card_n;

/* The shape of the page this time */
typedef struct {
    bool        above;          /* SPECTRUM, a card more over the others */
    bool        no_minutes;     /* no screensaver: its Minutes row left out */
    bool        hide_speed;     /* the Speed row hidden, */
    bool        off_speed;      /* or its control disabled */
    bool        other_lang;     /* the labels in another language */
    const char *call;           /* the callsign button's text */
    const char *nets[4];        /* the list's entries in their order; the rows after them hidden, nameless */
} shape_t;

/* What the page has, to compare with */
static struct {
    lv_obj_t *fill, *vol_spectrum, *date[3], *saver, *minutes, *speed, *vol_screen, *vol_a, *vol_b, *call, *net[4];
    lv_obj_t *date_label;
} o;

static void card(const char *title) {
    grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    rows = row_pool[card_n++];
    for (int i = 0; i < 16; i++) rows[i] = 40;

    lv_obj_t *head = lv_obj_create(grid);

    lv_obj_remove_style_all(head);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_grid_cell(head, LV_GRID_ALIGN_STRETCH, 0, 4, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_label_set_text(lv_label_create(head), title);
    lv_obj_t *line = lv_obj_create(head);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 100, 1);
}

static void card_end(int n) {
    rows[n] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(grid, cols, rows);
}

static lv_obj_t *row_label(int row, const char *text) {
    lv_obj_t *lbl = lv_label_create(grid);

    lv_label_set_text(lbl, text);
    lv_obj_set_grid_cell(lbl, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
    lv_obj_add_flag(lbl, SETTINGS_ROW_LABEL);
    return lbl;
}

/* A control in a frameless cell of its own, as row_cell() */
static lv_obj_t *cell_ctl(int row, int col) {
    lv_obj_t *cell = lv_obj_create(grid);

    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, 60, 36);
    lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_START, col, 1, LV_GRID_ALIGN_CENTER, row, 1);

    lv_obj_t *ctl = lv_obj_create(cell);

    lv_obj_set_size(ctl, 50, 30);
    lv_group_add_obj(group, ctl);
    return ctl;
}

/* A button on the grid showing its value, as wl_button() */
static lv_obj_t *button(int row, const char *text) {
    lv_obj_t *btn = lv_btn_create(grid);

    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_START, 1, 3, LV_GRID_ALIGN_CENTER, row, 1);
    lv_label_set_text(lv_label_create(btn), text);
    lv_group_add_obj(group, btn);
    return btn;
}

static void build(const shape_t *s) {
    const char *vol = s->other_lang ? "Lautstärke" : "Volume";
    int         row;

    if (page) lv_obj_del(page);     /* its controls leave the group with it */
    memset(&o, 0, sizeof(o));
    card_n = 0;
    page   = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page, 600, 300);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

    if (s->above) {
        card("SPECTRUM");
        row_label(1, s->other_lang ? "Füllung" : "Fill");
        o.fill = cell_ctl(1, 3);
        row_label(2, vol);      /* a Volume before the others */
        o.vol_spectrum = cell_ctl(2, 3);
        card_end(3);
    }

    card("DATE / TIME");
    o.date_label = row_label(1, s->other_lang ? "Datum" : "Date");
    for (int i = 0; i < 3; i++) o.date[i] = cell_ctl(1, 1 + i);
    card_end(2);

    card("SCREEN");
    row = 1;
    row_label(row, s->other_lang ? "Bildschirmschoner" : "Screensaver");
    o.saver = cell_ctl(row++, 3);
    if (!s->no_minutes) {
        row_label(row, s->other_lang ? "Minuten" : "Minutes");
        o.minutes = cell_ctl(row++, 3);
    }
    row_label(row, s->other_lang ? "Tempo" : "Speed");
    o.speed = cell_ctl(row++, 3);
    if (s->hide_speed) lv_obj_add_flag(lv_obj_get_parent(o.speed), LV_OBJ_FLAG_HIDDEN);
    if (s->off_speed) lv_obj_add_state(o.speed, LV_STATE_DISABLED);
    row_label(row, vol);
    o.vol_screen = cell_ctl(row++, 3);
    card_end(row);

    /* Two rows of one label in one card, and the label of a row in the card above */
    card("SPEAKER");
    row_label(1, vol);
    o.vol_a = cell_ctl(1, 3);
    row_label(2, vol);
    o.vol_b = cell_ctl(2, 3);
    card_end(3);

    card("STATION");
    row_label(1, s->other_lang ? "Rufzeichen" : "Callsign");
    o.call = button(1, s->call ? s->call : "-");
    card_end(2);

    /* A list over the columns, no label of its own: its rows made hidden, named and shown by the scan */
    card("NETWORKS");
    lv_obj_t *list = lv_obj_create(grid);

    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_grid_cell(list, LV_GRID_ALIGN_STRETCH, 0, 4, LV_GRID_ALIGN_START, 1, 1);
    rows[1] = LV_GRID_CONTENT;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = lv_obj_create(list);

        lv_obj_set_size(r, lv_pct(100), 36);
        lv_label_set_text(lv_label_create(r), s->nets[i] ? s->nets[i] : "");
        if (!s->nets[i]) lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        lv_group_add_obj(group, r);
        o.net[i] = r;
    }
    card_end(2);
    lv_obj_update_layout(page);
}

/* Kept on a page shaped `before`, looked for on one shaped `after` */
static lv_obj_t *again(const shape_t *before, lv_obj_t **which, const shape_t *after) {
    settings_place_t place;

    build(before);
    check(settings_place_get(&place, page, group, *which), "a control of the page: its place kept");
    build(after);
    return settings_place_find(&place, page, group);
}

/* Found again as `expect` (a control of the page built after), or not found with NULL. o is read once again() has
 * built the page after: in one expression with the call, C may read it first */
static bool found(const shape_t *before, lv_obj_t **which, const shape_t *after, lv_obj_t **expect) {
    lv_obj_t *r = again(before, which, after);

    return expect ? r && r == *expect : r == NULL;
}

/* The controls of the page in the group's order */
static int page_controls(lv_obj_t **out, int max) {
    int n = 0;

    for (lv_obj_t **c = _lv_ll_get_head(&group->obj_ll); c && n < max; c = _lv_ll_get_next(&group->obj_ll, c)) {
        lv_obj_t *p = *c;

        while (p && p != page) p = lv_obj_get_parent(p);
        if (p) out[n++] = *c;
    }
    return n;
}

static const shape_t plain = {.nets = {"Home", "Club", "Field"}};

/* Each control of the page, the page built again the same */
static void same_page(void) {
    lv_obj_t *before[32], *after[32];
    char      what[96];

    build(&plain);
    int n = page_controls(before, 32);

    check(n == 14, "the page has its 14 controls");
    for (int i = 0; i < n; i++) {
        settings_place_t place;

        build(&plain);
        page_controls(before, 32);
        if (!settings_reachable(before[i])) continue;   /* the list's hidden fourth row */
        settings_place_get(&place, page, group, before[i]);
        build(&plain);
        page_controls(after, 32);
        snprintf(what, sizeof(what), "the page built again: control %d found again", i);
        check(settings_place_find(&place, page, group) == after[i], what);
    }
}

static void moved(void) {
    shape_t above = plain;

    above.above = true;
    check(found(&plain, &o.minutes, &above, &o.minutes), "a card more above: Minutes still found");
    check(found(&plain, &o.date[1], &above, &o.date[1]), "a card more above: the month still found");
    check(found(&plain, &o.vol_b, &above, &o.vol_b), "a card more above: the second Volume still found");
    check(found(&plain, &o.vol_screen, &above, &o.vol_screen), "a Volume more above: SCREEN's Volume still found");
    check(found(&above, &o.vol_screen, &plain, &o.vol_screen), "the card above gone: SCREEN's Volume still found");

    shape_t no_minutes = plain;

    no_minutes.no_minutes = true;
    check(found(&plain, &o.speed, &no_minutes, &o.speed), "a row gone above it: Speed still found");
    check(found(&no_minutes, &o.speed, &plain, &o.speed), "a row come above it: Speed still found");
}

static void same_names(void) {
    for (int i = 0; i < 3; i++) {
        char what[64];

        snprintf(what, sizeof(what), "Date's field %d of 3: that one", i + 1);
        check(found(&plain, &o.date[i], &plain, &o.date[i]), what);
    }
    check(found(&plain, &o.vol_a, &plain, &o.vol_a), "the first of two Volume rows in a card: the first");
    check(found(&plain, &o.vol_b, &plain, &o.vol_b), "the second of two Volume rows in a card: the second");
    check(found(&plain, &o.vol_screen, &plain, &o.vol_screen), "Volume of the card above: that card's");
}

static void list(void) {
    shape_t sorted = {.nets = {"Field", "Club", "Home"}};
    shape_t gone = {.nets = {"Home", "Field"}};
    shape_t empty = {.nets = {NULL}};

    check(found(&plain, &o.net[1], &sorted, &o.net[1]), "the list sorted again: Club where it is now");
    check(found(&plain, &o.net[0], &sorted, &o.net[2]), "the list sorted again: Home where it is now");
    check(found(&plain, &o.net[1], &gone, NULL), "Club no longer listed: not found");
    check(found(&plain, &o.net[0], &empty, NULL), "the list not filled yet: not found");
}

static void not_found(void) {
    shape_t s = plain;

    s.no_minutes = true;
    check(found(&plain, &o.minutes, &s, NULL), "no screensaver now: no Minutes row, not found");
    s = plain;
    s.hide_speed = true;
    check(found(&plain, &o.speed, &s, NULL), "Speed hidden now: not found");
    s = plain;
    s.off_speed = true;
    check(found(&plain, &o.speed, &s, NULL), "Speed disabled now: not found");
    s = plain;
    s.other_lang = true;
    check(found(&plain, &o.saver, &s, NULL), "the labels in another language: not found");

    shape_t call = plain;

    call.call = "ON4ABC";
    check(found(&plain, &o.call, &call, &o.call), "the callsign typed: its button still found");
}

static void off_page(void) {
    settings_place_t place;

    /* a menu over the page, in the group as wl_menu: made before the page, first in the group */
    lv_obj_t *menu = lv_obj_create(lv_scr_act());

    lv_group_add_obj(group, menu);
    build(&plain);
    check(!settings_place_get(&place, page, group, menu) && !place.key[0], "a menu over the page: nothing kept");
    check(settings_place_find(&place, page, group) == NULL, "nothing kept: nothing found");
    check(!settings_place_get(&place, page, group, NULL), "no focus: nothing kept");
    check(!settings_place_get(&place, NULL, group, o.saver), "no page: nothing kept");
    check(!settings_place_get(&place, page, group, page), "the page itself: nothing kept");

    /* the menu, before them in the group, is not counted: the second Volume is still the second */
    check(settings_place_get(&place, page, group, o.vol_b) && place.n == 1, "the second Volume: n 1 with a menu first");
    lv_obj_del(menu);
    build(&plain);
    check(settings_place_find(&place, page, group) == o.vol_b, "and found as the second without it");
}

static void scroll_kept(void) {
    settings_place_t place;

    build(&plain);
    lv_obj_scroll_to_y(page, 120, LV_ANIM_OFF);
    settings_place_get(&place, page, group, o.vol_a);
    check(place.scroll_y == 120, "how far the page was scrolled: kept");
}

static void helpers(void) {
    build(&plain);
    check(settings_row_label(o.date[2]) == o.date_label, "a field in its cell: its row's label");
    check(settings_row_label(o.net[0]) == NULL, "a list's row: no label");
    check(settings_row_label(page) == NULL, "the page: no label");
    check(settings_reachable(o.saver), "a control in sight: reachable");
    check(!settings_reachable(o.net[3]), "a hidden list row: not reachable");
    lv_obj_add_state(o.saver, LV_STATE_DISABLED);
    check(!settings_reachable(o.saver), "a disabled control: not reachable");
    lv_obj_add_flag(grid, LV_OBJ_FLAG_HIDDEN);
    check(!settings_reachable(o.net[0]), "a control of a hidden card: not reachable");
    check(!settings_reachable(NULL), "none: not reachable");
}

int main(void) {
    display();
    group = lv_group_create();

    same_page();
    moved();
    same_names();
    list();
    not_found();
    off_page();
    scroll_kept();
    helpers();

    printf("settings_place: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
