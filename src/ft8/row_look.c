// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - how a row of the FT8/FT4/FT2 list looks, see row_look.h
 */

#include "row_look.h"

ft8_row_look_t ft8_row_look(const cell_data_t *cd, ft8_cq_rows_t cq_rows, bool mono) {
    ft8_row_look_t look = {.bg = TC_panel, .text = TC_COUNT, .text_opa = LV_OPA_COVER};

    if (cd && cd->cell_type == CELL_SLOT_SEP) {     /* its rules: decode_list.c's draw_extras_cb */
        look.center = true;
        look.text   = TC_text_mute;
    } else if (!cd || cd->cell_type == CELL_RX_INFO || cd->cell_type == CELL_START_QSO) {
        look.center = true;
        look.bg     = TC_row;
    } else if (cd->cell_type == CELL_RX_CQ && cq_rows == FT8_CQ_ROWS_FILLED && !mono) {
        /* As up to v0.3.12: green, darker for a station worked on another band or mode, darker still and struck
         * through for one worked here, in the list's text colour. No edge: green on green adds nothing, the tags
         * stay (NEW and worked say what the fill does not) */
        if (cd->worked_type == SEARCH_WORKED_YES) {
            look.bg       = TC_ft8_cq_worked;
            look.text_opa = LV_OPA_90;
        } else if (cd->worked_type == SEARCH_WORKED_SAME_MODE) {
            look.bg       = TC_ft8_cq_same;
            look.strike   = true;
            look.text_opa = LV_OPA_80;
        } else {
            look.bg = TC_ft8_cq;
        }
    } else if (cd->cell_type == CELL_RX_CQ) {
        /* F4OLI's redesign: plain rows in the text's colour, the green edge fainter once worked (1bit: its frame) */
        look.text = TC_text;
        look.edge = mono ? 0 : cd->worked_type == SEARCH_WORKED_NO ? LV_OPA_COVER : LV_OPA_40;
        if (cd->worked_type == SEARCH_WORKED_YES) {     /* on another band or mode */
            look.text_opa = LV_OPA_80;
        } else if (cd->worked_type == SEARCH_WORKED_SAME_MODE) {
            look.strike   = true;
            look.text_opa = LV_OPA_60;
        }
    } else if (cd->cell_type == CELL_RX_MSG) {      /* the other stations' QSOs grey */
        look.text = TC_text_mute;
    } else if (cd->cell_type == CELL_RX_TO_ME) {
        look.bg = TC_ft8_to_me;
    } else if (cd->cell_type == CELL_TX_MSG) {
        look.bg = TC_ft8_tx;
    }
    return look;
}

ft8_mfk_look_t ft8_row_mfk_look(const cell_data_t *cd) {
    ft8_mfk_look_t mfk = {.bg           = TC_sel_bg,
                          .text         = TC_sel_text,
                          .border       = TC_sel_border,
                          .border_width = 2,
                          .bg_opa       = LV_OPA_COVER};

    if (cd && (cd->cell_type == CELL_RX_TO_ME || cd->cell_type == CELL_TX_MSG)) {
        mfk.bg      = TC_COUNT;
        mfk.lighten = 20;
        mfk.text    = TC_COUNT;
    }
    return mfk;
}
