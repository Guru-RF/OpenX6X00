/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - I2C writes to the base that fail.
 *
 *  Hany El Imam (1KO125) caught it in his kernel log: "i2c ... sendbytes: NAK bailout" and
 *  "Can't write to i2c: Input/output error", with the TX relay closed and no RF for 8-10 s.
 *  The control library keeps a copy of the base registers and updates it before the write,
 *  so a lost write stayed lost until the 3 s refresh, which runs only in RX: a lost PTT off
 *  kept the radio keyed, a lost frequency landed seconds later.
 *
 *  hw/x6100/hw_x6100.c takes over x6100_control_cmd() (every setter of the library goes
 *  through it): a failed write is sent again and reported here, and radio.c resends the whole
 *  register block after any error and after every un-key. tx_watch is the stuck-TX rule (radio.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BASE_GUARD_ERROR    1u  /* a write failed, even if a resend got it through */
#define BASE_GUARD_UNKEY    2u  /* PTT, modem, tune, SWR scan or calibration went off */

#define TX_WATCH_UNKEY_MS   2000    /* armed this long after an un-key */
#define TX_WATCH_ERROR_MS   10000   /* and after a write error */
#define TX_WATCH_HOLD_MS    1000    /* base TX with nothing keying it from the GUI this long: resend */
#define TX_WATCH_CUT_MS     2000    /* our TX that didn't stop after a write error, this long: power to 0 */

#define TX_WATCH_CUT        1       /* tx_watch_power(): set TX power to 0 now */
#define TX_WATCH_RESTORE    2       /* base back in RX: put the user's power back */

/* From the backend: a write to register reg needed a resend (ok: it got through), the TX bits
 * the GUI sets went off */
void base_guard_write_failed(int reg, bool ok);
void base_guard_unkeyed(void);

/* Events since the last call, cleared. Counts: writes that failed and got through on a resend,
 * writes lost after all tries; reg: the last register that failed */
unsigned base_guard_take(unsigned *resent, unsigned *lost, int *reg);

/* A line to /mnt/base_i2c.log (two files of 16 kB at most) and stderr, one per second at most */
void base_guard_log(const char *fmt, ...);

#define BASE_SILENT_START_MS 10000   /* no line for the flow resets of the first 10 s */
#define BASE_SILENT_EVERY_MS 10000   /* then one line per 10 s at most */

/* Flow from the base stopped: true when it gets a line in the log. start_ms: GUI start,
 * *last_ms: the last line (0 at first) */
bool base_silent_due(uint64_t now_ms, uint64_t start_ms, uint64_t *last_ms);

typedef struct {
    uint64_t armed_until;
    bool     run;           /* base in TX with nothing keying it from the GUI, since run_start */
    uint64_t run_start;
    uint64_t last_fix;
    bool     gui_keyed;     /* last packet: base in TX keyed from the GUI */
    bool     ours;          /* the run went on straight from a TX the GUI keyed */
    bool     error_seen;
    uint64_t error_at;      /* last write error */
    bool     cut;           /* TX power set to 0 by tx_watch_power() */
} tx_watch_t;

void tx_watch_arm(tx_watch_t *w, uint64_t now_ms, uint32_t ms);

/* A write error: arms the watch for TX_WATCH_ERROR_MS and is remembered for the power cut */
void tx_watch_error(tx_watch_t *w, uint64_t now_ms);

/* Each flow packet. True: resend the registers (the PTT off in them) now */
bool tx_watch_check(tx_watch_t *w, uint64_t now_ms, bool base_tx, bool gui_tx);

/* Each flow packet, after tx_watch_check(): TX_WATCH_CUT, TX_WATCH_RESTORE or 0 */
int tx_watch_power(tx_watch_t *w, uint64_t now_ms, bool base_tx);

#ifdef __cplusplus
}
#endif
