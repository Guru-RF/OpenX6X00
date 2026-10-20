/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - WSPRnet spot upload
 */

#include "wsprnet.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HOST        "wsprnet.org"
#define VERSION     "OpenX6X00"
#define TIMEOUT_S   10

static void esc(char *out, size_t size, const char *in) {
    size_t o = 0;

    for (; *in && o + 4 < size; in++) {
        unsigned char c = *in;

        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '.') {
            out[o++] = c;
        } else {
            o += snprintf(out + o, size - o, "%%%02X", c);
        }
    }
    out[o] = 0;
}

void wsprnet_path(char *buf, size_t size, const char *rcall, const char *rgrid, uint32_t dial_hz,
                  time_t cycle, const wspr_spot_t *spot) {
    char      call[40], grid[20];
    struct tm tm;

    esc(call, sizeof(call), rcall);
    esc(grid, sizeof(grid), rgrid);
    if (!spot) {
        snprintf(buf, size,
                 "/post?function=wsprstat&rcall=%s&rgrid=%s&rqrg=%.6f&tpct=0&tqrg=%.6f&dbm=0&version=%s&mode=2",
                 call, grid, dial_hz / 1e6, dial_hz / 1e6, VERSION);
        return;
    }

    char tcall[40], tgrid[20];

    esc(tcall, sizeof(tcall), spot->call);
    esc(tgrid, sizeof(tgrid), spot->loc);
    gmtime_r(&cycle, &tm);
    snprintf(buf, size,
             "/post?function=wspr&rcall=%s&rgrid=%s&rqrg=%.6f&date=%02d%02d%02d&time=%02d%02d"
             "&sig=%d&dt=%.1f&drift=%d&tqrg=%.6f&tcall=%s&tgrid=%s&dbm=%d&version=%s&mode=2",
             call, grid, dial_hz / 1e6, tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             spot->snr, spot->dt, spot->drift, spot->freq_hz / 1e6, tcall, tgrid, spot->dbm, VERSION);
}

/* true on "HTTP/1.x 200" */
static bool get(const char *path) {
    struct addrinfo  hints = {0}, *res = NULL;
    struct timeval   tv = {TIMEOUT_S, 0};
    char             req[640], answer[64] = {0};
    int              fd = -1;
    bool             ok = false;

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(HOST, "80", &hints, &res) != 0) {
        return false;
    }
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));     /* connect() too */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return false;
    }

    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: " HOST "\r\nUser-Agent: " VERSION "\r\n\r\n", path);

    if (send(fd, req, n, MSG_NOSIGNAL) == n && recv(fd, answer, sizeof(answer) - 1, MSG_WAITALL) > 12) {
        ok = !strncmp(answer, "HTTP/1.", 7) && !strncmp(answer + 9, "200", 3);
    }
    close(fd);
    return ok;
}

int wsprnet_post(const char *rcall, const char *rgrid, uint32_t dial_hz, time_t cycle,
                 const wspr_spot_t *spots, int n) {
    char path[512];
    int  ok = 0;

    if (n == 0) {
        wsprnet_path(path, sizeof(path), rcall, rgrid, dial_hz, cycle, NULL);
        return get(path);
    }
    for (int k = 0; k < n; k++) {
        wsprnet_path(path, sizeof(path), rcall, rgrid, dial_hz, cycle, &spots[k]);
        ok += get(path);
    }
    return ok;
}
