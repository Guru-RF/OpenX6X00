/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Band conditions (conditions.h)
 */

#include "conditions.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif
#ifndef BOARD_DATA
#define BOARD_DATA "/mnt"
#endif

#define HELPER     X6100_SBIN "/x6100-conditions"
#define CACHE      BOARD_DATA "/conditions.txt"
#define EVERY_S    3600
#define FORCE_S    300

const char *cond_group_name[COND_GROUPS] = {"80-40m", "30-20m", "17-15m", "12-10m"};

static const char *group_key[COND_GROUPS] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static conditions_t    cur;
static bool            loaded, running, failed;
static time_t          tried;

void conditions_clear(conditions_t *c) {
    memset(c, 0, sizeof(*c));
    c->sfi = c->ssn = c->a = c->k = -1;
    for (int g = 0; g < COND_GROUPS; g++) c->band[g][0] = c->band[g][1] = COND_UNKNOWN;
}

static cond_rating_t rating(const char *s) {
    if (!strcasecmp(s, "Good")) return COND_GOOD;
    if (!strcasecmp(s, "Fair")) return COND_FAIR;
    if (!strcasecmp(s, "Poor")) return COND_POOR;
    return COND_UNKNOWN;
}

static int number(const char *s) {
    char *end;
    long  v = strtol(s, &end, 10);

    return end == s ? -1 : (int)v;
}

void conditions_parse_line(conditions_t *c, char *line) {
    char *f[4] = {line, NULL, NULL, NULL};
    int   n    = 1;

    line[strcspn(line, "\r\n")] = 0;
    for (char *p = line; *p && n < 4; p++)
        if (*p == '|') {
            *p     = 0;
            f[n++] = p + 1;
        }
    if (n < 2) return;

    const char *k = f[0], *v = f[1];

    if (!strcmp(k, "fetched")) c->fetched = (time_t)atoll(v);
    else if (!strcmp(k, "updated")) snprintf(c->updated, sizeof(c->updated), "%s", v);
    else if (!strcmp(k, "sfi")) c->sfi = number(v);
    else if (!strcmp(k, "ssn")) c->ssn = number(v);
    else if (!strcmp(k, "a")) c->a = number(v);
    else if (!strcmp(k, "k")) c->k = number(v);
    else if (!strcmp(k, "xray")) snprintf(c->xray, sizeof(c->xray), "%s", v);
    else if (!strcmp(k, "geomag")) snprintf(c->geomag, sizeof(c->geomag), "%s", v);
    else if (!strcmp(k, "noise")) snprintf(c->noise, sizeof(c->noise), "%s", v);
    else if (!strcmp(k, "esna")) snprintf(c->es_na, sizeof(c->es_na), "%s", v);
    else if (!strcmp(k, "band") && n == 4) {
        int when = !strcmp(f[2], "night");
        for (int g = 0; g < COND_GROUPS; g++)
            if (!strcmp(v, group_key[g])) c->band[g][when] = rating(f[3]);
    }
    if (c->sfi >= 0 || c->band[1][0] != COND_UNKNOWN) c->have = true;
}

static void read_file(FILE *f, conditions_t *c) {
    char line[160];

    conditions_clear(c);
    while (fgets(line, sizeof(line), f)) conditions_parse_line(c, line);
}

static void load_cache(void) {
    FILE *f = fopen(CACHE, "r");

    conditions_clear(&cur);
    if (f) {
        read_file(f, &cur);
        fclose(f);
    }
    loaded = true;
}

static void *fetch_thread(void *arg) {
    conditions_t c;
    char         out[2048] = "";
    size_t       used      = 0;
    FILE        *p         = popen(HELPER " 2>/dev/null", "r");

    (void)arg;
    if (p) {
        used = fread(out, 1, sizeof(out) - 1, p);
        pclose(p);
    }
    out[used] = 0;

    FILE *m = fmemopen(out, used ? used : 1, "r");
    if (m) {
        read_file(m, &c);
        fclose(m);
    } else {
        conditions_clear(&c);
    }

    pthread_mutex_lock(&lock);
    running = false;
    failed  = !c.have;
    if (c.have) {
        c.fetched = time(NULL);
        cur       = c;

        FILE *f = fopen(CACHE ".tmp", "w");
        if (f) {
            fprintf(f, "fetched|%lld\n%s", (long long)c.fetched, out);
            fclose(f);
            rename(CACHE ".tmp", CACHE);
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void conditions_get(conditions_t *c) {
    pthread_mutex_lock(&lock);
    if (!loaded) load_cache();
    *c = cur;
    pthread_mutex_unlock(&lock);
}

void conditions_refresh(bool force) {
    time_t now = time(NULL);

    pthread_mutex_lock(&lock);
    if (!loaded) load_cache();

    time_t last = cur.fetched > tried ? cur.fetched : tried;
    bool   due  = !running && (now - last >= (force ? FORCE_S : EVERY_S) || now < last);

    if (due) {
        pthread_t t;

        tried   = now;
        running = pthread_create(&t, NULL, fetch_thread, NULL) == 0;
        if (running) pthread_detach(t);
    }
    pthread_mutex_unlock(&lock);
}

const char *conditions_state(void) {
    const char *s;

    pthread_mutex_lock(&lock);
    s = running ? "Fetching" : failed ? "No answer" : "";
    pthread_mutex_unlock(&lock);
    return s;
}

const char *cond_rating_name(cond_rating_t r) {
    switch (r) {
        case COND_GOOD: return "Good";
        case COND_FAIR: return "Fair";
        case COND_POOR: return "Poor";
        default:        return "-";
    }
}

int cond_group_of(int f) {
    if (f >= 3500000 && f <= 7300000) return 0;
    if (f >= 10100000 && f <= 14350000) return 1;
    if (f >= 18068000 && f <= 21450000) return 2;
    if (f >= 24890000 && f <= 29700000) return 3;
    return -1;
}

const char *cond_k_word(int k) {
    if (k < 0) return "";
    if (k <= 2) return "quiet";
    if (k == 3) return "unsettled";
    if (k == 4) return "active";
    return "storm";
}
