// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - the FT8/FT4/FT2 stations heard: the tiles and the calls over the waterfall
 *
 *  Written after F4OLI's redesign of the app (OlivLoze's fork), see heard.h.
 */

#include "heard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ft8_heard_t heard[FT8_HEARD_MAX];
static int         heard_n;

/* lv_tick's milliseconds go round every 49 days: the time since `then`, whatever the turn */
static uint32_t since(uint32_t now, uint32_t then) {
    return now - then;
}

bool ft8_heard_calls_you(const ft8_heard_t *st, uint32_t now_ms) {
    return st->called && !st->logged && since(now_ms, st->called_ms) < FT8_CALLS_YOU_MS;
}

bool ft8_heard_shown(const ft8_heard_t *st, bool show_all, uint32_t now_ms) {
    return show_all || st->cd.cell_type == CELL_RX_CQ || ft8_heard_calls_you(st, now_ms);
}

static void drop_old(uint32_t now_ms) {
    int kept = 0;

    for (int i = 0; i < heard_n; i++)
        if (since(now_ms, heard[i].heard_ms) < FT8_HEARD_KEEP_MS || ft8_heard_calls_you(&heard[i], now_ms))
            heard[kept++] = heard[i];
    heard_n = kept;
}

void ft8_heard_clear(void) {
    heard_n = 0;
}

void ft8_heard_add(const cell_data_t *cd, uint32_t now_ms) {
    ft8_heard_t *st = NULL;

    if (cd->cell_type != CELL_RX_MSG && cd->cell_type != CELL_RX_CQ && cd->cell_type != CELL_RX_TO_ME) return;
    if (!cd->meta.call_de[0] || cd->meta.call_de[0] == '<') return;     /* no call, or one we only know hashed */
    drop_old(now_ms);
    for (int i = 0; i < heard_n && !st; i++)
        if (!strcmp(heard[i].cd.meta.call_de, cd->meta.call_de)) st = &heard[i];
    if (!st) {
        if (heard_n == FT8_HEARD_MAX) {     /* full: the one heard longest ago goes */
            int old = 0;
            for (int i = 1; i < heard_n; i++)
                if (since(now_ms, heard[i].heard_ms) > since(now_ms, heard[old].heard_ms)) old = i;
            heard[old] = heard[--heard_n];
        }
        st = &heard[heard_n++];
        memset(st, 0, sizeof(*st));
    }

    int16_t dist = st->cd.dist;

    st->cd       = *cd;
    st->heard_ms = now_ms;
    if (!cd->dist) st->cd.dist = dist;     /* a report has no grid: the distance its CQ or call gave stays */
    if (cd->cell_type == CELL_RX_TO_ME && cd->meta.type != FTX_MSG_TYPE_RR73 && cd->meta.type != FTX_MSG_TYPE_73) {
        st->called    = true;
        st->logged    = false;
        st->called_ms = now_ms;
        if (cd->meta.type == FTX_MSG_TYPE_REPORT || cd->meta.type == FTX_MSG_TYPE_R_REPORT) {
            st->has_report = true;
            st->report     = cd->meta.remote_snr;
        }
    }
}

void ft8_heard_logged(const char *call) {
    for (int i = 0; i < heard_n; i++)
        if (!strcmp(heard[i].cd.meta.call_de, call)) heard[i].logged = true;
}

/* 0 calling us, 1 a CQ of a new country, 2 a CQ not worked, 3 a CQ worked, 4 the others */
static int rank(const ft8_heard_t *st, uint32_t now_ms) {
    if (ft8_heard_calls_you(st, now_ms)) return 0;
    if (st->cd.cell_type != CELL_RX_CQ) return 4;
    if (st->cd.new_dxcc) return 1;
    return st->cd.worked_type == SEARCH_WORKED_NO ? 2 : 3;
}

static uint32_t order_now;      /* for qsort's compare */

static int order(const void *a, const void *b) {
    const ft8_heard_t *x = *(const ft8_heard_t *const *)a, *y = *(const ft8_heard_t *const *)b;
    int                rx = rank(x, order_now), ry = rank(y, order_now);

    if (rx != ry) return rx - ry;
    if (!rx && x->called_ms != y->called_ms)    /* the newest call first */
        return since(order_now, x->called_ms) < since(order_now, y->called_ms) ? -1 : 1;
    if (x->cd.meta.local_snr != y->cd.meta.local_snr) return y->cd.meta.local_snr - x->cd.meta.local_snr;
    return strcmp(x->cd.meta.call_de, y->cd.meta.call_de);  /* the same order each time */
}

int ft8_heard_list(const ft8_heard_t **out, int max, bool show_all, uint32_t now_ms) {
    int n = 0;

    drop_old(now_ms);
    for (int i = 0; i < heard_n && n < max; i++)
        if (ft8_heard_shown(&heard[i], show_all, now_ms)) out[n++] = &heard[i];
    order_now = now_ms;
    qsort(out, n, sizeof(out[0]), order);
    return n;
}

ft8_tag_t ft8_heard_tag(const ft8_heard_t *st, uint32_t now_ms) {
    if (ft8_heard_calls_you(st, now_ms)) return FT8_TAG_CALLS_YOU;
    if (st->cd.cell_type == CELL_RX_CQ) {       /* calling CQ now: what an answer would get */
        if (st->cd.new_dxcc) return FT8_TAG_NEW;
        return st->cd.worked_type == SEARCH_WORKED_NO ? FT8_TAG_CQ : FT8_TAG_WORKED;
    }
    return st->called ? FT8_TAG_CALLED_YOU : FT8_TAG_QSO;
}

/* ---- the countries of the log ---- */

#define WORKED_MAX 400      /* cty.dat has some 340 */

static char worked[WORKED_MAX][8];
static int  worked_n;

void ft8_worked_clear(void) {
    worked_n = 0;
}

bool ft8_worked_has(const char *prefix) {
    for (int i = 0; i < worked_n; i++)
        if (!strcmp(worked[i], prefix)) return true;
    return false;
}

void ft8_worked_add(const char *prefix) {
    if (!prefix || !prefix[0] || ft8_worked_has(prefix) || worked_n == WORKED_MAX) return;
    snprintf(worked[worked_n++], sizeof(worked[0]), "%s", prefix);
}
