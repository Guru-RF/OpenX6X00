/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - TUNE for an external tuner (ext_tune.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "ext_tune.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "button_bar.h"     /* btn_ext_tune */
#include "cfg/cfg_api.h"
#include "jtty_tx.h"
#include "msg_box.h"
#include "olivia_tx.h"
#include "params/params.h"
#include "psk_tx.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_main.h"
#include "tune_tx.h"
#include "winlink.h"

/* Why a tune is to stop before its limit */
typedef enum { WHY_NONE = 0, WHY_ASKED, WHY_FREQ, WHY_MODE, WHY_VFO, WHY_LINK } why_t;

static atomic_bool on;          /* from the start until its thread has put everything back */
static atomic_int  why;         /* WHY_NONE: it goes on */
static atomic_int  owner;       /* TUNE_BY_RADIO, TUNE_BY_PANEL + n */
static atomic_int  limit_s;     /* the running one's */
static tune_plan_t plan;        /* the running one's: made before its thread starts, read by it */

typedef struct {
    tune_end_t end;
    int        why;
    int        limit_s;
    bool       swr;             /* the SWR protection stopped it: it said so itself */
} over_t;

/* The open app's TX audio offset, where its signal goes over the TX frequency: the tune's tone goes there. 0 without an
 * app (or NavTex, which only listens): the carrier on the TX frequency itself */
static int32_t app_offset(void) {
    switch (main_screen_digital_app_action()) {
        case ACTION_APP_FT8:    return params.ft8_tx_freq.x;
        case ACTION_APP_JS8:    return params.js8_tx_freq.x;
        case ACTION_APP_WSPR:   return param_i_get(cfg_wspr_tx_freq);
        case ACTION_APP_PSK:    return psk_tx_get_center();
        case ACTION_APP_OLIVIA: return olivia_tx_get_center();
        case ACTION_APP_JTTY:   return jtty_tx_get_freq();
        case ACTION_APP_RTTY:   return params.rtty_center;
        case ACTION_APP_SSTV:
        case ACTION_APP_WEFAX:  return 1900;    /* the middle of their 1500-2300 Hz picture */
        default:                return 0;
    }
}

static bool stop_asked(void *ctx) {
    (void)ctx;
    return atomic_load(&why) != WHY_NONE;
}

/* LVGL thread: how it ended, the button back */
static void over_cb(void *arg) {
    const over_t *o = arg;
    /* Short: the message box shows about 30 characters still */
    static const char *const whys[] = {"", "", ": tuned away", ": mode changed", ": VFO changed",
                                       ": browser gone"};

    switch (o->end) {
        case TUNE_END_LIMIT:
            msg_update_text_fmt("TUNE over: %d s limit", o->limit_s);
            break;
        case TUNE_END_STOP:
            msg_update_text_fmt("TUNE stopped%s", whys[o->why >= 0 && o->why <= WHY_LINK ? o->why : 0]);
            break;
        case TUNE_END_CUT:
            if (!o->swr)    /* else "TX stopped: SWR 6.0" is on the screen already */
                msg_update_text_fmt("TUNE stopped by the radio");
            break;
        case TUNE_END_REFUSED:
            msg_update_text_fmt("No tune: on the air");
            break;
    }
    buttons_mark(&btn_ext_tune, false);
    buttons_refresh(&btn_ext_tune);
}

static void *tune_thread(void *arg) {
    unsigned trips = radio_swr_trips();
    over_t   o;

    (void)arg;
    o.end = tune_tx_run(&plan, stop_asked, NULL);
    o.why     = atomic_load(&why);
    o.limit_s = (int)(plan.limit_ms / 1000);
    o.swr     = radio_swr_trips() != trips;
    atomic_store(&owner, TUNE_BY_RADIO);
    atomic_store(&on, false);
    scheduler_put_urgent(over_cb, &o, sizeof(o));   /* the button's mark: the urgent reserve, as a stop's */
    return NULL;
}

const char *ext_tune_refusal(void) {
    static _Thread_local char text[48];
    int32_t    f = radio_tx_freq();     /* in split the other VFO's, the TX shift's */
    tune_ask_t a = {
        .tx_freq_ok = cfg_is_tx_freq(f),
        .on_air     = radio_get_modem() || radio_get_state() != RADIO_RX,
        .winlink    = winlink_busy(),
    };

    switch (tune_refusal(&a)) {
        case TUNE_NO_TX: {
            int32_t m = f < 0 ? -f : f;     /* a TX shift below 0 Hz */

            snprintf(text, sizeof(text), "no TX on %s%d.%03d MHz", f < 0 ? "-" : "", (int)(m / 1000000),
                     (int)(m / 1000 % 1000));
            return text;
        }
        case TUNE_ON_AIR:
            return "on the air";
        case TUNE_WINLINK:
            return "Winlink in session";
        default:
            return NULL;
    }
}

const char *ext_tune_start(int who) {
    pthread_t   th;
    const char *no;

    if (atomic_load(&on)) {
        return NULL;
    }
    if ((no = ext_tune_refusal())) {
        msg_update_text_fmt("No tune: %s", no);
        return no;
    }
    tune_plan(&plan, cparam_i_get(cfg_cur_mode), app_offset(), param_f_get(cfg_tune_pwr), cfg_pwr_max_get(),
              param_i_get(cfg_tune_time));
    atomic_store(&why, WHY_NONE);
    atomic_store(&owner, who);
    atomic_store(&limit_s, (int)(plan.limit_ms / 1000));
    atomic_store(&on, true);
    if (pthread_create(&th, NULL, tune_thread, NULL) != 0) {
        atomic_store(&on, false);
        msg_update_text_fmt("No tune: no thread for it");
        return "no thread for it";
    }
    pthread_detach(th);
    msg_update_text_fmt("TUNE %.1f W, %d s at most", plan.pwr_w, (int)(plan.limit_ms / 1000));
    buttons_mark(&btn_ext_tune, true);
    buttons_refresh(&btn_ext_tune);
    return NULL;
}

void ext_tune_toggle(void) {
    if (atomic_load(&on)) {
        ext_tune_stop();
    } else {
        ext_tune_start(TUNE_BY_RADIO);
    }
}

/* The first reason counts */
static void stop_for(int w) {
    int none = WHY_NONE;

    if (atomic_load(&on)) {
        atomic_compare_exchange_strong(&why, &none, w);
    }
}

void ext_tune_stop(void) {
    stop_for(WHY_ASKED);
}

bool ext_tune_stop_owner(int who) {
    if (!atomic_load(&on) || atomic_load(&owner) != who) {
        return false;
    }
    stop_for(WHY_LINK);
    return true;
}

bool ext_tune_on(void) {
    return atomic_load(&on);
}

int ext_tune_limit_s(void) {
    if (atomic_load(&on)) {
        return atomic_load(&limit_s);
    }
    return LV_CLAMP(TUNE_TIME_MIN_S, param_i_get(cfg_tune_time), TUNE_TIME_MAX_S);
}

/* The radio moved while it tunes (the knob, a band key, CAT, TCI, Pat, the Panel, an app): the tone would go out
 * beside where the tune was asked, past a band edge too; the tune stops and is asked again there */
static void changed_cb(Subject *subj, void *user_data) {
    (void)subj;
    stop_for((int)(intptr_t)user_data);
}

void ext_tune_init(void) {
    subject_subscribe((Subject *)cfg_band_vfoa_freq, changed_cb, (void *)WHY_FREQ);
    subject_subscribe((Subject *)cfg_band_vfob_freq, changed_cb, (void *)WHY_FREQ);
    subject_subscribe((Subject *)cfg_band_vfoa_mode, changed_cb, (void *)WHY_MODE);
    subject_subscribe((Subject *)cfg_band_vfob_mode, changed_cb, (void *)WHY_MODE);
    subject_subscribe((Subject *)cfg_band_current_vfo, changed_cb, (void *)WHY_VFO);
    subject_subscribe((Subject *)cfg_band_split, changed_cb, (void *)WHY_VFO);
}
