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
 */

#include "ft8/row_look.h"

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

int main(void) {
    edge();
    filled();
    others();
    onebit();
    printf("ft8_rows: %d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
