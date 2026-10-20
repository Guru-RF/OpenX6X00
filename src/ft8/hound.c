/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  FT8 DXpedition mode, Hound side. The rules follow WSJT-X
 *  (widgets/mainwindow.cpp, GPL v3, K1JT and the WSJT-X team):
 *  - call "FOX MYCALL GRID" above 1000 Hz, in the second period, for 2 minutes at most
 *  - the Fox's report, alone or in a multi-stream message "X RR73; MYCALL <FOX> -12":
 *    answer "FOX MYCALL R-xx" on the Fox's frequency, then 300 Hz off it, for 11 periods at most
 *  - "MYCALL FOX RR73" or "MYCALL RR73; X <FOX> -05": log, stop
 */

#include "hound.h"

#include <ft8lib/hashtable.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hound_call(hound_t *h, const char *my_call, const char *my_grid, const char *fox, const char *fox_grid,
                time_t now, char *tx) {
    memset(h, 0, sizeof(*h));
    snprintf(h->my_call, sizeof(h->my_call), "%s", my_call);
    snprintf(h->fox, sizeof(h->fox), "%s", fox);
    snprintf(h->fox_grid, sizeof(h->fox_grid), "%s", fox_grid);
    h->stage = HOUND_CALLING;
    h->since = now;
    snprintf(tx, 35, "%s %s %.4s", fox, my_call, my_grid);
}

static hound_event_t reply(hound_t *h, const char *rpt, int snr, int freq_hz, time_t now, char *tx) {
    h->stage       = HOUND_REPORT;
    h->since       = now;
    h->have_report = true;
    h->rcvd        = atoi(rpt);
    h->sent        = snr;
    h->fox_freq    = freq_hz;
    h->r_tx        = 0;
    snprintf(tx, 35, "%s %s R%+03d", h->fox, h->my_call, snr);
    return HOUND_REPLY;
}

/* RR73 without a report decoded first: stop, nothing to log */
static hound_event_t rr73(hound_t *h) {
    bool log       = h->have_report;
    h->stage       = HOUND_IDLE;
    h->have_report = false;
    return log ? HOUND_LOGGED : HOUND_NONE;
}

hound_event_t hound_rx(hound_t *h, const char *text, int snr, int freq_hz, bool odd, time_t now, char *tx) {
    if (odd || !h->fox[0]) return HOUND_NONE;

    char  buf[64], *w[6];
    int   n = 0, j = 0;
    for (const char *s = text; *s && j < (int)sizeof(buf) - 1; s++)
        if (*s != '<' && *s != '>') buf[j++] = *s;
    buf[j] = '\0';
    for (char *t = strtok(buf, " "); t && n < 6; t = strtok(NULL, " ")) w[n++] = t;

    if (strchr(text, ';')) { /* "C1 RR73; C2 <FOX> -12" */
        if (n < 5 || (strcmp(w[3], h->fox) && strcmp(w[3], "..."))) return HOUND_NONE;
        if (!strcmp(w[0], h->my_call)) return rr73(h);
        if (!strcmp(w[2], h->my_call)) return reply(h, w[4], snr, freq_hz, now, tx);
        return HOUND_NONE;
    }
    if (n < 3 || strcmp(w[0], h->my_call) || strcmp(w[1], h->fox)) return HOUND_NONE;
    if (!strcmp(w[2], "RR73")) return rr73(h);
    if (w[2][0] == '+' || w[2][0] == '-') return reply(h, w[2], snr, freq_hz, now, tx);
    return HOUND_NONE;
}

int hound_tx_freq(hound_t *h, int cur_hz, unsigned rnd) {
    if (h->stage == HOUND_CALLING) return cur_hz < HOUND_MIN_TX_HZ ? HOUND_MIN_TX_HZ + (int)(rnd % 2000) : cur_hz;
    if (h->stage != HOUND_REPORT) return cur_hz;
    if (h->r_tx++ == 0) return h->fox_freq;
    return h->fox_freq > 600 ? h->fox_freq - 300 : h->fox_freq + 300; /* stays under 900 Hz */
}

bool hound_timed_out(hound_t *h, time_t now) {
    int limit = h->stage == HOUND_CALLING ? HOUND_CALL_S : h->stage == HOUND_REPORT ? HOUND_REPORT_S : 0;
    if (!limit || now - h->since < limit) return false;
    h->stage = HOUND_IDLE;
    return true;
}

void hound_stop(hound_t *h) {
    h->stage = HOUND_IDLE;
}

/* ---- i3=0 n3=1, the Fox's multi-stream message (ft8_lib leaves it out) ----
 * "K1ABC RR73; W9XYZ <KH1/KH7Z> -12": n28a n28b n10 n5, report 2*n5-30
 * (WSJT-X lib/77bit/packjt77.f90). The two calls go through ft8_lib as a
 * standard message; the Fox is the 10-bit hash of a call heard before. */

static uint32_t get_bits(const uint8_t *p, int start, int len) {
    uint32_t v = 0;
    for (int i = start; i < start + len; i++) v = (v << 1) | ((p[i / 8] >> (7 - i % 8)) & 1u);
    return v;
}

static void put_bits(uint8_t *p, int start, int len, uint32_t v) {
    for (int i = start + len - 1; i >= start; i--, v >>= 1) {
        if (v & 1u) p[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    }
}

bool hound_unpack_dxpedition(const ftx_message_t *msg, char *text) {
    if (ftx_message_get_type(msg) != FTX_MESSAGE_TYPE_DXPEDITION) return false;

    uint32_t n28a = get_bits(msg->payload, 0, 28), n28b = get_bits(msg->payload, 28, 28);
    uint32_t n10 = get_bits(msg->payload, 56, 10), n5 = get_bits(msg->payload, 66, 5);
    if (n28a <= 2 || n28b <= 2) return false; /* DE, QRZ, CQ */

    ftx_message_t std = {0};
    put_bits(std.payload, 0, 29, n28a << 1);
    put_bits(std.payload, 29, 29, n28b << 1);
    put_bits(std.payload, 59, 16, 32400 + 1); /* ir 0, igrid4 MAXGRID4+1: no grid */
    put_bits(std.payload, 74, 3, 1);          /* i3 1 */

    char c1[14], c2[14], extra[8], fox[14];
    if (ftx_message_decode_std(&std, &hash_if, c1, c2, extra) != FTX_MESSAGE_RC_OK) return false;
    if (!hash_if.lookup_hash(FTX_CALLSIGN_HASH_10_BITS, n10, fox)) strcpy(fox, "...");
    snprintf(text, FTX_MAX_MESSAGE_LENGTH, "%s RR73; %s <%s> %+03d", c1, c2, fox, 2 * (int)n5 - 30);
    return true;
}
