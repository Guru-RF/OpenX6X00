/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "band_plan.h"

#include "band_names.h"
#include "cfg/cfg_api.h"
#include "dxcc.h"
#include "gps.h"
#include "itu_region.h"
#include "params/params.h"
#include "qth/qth.h"
#include "scheduler.h"
#include "styles.h"
#include "theme.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fonts/font_select.h"

/* VISION: the sizes of the other themes here (an overlay on VISION's shorter waterfall) */
#define NF(n) (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS) : &sony_##n)

#define WIDTH   800
#define POOL    16      /* segments in view: a span is 100 kHz at most */
#define MAX_SEG 128     /* a plan (60 segments at most: US General) and the bands added to the band list */
#define BANDS   48
#define OPA     LV_OPA_80     /* the waterfall shows through a little, the labels stay readable */

extern lv_font_t sony_18;


typedef struct {
    int32_t   start, end;   /* Hz */
    bp_type_t type;
} bp_seg_t;

/* The built-in plans, 160 m to 10 m and 6 m: our own reduction of the documents to the segment edges and one of our
 * types each, adjacent ranges of the same type merged. From
 *   IARU Region 1: the HF band plan effective from 16 Oct 2020, 6 m from the VHF plan
 *   IARU Region 2: the band plan of September 2020 (re-issued in 2022 with typo fixes only)
 *   IARU Region 3: the band plan R3-004 Rev. 2, November 2024
 *   the US: 47 CFR Part 97 (FCC) 97.301, 97.303(h), 97.305 and 97.307(f) as of October 2026, 60 m as changed on
 *   14 Jan 2026 (91 FR 1430), with the RTTY/data, packet and beacon windows of the ARRL band plan
 * The same rules for all of them: CW where the plan has CW only; DIG for narrow band and digital modes without phone,
 * and the "all modes, digimodes" bits between them and the beacon on 20, 17 and 15 m; BCN for beacons only; ALL where
 * phone goes. 21120-21149 is CW in Regions 1 and 2 (narrow band modes, all modes in 500 Hz) */
/* IARU Region 1 */
static const bp_seg_t plan_r1[] = {
    {  1810000,  1838000, BP_CW  }, {  1838000,  1843000, BP_DIG }, {  1843000,  2000000, BP_ALL },
    {  3500000,  3570000, BP_CW  }, {  3570000,  3600000, BP_DIG }, {  3600000,  3800000, BP_ALL },
    {  5351500,  5354000, BP_CW  }, {  5354000,  5366000, BP_ALL }, {  5366000,  5366500, BP_DIG },  /* WRC-15 */
    {  7000000,  7040000, BP_CW  }, {  7040000,  7050000, BP_DIG }, {  7050000,  7200000, BP_ALL },
    { 10100000, 10130000, BP_CW  }, { 10130000, 10150000, BP_DIG },
    { 14000000, 14070000, BP_CW  }, { 14070000, 14099000, BP_DIG }, { 14099000, 14101000, BP_BCN },
    { 14101000, 14112000, BP_DIG }, { 14112000, 14350000, BP_ALL },
    { 18068000, 18095000, BP_CW  }, { 18095000, 18109000, BP_DIG }, { 18109000, 18111000, BP_BCN },
    { 18111000, 18120000, BP_DIG }, { 18120000, 18168000, BP_ALL },
    { 21000000, 21070000, BP_CW  }, { 21070000, 21120000, BP_DIG }, { 21120000, 21149000, BP_CW  },
    { 21149000, 21151000, BP_BCN }, { 21151000, 21450000, BP_ALL },
    { 24890000, 24915000, BP_CW  }, { 24915000, 24929000, BP_DIG }, { 24929000, 24931000, BP_BCN },
    { 24931000, 24990000, BP_ALL },
    { 28000000, 28070000, BP_CW  }, { 28070000, 28190000, BP_DIG }, { 28190000, 28225000, BP_BCN },
    { 28225000, 29700000, BP_ALL },
    { 50000000, 50100000, BP_CW  }, { 50100000, 50300000, BP_ALL }, { 50300000, 50400000, BP_DIG },
    { 50400000, 50500000, BP_BCN }, { 50500000, 52000000, BP_ALL },
};

/* IARU Region 2. 75 m phone to 4 MHz, 40 m to 7.3 MHz */
static const bp_seg_t plan_r2[] = {
    {  1800000,  1810000, BP_DIG }, {  1810000,  1839000, BP_CW  }, {  1839000,  1840000, BP_DIG },
    {  1840000,  2000000, BP_ALL },
    {  3500000,  3570000, BP_CW  }, {  3570000,  3600000, BP_DIG }, {  3600000,  4000000, BP_ALL },
    {  5351500,  5354000, BP_CW  }, {  5354000,  5366000, BP_ALL }, {  5366000,  5366500, BP_DIG },
    {  7000000,  7040000, BP_CW  }, {  7040000,  7050000, BP_DIG }, {  7050000,  7300000, BP_ALL },
    { 10100000, 10130000, BP_CW  }, { 10130000, 10150000, BP_DIG },
    { 14000000, 14070000, BP_CW  }, { 14070000, 14099000, BP_DIG }, { 14099000, 14101000, BP_BCN },
    { 14101000, 14112000, BP_DIG }, { 14112000, 14350000, BP_ALL },
    { 18068000, 18095000, BP_CW  }, { 18095000, 18109000, BP_DIG }, { 18109000, 18111000, BP_BCN },
    { 18111000, 18120000, BP_DIG }, { 18120000, 18168000, BP_ALL },
    { 21000000, 21070000, BP_CW  }, { 21070000, 21120000, BP_DIG }, { 21120000, 21149000, BP_CW  },
    { 21149000, 21151000, BP_BCN }, { 21151000, 21450000, BP_ALL },
    { 24890000, 24915000, BP_CW  }, { 24915000, 24929000, BP_DIG }, { 24929000, 24931000, BP_BCN },
    { 24931000, 24990000, BP_ALL },
    { 28000000, 28070000, BP_CW  }, { 28070000, 28190000, BP_DIG }, { 28190000, 28225000, BP_BCN },
    { 28225000, 29700000, BP_ALL },
    { 50000000, 50100000, BP_CW  }, { 50100000, 50400000, BP_ALL }, { 50400000, 50500000, BP_BCN },
    { 50500000, 54000000, BP_ALL },
};

/* IARU Region 3: few parts for CW or data only, most are "CW, phone, digimodes". The beacons get 500 Hz either
 * side; 80 m ends at 3.9 MHz, 40 m at 7.2 */
static const bp_seg_t plan_r3[] = {
    {  1800000,  1830000, BP_CW  }, {  1830000,  1840000, BP_DIG }, {  1840000,  2000000, BP_ALL },
    {  3500000,  3535000, BP_CW  }, {  3535000,  3900000, BP_ALL },
    {  5351500,  5354000, BP_CW  }, {  5354000,  5366000, BP_ALL }, {  5366000,  5366500, BP_DIG },
    {  7000000,  7030000, BP_CW  }, {  7030000,  7200000, BP_ALL },
    { 10100000, 10130000, BP_CW  }, { 10130000, 10150000, BP_DIG },
    { 14000000, 14070000, BP_CW  }, { 14070000, 14099500, BP_DIG }, { 14099500, 14100500, BP_BCN },
    { 14100500, 14350000, BP_ALL },
    { 18068000, 18095000, BP_CW  }, { 18095000, 18109500, BP_DIG }, { 18109500, 18110500, BP_BCN },
    { 18110500, 18168000, BP_ALL },
    { 21000000, 21070000, BP_CW  }, { 21070000, 21149500, BP_DIG }, { 21149500, 21150500, BP_BCN },
    { 21150500, 21450000, BP_ALL },
    { 24890000, 24915000, BP_CW  }, { 24915000, 24929500, BP_DIG }, { 24929500, 24930500, BP_BCN },
    { 24930500, 24990000, BP_ALL },
    { 28000000, 28070000, BP_CW  }, { 28070000, 28190000, BP_DIG }, { 28190000, 28225000, BP_BCN },
    { 28225000, 29700000, BP_ALL },
    { 50000000, 50100000, BP_CW  }, { 50100000, 54000000, BP_ALL },
};

/* The US licence classes, from the most privileges to the fewest; Technician stands for Novice too */
typedef enum { LIC_EXTRA, LIC_ADVANCED, LIC_GENERAL, LIC_TECH } lic_t;

/* The US: what the FCC allows (phone or not, and for which class), the parts without phone split as the ARRL plan
 * uses them. cls: the lowest class that has the segment. No phone below 7.125 MHz on the mainland, nor below 14.150
 * and 21.200 MHz; 75 m phone from 3.6 (Extra), 3.7 (Advanced) and 3.8 MHz (General); 60 m: its 15 kHz and the four
 * channels of 2.8 kHz around 5332, 5348, 5373 and 5405 kHz */
static const struct {
    int32_t   start, end;
    bp_type_t type;
    lic_t     cls;
} plan_us[] = {
    {  1800000,  1810000, BP_DIG, LIC_GENERAL  }, {  1810000,  1840000, BP_CW,  LIC_GENERAL  },
    {  1840000,  1843000, BP_DIG, LIC_GENERAL  }, {  1843000,  2000000, BP_ALL, LIC_GENERAL  },
    {  3500000,  3525000, BP_CW,  LIC_EXTRA    }, {  3525000,  3570000, BP_CW,  LIC_TECH     },
    {  3570000,  3600000, BP_DIG, LIC_TECH     }, {  3600000,  3700000, BP_ALL, LIC_EXTRA    },
    {  3700000,  3800000, BP_ALL, LIC_ADVANCED }, {  3800000,  4000000, BP_ALL, LIC_GENERAL  },
    {  5330600,  5333400, BP_ALL, LIC_GENERAL  }, {  5346600,  5349400, BP_ALL, LIC_GENERAL  },
    {  5351500,  5354000, BP_CW,  LIC_GENERAL  }, {  5354000,  5366000, BP_ALL, LIC_GENERAL  },
    {  5366000,  5366500, BP_DIG, LIC_GENERAL  }, {  5371600,  5374400, BP_ALL, LIC_GENERAL  },
    {  5403600,  5406400, BP_ALL, LIC_GENERAL  },
    {  7000000,  7025000, BP_CW,  LIC_EXTRA    }, {  7025000,  7040000, BP_CW,  LIC_TECH     },
    {  7040000,  7050000, BP_DIG, LIC_TECH     }, {  7050000,  7070000, BP_CW,  LIC_TECH     },
    {  7070000,  7125000, BP_DIG, LIC_TECH     }, {  7125000,  7175000, BP_ALL, LIC_ADVANCED },
    {  7175000,  7300000, BP_ALL, LIC_GENERAL  },
    { 10100000, 10130000, BP_CW,  LIC_GENERAL  }, { 10130000, 10150000, BP_DIG, LIC_GENERAL  },
    { 14000000, 14025000, BP_CW,  LIC_EXTRA    }, { 14025000, 14070000, BP_CW,  LIC_GENERAL  },
    { 14070000, 14099500, BP_DIG, LIC_GENERAL  }, { 14099500, 14100500, BP_BCN, LIC_GENERAL  },
    { 14100500, 14112000, BP_DIG, LIC_GENERAL  }, { 14112000, 14150000, BP_CW,  LIC_GENERAL  },
    { 14150000, 14175000, BP_ALL, LIC_EXTRA    }, { 14175000, 14225000, BP_ALL, LIC_ADVANCED },
    { 14225000, 14350000, BP_ALL, LIC_GENERAL  },
    { 18068000, 18095000, BP_CW,  LIC_GENERAL  }, { 18095000, 18109500, BP_DIG, LIC_GENERAL  },
    { 18109500, 18110500, BP_BCN, LIC_GENERAL  }, { 18110500, 18168000, BP_ALL, LIC_GENERAL  },
    { 21000000, 21025000, BP_CW,  LIC_EXTRA    }, { 21025000, 21070000, BP_CW,  LIC_TECH     },
    { 21070000, 21110000, BP_DIG, LIC_TECH     }, { 21110000, 21149500, BP_CW,  LIC_TECH     },
    { 21149500, 21150500, BP_BCN, LIC_TECH     }, { 21150500, 21200000, BP_CW,  LIC_TECH     },
    { 21200000, 21225000, BP_ALL, LIC_EXTRA    }, { 21225000, 21275000, BP_ALL, LIC_ADVANCED },
    { 21275000, 21450000, BP_ALL, LIC_GENERAL  },
    { 24890000, 24915000, BP_CW,  LIC_GENERAL  }, { 24915000, 24929500, BP_DIG, LIC_GENERAL  },
    { 24929500, 24930500, BP_BCN, LIC_GENERAL  }, { 24930500, 24990000, BP_ALL, LIC_GENERAL  },
    { 28000000, 28070000, BP_CW,  LIC_TECH     }, { 28070000, 28150000, BP_DIG, LIC_TECH     },
    { 28150000, 28190000, BP_CW,  LIC_TECH     }, { 28190000, 28300000, BP_BCN, LIC_TECH     },
    { 28300000, 28500000, BP_ALL, LIC_TECH     }, { 28500000, 29700000, BP_ALL, LIC_GENERAL  },
    { 50000000, 50060000, BP_CW,  LIC_TECH     }, { 50060000, 50080000, BP_BCN, LIC_TECH     },
    { 50080000, 50100000, BP_CW,  LIC_TECH     }, { 50100000, 50600000, BP_ALL, LIC_TECH     },
    { 50600000, 50800000, BP_DIG, LIC_TECH     }, { 50800000, 54000000, BP_ALL, LIC_TECH     },
};

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The IARU region of a plan (not Auto): the US is in Region 2 */
static int region_of(int plan) {
    return plan >= BP_PLAN_US_EXTRA ? 2 : plan;
}

/* The segments of plan (bp_plan_t, not Auto) into out, by start. The US ones for the class: what the class does
 * not have is BP_OUT, a Technician's below 10 m CW (97.307(f)(9)) but for the beacons; neighbours of one type
 * merged then */
static int plan_build(int plan, bp_seg_t *out) {
    int n = 0;

    if (plan < BP_PLAN_US_EXTRA) {
        const bp_seg_t *t   = plan == BP_PLAN_R2 ? plan_r2 : plan == BP_PLAN_R3 ? plan_r3 : plan_r1;
        int             len = plan == BP_PLAN_R2 ? LEN(plan_r2) : plan == BP_PLAN_R3 ? LEN(plan_r3) : LEN(plan_r1);

        memcpy(out, t, len * sizeof(*t));
        return len;
    }
    lic_t lic = (lic_t)(plan - BP_PLAN_US_EXTRA);

    for (int i = 0; i < LEN(plan_us); i++) {
        bp_type_t t = plan_us[i].type;

        if (lic > plan_us[i].cls) {
            t = BP_OUT;
        } else if (lic == LIC_TECH && plan_us[i].start < 28000000 && t != BP_BCN) {
            t = BP_CW;
        }
        if (n && out[n - 1].end == plan_us[i].start && out[n - 1].type == t) {
            out[n - 1].end = plan_us[i].end;
        } else {
            out[n++] = (bp_seg_t){plan_us[i].start, plan_us[i].end, t};
        }
    }
    return n;
}

static const theme_color_t colors[] = { TC_bp_cw, TC_bp_dig, TC_bp_ssb, TC_bp_bcn, TC_bp_out };
/* Longest first: the first label that fits the segment is used */
static const char *const labels[][2] = {
    { "CW", NULL }, { "DIG", NULL }, { "ALL MODES / SSB", "SSB" }, { "BCN", NULL }, { "NOT YOUR CLASS", "OUT" },
};

typedef struct {
    lv_obj_t *box;
    lv_obj_t *label;
} seg_t;

static seg_t   segs[POOL];
static bool    ready;
static int32_t span_center, span_half;

/* The plan in use (never Auto): its table and, where it has nothing, the active bands of the band list (a band added
 * on the web page, "60m CW" at 5.3 MHz, had no strip and no CW keyboard: issue #5). Read from any thread
 * (remote_server, cfg_socket) */
static int             plan_id = BP_PLAN_R1;
static bp_seg_t        cur[MAX_SEG];
static int             ncur;
static pthread_mutex_t cur_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint     plan_gen;    /* one up with every change of the segments or the active bands */

/* What Auto takes (Settings and the web page show it): the region, how ("gps", "qth", "call", "" for nothing) and
 * from what (the locator, the callsign). Under cur_lock */
static int             auto_region = 1;
static const char     *auto_how    = "";
static char            auto_at[16];

/* The callsign's country, for Auto without a locator: looked up on a worker, as cty.dat is read on the first lookup
 * (about 0.1 s). call_asked is the callsign looked up last; call_pos its country's centre. Under cur_lock */
static char            call_asked[16];
static bool            call_pos;
static double          call_lat, call_lon;
static atomic_bool     call_busy;

static void follow_cb(void *arg) {
    band_plan_follow();
}

static void *call_thread(void *arg) {
    char  *call = arg;
    double lat, lon;
    bool   found = dxcc_pos(call, &lat, &lon);

    pthread_mutex_lock(&cur_lock);
    if (strcmp(call, call_asked) == 0) {
        call_pos = found;
        call_lat = lat;
        call_lon = lon;
    }
    pthread_mutex_unlock(&cur_lock);
    free(call);
    atomic_store(&call_busy, false);
    scheduler_put_noargs(follow_cb);    /* a full queue: the next band change takes it */
    return NULL;
}

/* Auto, the first that answers: a GPS fix under 10 s old (as station_qth()), the QTH locator, the country of the
 * callsign (cty.dat's centre of it; Region 1 until the worker has it), else Region 1. Never a US plan: the licence
 * class cannot be known. LVGL thread */
static int find_region(const char **how, char *at, size_t size) {
    double lat, lon;
    int    age;

    if (gps_last_fix(&lat, &lon, &age) && age < 10) {
        char grid[9];

        qth_pos_to_str(lat, lon, grid);
        grid[6] = 0;
        snprintf(at, size, "%s", grid);
        *how = "gps";
        return itu_region(lat, lon);
    }
    if (qth_grid_check(params.qth.x)) {
        qth_str_to_pos(params.qth.x, &lat, &lon);
        snprintf(at, size, "%s", params.qth.x);
        *how = "qth";
        return itu_region(lat, lon);
    }
    *how = "";
    at[0] = 0;
    if (!params.callsign.x[0]) {
        return 1;
    }
    pthread_mutex_lock(&cur_lock);
    bool asked = strcmp(call_asked, params.callsign.x) == 0, found = asked && call_pos;

    lat = call_lat;
    lon = call_lon;
    pthread_mutex_unlock(&cur_lock);
    if (found) {
        snprintf(at, size, "%s", params.callsign.x);
        *how = "call";
        return itu_region(lat, lon);
    }
    bool idle = false;

    if (!asked && atomic_compare_exchange_strong(&call_busy, &idle, true)) {
        char     *call = strdup(params.callsign.x);
        pthread_t t;

        pthread_mutex_lock(&cur_lock);
        snprintf(call_asked, sizeof(call_asked), "%s", params.callsign.x);
        call_pos = false;
        pthread_mutex_unlock(&cur_lock);
        if (call && pthread_create(&t, NULL, call_thread, call) == 0) {
            pthread_detach(t);
        } else {
            free(call);
            atomic_store(&call_busy, false);
        }
    }
    return 1;
}

/* A band's own segment type, from its name as the presets are named ("40m CW", "20m SSB"); its last used mode
 * is no type: it changes with every mode change */
static bp_type_t type_from_name(const char *name) {
    if (strcasestr(name, "CW")) return BP_CW;
    if (strcasestr(name, "DIG") || strcasestr(name, "FT8") || strcasestr(name, "FT4") || strcasestr(name, "DATA")) return BP_DIG;
    if (strcasestr(name, "BCN") || strcasestr(name, "BEACON")) return BP_BCN;
    return BP_ALL;
}

static int by_start(const void *a, const void *b) {
    const bp_seg_t *x = a, *y = b;

    return x->start < y->start ? -1 : x->start > y->start;
}

void band_plan_reload(void) {
    static int32_t ids[BANDS], start[BANDS], stop[BANDS];
    static char    names[BANDS][16];
    static int32_t last_start[BANDS], last_stop[BANDS];
    static char    last_names[BANDS][16];
    static int     last_nb = -1;
    static bp_seg_t list[MAX_SEG];
    const char    *how;
    char           at[sizeof(auto_at)];
    int            region = find_region(&how, at, sizeof(at));
    int            want   = param_i_get(cfg_band_region);
    int            plan   = want > BP_PLAN_AUTO && want < BP_PLAN_COUNT ? want : region;

    /* The band list's region edges (80 m to 4 MHz in Region 2 and the US) before it is read */
    if (cfg_bands_follow_region(region_of(plan))) {
        band_info_reload();
    }
    int            nb     = cfg_bands_active_ids(ids, start, stop, names, BANDS);
    int            n      = plan_build(plan, list);
    /* The browser panel shows the active bands too: they count as a change of the plan */
    bool           bands  = nb != last_nb || memcmp(start, last_start, sizeof(start)) || memcmp(stop, last_stop, sizeof(stop))
                            || memcmp(names, last_names, sizeof(names));

    if (bands) {
        last_nb = nb;
        memcpy(last_start, start, sizeof(start));
        memcpy(last_stop, stop, sizeof(stop));
        memcpy(last_names, names, sizeof(names));
    }

    for (int i = 0; i < nb; i++) {
        /* Broadcast, the X6200's FM broadcast and airband, CB (ids 100-299 in sql/params.sql and migrations.c) are no
         * amateur bands; by their names too, so a user's band that got such an id keeps its strip */
        if (ids[i] >= 100 && ids[i] < 300
            && (strncasecmp(names[i], "Broadcast", 9) == 0 || strncasecmp(names[i], "CB", 2) == 0
                || strcasecmp(names[i], "FM broadcast") == 0 || strcasecmp(names[i], "Airband") == 0)) {
            continue;
        }
        int32_t   a = start[i], e = stop[i];
        bp_type_t t = type_from_name(names[i]);
        int       base = n;

        qsort(list, n, sizeof(list[0]), by_start);
        /* The parts of [a, e] no segment covers yet */
        for (int k = 0; k < base && a < e && n < MAX_SEG; k++) {
            if (list[k].end <= a) continue;
            if (list[k].start >= e) break;
            if (list[k].start > a) list[n++] = (bp_seg_t){a, list[k].start, t};
            a = list[k].end > a ? list[k].end : a;
        }
        if (a < e && n < MAX_SEG) list[n++] = (bp_seg_t){a, e, t};
    }
    qsort(list, n, sizeof(list[0]), by_start);
    pthread_mutex_lock(&cur_lock);
    if (bands || n != ncur || memcmp(cur, list, n * sizeof(list[0])) != 0) {
        atomic_fetch_add(&plan_gen, 1);
    }
    memcpy(cur, list, n * sizeof(list[0]));
    ncur        = n;
    plan_id     = plan;
    auto_region = region;
    auto_how    = how;
    memcpy(auto_at, at, sizeof(auto_at));
    pthread_mutex_unlock(&cur_lock);
}

uint32_t band_plan_gen(void) {
    return atomic_load(&plan_gen);
}

void band_plan_follow(void) {
    band_plan_reload();
    band_plan_refresh();
}

/* A GPS fix that comes or goes (or moves the radio into another region) shows within 10 s; the locator and the
 * callsign call band_plan_follow() when saved */
static void where_timer_cb(lv_timer_t *t) {
    const char *how;
    char        at[sizeof(auto_at)];
    int         region = find_region(&how, at, sizeof(at));

    pthread_mutex_lock(&cur_lock);
    bool moved = region != auto_region;

    if (!moved) {
        auto_how = how;
        memcpy(auto_at, at, sizeof(auto_at));
    }
    pthread_mutex_unlock(&cur_lock);
    if (moved) {
        band_plan_follow();
    }
}

int band_plan_now(void) {
    pthread_mutex_lock(&cur_lock);
    int plan = plan_id;
    pthread_mutex_unlock(&cur_lock);
    return plan;
}

int band_plan_region(void) {
    return region_of(band_plan_now());
}

int band_plan_auto(const char **how, char *at, size_t size) {
    pthread_mutex_lock(&cur_lock);
    int region = auto_region;

    if (how) *how = auto_how;
    if (at && size) snprintf(at, size, "%s", auto_at);
    pthread_mutex_unlock(&cur_lock);
    return region;
}

const char *band_plan_name(int plan) {
    static const char *const names[BP_PLAN_COUNT] = {
        "Auto", "Region 1", "Region 2", "Region 3", "US Extra", "US Advanced", "US General", "US Technician",
    };
    return plan >= 0 && plan < BP_PLAN_COUNT ? names[plan] : "";
}

static void redraw(void) {
    int n = 0;

    if (!ready) {
        return;
    }
    if (span_half > 0 && param_i_get(cfg_band_plan)) {
        int32_t left = span_center - span_half;
        int64_t span = 2 * (int64_t)span_half;

        pthread_mutex_lock(&cur_lock);
        for (int i = 0; i < ncur && n < POOL; i++) {
            const bp_seg_t *p    = &cur[i];
            int64_t         a    = (int64_t)(p->start - left) * WIDTH / span;
            int64_t         b    = (int64_t)(p->end - left) * WIDTH / span;
            seg_t          *s    = &segs[n];
            const char     *text = NULL;

            if (b <= 0 || a >= WIDTH) {
                continue;
            }
            if (a < 0)     a = 0;
            if (b > WIDTH) b = WIDTH;
            if (b <= a)    b = a + 1;

            lv_obj_set_x(s->box, a);
            /* 1bit: a 1 px paper gap between segments, which no longer differ in colour */
            lv_obj_set_width(s->box, theme_shape.mono ? LV_MAX(b - a - 1, 1) : b - a);
            lv_obj_set_style_bg_color(s->box, lv_color_hex(theme_hex(colors[p->type])), 0);

            /* No label when it does not fit the visible part */
            for (int k = 0; k < 2 && labels[p->type][k] && !text; k++) {
                const char *t = labels[p->type][k];

                if (lv_txt_get_width(t, strlen(t), NF(16), 0, LV_TEXT_FLAG_NONE) + 6 <= b - a) {
                    text = t;
                }
            }
            if (text) {
                lv_label_set_text_static(s->label, text);
                lv_obj_center(s->label);
                lv_obj_clear_flag(s->label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s->label, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_clear_flag(s->box, LV_OBJ_FLAG_HIDDEN);
            n++;
        }
        pthread_mutex_unlock(&cur_lock);
    }
    for (; n < POOL; n++) {
        lv_obj_add_flag(segs[n].box, LV_OBJ_FLAG_HIDDEN);
    }
}

void band_plan_init(lv_obj_t *parent, lv_coord_t y) {
    static lv_timer_t *where_timer;     /* once: a theme change makes the strip again */

    band_plan_reload();
    if (!where_timer) {
        where_timer = lv_timer_create(where_timer_cb, 10000, NULL);
    }
    for (int i = 0; i < POOL; i++) {
        lv_obj_t *b = lv_obj_create(parent);

        lv_obj_remove_style_all(b);
        lv_obj_set_y(b, y);
        lv_obj_set_height(b, BAND_PLAN_H);
        lv_obj_set_style_bg_opa(b, OPA, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        segs[i].box = b;

        lv_obj_t *l = lv_label_create(b);

        lv_obj_set_style_text_font(l, NF(16), 0);
        lv_obj_set_style_text_color(l, TC(bp_text), 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        segs[i].label = l;
    }
    ready = true;
    redraw();
}

void band_plan_set_y(lv_coord_t y) {
    for (int i = 0; i < POOL; i++) {
        if (segs[i].box) {
            lv_obj_set_y(segs[i].box, y);
        }
    }
}

void band_plan_update(int32_t center, int32_t half_width) {
    if (center == span_center && half_width == span_half) {
        return;
    }
    span_center = center;
    span_half   = half_width;
    redraw();
}

void band_plan_refresh(void) {
    redraw();
}

bool band_plan_segment(int i, int32_t *start, int32_t *end, int *type) {
    bool ok;

    pthread_mutex_lock(&cur_lock);
    ok = i >= 0 && i < ncur;
    if (ok) {
        *start = cur[i].start;
        *end   = cur[i].end;
        *type  = cur[i].type;
    }
    pthread_mutex_unlock(&cur_lock);
    return ok;
}

int band_plan_type(int32_t freq) {
    int type = -1;

    pthread_mutex_lock(&cur_lock);
    for (int i = 0; i < ncur; i++) {
        if (freq >= cur[i].start && freq < cur[i].end) {
            type = cur[i].type;
            break;
        }
    }
    pthread_mutex_unlock(&cur_lock);
    return type;
}

bool band_plan_is_cw(int32_t freq) {
    return band_plan_type(freq) == BP_CW;
}
