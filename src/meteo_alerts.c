/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Weather warnings as notices on any screen (meteo_alerts.h)
 *
 *  With APP > Weather > Notices on (off by default): every 30 minutes, x6100-meteo alerts for where the radio is
 *  (meteo_position). Each warning for the place itself not shown before (from the country's service yellow and up,
 *  worked out from the forecast orange and up) goes up in the message box for 10 s: only while receiving, with the
 *  screen lit, and not while APP > Weather is open, which shows them itself; one held back waits for that. The ids
 *  shown are kept in /mnt/meteo/seen until their warning is over, so a restart does not show them again.
 *
 *  After the CX8ANM edition of OpenX6X00 (condoruy), whose Meteo app had its alerts on any screen.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "meteo_alerts.h"

#include "board.h"
#include "cJSON.h"
#include "cfg/cfg_api.h"
#include "dialog_meteo.h"
#include "meteo.h"
#include "msg_box.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"

#include "lvgl/lvgl.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SEEN_FILE   BOARD_DATA "/meteo/seen"
#define CHECK_S     (30 * 60)
#define FIRST_S     120         /* after the start: the network up first */
#define SEEN_MAX    64
#define WAIT_MAX    4
#define SHOW_MS     10000       /* a long first line scrolls */

typedef struct {
    char   id[20];
    time_t until;
} seen_t;

typedef struct {
    char   id[20];
    time_t until;
    char   text[200];   /* the message box cuts it to what it holds */
} notice_t;

static seen_t   seen[SEEN_MAX];
static int      seen_n;
static notice_t waiting[WAIT_MAX];
static int      waiting_n;
static bool     running;
static time_t   next_at;

/* ---- the ids shown ---- */

static void seen_load(void) {
    FILE *f = fopen(SEEN_FILE, "r");
    char  line[64];

    seen_n = 0;
    if (!f) return;
    while (seen_n < SEEN_MAX && fgets(line, sizeof(line), f)) {
        long long until;

        if (sscanf(line, "%19s %lld", seen[seen_n].id, &until) == 2) {
            seen[seen_n].until = (time_t)until;
            seen_n++;
        }
    }
    fclose(f);
}

static void seen_save(void) {
    FILE *f;

    mkdir(BOARD_DATA "/meteo", 0755);
    if (!(f = fopen(SEEN_FILE ".tmp", "w"))) return;
    for (int i = 0; i < seen_n; i++) fprintf(f, "%s %lld\n", seen[i].id, (long long)seen[i].until);
    if (fclose(f) == 0) rename(SEEN_FILE ".tmp", SEEN_FILE);
}

static bool is_seen(const char *id) {
    for (int i = 0; i < seen_n; i++)
        if (!strcmp(seen[i].id, id)) return true;
    for (int i = 0; i < waiting_n; i++)
        if (!strcmp(waiting[i].id, id)) return true;
    return false;
}

/* Kept as seen; the oldest goes when the list is full. Those over are dropped. False: it was seen already */
static bool mark_seen(const char *id, time_t until) {
    time_t now = time(NULL);
    int    k   = 0;

    for (int i = 0; i < seen_n; i++) {
        if (!strcmp(seen[i].id, id)) return false;
    }
    for (int i = 0; i < seen_n; i++)
        if (seen[i].until > now) seen[k++] = seen[i];
    seen_n = k;
    if (seen_n == SEEN_MAX) memmove(seen, seen + 1, sizeof(seen[0]) * --seen_n);
    snprintf(seen[seen_n].id, sizeof(seen[0].id), "%s", id);
    seen[seen_n++].until = until;
    return true;
}

static const char *id_of(const cJSON *a) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "id"));

    return id ? id : "";
}

void meteo_alerts_seen(const cJSON *data) {
    const cJSON *a;
    bool         changed = false;
    time_t       until;

    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(data, "alerts")) {
        if (meteo_alert_worth(a, time(NULL), &until)) changed |= mark_seen(id_of(a), until);
    }
    if (changed) seen_save();
}

/* ---- the check: the helper in a thread of its own, its answer back on the LVGL thread ---- */

static cJSON *load_file(const char *path) {
    FILE  *f = fopen(path, "r");
    char  *t = NULL;
    cJSON *j;
    long   len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && len < 1024 * 1024 && (t = malloc(len + 1))) t[fread(t, 1, len, f)] = 0;
    fclose(f);
    if (!t) return NULL;
    j = cJSON_Parse(t);
    free(t);
    return j;
}

static void check_done(void *arg) {
    bool         ok = *(bool *)arg;
    cJSON       *data, *a;
    const cJSON *tz;
    time_t       until;

    running = false;
    if (!ok || !(data = load_file(METEO_ALERTS_FILE))) return;
    tz = cJSON_GetObjectItemCaseSensitive(data, "tz_abbr");
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(data, "alerts")) {
        char title[128], when[64];

        if (waiting_n == WAIT_MAX) break;
        if (!meteo_alert_worth(a, time(NULL), &until) || is_seen(id_of(a))) continue;
        meteo_warning_title(title, sizeof(title), a);
        meteo_warning_when(when, sizeof(when), a, cJSON_GetStringValue(tz));
        notice_t *n = &waiting[waiting_n++];

        snprintf(n->id, sizeof(n->id), "%s", id_of(a));
        n->until = until;
        snprintf(n->text, sizeof(n->text), "%s\n%s", title, when);
    }
    cJSON_Delete(data);
}

static void *check_thread(void *arg) {
    char *cmd = arg, line[200] = "", l[200];
    bool  ok;
    FILE *p = popen(cmd, "r");

    if (p) {
        while (fgets(l, sizeof(l), p)) snprintf(line, sizeof(line), "%s", l);
        pclose(p);
    }
    free(cmd);
    ok = !strncmp(line, "ok", 2);
    scheduler_put(check_done, &ok, sizeof(ok));
    return NULL;
}

static void check_start(void) {
    double      lat, lon;
    const char *src;
    char        args[160], *cmd;
    size_t      size;
    pthread_t   t;

    if (!meteo_position(&lat, &lon, &src)) return;
    meteo_args(args, sizeof(args), lat, lon, src);
    size = sizeof(METEO_HELPER " alerts  " METEO_ALERTS_FILE " 2>/dev/null") + strlen(args);
    if (!(cmd = malloc(size))) return;
    snprintf(cmd, size, METEO_HELPER " alerts %s " METEO_ALERTS_FILE " 2>/dev/null", args);
    running = true;
    if (pthread_create(&t, NULL, check_thread, cmd) == 0) {
        pthread_detach(t);
    } else {
        free(cmd);
        running = false;
    }
}

/* Every 5 s: a notice waiting goes up when it may; the check when it is due */
static void timer_cb(lv_timer_t *t) {
    time_t now = time(NULL);

    (void)t;
    if (!param_i_get(cfg_meteo_alerts)) {
        waiting_n = 0;
        next_at   = 0;
        return;
    }
    if (waiting_n && !dialog_meteo->run && radio_get_state() == RADIO_RX && backlight_seen()) {
        notice_t n = waiting[0];

        memmove(waiting, waiting + 1, sizeof(waiting[0]) * --waiting_n);
        if (mark_seen(n.id, n.until)) {
            seen_save();
            msg_schedule_text_ms(SHOW_MS, "Weather warning. %s", n.text);    /* the title, its time below */
        }
    }
    if (!next_at || next_at - now > CHECK_S) next_at = now + FIRST_S;    /* the start; a clock set back */
    if (!running && now >= next_at && !dialog_meteo->run) {
        next_at = now + CHECK_S;
        check_start();
    }
}

void meteo_alerts_init(void) {
    seen_load();
    lv_timer_create(timer_cb, 5000, NULL);
}
