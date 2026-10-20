/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - PSK Reporter upload
 */

#include "pskreporter.h"

#include <ctype.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HOST         "report.pskreporter.info"
#define PORT         "4739"
#define PEN          30351          /* PSK Reporter's IANA enterprise number */
#define RX_TEMPLATE  0x9992
#define TX_TEMPLATE  0x9993
#define SEND_S       300            /* at most one batch in 5 minutes... */
#define JITTER_S     30             /* ...give or take, so radios don't send in step */
#define DUP_S        300            /* a call once per band in 5 minutes */
#define QUEUE_MAX    256
#define RECENT_MAX   512
#define PER_PACKET   25             /* ~1.2 kB, under the MTU */
#define TEMPLATE_S   3600           /* templates in the first 3 packets, then hourly */

/* ---- IPFIX message ------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t   n, size;
    bool     full;
} wr_t;

static void put8(wr_t *w, uint8_t v) {
    if (w->n >= w->size) {
        w->full = true;
        return;
    }
    w->p[w->n++] = v;
}

static void put16(wr_t *w, uint16_t v) {
    put8(w, v >> 8);
    put8(w, v);
}

static void put32(wr_t *w, uint32_t v) {
    put16(w, v >> 16);
    put16(w, v);
}

static void put_str(wr_t *w, const char *s) {
    size_t len = strlen(s);
    if (len > 254) len = 254;
    put8(w, len);
    for (size_t i = 0; i < len; i++) put8(w, s[i]);
}

/* An enterprise field: id | 0x8000, length (0xffff: variable), PEN */
static void field(wr_t *w, uint16_t id, uint16_t len) {
    put16(w, 0x8000 | id);
    put16(w, len);
    put32(w, PEN);
}

static size_t set_begin(wr_t *w, uint16_t id) {
    size_t at = w->n;
    put16(w, id);
    put16(w, 0);    /* the length, once known */
    return at;
}

/* Padded to 4 bytes; the length counts the padding */
static void set_end(wr_t *w, size_t at) {
    while ((w->n - at) % 4) put8(w, 0);
    if (w->full) return;
    w->p[at + 2] = (w->n - at) >> 8;
    w->p[at + 3] = (w->n - at);
}

size_t pskr_packet(uint8_t *buf, size_t size, const char *rcall, const char *rgrid, const char *software,
                   const pskr_spot_t *spots, int n, bool templates, uint32_t seq, uint32_t domain, uint32_t now) {
    wr_t   w = {buf, 0, size, false};
    size_t s;

    put16(&w, 10);      /* IPFIX */
    put16(&w, 0);       /* the length, at the end */
    put32(&w, now);
    put32(&w, seq);
    put32(&w, domain);

    if (templates) {
        s = set_begin(&w, 3);   /* options template: the receiver */
        put16(&w, RX_TEMPLATE);
        put16(&w, 4);           /* fields */
        put16(&w, 0);           /* scope fields */
        field(&w, 2, 0xffff);   /* receiverCallsign */
        field(&w, 4, 0xffff);   /* receiverLocator */
        field(&w, 8, 0xffff);   /* decodingSoftware */
        field(&w, 9, 0xffff);   /* antennaInformation */
        set_end(&w, s);

        s = set_begin(&w, 2);   /* template: the senders */
        put16(&w, TX_TEMPLATE);
        put16(&w, 7);
        field(&w, 1, 0xffff);   /* senderCallsign */
        field(&w, 5, 4);        /* frequency */
        field(&w, 6, 1);        /* sNR */
        field(&w, 10, 0xffff);  /* mode */
        field(&w, 11, 1);       /* informationSource */
        field(&w, 3, 0xffff);   /* senderLocator */
        put16(&w, 150);         /* flowStartSeconds (IANA) */
        put16(&w, 4);
        set_end(&w, s);
    }

    s = set_begin(&w, RX_TEMPLATE);
    put_str(&w, rcall);
    put_str(&w, rgrid);
    put_str(&w, software);
    put_str(&w, "");
    set_end(&w, s);

    if (n > 0) {
        s = set_begin(&w, TX_TEMPLATE);
        for (int i = 0; i < n; i++) {
            put_str(&w, spots[i].call);
            put32(&w, spots[i].freq_hz);
            put8(&w, (uint8_t)spots[i].snr);
            put_str(&w, spots[i].mode);
            put8(&w, 1);        /* automatically extracted */
            put_str(&w, spots[i].grid);
            put32(&w, spots[i].t);
        }
        set_end(&w, s);
    }

    if (w.full) return 0;
    buf[2] = w.n >> 8;
    buf[3] = w.n;
    return w.n;
}

/* ---- Queue and sender thread --------------------------------------------- */

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  once  = PTHREAD_ONCE_INIT;
static pskr_spot_t     queue[QUEUE_MAX];
static int             queued;
static char            my_call[16], my_grid[5], software[48];

static struct {
    char     call[16];
    uint32_t mhz;       /* the band, roughly */
    time_t   t;
} recent[RECENT_MAX];

static void send_batch(const pskr_spot_t *spots, int n, const char *rcall, const char *rgrid, const char *sw) {
    static uint32_t seq, domain;
    static int      templates_left;
    static time_t   templates_t;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    time_t          now = time(NULL);

    if (!domain) domain = (uint32_t)rand() | 1;
    if (now - templates_t >= TEMPLATE_S) {
        templates_left = 3;
        templates_t    = now;
    }
    if (getaddrinfo(HOST, PORT, &hints, &res) != 0) return;   /* no network: these are lost, quietly */

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
        for (int i = 0; i < n; i += PER_PACKET) {
            uint8_t buf[1500];
            int     k   = n - i < PER_PACKET ? n - i : PER_PACKET;
            size_t  len = pskr_packet(buf, sizeof(buf), rcall, rgrid, sw, spots + i, k, templates_left > 0, seq,
                                      domain, (uint32_t)now);
            if (len && sendto(fd, buf, len, 0, res->ai_addr, res->ai_addrlen) == (ssize_t)len) {
                seq += 1 + k;   /* data records sent */
                if (templates_left > 0) templates_left--;
                fprintf(stderr, "PSK Reporter: %d spots, %zu bytes\n", k, len);
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
}

static void *sender(void *arg) {
    (void)arg;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (;;) {
        sleep(SEND_S - JITTER_S + rand() % (2 * JITTER_S + 1));

        static pskr_spot_t batch[QUEUE_MAX];
        char               rcall[16], rgrid[5], sw[48];
        pthread_mutex_lock(&mutex);
        int n = queued;
        memcpy(batch, queue, n * sizeof(batch[0]));
        queued = 0;
        memcpy(rcall, my_call, sizeof(rcall));
        memcpy(rgrid, my_grid, sizeof(rgrid));
        memcpy(sw, software, sizeof(sw));
        pthread_mutex_unlock(&mutex);
        if (n) send_batch(batch, n, rcall, rgrid, sw);
    }
    return NULL;
}

static void start(void) {
    pthread_t      t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, sender, NULL);
    pthread_attr_destroy(&attr);
}

/* A plain call with a digit: not "<...>" (an unresolved hash) nor "CQ" */
static bool call_ok(const char *c) {
    bool digit = false, alpha = false;
    size_t len = strlen(c);
    if (len < 3 || len >= sizeof(queue[0].call)) return false;
    for (; *c; c++) {
        if (isdigit((unsigned char)*c)) digit = true;
        else if (isupper((unsigned char)*c)) alpha = true;
        else if (*c != '/') return false;
    }
    return digit && alpha;
}

static bool grid_ok(const char *g) {
    size_t len = strlen(g);
    return (len == 4 || len == 6) && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && isdigit((unsigned char)g[2]) &&
           isdigit((unsigned char)g[3]) && strcmp(g, "RR73");
}

void pskr_spot(const char *rcall, const char *rgrid, const char *version, const char *call, const char *grid,
               uint32_t freq_hz, int snr, const char *mode) {
    if (!rcall[0] || strlen(rgrid) < 4 || !call_ok(call) || !strcasecmp(call, rcall)) return;

    time_t   now = time(NULL);
    uint32_t mhz = freq_hz / 1000000;
    bool     dup = false;

    pthread_mutex_lock(&mutex);
    int old = 0, i;
    for (i = 0; i < RECENT_MAX && recent[i].call[0]; i++) {
        if (recent[i].mhz == mhz && !strcmp(recent[i].call, call)) break;
        if (recent[i].t < recent[old].t) old = i;
    }
    if (i == RECENT_MAX) i = old;   /* full: the oldest goes */
    if (recent[i].call[0] && recent[i].mhz == mhz && !strcmp(recent[i].call, call) && now - recent[i].t < DUP_S) {
        dup = true;
    } else if (queued < QUEUE_MAX) {
        snprintf(recent[i].call, sizeof(recent[i].call), "%s", call);
        recent[i].mhz = mhz;
        recent[i].t   = now;

        pskr_spot_t *s = &queue[queued++];
        snprintf(s->call, sizeof(s->call), "%s", call);
        snprintf(s->grid, sizeof(s->grid), "%s", grid && grid_ok(grid) ? grid : "");
        snprintf(s->mode, sizeof(s->mode), "%s", mode);
        s->freq_hz = freq_hz;
        s->snr     = snr < -128 ? -128 : snr > 127 ? 127 : snr;
        s->t       = (uint32_t)now;
    }
    snprintf(my_call, sizeof(my_call), "%s", rcall);
    snprintf(my_grid, sizeof(my_grid), "%.4s", rgrid);   /* the square only: privacy */
    const char *v = strrchr(version, ' ');
    snprintf(software, sizeof(software), "OpenX6X00 %s", v ? v + 1 : version);
    pthread_mutex_unlock(&mutex);

    if (!dup) pthread_once(&once, start);
}
