/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - PSK Reporter upload
 *
 *  IPFIX over UDP to report.pskreporter.info:4739, as WSJT-X does
 *  (https://pskreporter.info/pskdev.html): the receiver (call, 4-char
 *  locator, software) and one record per station heard. Spots are queued
 *  and sent in batches every ~5 minutes from a background thread; each call
 *  is reported once per band in 5 minutes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     call[16];
    char     grid[7];   /* "" when the message has none */
    uint32_t freq_hz;   /* dial + audio offset */
    int8_t   snr;
    char     mode[8];   /* "FT8", "FT4", "FT2", "JS8", "PSK31", "PSK63", "OLIVIA" */
    uint32_t t;         /* when heard, UTC seconds */
} pskr_spot_t;

/* One IPFIX message: the header, the two templates when asked, the receiver
 * record and the n sender records. Returns its length, 0 when it does not fit. */
size_t pskr_packet(uint8_t *buf, size_t size, const char *rcall, const char *rgrid, const char *software,
                   const pskr_spot_t *spots, int n, bool templates, uint32_t seq, uint32_t domain, uint32_t now);

/* Queues a station heard (never blocks). rgrid is cut to 4 characters; version is
 * main.h's VERSION. Our own call is skipped. */
void pskr_spot(const char *rcall, const char *rgrid, const char *version, const char *call, const char *grid,
               uint32_t freq_hz, int snr, const char *mode);

#ifdef __cplusplus
}
#endif
