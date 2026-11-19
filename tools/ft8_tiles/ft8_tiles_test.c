/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the FT8 app's stations heard (src/ft8/heard.c): the tiles and the calls over the waterfall of F4OLI's
 *  redesign (OlivLoze's fork), and of the time a slot's rows carry (ftx_slot_start, src/ft8/utils.c).
 *  - A slot's rows and our TX row carry the slot's start, however late its tick came: 07:42:18 and 07:42:46 on a busy
 *    40 m were the slots of 07:42:15 and 07:42:45. FT4's and FT2's slots start on fractions of a second.
 *  - A station that calls us is red (FT8_TAG_CALLS_YOU) for 3 minutes after its call, then plain with "called you";
 *    its RR73 or 73 to us does not make it red again, a new call does, a QSO logged ends it. It stays while red, even
 *    with no new row for 2 minutes; lv_tick going round changes nothing.
 *  - Show: CQ filters the tiles (and the lane): a station seen in another QSO under Show: All goes at once.
 *  - The tiles' order and tags, the 64 stations kept, the countries of the log.
 */

#include "ft8/heard.h"
#include "ft8/utils.h"

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

/* A decode as the window makes it: a CQ, a message of another QSO, or one to us */
static cell_data_t row(ft8_cell_type_t type, const char *call, ftx_msg_type_t msg, int snr, float hz) {
    cell_data_t cd = {.cell_type = type, .flag = -1};

    snprintf(cd.meta.call_de, sizeof(cd.meta.call_de), "%s", call);
    cd.meta.type      = msg;
    cd.meta.local_snr = snr;
    cd.meta.freq_hz   = hz;
    cd.meta.to_me     = type == CELL_RX_TO_ME;
    snprintf(cd.text, sizeof(cd.text), "12:00:00 %s", call);
    return cd;
}

static const ft8_heard_t *find(const char *call, bool show_all, uint32_t now) {
    const ft8_heard_t *st[FT8_HEARD_MAX];
    int                n = ft8_heard_list(st, FT8_HEARD_MAX, show_all, now);

    for (int i = 0; i < n; i++)
        if (!strcmp(st[i]->cd.meta.call_de, call)) return st[i];
    return NULL;
}

/* 2026-11-19 07:42:00 UTC */
#define MIN0 1795160520LL

static void slot_start(void) {
    check(ftx_slot_start(MIN0 + 18, 300000000, 15.0f) == MIN0 + 15, "FT8: 07:42:18.3 is the slot of 07:42:15");
    check(ftx_slot_start(MIN0 + 46, 0, 15.0f) == MIN0 + 45, "FT8: 07:42:46 is the slot of 07:42:45");
    check(ftx_slot_start(MIN0 + 15, 0, 15.0f) == MIN0 + 15, "FT8: 07:42:15.0 starts its own slot");
    check(ftx_slot_start(MIN0 + 14, 999999999, 15.0f) == MIN0, "FT8: 07:42:14.999 is the slot of 07:42:00");
    check(ftx_slot_start(MIN0 + 59, 999000000, 15.0f) == MIN0 + 45, "FT8: the minute's last slot");
    check(ftx_slot_start(MIN0 + 9, 0, 7.5f) == MIN0 + 7, "FT4: 07:42:09 is the slot of 07:42:07.5, shown :07");
    check(ftx_slot_start(MIN0 + 7, 400000000, 7.5f) == MIN0, "FT4: 07:42:07.4 is still the slot of 07:42:00");
    check(ftx_slot_start(MIN0 + 16, 0, 7.5f) == MIN0 + 15, "FT4: 07:42:16 is the slot of 07:42:15");
    check(ftx_slot_start(MIN0 + 58, 0, 3.75f) == MIN0 + 56, "FT2: 07:42:58 is the slot of 07:42:56.25, shown :56");
    check(ftx_slot_start(MIN0 + 4, 0, 3.75f) == MIN0 + 3, "FT2: 07:42:04 is the slot of 07:42:03.75, shown :03");
    check(ftx_slot_start(MIN0 + 60 + 1, 0, 3.75f) == MIN0 + 60, "FT2: the next minute starts a slot");
}

static void red_three_minutes(void) {
    uint32_t    t  = 1000000;
    cell_data_t cq = row(CELL_RX_CQ, "DL1ABC", FTX_MSG_TYPE_CQ, -10, 1200);
    cell_data_t rp = row(CELL_RX_TO_ME, "DL1ABC", FTX_MSG_TYPE_REPORT, -10, 1200);

    ft8_heard_clear();
    ft8_heard_add(&cq, t);
    check(ft8_heard_tag(find("DL1ABC", true, t), t) == FT8_TAG_CQ, "a CQ not worked: CQ");
    rp.meta.remote_snr = -12;
    ft8_heard_add(&rp, t + 15000);
    t += 15000;

    const ft8_heard_t *st = find("DL1ABC", false, t);

    check(st && ft8_heard_calls_you(st, t) && ft8_heard_tag(st, t) == FT8_TAG_CALLS_YOU, "its call: red");
    check(st && st->has_report && st->report == -12, "its report kept for the tag");
    check(st && ft8_heard_calls_you(st, t + FT8_CALLS_YOU_MS - 1), "red until 3 minutes after the call");
    st = find("DL1ABC", true, t + FT8_HEARD_KEEP_MS + 1000);
    check(st != NULL, "kept past 2 minutes with no new row while red");
    st = find("DL1ABC", true, t + FT8_CALLS_YOU_MS - 1);
    check(st && !ft8_heard_calls_you(st, t + FT8_CALLS_YOU_MS), "plain 3 minutes after the call");
    check(find("DL1ABC", true, t + FT8_CALLS_YOU_MS + 1) == NULL, "gone once plain and not heard for 2 minutes");

    /* its RR73 to us ends the QSO: no new red; a new call does it */
    ft8_heard_clear();
    t = 5000000;
    ft8_heard_add(&rp, t);
    cell_data_t rr = row(CELL_RX_TO_ME, "DL1ABC", FTX_MSG_TYPE_RR73, -9, 1200);

    ft8_heard_add(&rr, t + 170000);
    st = find("DL1ABC", true, t + FT8_CALLS_YOU_MS);
    check(st && !ft8_heard_calls_you(st, t + FT8_CALLS_YOU_MS), "an RR73 to us made it red again");
    check(st && ft8_heard_tag(st, t + FT8_CALLS_YOU_MS) == FT8_TAG_CALLED_YOU, "plain, then: called you");
    cell_data_t r73 = row(CELL_RX_TO_ME, "DL1ABC", FTX_MSG_TYPE_73, -9, 1200);

    ft8_heard_add(&r73, t + FT8_CALLS_YOU_MS + 1000);
    st = find("DL1ABC", true, t + FT8_CALLS_YOU_MS + 1000);
    check(st && !ft8_heard_calls_you(st, t + FT8_CALLS_YOU_MS + 1000), "a 73 to us made it red again");

    /* a QSO logged ends the red; its next call brings it back */
    ft8_heard_add(&rp, t + 400000);
    ft8_heard_logged("DL1ABC");
    st = find("DL1ABC", true, t + 401000);
    check(st && !ft8_heard_calls_you(st, t + 401000), "still red after the QSO was logged");
    ft8_heard_add(&rp, t + 430000);
    st = find("DL1ABC", true, t + 431000);
    check(st && ft8_heard_calls_you(st, t + 431000), "a call after the logged QSO not red");

    /* lv_tick goes round every 49 days */
    ft8_heard_clear();
    t = 0xffffffffu - 30000;
    ft8_heard_add(&rp, t);
    st = find("DL1ABC", false, t + 60000);      /* 30 s past the turn */
    check(st && ft8_heard_calls_you(st, t + 60000), "lv_tick round: no longer red");
    check(find("DL1ABC", true, t + FT8_CALLS_YOU_MS + 1) == NULL, "lv_tick round: never goes");
}

static void show_cq(void) {
    uint32_t    t  = 2000000;
    cell_data_t cq = row(CELL_RX_CQ, "EA5KDD", FTX_MSG_TYPE_CQ, -5, 800);
    cell_data_t qs = row(CELL_RX_MSG, "G4ABC", FTX_MSG_TYPE_REPORT, -3, 1500);
    cell_data_t me = row(CELL_RX_TO_ME, "OH2XYZ", FTX_MSG_TYPE_GRID, -20, 2100);

    ft8_heard_clear();
    ft8_heard_add(&cq, t);
    ft8_heard_add(&qs, t);
    ft8_heard_add(&me, t);

    const ft8_heard_t *st[FT8_HEARD_MAX];

    check(ft8_heard_list(st, FT8_HEARD_MAX, true, t + 1000) == 3, "Show: All: the three stations");
    check(ft8_heard_list(st, FT8_HEARD_MAX, false, t + 1000) == 2, "Show: CQ: the CQ and the call to us");
    check(!find("G4ABC", false, t + 1000), "Show: CQ: another QSO's station stays on the tiles");
    check(find("G4ABC", true, t + 1000) != NULL, "Show: All again: the station back while heard");
    check(find("OH2XYZ", false, t + FT8_CALLS_YOU_MS - 1) != NULL, "Show: CQ: the call to us while red");
    check(!find("OH2XYZ", false, t + FT8_CALLS_YOU_MS + 1), "Show: CQ: the call to us once plain (not a CQ)");

    /* a CQ station gone into a QSO: its last row is no CQ any more */
    cell_data_t cq_then = row(CELL_RX_MSG, "EA5KDD", FTX_MSG_TYPE_GRID, -5, 800);

    ft8_heard_add(&cq_then, t + 15000);
    check(!find("EA5KDD", false, t + 16000), "Show: CQ: a CQ station gone into a QSO goes");
    check(ft8_heard_tag(find("EA5KDD", true, t + 16000), t + 16000) == FT8_TAG_QSO, "in a QSO: QSO");
    check(ft8_heard_list(st, 1, true, t + 16000) == 1, "at most max");
}

static void order_and_tags(void) {
    uint32_t    t    = 3000000;
    cell_data_t wk   = row(CELL_RX_CQ, "W1AW", FTX_MSG_TYPE_CQ, 5, 900);
    cell_data_t nw   = row(CELL_RX_CQ, "VK2XX", FTX_MSG_TYPE_CQ, -18, 1000);
    cell_data_t cq1  = row(CELL_RX_CQ, "DL2AA", FTX_MSG_TYPE_CQ, -2, 1100);
    cell_data_t cq2  = row(CELL_RX_CQ, "DL3BB", FTX_MSG_TYPE_CQ, -8, 1300);
    cell_data_t qso  = row(CELL_RX_MSG, "F5ZZZ", FTX_MSG_TYPE_REPORT, 10, 1400);
    cell_data_t me1  = row(CELL_RX_TO_ME, "I1OLD", FTX_MSG_TYPE_GRID, -1, 1500);
    cell_data_t me2  = row(CELL_RX_TO_ME, "I2NEW", FTX_MSG_TYPE_GRID, -15, 1600);

    wk.worked_type = SEARCH_WORKED_SAME_MODE;
    nw.new_dxcc    = true;
    nw.dist        = 16000;
    ft8_heard_clear();
    ft8_heard_add(&qso, t);
    ft8_heard_add(&wk, t);
    ft8_heard_add(&cq2, t);
    ft8_heard_add(&nw, t);
    ft8_heard_add(&me1, t);
    ft8_heard_add(&cq1, t);
    ft8_heard_add(&me2, t + 15000);

    const ft8_heard_t *st[FT8_HEARD_MAX];
    const char        *want[] = {"I2NEW", "I1OLD", "VK2XX", "DL2AA", "DL3BB", "W1AW", "F5ZZZ"};
    int                n      = ft8_heard_list(st, FT8_HEARD_MAX, true, t + 16000);
    bool               same   = n == 7;

    for (int i = 0; same && i < n; i++) same = !strcmp(st[i]->cd.meta.call_de, want[i]);
    check(same, "the order: calling us (newest first), NEW CQ, CQs strongest first, worked, the others");
    check(ft8_heard_tag(st[2], t + 16000) == FT8_TAG_NEW, "a CQ of a new country: NEW");
    check(ft8_heard_tag(st[3], t + 16000) == FT8_TAG_CQ, "a CQ not worked: CQ");
    check(ft8_heard_tag(st[5], t + 16000) == FT8_TAG_WORKED, "a CQ worked: worked");
    check(ft8_heard_tag(st[6], t + 16000) == FT8_TAG_QSO, "another QSO: QSO");

    /* a report carries no grid: the distance its CQ gave stays */
    cell_data_t nw_rep = row(CELL_RX_MSG, "VK2XX", FTX_MSG_TYPE_REPORT, -17, 1000);

    ft8_heard_add(&nw_rep, t + 30000);
    check(find("VK2XX", true, t + 30000)->cd.dist == 16000, "the distance lost with a report");

    /* no call, or a hashed one: no tile */
    cell_data_t none = row(CELL_RX_MSG, "", FXT_MSG_TYPE_OTHER, 0, 500);
    cell_data_t hash = row(CELL_RX_TO_ME, "<...>", FTX_MSG_TYPE_REPORT, 0, 500);
    cell_data_t slot = row(CELL_RX_INFO, "RX", FXT_MSG_TYPE_OTHER, 0, 0);

    ft8_heard_add(&none, t + 30000);
    ft8_heard_add(&hash, t + 30000);
    ft8_heard_add(&slot, t + 30000);
    check(ft8_heard_list(st, FT8_HEARD_MAX, true, t + 30000) == 7, "a row with no call made a tile");
}

static void sixty_four(void) {
    uint32_t t = 4000000;
    char     call[16];

    ft8_heard_clear();
    for (int i = 0; i < FT8_HEARD_MAX + 1; i++) {
        snprintf(call, sizeof(call), "DL%dXX", i);
        cell_data_t cd = row(CELL_RX_CQ, call, FTX_MSG_TYPE_CQ, -10, 1000);
        ft8_heard_add(&cd, t + (uint32_t)i * 10);
    }

    const ft8_heard_t *st[FT8_HEARD_MAX];

    check(ft8_heard_list(st, FT8_HEARD_MAX, true, t + 1000) == FT8_HEARD_MAX, "64 stations kept");
    check(!find("DL0XX", true, t + 1000) && find("DL64XX", true, t + 1000), "full: the one heard longest ago goes");
}

static void countries(void) {
    ft8_worked_clear();
    check(!ft8_worked_has("DL"), "an empty log has no country");
    ft8_worked_add("DL");
    ft8_worked_add("DL");
    ft8_worked_add("");
    ft8_worked_add(NULL);
    ft8_worked_add("ON");
    check(ft8_worked_has("DL") && ft8_worked_has("ON") && !ft8_worked_has("F"), "the countries of the log");
    for (int i = 0; i < 500; i++) {
        char p[8];
        snprintf(p, sizeof(p), "X%d", i);
        ft8_worked_add(p);
    }
    check(ft8_worked_has("X0") && ft8_worked_has("DL"), "a long list keeps the first");
    ft8_worked_clear();
    check(!ft8_worked_has("DL"), "cleared");
}

int main(void) {
    slot_start();
    red_three_minutes();
    show_cq();
    order_and_tags();
    sixty_four();
    countries();
    printf("ft8_tiles: %d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
