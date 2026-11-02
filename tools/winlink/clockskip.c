/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  For winlink_modems_test.py, as LD_PRELOAD of ardopcf: each SIGUSR1 moves CLOCK_MONOTONIC on 10 minutes, so
 *  its 10 minute ID (9 minutes after a transmission that was not an ID) comes at once, not after the wait.
 *  Nothing else changes. SIGUSR1 is blocked in ardopcf's thread and taken by a thread of ours (sigwait), which
 *  blocks every other signal: no system call of ardopcf's is cut short by it (its host port's select() with
 *  EINTR drops the host connection). Build with `make clockskip.so` here.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

static _Atomic int skips;

static void *on_usr1(void *arg) {
    sigset_t usr1;
    int sig;

    (void)arg;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    for (;;)
        if (sigwait(&usr1, &sig) == 0)
            skips++;
    return NULL;
}

__attribute__((constructor)) static void init(void) {
    sigset_t all, old;
    pthread_t t;

    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);       /* ours starts with every signal blocked */
    if (pthread_create(&t, NULL, on_usr1, NULL) == 0)
        pthread_detach(t);
    sigaddset(&old, SIGUSR1);
    pthread_sigmask(SIG_SETMASK, &old, NULL);       /* ardopcf's: SIGUSR1 blocked, its threads inherit that */
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
