/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  FreeDV Reporter (qso.freedv.org) for the RADE app, #37: while the app is open with Reporter on, the radio shows on
 *  FreeDV's public list of the stations on the air. The connection is rootfs/usr/sbin/x6100-freedv-reporter, a process
 *  of its own (Python and its ssl module, both in the image): this starts it, writes it a line for each event over a
 *  socket pair, reads its lines back, and ends it.
 *
 *  Nothing here waits on the network or on the helper: the writes are MSG_DONTWAIT (a line that doesn't fit is
 *  dropped), the reads too, and the stop is a close of the socket and a thread that reaps the helper (SIGTERM after
 *  1 s, SIGKILL after 2 s). A helper that is gone (crashed, no Python) costs nothing: no SIGPIPE, the writes fail.
 *  No LVGL in here: tools/freedv_report tests it on a PC. All of it from one thread (the LVGL thread).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "freedv_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREEDV_REPORT_MSG_MAX 64    /* characters of the status message (the helper keeps as many) */

/* Starts the helper (false: it could not be started). version: "OpenX6X00 0.3.12" */
bool freedv_report_start(const char *call, const char *grid, bool rx_only, uint32_t freq_hz, const char *version);

/* Ends it; returns at once (the reaper thread waits for it). Nothing happens when it does not run */
void freedv_report_stop(void);

bool freedv_report_running(void);

/* The events; each one sent only when it changes (rx always). Nothing happens when the helper does not run */
void freedv_report_freq(uint32_t hz);
void freedv_report_tx(bool on);
void freedv_report_rx(const char *call, int snr_db);
void freedv_report_message(const char *text);

/* Every UI tick with the receiver's numbers: an End-of-Over that carries a callsign (rx.st.callsign, set by
 * freedv_rx.c) goes out as rx_report with the last SNR the sync had, the End-of-Over ends the sync. The callsign may
 * come up to a second after the frame; the receiver started again counts from 0 */
void freedv_report_rx_stats(const freedv_rx_stats_t *s);

/* One line of the helper ("connected", "offline <why>", "qsy <call> <hz> <text>", "error <text>") without the line
 * end: true, or false when there is none now. When the helper is gone, "gone" once, and it is stopped */
bool freedv_report_poll(char *line, size_t size);

/* The status message, kept on the DATA partition (freedv_reporter.txt): "" when there is none */
void freedv_report_message_load(char *buf, size_t size);
bool freedv_report_message_save(const char *text);

/* The helper to run, for the tests (default X6100_SBIN "/x6100-freedv-reporter") */
void freedv_report_set_helper(const char *path);

#ifdef __cplusplus
}
#endif
