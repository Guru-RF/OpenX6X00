/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: Reverse Beacon Network client
 */

#define _GNU_SOURCE

#include "rbn.h"

#include "bands.h"
#include "../cluster/spots.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define HOST        "telnet.reversebeacon.net"
#define PORT        "7000"
#define RETRY_S     30
#define MAX_HITS    64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint     gen;            /* a thread runs while its gen is the current one */
static char            my_call[16];
static const char     *state = "off";

/* Spots per band and minute, a ring of FIELD_RBN_MINUTES minutes */
static int             counts[FIELD_RBN_MINUTES][FIELD_RBN_BANDS];
static time_t          count_min[FIELD_RBN_MINUTES];
static field_rbn_hit_t hits[MAX_HITS]; /* ring, newest at hits_head - 1 */
static int             hits_head, hits_n;

/* The longest part between the slashes: the callsign without /P, I/... */
static void base_call(const char *c, char *out, size_t size) {
    size_t best = 0;

    out[0] = 0;
    while (*c) {
        size_t n = strcspn(c, "/");
        if (n > best && n < size) {
            memcpy(out, c, n);
            out[n] = 0;
            best   = n;
        }
        c += n + (c[n] == '/');
    }
}

int field_same_call(const char *a, const char *b) {
    char ba[16], bb[16];

    base_call(a, ba, sizeof(ba));
    base_call(b, bb, sizeof(bb));
    return ba[0] && !strcasecmp(ba, bb);
}

static int number_before(const char *s, const char *word) {
    const char *p = strcasestr(s, word);

    while (p && p > s && p[-1] == ' ')
        p--;
    while (p && p > s && (isdigit((unsigned char)p[-1]) || p[-1] == '-'))
        p--;
    return p ? atoi(p) : 0;
}

void field_rbn_feed_line(const char *line, time_t now) {
    cluster_spot_t s;

    if (!cluster_parse_line(line, &s))
        return;
    int    band = field_band_of(s.freq_hz);
    time_t min  = now / 60;
    int    slot = (int)(min % FIELD_RBN_MINUTES);

    pthread_mutex_lock(&lock);
    if (band >= 0 && band < FIELD_RBN_BANDS) {
        if (count_min[slot] != min) {
            memset(counts[slot], 0, sizeof(counts[slot]));
            count_min[slot] = min;
        }
        counts[slot][band]++;
    }
    if (my_call[0] && field_same_call(s.dx, my_call)) {
        field_rbn_hit_t *h = &hits[hits_head];

        snprintf(h->skimmer, sizeof(h->skimmer), "%.*s", (int)strcspn(s.spotter, "-"), s.spotter);
        h->freq_hz = s.freq_hz;
        h->snr     = number_before(s.comment, "dB");
        h->wpm     = number_before(s.comment, "WPM");
        h->when    = now;
        hits_head  = (hits_head + 1) % MAX_HITS;
        if (hits_n < MAX_HITS)
            hits_n++;
    }
    pthread_mutex_unlock(&lock);
}

int field_rbn_heard(int max_age_s, field_rbn_hit_t *out, int max) {
    time_t now = time(NULL);
    int    n   = 0;

    pthread_mutex_lock(&lock);
    for (int k = 0; k < hits_n && n < max; k++) {
        const field_rbn_hit_t *h   = &hits[(hits_head - 1 - k + MAX_HITS) % MAX_HITS];
        bool                   dup = false;

        /* Each skimmer once, its latest spot */
        for (int i = 0; i < n && !dup; i++)
            dup = !strcmp(out[i].skimmer, h->skimmer);
        if (!dup && now - h->when <= max_age_s)
            out[n++] = *h;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

void field_rbn_activity(int out[FIELD_RBN_BANDS]) {
    time_t min = time(NULL) / 60;

    memset(out, 0, sizeof(int) * FIELD_RBN_BANDS);
    pthread_mutex_lock(&lock);
    for (int s = 0; s < FIELD_RBN_MINUTES; s++)
        if (min - count_min[s] < FIELD_RBN_MINUTES)
            for (int b = 0; b < FIELD_RBN_BANDS; b++)
                out[b] += counts[s][b];
    pthread_mutex_unlock(&lock);
}

const char *field_rbn_state(void) {
    return state;
}

static int open_socket(void) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res, *ai;
    struct timeval  tv    = {.tv_sec = 10};
    int             fd    = -1;

    if (getaddrinfo(HOST, PORT, &hints, &res) != 0)
        return -1;
    for (ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));   /* also bounds connect() */
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

static void session(int fd, unsigned my_gen, const char *call) {
    char   line[512], buf[2048];
    size_t len       = 0;
    bool   logged_in = false;
    time_t started   = time(NULL);

    while (atomic_load(&gen) == my_gen) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int           r   = poll(&pfd, 1, 1000);

        /* The prompt is "Please enter your call:", without a newline after it */
        if (!logged_in && (time(NULL) - started > 5 || (len && strcasestr(line, "call")))) {
            dprintf(fd, "%s\r\n", call);
            logged_in = true;
        }
        if (r < 0 && errno != EINTR)
            return;
        if (r <= 0)
            continue;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];

            if (c == '\r' || c == '\n') {
                if (len) {
                    line[len] = 0;
                    field_rbn_feed_line(line, time(NULL));
                    len = 0;
                }
            } else if (len < sizeof(line) - 1 && (unsigned char)c >= ' ') {
                line[len++] = c;
                line[len]   = 0;
            }
        }
    }
}

/* Only the current thread tells its state */
static void set_state(unsigned my_gen, const char *s) {
    if (atomic_load(&gen) == my_gen)
        state = s;
}

static void *thread_cb(void *arg) {
    unsigned my_gen = (unsigned)(uintptr_t)arg;
    char     call[16];

    pthread_mutex_lock(&lock);
    snprintf(call, sizeof(call), "%s", my_call);
    pthread_mutex_unlock(&lock);
    while (atomic_load(&gen) == my_gen) {
        set_state(my_gen, "connecting");
        int fd = open_socket();
        if (fd >= 0) {
            set_state(my_gen, "connected");
            session(fd, my_gen, call);
            close(fd);
        }
        if (atomic_load(&gen) != my_gen)
            break;
        set_state(my_gen, fd >= 0 ? "connection closed" : "no answer");
        for (int s = 0; s < RETRY_S && atomic_load(&gen) == my_gen; s++)
            sleep(1);
    }
    return NULL;
}

void field_rbn_start(const char *call) {
    pthread_t t;

    pthread_mutex_lock(&lock);
    bool same = !strcasecmp(call, my_call) && strcmp(state, "off");
    snprintf(my_call, sizeof(my_call), "%s", call);
    pthread_mutex_unlock(&lock);
    if (same || !call[0])
        return;
    unsigned g = atomic_fetch_add(&gen, 1) + 1;
    state      = "connecting";
    if (pthread_create(&t, NULL, thread_cb, (void *)(uintptr_t)g) == 0)
        pthread_detach(t);
}

void field_rbn_stop(void) {
    atomic_fetch_add(&gen, 1);
    state = "off";
}
