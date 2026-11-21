/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of a long dropdown list (src/dropdown_fit.c, #310): the countries of Settings > General >
 *  Country and of the first-start guide's Country step. The real dropdown_fit.c, the real country list and the real
 *  fonts, put in place by font_select.c's fonts_apply() as at boot, on the PC build's LVGL and its default theme (the
 *  lists' spacing is the theme's, as on the radio).
 *
 *  Up to v0.3.13 the list stopped scrolling at Suriname: LVGL keeps an object under LV_COORD_MAX (8191 px) high, and
 *  the list's label, 246 lines of 39 px in Settings, was cut there. The knob moved the choice on below, out of sight.
 *
 *  The cases:
 *  - the spacing's sums: a short list keeps the theme's, a long one gets what fits and fits, never under 0;
 *  - for each font family and VISION's sizes, the list with Settings' font and with the guide's, opened low on the
 *    screen (it opens upwards, as Settings' Country) and high (downwards): the label whole, the choice in sight at each
 *    step of the knob from "-" down to the last country and back up, and the last one chosen with a press;
 *  - the same list without the fit, at Settings' font in the default family: cut, the last country out of sight (what
 *    the test looks for is the fault the radio had);
 *  - a short list keeps the theme's spacing, and a long list made short again gets it back at its next opening.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lvgl/lvgl.h"
#include "countries.h"
#include "dropdown_fit.h"
#include "fonts/font_select.h"
#include "cfg/parameter_api.h"

extern lv_font_t sony_22;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W               800
#define H               480
#define VISION_STEPS    3       /* theme.h's VISION_FONT_STEPS */

static int fails, checks;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

/* ---- What countries.c and font_select.c call, stood in for ---- */

const char *i18n_code(void) {
    return "";
}

const char *i18n_font(void) {
    return NULL;
}

void band_plan_follow(void) {
}

struct ParamText {
    char v[4];
};

static ParamText  country_param;
ParamText        *cfg_country = &country_param;

size_t param_t_get(const ParamText *p, char *out, size_t size) {
    snprintf(out, size, "%s", p->v);
    return strlen(p->v);
}

void param_t_set(ParamText *p, const char *v) {
    snprintf(p->v, sizeof(p->v), "%s", v ? v : "");
}

bool dxcc_lookup(const char *call, const char **name, const char **prefix) {
    (void)call; (void)name; (void)prefix;
    return false;
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

/* ---- The sums ---- */

static bool fits(uint32_t lines, lv_coord_t font_h, lv_coord_t space) {
    return (int32_t)lines * font_h + (int32_t)(lines - 1) * space <= LV_COORD_MAX;
}

static void sums(void) {
    check(dropdown_fit_space(20, 23, 16) == 16, "20 lines: the theme's spacing");
    check(dropdown_fit_space(1, 23, 16) == 16 && dropdown_fit_space(0, 23, 16) == 16, "one line, none: as it was");
    check(dropdown_fit_space(246, 23, 16) == 10, "the countries at 23 px: 10 px between them");
    check(dropdown_fit_space(246, 40, 16) == 0, "lines too tall to fit at all: 0, not less");
    for (uint32_t n = 2; n < 600; n++) {
        for (lv_coord_t h = 8; h <= 44; h++) {
            lv_coord_t s = dropdown_fit_space(n, h, 16);

            if (s < 0 || s > 16 || (fits(n, h, 0) && !fits(n, h, s)) || (s < 16 && fits(n, h, s + 1))) {
                printf("FAIL %u lines of %d px: %d px between\n", n, h, s);
                fails++;
                return;
            }
        }
    }
    checks++;
}

/* ---- The list ---- */

static char *options;
static int   n_options;

static void country_options(void) {
    size_t size = 4, k;

    for (int i = 0; i < countries_n(); i++)
        size += strlen(country_name(country_sorted(i))) + 1;
    options = malloc(size);
    k       = (size_t)snprintf(options, size, "-");
    for (int i = 0; i < countries_n(); i++)
        k += (size_t)snprintf(options + k, size - k, "\n%s", country_name(country_sorted(i)));
    n_options = countries_n() + 1;
}

/* A dropdown as Settings' (a box at the right of a row) or the guide's, at y; fitted or not */
static lv_obj_t *dropdown(const lv_font_t *font, lv_coord_t y, const char *opts, bool fit) {
    lv_obj_t *dd = lv_dropdown_create(lv_scr_act());

    if (fit) dropdown_fit(dd);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_style_text_font(dd, font, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), font, 0);
    lv_obj_set_size(dd, 367, 44);
    lv_obj_set_pos(dd, 415, y);
    lv_obj_update_layout(dd);
    return dd;
}

static lv_obj_t *list_label(lv_obj_t *dd) {
    return lv_obj_get_child(lv_dropdown_get_list(dd), 0);
}

static lv_coord_t unit(lv_obj_t *dd) {
    lv_obj_t *label = list_label(dd);

    return lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)) + lv_obj_get_style_text_line_space(label, 0);
}

/* The choice's line inside the open list, where it shows */
static bool choice_seen(lv_obj_t *dd) {
    lv_obj_t  *list  = lv_dropdown_get_list(dd);
    lv_obj_t  *label = list_label(dd);
    lv_coord_t font_h = lv_font_get_line_height(lv_obj_get_style_text_font(label, 0));
    lv_coord_t y1    = label->coords.y1 + lv_dropdown_get_selected(dd) * unit(dd);
    lv_area_t  shown;

    lv_obj_get_content_coords(list, &shown);
    /* the label's own end too: past it LVGL draws no text */
    return y1 >= shown.y1 && y1 + font_h - 1 <= shown.y2 && y1 + font_h - 1 <= label->coords.y2 && shown.y1 >= 0 &&
           shown.y2 < H;
}

static void key(lv_obj_t *dd, uint32_t k) {
    lv_event_send(dd, LV_EVENT_KEY, &k);
}

/* Open, the knob down to the last line and up again, each step in sight; then down and chosen */
static void walk(const char *name, const lv_font_t *font, lv_coord_t y) {
    char      what[256], sel[64], last[64];
    lv_obj_t *dd = dropdown(font, y, options, true);
    int       lost = -1;

    lv_dropdown_open(dd);
    lv_obj_t  *label  = list_label(dd);
    lv_coord_t font_h = lv_font_get_line_height(lv_obj_get_style_text_font(label, 0));
    lv_coord_t space  = lv_obj_get_style_text_line_space(label, 0);

    snprintf(what, sizeof(what), "%s at y %d: the list's label whole (%d lines of %d px, %d between: %d px)", name, y,
             n_options, font_h, space, lv_obj_get_height(label));
    check(lv_obj_get_height(label) == n_options * font_h + (n_options - 1) * space, what);
    snprintf(what, sizeof(what), "%s at y %d: the list opened %s", name, y, y > H / 2 ? "upwards" : "downwards");
    check(y > H / 2 ? lv_dropdown_get_list(dd)->coords.y2 <= y : lv_dropdown_get_list(dd)->coords.y1 >= y + 44, what);

    for (int i = 1; i < n_options && lost < 0; i++) {
        key(dd, LV_KEY_DOWN);
        if (!choice_seen(dd)) lost = i;
    }
    for (int i = n_options - 2; i >= 0 && lost < 0; i--) {
        key(dd, LV_KEY_UP);
        if (!choice_seen(dd)) lost = i;
    }
    snprintf(what, sizeof(what), "%s at y %d: the choice in sight at each step (lost at line %d)", name, y, lost);
    check(lost < 0, what);

    for (int i = 1; i < n_options; i++)
        key(dd, LV_KEY_DOWN);
    key(dd, LV_KEY_ENTER);
    lv_dropdown_get_selected_str(dd, sel, sizeof(sel));
    snprintf(last, sizeof(last), "%s", country_name(country_sorted(countries_n() - 1)));
    snprintf(what, sizeof(what), "%s at y %d: the last country chosen (%s, not %s)", name, y, last, sel);
    check(!lv_dropdown_is_open(dd) && !strcmp(sel, last), what);
    lv_obj_del(dd);
}

static void families(void) {
    static const struct {
        uint8_t     font;
        const char *name;
    } fam[] = {
        {FONT_SONY, "Sony"}, {FONT_ROBOTO, "Roboto"}, {FONT_DEJAVU, "DejaVu"}, {FONT_ARIAL, "Arial"},
        {FONT_RCOND, "Roboto Condensed"}, {FONT_PIXEL, "pixel (1bit)"}, {FONT_CODEC, "Codec"},
    };
    char name[96];

    for (size_t f = 0; f < sizeof(fam) / sizeof(fam[0]); f++) {
        for (int vision = 0; vision < 2; vision++) {
            fonts_apply(fam[f].font);
            if (vision) fonts_scale(VISION_STEPS);

            /* Settings' SF(22) (settings_window.cpp), the guide's sony_22 (first_start.c) */
            snprintf(name, sizeof(name), "%s%s, Settings", fam[f].name, vision ? " VISION" : "");
            walk(name, font_down(&sony_22, vision ? 0 : -2), 305);
            walk(name, font_down(&sony_22, vision ? 0 : -2), 150);
            snprintf(name, sizeof(name), "%s%s, first-start guide", fam[f].name, vision ? " VISION" : "");
            walk(name, &sony_22, 250);
            walk(name, &sony_22, 60);
        }
    }
}

/* Without the fit: what the radio did. Settings' font in the default family */
static void unfitted(void) {
    fonts_apply(FONT_DEJAVU);

    lv_obj_t *dd = dropdown(font_down(&sony_22, -2), 305, options, false);

    lv_dropdown_open(dd);
    check(lv_obj_get_height(list_label(dd)) == LV_COORD_MAX, "unfitted: the label cut at LV_COORD_MAX");
    for (int i = 1; i < n_options; i++)
        key(dd, LV_KEY_DOWN);
    check(!choice_seen(dd), "unfitted: the last country out of sight");
    lv_obj_del(dd);
}

/* A short list as the theme has it; a list made short again gets the theme's spacing back */
static void short_lists(void) {
    fonts_apply(FONT_DEJAVU);

    lv_obj_t  *dd    = dropdown(&sony_22, 250, "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl", true);
    lv_coord_t theme = lv_obj_get_style_text_line_space(lv_dropdown_get_list(dd), 0);

    lv_dropdown_open(dd);
    check(lv_obj_get_style_text_line_space(list_label(dd), 0) == theme, "12 lines: the theme's spacing");
    lv_dropdown_close(dd);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_open(dd);
    check(lv_obj_get_style_text_line_space(list_label(dd), 0) < theme, "the countries: closer together");
    lv_dropdown_close(dd);
    lv_dropdown_set_options(dd, "a\nb\nc");
    lv_dropdown_open(dd);
    check(lv_obj_get_style_text_line_space(list_label(dd), 0) == theme, "3 lines again: the theme's spacing back");
    lv_obj_del(dd);
}

int main(void) {
    display();
    country_options();
    check(n_options == 246, "246 lines: \"-\" and the 245 countries");
    sums();
    families();
    unfitted();
    short_lists();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
