/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - I2C writes to the base that fail (base_guard.h)
 */

#include "base_guard.h"
#include "board.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef BASE_GUARD_LOG
#define BASE_GUARD_LOG  BOARD_DATA "/base_i2c.log"
#endif
#define LOG_MAX         (16 * 1024)

static atomic_uint  events;
static atomic_uint  resent;
static atomic_uint  lost;
static atomic_int   last_reg = -1;

void base_guard_write_failed(int reg, bool ok) {
    atomic_store(&last_reg, reg);
    atomic_fetch_add(ok ? &resent : &lost, 1);
    atomic_fetch_or(&events, BASE_GUARD_ERROR);
}

void base_guard_unkeyed(void) {
    atomic_fetch_or(&events, BASE_GUARD_UNKEY);
}

unsigned base_guard_take(unsigned *r, unsigned *l, int *reg) {
    *r = atomic_exchange(&resent, 0);
    *l = atomic_exchange(&lost, 0);
    *reg = atomic_load(&last_reg);
    return atomic_exchange(&events, 0);
}

bool base_silent_due(uint64_t now_ms, uint64_t start_ms, uint64_t *last_ms) {
    if (now_ms - start_ms < BASE_SILENT_START_MS || (*last_ms && now_ms - *last_ms < BASE_SILENT_EVERY_MS)) {
        return false;
    }
    *last_ms = now_ms;
    return true;
}

void base_guard_log(const char *fmt, ...) {
    static time_t   last;
    static unsigned skipped;
    time_t          t = time(NULL);
    char            line[200], stamp[24];
    struct tm       tm;
    struct stat     st;
    va_list         ap;
    FILE            *f;

    if (t == last) {
        skipped++;
        return;
    }
    last = t;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "base i2c: %s\n", line);

    if (stat(BASE_GUARD_LOG, &st) == 0 && st.st_size > LOG_MAX) {
        rename(BASE_GUARD_LOG, BASE_GUARD_LOG ".1");
    }
    if (!(f = fopen(BASE_GUARD_LOG, "a"))) {
        return;
    }
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", gmtime_r(&t, &tm));
    fprintf(f, "%s UTC %s", stamp, line);
    if (skipped) {
        fprintf(f, " (%u more in the same second)", skipped);
        skipped = 0;
    }
    fputc('\n', f);
    fclose(f);
}

void tx_watch_arm(tx_watch_t *w, uint64_t now_ms, uint32_t ms) {
    if (now_ms + ms > w->armed_until) {
        w->armed_until = now_ms + ms;
    }
}

/* Only a TX that began (or was still on) inside the window after an un-key or a write error
 * counts: the flow tells no PTT source apart, and a hand mic, VOX or the radio's own key
 * are keyed by the base itself. Resending is safe anyway: PTT off is already what the GUI
 * holds, it can't unkey what the base keys on its own */
bool tx_watch_check(tx_watch_t *w, uint64_t now_ms, bool base_tx, bool gui_tx) {
    bool gui_keyed = w->gui_keyed;

    w->gui_keyed = base_tx && gui_tx;
    if (!base_tx || gui_tx) {
        w->run = false;
        return false;
    }
    if (!w->run) {
        w->run = true;
        w->run_start = now_ms;
        w->ours = gui_keyed;
    }
    if (w->run_start > w->armed_until) {
        return false;
    }
    if (now_ms - w->run_start < TX_WATCH_HOLD_MS || now_ms - w->last_fix < TX_WATCH_HOLD_MS) {
        return false;
    }
    w->last_fix = now_ms;
    return true;
}

void tx_watch_error(tx_watch_t *w, uint64_t now_ms) {
    tx_watch_arm(w, now_ms, TX_WATCH_ERROR_MS);
    w->error_seen = true;
    w->error_at = now_ms;
}

/* PTT off may not be enough: a base that missed it can keep the carrier up. Power goes to 0
 * only for a TX the GUI keyed that didn't stop on our un-key, near a write error. A hand mic
 * or VOX TX starts from RX, the GUI not keying it: never ours, never cut */
int tx_watch_power(tx_watch_t *w, uint64_t now_ms, bool base_tx) {
    if (w->cut) {
        if (base_tx) {
            return 0;
        }
        w->cut = false;
        return TX_WATCH_RESTORE;
    }
    if (w->run && w->ours && w->error_seen && w->error_at + TX_WATCH_ERROR_MS >= w->run_start
        && now_ms - w->run_start >= TX_WATCH_CUT_MS) {
        w->cut = true;
        return TX_WATCH_CUT;
    }
    return 0;
}
