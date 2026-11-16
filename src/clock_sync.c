/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the clock: what keeps it, who may move it (clock_sync.h).
 */

#include "clock_sync.h"

#include "board.h"
#include "btaudio.h"
#include "gps.h"
#include "time_station.h"
#include "cfg/cfg_api.h"
#include "ft8/time_sync.h"
#include "ft8/utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONF_FILE       BOARD_DATA "/clock_sync.conf"
#define NTPD_ASK_S      10          /* ntpd asked at most this often */
#define NTPD_WAIT_MS    200
#define SAVE_GAP_S      120         /* the learned values to the card at most this often, */
#define SAVE_MOVE       0.005f      /* and when one moved this much */
#define RX_DT_FAR       0.3f        /* a minute's median DT this far from the learned one is the band's, not ours */
#define DELAY_FAR       0.1f        /* a marker this far from the learned delay: a bad marker */
#define RX_DT_MIN       -0.5f       /* what a learned DT may be */
#define RX_DT_MAX       1.0f

/* ---- the radio ---- */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static clock_by_t last_by;
static char       last_name[32];
static time_t     last_when;
static double     last_step;

static clock_ft8_word_t ft8_word;

static clock_learned_t rx_dt = {FTX_SYNC_RX_DT_S, 0}, delay = {0.04f, 0};    /* 40 ms: the fork's first guess */
static bool            loaded;
static time_t          saved_at;
static float           saved_rx_dt = NAN, saved_delay = NAN;

/* Under lock */
static void load(void) {
    FILE *f;
    char  line[64];

    if (loaded)
        return;
    loaded = true;
    if ((f = fopen(CONF_FILE, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            float v;
            int   n;

            if (sscanf(line, "ft8_rx_dt %f %d", &v, &n) == 2 && v >= RX_DT_MIN && v <= RX_DT_MAX && n > 0)
                rx_dt = (clock_learned_t){v, n};
            else if (sscanf(line, "station_delay %f %d", &v, &n) == 2 && v > -0.5f && v < 1.0f && n > 0)
                delay = (clock_learned_t){v, n};
        }
        fclose(f);
    }
    saved_rx_dt = rx_dt.value;
    saved_delay = delay.value;
    ftx_sync_set_rx_dt(rx_dt.value);
}

/* Under lock; the card spared: a move of 5 ms or more, two minutes apart */
static void save(void) {
    time_t now = time(NULL);
    FILE  *f;

    if (now - saved_at < SAVE_GAP_S)
        return;
    if (fabsf(rx_dt.value - saved_rx_dt) < SAVE_MOVE && fabsf(delay.value - saved_delay) < SAVE_MOVE)
        return;
    saved_at = now;
    if (!(f = fopen(CONF_FILE ".tmp", "w")))
        return;
    fprintf(f, "ft8_rx_dt %.4f %d\nstation_delay %.4f %d\n", rx_dt.value, rx_dt.n, delay.value, delay.n);
    if (fclose(f) == 0 && rename(CONF_FILE ".tmp", CONF_FILE) == 0) {
        saved_rx_dt = rx_dt.value;
        saved_delay = delay.value;
    } else {
        remove(CONF_FILE ".tmp");
    }
}

/* ntpd's own word (ntpq -c rv), 1, 0, or -1 for no answer: the kernel's STA_UNSYNC is no use right after a step
 * (ours or ntpd's), ntpd clears it only at its next poll, up to 17 minutes on */
static int ntpd_ask(void) {
    static pthread_mutex_t ask = PTHREAD_MUTEX_INITIALIZER;
    static time_t          asked;
    static int             said = -1;
    time_t                 now  = time(NULL);

    pthread_mutex_lock(&ask);
    if (asked && now - asked < NTPD_ASK_S && now >= asked) {
        int s = said;

        pthread_mutex_unlock(&ask);
        return s;
    }
    asked = now;
    said  = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd >= 0) {
        struct sockaddr_in to    = {.sin_family = AF_INET, .sin_port = htons(123)};
        uint8_t            q[12] = {0x16, 2, 0, 1};     /* version 2, mode 6; READVAR, sequence 1, association 0 */
        uint8_t            r[512];
        struct pollfd      pfd   = {.fd = fd, .events = POLLIN};
        ssize_t            got;

        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&to, sizeof(to)) == 0 && send(fd, q, sizeof(q), 0) == sizeof(q) &&
            poll(&pfd, 1, NTPD_WAIT_MS) == 1 && (got = recv(fd, r, sizeof(r), 0)) > 0)
            said = clock_sync_ntpd_reply(r, (size_t)got);
        close(fd);
    }

    int s = said;

    pthread_mutex_unlock(&ask);
    return s;
}

clock_by_t clock_sync_keeper(int *err_ms) {
    double     lat, lon;
    int        age, err = 0;
    bool       gps    = gps_last_fix(&lat, &lon, &age) && age <= CLOCK_SYNC_GPS_AGE_S;
    bool       kernel = !gps && ftx_clock_synced(&err);
    clock_by_t by     = clock_sync_keeper_of(gps, gps ? -1 : ntpd_ask(), kernel);

    if (err_ms)
        *err_ms = by == CLOCK_BY_NTP ? err : 0;
    return by;
}

bool clock_sync_allowed(clock_by_t by, clock_by_t *keeper) {
    clock_by_t k = clock_sync_keeper(NULL);

    pthread_mutex_lock(&lock);
    clock_by_t lb    = last_by;
    long       since = last_when ? (long)(time(NULL) - last_when) : -1;
    pthread_mutex_unlock(&lock);

    if (keeper)
        *keeper = k;
    return clock_sync_may_step(by, k, lb, since, param_i_get(cfg_ft8_auto_sync));
}

/* Into the clock chip too, in UTC as the kernel reads it at boot: sh_run runs it on a thread of its own (hwclock
 * waits for the second to set it) */
static void rtc_write(void) {
    sh_run("hwclock -w -u -f " BOARD_RTC_DEV, NULL, NULL);
}

bool clock_sync_step(double step, clock_by_t by, const char *name) {
    struct timespec now;
    int64_t         ns;

    clock_gettime(CLOCK_REALTIME, &now);
    ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + (int64_t)llround(step * 1e9);

    struct timespec to = {.tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL};

    if (clock_settime(CLOCK_REALTIME, &to) != 0) {
        int why = errno;    /* for the caller's message, past the printf */

        printf("CLOCK: can't set the clock: %s\n", strerror(why));
        errno = why;
        return false;
    }
    rtc_write();
    time_station_restart();     /* its markers so far were of the clock before */

    pthread_mutex_lock(&lock);
    last_by   = by;
    last_when = to.tv_sec;
    last_step = step;
    snprintf(last_name, sizeof(last_name), "%s", name ? name : clock_sync_by_name(by));
    pthread_mutex_unlock(&lock);
    printf("CLOCK: %+.3f s by %s (%s)\n", step, clock_sync_by_name(by), name ? name : "");
    return true;
}

bool clock_sync_last(clock_by_t *by, char *name, size_t size, time_t *when, double *step) {
    pthread_mutex_lock(&lock);
    bool any = last_when != 0;

    *by   = last_by;
    *when = last_when;
    *step = last_step;
    snprintf(name, size, "%s", last_name);
    pthread_mutex_unlock(&lock);
    return any;
}

/* ---- FT8 ---- */

void clock_sync_ft8_word(int rc, float step, float median_dt, int stations, int slots) {
    pthread_mutex_lock(&lock);
    ft8_word = (clock_ft8_word_t){true, rc, step, median_dt, stations, slots, time(NULL)};
    pthread_mutex_unlock(&lock);
}

void clock_sync_ft8_get(clock_ft8_word_t *out) {
    pthread_mutex_lock(&lock);
    *out = ft8_word;
    pthread_mutex_unlock(&lock);
}

float clock_sync_ft8_rx_dt(int *n) {
    pthread_mutex_lock(&lock);
    load();
    float v = rx_dt.value;

    if (n)
        *n = rx_dt.n;
    pthread_mutex_unlock(&lock);
    return v;
}

void clock_sync_ft8_rx_dt_learn(float median_dt) {
    if (!isfinite(median_dt) || median_dt < RX_DT_MIN || median_dt > RX_DT_MAX)
        return;
    pthread_mutex_lock(&lock);
    load();
    if (clock_sync_learn(&rx_dt, median_dt, RX_DT_FAR)) {
        ftx_sync_set_rx_dt(rx_dt.value);
        save();
    }
    pthread_mutex_unlock(&lock);
}

/* ---- time stations ---- */

float clock_sync_station_delay(int *n) {
    pthread_mutex_lock(&lock);
    load();
    float v = delay.value;

    if (n)
        *n = delay.n;
    pthread_mutex_unlock(&lock);
    return v;
}

void clock_sync_station_delay_learn(float d) {
    if (!isfinite(d) || d < -0.5f || d > 1.0f)
        return;
    pthread_mutex_lock(&lock);
    load();
    if (clock_sync_learn(&delay, d, DELAY_FAR))
        save();
    pthread_mutex_unlock(&lock);
}
