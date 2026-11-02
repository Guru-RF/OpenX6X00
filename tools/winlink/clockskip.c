/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  For winlink_modems_test.py, as LD_PRELOAD of ardopcf: each SIGUSR1 moves CLOCK_MONOTONIC on 10 minutes, so
 *  its 10 minute ID (9 minutes after a transmission that was not an ID) comes at once, not after the wait.
 *  Nothing else changes. Build with `make clockskip.so` here.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <signal.h>
#include <time.h>

static volatile sig_atomic_t skips;

static void on_usr1(int sig) {
    (void)sig;
    skips++;
}

__attribute__((constructor)) static void init(void) {
    signal(SIGUSR1, on_usr1);
}

int clock_gettime(clockid_t id, struct timespec *tp) {
    static int (*real)(clockid_t, struct timespec *);
    int r;

    if (!real)
        real = (int (*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
    r = real(id, tp);
    if (r == 0 && id == CLOCK_MONOTONIC)
        tp->tv_sec += 600 * (time_t)skips;
    return r;
}
