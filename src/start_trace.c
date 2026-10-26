/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the GUI's start on the card (start_trace.h)
 */

#include "start_trace.h"

#include "board.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TRACE_DIR   BOARD_DATA "/app_logs"
#define TRACE       TRACE_DIR "/start_trace.txt"
#define TRACE_OLD   TRACE_DIR "/start_trace.1.txt"
#define TRACE_MAX   (64 * 1024)     /* some 15 starts with the init scripts' lines; the ones before go to .1 */

/* x6100-starttrace's file on the system partition, turned over by it at each start: DATA not mounted (BOARD_DATA
 * is then the system's own folder, which the manual does not point at), or read-only (the kernel's answer to FAT
 * errors) */
#ifndef START_TRACE_SYS
#define START_TRACE_SYS "/var/lib/x6100/start_trace.txt"
#endif

static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;

/* BOARD_DATA is a mount point: another filesystem than its parent's. On a PC the data folder is just a folder */
static bool data_mounted(void) {
#ifdef HOST_BUILD
    return true;
#else
    struct stat data, up;

    return stat(BOARD_DATA, &data) == 0 && stat(BOARD_DATA "/..", &up) == 0 && data.st_dev != up.st_dev;
#endif
}

void start_trace(const char *fmt, ...) {
    struct timespec up;
    time_t          now = time(NULL);
    struct tm       tm;
    struct stat     st;
    char            when[32];
    const char     *env = getenv("X6100_START_TRACE");
    FILE           *f;
    va_list         ap;

    /* x6100_daemon after 10 starts in a row: a GUI that keeps dying would push the first ones out of the file */
    if (env && strcmp(env, "off") == 0) {
        return;
    }
    pthread_mutex_lock(&mux);
    f = NULL;
    if (data_mounted()) {
        mkdir(TRACE_DIR, 0755);
        /* The same rule as x6100-starttrace, which writes the init scripts' and x6100_daemon's lines into the file */
        if (stat(TRACE, &st) == 0 && st.st_size > TRACE_MAX) {
            rename(TRACE, TRACE_OLD);
        }
        f = fopen(TRACE, "a");
    }
    if (!f) {
        f = fopen(START_TRACE_SYS, "a");
    }
    if (f) {
        /* The kernel's clock beside the wall clock, which may still be the RTC's at this point, or wrong. UTC: the
         * GUI's own zone (clock_tz_init) is not set for the first line, and the scripts' lines are in UTC too */
        clock_gettime(CLOCK_BOOTTIME, &up);
        gmtime_r(&now, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%SZ", &tm);
        fprintf(f, "%s, up %ld.%02ld s: ", when, (long)up.tv_sec, up.tv_nsec / 10000000L);
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        /* Through to the card now: a start that hangs is usually ended by holding POWER */
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    pthread_mutex_unlock(&mux);
}
