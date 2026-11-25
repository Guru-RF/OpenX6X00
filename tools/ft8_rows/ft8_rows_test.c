/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of how the FT8 app's list draws a row (src/ft8/row_look.c), with its CQ rows setting (#359).
 *  - Edge, the default (cfg_ft8_cq_rows 0): F4OLI's redesign as v0.3.13 drew it, the CQs plain in the text's colour
 *    with a green edge, fainter once worked, the one worked here struck through.
 *  - Filled: the CQs on the green rows of v0.3.12, ft8_cq, ft8_cq_worked and ft8_cq_same with that version's text
 *    opacity and strike-through, in the list's own text colour, and no edge.
 *  - The other rows look the same with either: notes, slot lines, the other QSOs grey, to us red, ours blue.
 *  - 1bit: Filled changes nothing (its green rows are paper, theme.c), no edge (its frame instead).
 *  - The MFK's row (#355, #371): solid and framed in the theme's selected-row colours, which stand out of the list's
 *    ground in every built-in theme; v0.3.13's row colour 20 lighter, see-through, was a ghost of a highlight.
 */

#include "ft8/row_look.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static cell_data_t row(ft8_cell_type_t type, qso_log_search_worked_t worked) {
    cell_data_t cd = {.cell_type = type, .worked_type = worked, .flag = -1};

    snprintf(cd.text, sizeof(cd.text), "CQ DL1ABC JO31");
    return cd;
}

static bool same(ft8_row_look_t a, ft8_row_look_t b) {
    return a.bg == b.bg && a.text == b.text && a.text_opa == b.text_opa && a.strike == b.strike &&
           a.center == b.center && a.edge == b.edge;
}

static ft8_row_look_t look(ft8_cell_type_t type, qso_log_search_worked_t worked, ft8_cq_rows_t cq_rows, bool mono) {
    cell_data_t cd = row(type, worked);

    return ft8_row_look(&cd, cq_rows, mono);
}

static void edge(void) {
    ft8_row_look_t l = look(CELL_RX_CQ, SEARCH_WORKED_NO, FT8_CQ_ROWS_EDGE, false);

    check(FT8_CQ_ROWS_EDGE == 0, "Edge is 0, the setting's default (settings_manager.h)");
    check(l.bg == TC_panel && l.text == TC_text && l.text_opa == LV_OPA_COVER && !l.strike && !l.center,
          "Edge: a CQ not worked plain, in the text's colour");
    check(l.edge == LV_OPA_COVER, "Edge: its green edge solid");

    l = look(CELL_RX_CQ, SEARCH_WORKED_YES, FT8_CQ_ROWS_EDGE, false);
    check(l.bg == TC_panel && l.text == TC_text && l.text_opa == LV_OPA_80 && !l.strike,
          "Edge: worked on another band or mode, fainter");
    check(l.edge == LV_OPA_40, "Edge: worked, a fainter edge");

    l = look(CELL_RX_CQ, SEARCH_WORKED_SAME_MODE, FT8_CQ_ROWS_EDGE, false);
    check(l.bg == TC_panel && l.text_opa == LV_OPA_60 && l.strike && l.edge == LV_OPA_40,
          "Edge: worked here, struck through, fainter still");
}

static void filled(void) {
    ft8_row_look_t l = look(CELL_RX_CQ, SEARCH_WORKED_NO, FT8_CQ_ROWS_FILLED, false);

    check(l.bg == TC_ft8_cq && l.text == TC_COUNT && l.text_opa == LV_OPA_COVER && !l.strike && !l.center,
          "Filled: a CQ not worked on ft8_cq, the list's text colour");
    check(l.edge == 0, "Filled: no edge on a green row");

    l = look(CELL_RX_CQ, SEARCH_WORKED_YES, FT8_CQ_ROWS_FILLED, false);
    check(l.bg == TC_ft8_cq_worked && l.text == TC_COUNT && l.text_opa == LV_OPA_90 && !l.strike && l.edge == 0,
          "Filled: worked on another band or mode on ft8_cq_worked, as v0.3.12");

    l = look(CELL_RX_CQ, SEARCH_WORKED_SAME_MODE, FT8_CQ_ROWS_FILLED, false);
    check(l.bg == TC_ft8_cq_same && l.text == TC_COUNT && l.text_opa == LV_OPA_80 && l.strike && l.edge == 0,
          "Filled: worked here on ft8_cq_same, struck through, as v0.3.12");
}

/* Every row that is not a CQ looks the same with Edge and Filled, in colour and in 1bit */
static void others(void) {
    static const ft8_cell_type_t types[] = {CELL_RX_INFO, CELL_START_QSO, CELL_RX_MSG, CELL_RX_TO_ME, CELL_TX_MSG,
                                            CELL_SLOT_SEP};
    char what[64];

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        for (int mono = 0; mono < 2; mono++) {
            snprintf(what, sizeof(what), "row type %d%s: the same with either", types[i], mono ? " in 1bit" : "");
            check(same(look(types[i], SEARCH_WORKED_NO, FT8_CQ_ROWS_EDGE, mono),
                       look(types[i], SEARCH_WORKED_NO, FT8_CQ_ROWS_FILLED, mono)),
                  what);
        }

    ft8_row_look_t l = look(CELL_RX_MSG, SEARCH_WORKED_NO, FT8_CQ_ROWS_FILLED, false);
    check(l.bg == TC_panel && l.text == TC_text_mute && l.edge == 0, "another QSO grey, no edge");
    l = look(CELL_RX_TO_ME, SEARCH_WORKED_NO, FT8_CQ_ROWS_FILLED, false);
    check(l.bg == TC_ft8_to_me && l.text == TC_COUNT && l.edge == 0, "to us red");
    l = look(CELL_TX_MSG, SEARCH_WORKED_NO, FT8_CQ_ROWS_FILLED, false);
    check(l.bg == TC_ft8_tx && l.edge == 0, "ours blue");
    l = look(CELL_SLOT_SEP, SEARCH_WORKED_NO, FT8_CQ_ROWS_FILLED, false);
    check(l.bg == TC_panel && l.text == TC_text_mute && l.center, "a slot line grey, in the middle");
    l = look(CELL_RX_INFO, SEARCH_WORKED_NO, FT8_CQ_ROWS_FILLED, false);
    check(l.bg == TC_row && l.center, "a note on the row colour, in the middle");

    ft8_row_look_t wait  = ft8_row_look(NULL, FT8_CQ_ROWS_FILLED, false);
    ft8_row_look_t wait2 = ft8_row_look(NULL, FT8_CQ_ROWS_EDGE, true);
    check(wait.bg == TC_row && wait.center && same(wait, wait2), "the \"Wait sync\" row, no row data");
}

static void onebit(void) {
    static const qso_log_search_worked_t kinds[] = {SEARCH_WORKED_NO, SEARCH_WORKED_YES, SEARCH_WORKED_SAME_MODE};

    for (size_t i = 0; i < 3; i++) {
        ft8_row_look_t e = look(CELL_RX_CQ, kinds[i], FT8_CQ_ROWS_EDGE, true);
        ft8_row_look_t f = look(CELL_RX_CQ, kinds[i], FT8_CQ_ROWS_FILLED, true);

        check(same(e, f), "1bit: Filled draws a CQ as Edge does");
        check(e.bg == TC_panel && e.text == TC_text && e.edge == 0, "1bit: a CQ plain, no edge (its frame)");
    }
    check(look(CELL_RX_CQ, SEARCH_WORKED_SAME_MODE, FT8_CQ_ROWS_FILLED, true).strike, "1bit: worked here struck through");
}

/* The built-in themes' values (theme.h): RF.Guru, Legacy, Outdoor, Classic */
#define HEX4(role, a, b, c, d, ...) static const uint32_t hex_##role[4] __attribute__((unused)) = {a, b, c, d};
THEME_COLORS(HEX4)
#undef HEX4

static double channel(uint32_t c, int shift) {
    double v = ((c >> shift) & 0xff) / 255.0;

    return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t c) {
    return 0.2126 * channel(c, 16) + 0.7152 * channel(c, 8) + 0.0722 * channel(c, 0);
}

/* WCAG's contrast ratio, 1 the same, 21 black on white */
static double contrast(uint32_t a, uint32_t b) {
    double la = luminance(a), lb = luminance(b);

    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

/* LVGL's lv_color_lighten() and an opacity over a ground, per channel */
static uint32_t mix(uint32_t fg, uint32_t bg, unsigned opa) {
    uint32_t out = 0;

    for (int shift = 0; shift < 24; shift += 8) {
        unsigned f = (fg >> shift) & 0xff, b = (bg >> shift) & 0xff;

        out |= ((f * opa + b * (255 - opa)) / 255) << shift;
    }
    return out;
}

static void mfk(void) {
    static const ft8_cell_type_t plain[] = {CELL_RX_INFO, CELL_START_QSO, CELL_RX_MSG, CELL_RX_CQ, CELL_SLOT_SEP};
    static const char           *names[] = {"RF.Guru", "Legacy", "Outdoor", "Classic"};
    char what[96];

    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        cell_data_t    cd = row(plain[i], SEARCH_WORKED_NO);
        ft8_mfk_look_t m  = ft8_row_mfk_look(&cd);

        snprintf(what, sizeof(what), "MFK on row type %d: the theme's selected row, solid and framed", plain[i]);
        check(m.bg == TC_sel_bg && m.text == TC_sel_text && m.border == TC_sel_border && m.border_width >= 2 &&
                  m.bg_opa == LV_OPA_COVER,
              what);
    }
    check(ft8_row_mfk_look(NULL).bg == TC_sel_bg, "MFK on the \"Wait sync\" row: the theme's selected row");

    static const ft8_cell_type_t own[] = {CELL_RX_TO_ME, CELL_TX_MSG};
    for (size_t i = 0; i < 2; i++) {
        cell_data_t    cd = row(own[i], SEARCH_WORKED_NO);
        ft8_mfk_look_t m  = ft8_row_mfk_look(&cd);

        snprintf(what, sizeof(what), "MFK on row type %d: its red or blue lightened, solid, framed", own[i]);
        check(m.bg == TC_COUNT && m.lighten > 0 && m.border == TC_sel_border && m.border_width >= 2 &&
                  m.bg_opa == LV_OPA_COVER,
              what);
    }
    check(ft8_row_mfk_look(NULL).bg_opa > FT8_ROW_BG_OPA, "MFK row more solid than the rows around it");

    /* The traffic and Edge's CQs lie on the panel: the MFK's row must stand out of them in every built-in theme */
    for (int t = 0; t < 4; t++) {
        uint32_t rowc  = mix(hex_panel[t], hex_panel[t], FT8_ROW_BG_OPA);
        uint32_t ghost = mix(mix(0xffffff, hex_panel[t], 20), hex_panel[t], FT8_ROW_BG_OPA);   /* v0.3.13 */

        snprintf(what, sizeof(what), "%s: v0.3.13's MFK row a ghost (%.2f)", names[t], contrast(ghost, rowc));
        check(contrast(ghost, rowc) < 1.5, what);
        snprintf(what, sizeof(what), "%s: the MFK row's frame stands out (%.2f)", names[t],
                 contrast(hex_sel_border[t], rowc));
        check(contrast(hex_sel_border[t], rowc) >= 3.0, what);
        snprintf(what, sizeof(what), "%s: the MFK row's text readable on it (%.2f)", names[t],
                 contrast(hex_sel_text[t], hex_sel_bg[t]));
        check(contrast(hex_sel_text[t], hex_sel_bg[t]) >= 4.5, what);
    }
}

int main(void) {
    edge();
    filled();
    others();
    onebit();
    mfk();
    printf("ft8_rows: %d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
