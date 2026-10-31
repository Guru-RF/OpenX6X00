/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *  QRSS3 (#71): Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "cw_encoder.h"

#include "cw_beacon.h"
#include "cw_decoder.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "panel.h"
#include "msg_box.h"
#include "util.h"

#include "lvgl/lvgl.h"
#include <ctype.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

/* QRSS3: the radio thread lets go of a key down longer than a dash and a second (its TX time limit,
 * radio_set_morse_key_limit), and TX this long past the break-in delay after our key went up is someone else's */
#define QRSS_KEY_LIMIT_MS   (QRSS3_DASH_MS + 1000)
#define QRSS_OTHER_MS       1500
#define QRSS_STEP_NS        50000000L   /* how often a QRSS3 element looks whether to stop */

/* Why QRSS3 is to stop: the first reason counts */
typedef enum { WHY_NONE = 0, WHY_ASKED, WHY_FREQ, WHY_MODE, WHY_VFO, WHY_PTT, WHY_OTHER_TX, WHY_LIMIT } why_t;

static cw_encoder_state_t   state = CW_ENCODER_IDLE;
static pthread_t            thread;
static bool                 thread_started;

static char                 *current_msg = NULL;
static char                 *current_char = NULL;

static bool                 qrss;       /* the text goes in QRSS3: set before the thread starts, read by it */
static cw_el_t              *plan;      /* its elements */
static int                  plan_n;
static atomic_bool          qrss_on;    /* a QRSS3 send or beacon, from its start until its thread let go */
static atomic_int           why;
static volatile int         send_left;  /* seconds of the QRSS3 send on the air */

/* A beacon's schedule: the next one's (cw_encoder_schedule), the running one's */
static bcn_sched_t          next_sched = {BCN_NOW, BCN_UNTIL_STOP, 1, BCN_NOW};
static bcn_sched_t          sched;
static volatile time_t      start_at;   /* when its first send starts */
static volatile int         sends_done;

static uint8_t get_morse(char *str, char **morse) {
    cw_characters_t *character = &cw_characters[0];

    while (character->morse) {
        uint8_t char_len = strlen(character->character);

        if (strncasecmp(character->character, str, char_len) == 0) {
            *morse = character->morse;

            return char_len;
        }

        character++;
    }

    return 0;
}

static volatile bool stop_req;
static volatile bool need_train;

void cw_encoder_require_train(bool on) {
    need_train = on;
}

/* Closes the key; with the train requirement and no train, stops instead. */
static bool key_down(void) {
    if (need_train && !param_i_get(cfg_key_train)) {
        stop_req = true;
        return false;
    }
    radio_set_morse_key(true);
    return true;
}

static volatile int  pause_left;

/* Sleeps nsec, in short steps so a stop is quick. False when stopped. */
static bool wait_nsec(long nsec) {
    struct timespec t = {0, 0};
    while (nsec > 0 && !stop_req) {
        t.tv_nsec = nsec > 50000000L ? 50000000L : nsec;
        nanosleep(&t, NULL);
        nsec -= t.tv_nsec;
    }
    return !stop_req;
}

static void hold_tx(long world_space_nsec, bool on);

/* last: the final character of the text. Its last element puts the break-in delay set back
 * while the key is down, so the receiver returns after that delay and not the held one */
static void send_morse(char *str, long dit_nsec, long dah_nsec, bool last) {
    while (*str && !stop_req) {
        switch (*str) {
            case '.':
                if (!key_down()) return;
                if (last && !str[1]) hold_tx(0, false);
                wait_nsec(dit_nsec);
                radio_set_morse_key(false);
                break;

            case '-':
                if (!key_down()) return;
                if (last && !str[1]) hold_tx(0, false);
                wait_nsec(dah_nsec);
                radio_set_morse_key(false);
                break;

            default:
                break;
        }
        str++;
        wait_nsec(dit_nsec);
    }
    wait_nsec(dah_nsec - dit_nsec);
}

/* A message holds the transmitter through its gaps: the break-in delay set (100-130 ms for
 * the key) dropped the relay between letters and words, which clicked on and off. Longer
 * than a word space, 1 s at most; the setting is back from the last element on (send_morse). */
static void hold_tx(long world_space_nsec, bool on) {
    long ms = world_space_nsec / 1000000L + 150;
    radio_set_qsk_time(on ? (ms > 1000 ? 1000 : (uint16_t)ms) : (uint16_t)param_i_get(cfg_qsk_time));
}

/* ---- QRSS3 ---------------------------------------------------------------------------------------------------- */

static void stop_for(int w) {
    int none = WHY_NONE;

    if (atomic_load(&qrss_on)) {
        atomic_compare_exchange_strong(&why, &none, w);
        stop_req = true;
    }
}

/* The radio moved (the knob, a band key, CAT, TCI, the Panel): the carrier would go out beside the QRSS frequency,
 * where no grabber looks, or outside the band. Any thread */
static void changed_cb(Subject *subj, void *user_data) {
    (void)subj;
    stop_for((int)(intptr_t)user_data);
}

/* Once, at the first QRSS3: before it, the move onto its frequency is not taken for one (qrss_on is still off) */
static void watch_init(void) {
    static bool done;

    if (done) {
        return;
    }
    done = true;
    subject_subscribe((Subject *)cfg_band_vfoa_freq, changed_cb, (void *)WHY_FREQ);
    subject_subscribe((Subject *)cfg_band_vfob_freq, changed_cb, (void *)WHY_FREQ);
    subject_subscribe((Subject *)cfg_band_tx_shift, changed_cb, (void *)WHY_FREQ);
    subject_subscribe((Subject *)cfg_band_vfoa_mode, changed_cb, (void *)WHY_MODE);
    subject_subscribe((Subject *)cfg_band_vfob_mode, changed_cb, (void *)WHY_MODE);
    subject_subscribe((Subject *)cfg_band_current_vfo, changed_cb, (void *)WHY_VFO);
    subject_subscribe((Subject *)cfg_band_split, changed_cb, (void *)WHY_VFO);
}

static void ts_add(struct timespec *t, uint32_t ms) {
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

static long long ts_left_ns(const struct timespec *t, const struct timespec *now) {
    return (long long)(t->tv_sec - now->tv_sec) * 1000000000LL + (t->tv_nsec - now->tv_nsec);
}

static struct timespec send_end;
static unsigned        cuts;        /* radio_morse_key_cuts() as the send started */

/* Until the deadline t, in steps of QRSS_STEP_NS: false when QRSS3 is to stop. The key held past the TX time limit (the
 * radio thread let go of it), or between elements (up_ms: when our key went up, 0 while it is down) the radio in TX
 * longer than the break-in delay holds it: the mic's PTT, CAT, TCI, the Panel. The front PTT key stops it before it
 * keys (cw_encoder_ptt_pressed) */
static bool qrss_wait(const struct timespec *t, uint64_t up_ms) {
    for (;;) {
        struct timespec now;

        clock_gettime(CLOCK_MONOTONIC, &now);
        send_left = (int)((ts_left_ns(&send_end, &now) + 999999999LL) / 1000000000LL);
        if (radio_morse_key_cuts() != cuts) {
            stop_for(WHY_LIMIT);
        } else if (up_ms && radio_get_state() != RADIO_RX &&
                   get_time() - up_ms > (uint64_t)param_i_get(cfg_qsk_time) + QRSS_OTHER_MS) {
            stop_for(WHY_OTHER_TX);
        }
        if (stop_req) {
            return false;
        }

        long long ns = ts_left_ns(t, &now);

        if (ns <= 0) {
            return true;
        }

        struct timespec s = {0, ns > QRSS_STEP_NS ? QRSS_STEP_NS : (long)ns};

        nanosleep(&s, NULL);
    }
}

/* The text once in QRSS3, every element on a deadline from the first one. The break-in delay stays as set, so the
 * radio receives in every gap (3 s at least) */
static void qrss_send(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    send_end = t;
    ts_add(&send_end, cw_plan_ms(plan, plan_n));
    cuts = radio_morse_key_cuts();
    for (int i = 0; i < plan_n && !stop_req; i++) {
        if (!key_down()) {
            break;
        }
        ts_add(&t, plan[i].on_ms);

        bool on = qrss_wait(&t, 0);

        radio_set_morse_key(false);
        if (!on) {
            break;
        }

        uint64_t up = get_time();

        ts_add(&t, plan[i].off_ms);
        if (!qrss_wait(&t, up)) {
            break;
        }
    }
    radio_set_morse_key(false);
    send_left = 0;
}

/* Short: the message box shows about 30 characters */
static void qrss_said(void) {
    static const char *const whys[] = {
        [WHY_FREQ] = "tuned away", [WHY_MODE] = "mode changed", [WHY_VFO] = "VFO changed", [WHY_PTT] = "PTT",
        [WHY_OTHER_TX] = "other TX", [WHY_LIMIT] = "key held 10 s",
    };
    int w = atomic_load(&why);

    if (w > WHY_ASKED && w <= WHY_LIMIT) {
        msg_schedule_text_fmt("QRSS3 stopped: %s", whys[w]);
    }
}

/* ---- The thread ----------------------------------------------------------------------------------------------- */

/* A beacon over by its schedule: the screen says so */
static void over_said(void) {
    if (sched.until == BCN_UNTIL_SENDS) {
        msg_schedule_text_fmt("Beacon over: %i send%s", sched.sends, sched.sends == 1 ? "" : "s");
    } else {
        msg_schedule_text_fmt("Beacon over: %02i:%02i", sched.end_min / 60, sched.end_min % 60);
    }
}

/* Keys the text (again after a pause, as a beacon) until done or stopped.
 * Stopping is by flag: the key is always released by this thread. */
static void * endecode_thread(void *arg) {
    (void)arg;
    long dit_nsec         = 20000000L / (param_i_get(cfg_key_speed)) * 60;
    long dah_nsec         = dit_nsec * param_f_get(cfg_key_ratio);
    long world_space_nsec = dit_nsec * 7;
    time_t end            = 0;
    bool   over           = false;

    /* A beacon waits for its start time receiving (BEACON_WAIT, start()); its end comes from the start */
    if (state != CW_ENCODER_SEND) {
        end = bcn_end_at(&sched, start_at);
        while (!stop_req && time(NULL) < start_at) {
            usleep(100000);
        }
        state = CW_ENCODER_BEACON;
    }
    if (!qrss) {
        hold_tx(world_space_nsec, true);
    }

    while (!stop_req) {
        char    *morse;
        uint8_t len;

        if (qrss) {
            qrss_send();
            current_char = current_msg + strlen(current_msg);
        } else if (*current_char == ' ') {
            current_char++;
            wait_nsec(world_space_nsec - dah_nsec);
        } else {
            len = get_morse(current_char, &morse);
            if (len) {
                send_morse(morse, dit_nsec, dah_nsec, !current_char[len + strspn(current_char + len, " ")]);
                current_char += len;
            } else {
                current_char++;
                wait_nsec(world_space_nsec - dah_nsec);
            }
        }
        if (*current_char == 0) {
            if (state == CW_ENCODER_SEND) {
                break;
            }
            if (!stop_req && !bcn_after_send(&sched, end, ++sends_done, time(NULL), params.cw_encoder_period)) {
                over = true;
                break;
            }
            state = CW_ENCODER_BEACON_IDLE;
            if (!qrss) {
                hold_tx(world_space_nsec, false);   /* receive between beacons */
            }
            for (int t = params.cw_encoder_period * 10; t > 0 && !stop_req; t--) {
                pause_left = (t + 9) / 10;
                usleep(100000);
            }
            if (!qrss) {
                hold_tx(world_space_nsec, true);
            }
            state        = CW_ENCODER_BEACON;
            current_char = current_msg;
        }
    }
    radio_set_morse_key(false);
    hold_tx(world_space_nsec, false);
    if (qrss) {
        radio_set_morse_key_limit(0);
        qrss_said();
        atomic_store(&qrss_on, false);
    }
    if (over) {
        over_said();
    }
    state = CW_ENCODER_IDLE;
    return NULL;
}

void cw_encoder_stop() {
    if (thread_started) {
        stop_req = true;
        pthread_join(thread, NULL);
        thread_started = false;
        radio_set_morse_key(false);
        state = CW_ENCODER_IDLE;
    }
}

void cw_encoder_ptt_pressed(void) {
    if (atomic_load(&qrss_on)) {
        stop_for(WHY_PTT);
        cw_encoder_stop();
    } else if (state == CW_ENCODER_BEACON || state == CW_ENCODER_BEACON_IDLE || state == CW_ENCODER_BEACON_WAIT) {
        cw_encoder_stop();
    }
}

/* {CALL}: my callsign; {HIS}: the other station's, the last call the decoder confirmed
 * (the one Log QSO offers), nothing when there is none */
const char *cw_encoder_expand(const char *text) {
    static char buf[512];
    const char *call = params.callsign.x;
    size_t      n    = 0;

    for (; *text && n < sizeof(buf) - 1; text++) {
        if (call[0] && !strncasecmp(text, "{CALL}", 6)) {
            for (const char *c = call; *c && n < sizeof(buf) - 1; c++) {
                buf[n++] = toupper((unsigned char)*c);
            }
            text += 5;
        } else if (!strncasecmp(text, "{HIS}", 5)) {
            for (const char *c = panel_last_call(); *c && n < sizeof(buf) - 1; c++) {
                buf[n++] = toupper((unsigned char)*c);
            }
            text += 4;
        } else {
            buf[n++] = *text;
        }
    }
    buf[n] = 0;
    return buf;
}

static bool start(const char *text, bool beacon, bool slow) {
    cw_encoder_stop();
    sched      = next_sched;    /* for this one only, started or not */
    next_sched = (bcn_sched_t){BCN_NOW, BCN_UNTIL_STOP, 1, BCN_NOW};

    if (!text || !*text) {
        return false;
    }
    if (current_msg) {
        free(current_msg);
    }
    free(plan);
    plan     = NULL;
    plan_n   = 0;
    qrss     = false;
    stop_req = false;   /* before qrss_on: a stop asked from now on is kept */
    if (slow) {
        int n = qrss3_plan(text, NULL, 0);

        if (n == 0 || !(plan = malloc(n * sizeof(*plan)))) {
            current_msg = current_char = NULL;
            return false;
        }
        plan_n = qrss3_plan(text, plan, n);
        qrss   = true;
        watch_init();
        atomic_store(&why, WHY_NONE);
        radio_set_morse_key_limit(QRSS_KEY_LIMIT_MS);
        atomic_store(&qrss_on, true);
    }

    current_msg = strdup(text);
    current_char = current_msg;
    sends_done = 0;
    start_at = beacon ? bcn_start_at(&sched, time(NULL)) : time(NULL);
    state = !beacon ? CW_ENCODER_SEND : start_at > time(NULL) ? CW_ENCODER_BEACON_WAIT : CW_ENCODER_BEACON;

    pthread_attr_t attr;
    struct sched_param param;
    pthread_attr_init(&attr);
    pthread_attr_getschedparam(&attr, &param);
    param.sched_priority += 5;
    pthread_attr_setschedparam(&attr, &param);

    thread_started = pthread_create(&thread, &attr, endecode_thread, NULL) == 0;
    if (!thread_started) {
        state = CW_ENCODER_IDLE;
        if (slow) {
            radio_set_morse_key_limit(0);
            atomic_store(&qrss_on, false);
        }
    }
    return thread_started;
}

void cw_encoder_send(const char *text, bool beacon) {
    start(text, beacon, false);
}

bool cw_encoder_send_qrss3(const char *text, bool beacon) {
    return start(text, beacon, true);
}

bool cw_encoder_qrss3(void) {
    return atomic_load(&qrss_on);
}

void cw_encoder_schedule(const bcn_sched_t *s) {
    next_sched = *s;
    if (next_sched.sends < 1) {
        next_sched.sends = 1;
    }
}

int cw_encoder_wait_left(void) {
    time_t left = start_at - time(NULL);

    return state == CW_ENCODER_BEACON_WAIT && left > 0 ? (int)left : 0;
}

int cw_encoder_sends_done(void) {
    return sends_done;
}

int cw_encoder_send_left(void) {
    return send_left;
}

int cw_encoder_sent() {
    return state == CW_ENCODER_IDLE || !current_msg ? 0 : (int)(current_char - current_msg);
}

int cw_encoder_pause_left() {
    return pause_left;
}

cw_encoder_state_t cw_encoder_state() {
    return state;
}
