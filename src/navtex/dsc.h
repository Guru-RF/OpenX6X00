/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - HF/MF DSC receiver (ITU-R M.493), receive only.
 *
 *  Fed with the bits of the NavTex FSK demodulator (same 100 Bd / 170 Hz).
 *  10-bit symbols (7 information bits LSB first, Y = 1, then the count of
 *  B elements MSB first), phasing DX 125 / RX 111..104, every character
 *  sent twice (DX, and RX 4 characters later), EOS three times, ECC.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSC_MAX_CHARS 40

typedef struct {
    int  fmt;          /* 102 area, 112 distress, 114 group, 116 all ships, 120 individual, 123 automatic */
    int  cat;          /* 100 routine, 108 safety, 110 urgency, 112 distress; -1 in a distress alert */
    char to[24];       /* called MMSI, the area ("43N 010E 10x20") or "" for all ships */
    char from[10];     /* self-ID, 9 digits */
    char dmmsi[10];    /* MMSI in distress (alert, acknowledgement, relay), else "" */
    int  tc1, tc2;     /* telecommands, -1 none */
    int  nature;       /* nature of distress, -1 none */
    char pos[32];      /* distress position "43°12'N 010°05'E" (UTF-8), "" unknown */
    char utc[8];       /* "11:58", "" unknown */
    int  subseq;       /* subsequent communications (telecommand), -1 none */
    char freq[24];     /* proposed working frequency "8291 kHz" / "ch 12345", "" none */
    int  eos;          /* 117 ack RQ, 122 ack BQ, 127 */
    bool ecc_ok;
} dsc_call_t;

typedef void (*dsc_call_cb_t)(const dsc_call_t *call, void *userdata);

typedef struct {
    dsc_call_cb_t cb;
    void         *userdata;
    unsigned char bits[80];    /* the last 80 bits (8 symbols), ring */
    int           nbits;
    bool          locked;
    int           pos;         /* stream position of the last symbol (0 = first DX phasing) */
    int           bitcnt;      /* bits of the symbol being received */
    short         sym[12 + 2 * DSC_MAX_CHARS + 8];  /* by stream position, -1 invalid */
} dsc_rx_t;

void dsc_rx_init(dsc_rx_t *rx, dsc_call_cb_t cb, void *userdata);

/* One bit; y = 1 for the Y state (the lower tone). Calls cb when a call ends. */
void dsc_rx_bit(dsc_rx_t *rx, int y);

/* "distress", "urgency", "safety", "routine" or "ack" (distress acknowledgement) */
const char *dsc_cat_name(const dsc_call_t *c);
/* "FIRE / EXPLOSION"... "" for other codes */
const char *dsc_nature_name(int code);
/* "J3E tel.", "test call"... "" for none */
const char *dsc_tc_name(int code);
/* Country of the MMSI's MID (coast 00MID..., group 0MID... too), NULL unknown;
 * *prefix its cty.dat primary prefix for the flag */
const char *dsc_country(const char *mmsi, const char **prefix);

#ifdef __cplusplus
}
#endif
