/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: the activation log, one ADIF file per activation
 *  (MY_SOTA_REF, MY_SIG POTA / MY_POTA_REF, MY_SIG WWBOTA or HOTA / MY_SIG_INFO)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    time_t  t;
    char    call[16];
    int32_t freq_hz;
    char    mode[8];    /* ADIF: CW, SSB, AM, FM, FT8 */
    char    rst_s[4], rst_r[4];
    char    name[48], qth[48], grid[8], country[32];   /* the other station, from the phone's QRZ lookup; "" unknown */
} field_qso_t;

typedef struct {
    char call[16];
    char grid[8];
    char prog;          /* 'S' SOTA, 'P' POTA, 'B' WWBOTA, 'H' HOTA */
    char ref[16];
} field_station_t;

/* ADIF band ("20m"), "" outside the amateur bands */
const char *field_band(int32_t hz);

/* One ADIF record (with <eor>) into buf; returns its length */
size_t field_adif_record(char *buf, size_t size, const field_qso_t *q, const field_station_t *st);

/* Appends the QSO (a header first when the file is new) */
bool field_log_add(const char *path, const field_qso_t *q, const field_station_t *st);

/* What went wrong with field_log_add, from its errno, in words for the operator */
const char *field_log_error(int err);

/* The QSOs of the file, oldest first; returns the count (0 when missing) */
int field_log_read(const char *path, field_qso_t *out, int max);

#ifdef __cplusplus
}
#endif
