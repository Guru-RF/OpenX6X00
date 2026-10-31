/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the MSG > CW beacon (#71); see the Makefile.
 *
 *  The key: radio_set_morse_key below logs each change on the test's clock. The base: in TX while the key is down and
 *  for its tail after (the break-in delay), and in TX from elsewhere when a test says so (the mic's PTT). The radio
 *  thread's TX time limit is a cut at a time a test chooses, as radio.c makes it: the key let go and the count moved.
 *  The keyer's sleeps move the clock on, the wall clock (time()) with it; events fire on its thread as the clock
 *  passes them. The schedule's rules run in UTC and in a time zone with summer time.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cfg/cfg_api.h"
#include "cw_beacon.h"
#include "cw_encoder.h"
#include "msg_box.h"
#include "panel.h"
#include "params/params.h"
#include "radio.h"
#include "util.h"

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- The clock: the keyer's sleeps move it on; the test's own thread sleeps for real ---------------------------- */

static _Atomic uint64_t virt_ns = 1000ULL * 1000000000ULL;
static pthread_t        main_thread;

/* The wall clock: wall_at() sets it, the keyer's sleeps move it on with the rest */
static time_t   wall0;
static uint64_t wall_ns0;

static void wall_at(time_t t) {
    wall0    = t;
    wall_ns0 = atomic_load(&virt_ns);
}

time_t __wrap_time(time_t *t) {
    time_t now = wall0 + (time_t)((atomic_load(&virt_ns) - wall_ns0) / 1000000000ULL);

    if (t) *t = now;
    return now;
}

int __real_clock_gettime(clockid_t c, struct timespec *ts);
int __real_nanosleep(const struct timespec *req, struct timespec *rem);
int __real_usleep(useconds_t us);

static bool keyer_thread(void) {
    return !pthread_equal(pthread_self(), main_thread);
}

static uint64_t now_ms(void) {
    return atomic_load(&virt_ns) / 1000000ULL;
}

uint64_t get_time(void) {
    return now_ms();
}

static void tick(void);

int __wrap_clock_gettime(clockid_t c, struct timespec *ts) {
    if (c != CLOCK_MONOTONIC || !keyer_thread()) {
        return __real_clock_gettime(c, ts);
    }

    uint64_t ns = atomic_load(&virt_ns);

    ts->tv_sec  = ns / 1000000000ULL;
    ts->tv_nsec = ns % 1000000000ULL;
    return 0;
}

int __wrap_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!keyer_thread()) {
        return __real_nanosleep(req, rem);
    }
    atomic_fetch_add(&virt_ns, (uint64_t)req->tv_sec * 1000000000ULL + req->tv_nsec);
    tick();
    return 0;
}

/* What each sleep takes over its time, as on the radio: the beacon's pause of 100 ms sleeps comes out longer */
static uint32_t usleep_extra_us;

int __wrap_usleep(useconds_t us) {
    if (!keyer_thread()) {
        return __real_usleep(us);
    }
    atomic_fetch_add(&virt_ns, ((uint64_t)us + usleep_extra_us) * 1000ULL);
    tick();
    return 0;
}

/* ---- The settings, the subjects, the screen ------------------------------------------------------------------- */

static char dummies[16];
ParamInt   *cfg_key_speed        = (ParamInt *)&dummies[0];
ParamInt   *cfg_key_train        = (ParamInt *)&dummies[1];
ParamInt   *cfg_qsk_time         = (ParamInt *)&dummies[2];
ParamFloat *cfg_key_ratio        = (ParamFloat *)&dummies[3];
ParamInt   *cfg_band_vfoa_freq   = (ParamInt *)&dummies[4];
ParamInt   *cfg_band_vfob_freq   = (ParamInt *)&dummies[5];
ParamInt   *cfg_band_vfoa_mode   = (ParamInt *)&dummies[6];
ParamInt   *cfg_band_vfob_mode   = (ParamInt *)&dummies[7];
ParamInt   *cfg_band_current_vfo = (ParamInt *)&dummies[8];
ParamInt   *cfg_band_split       = (ParamInt *)&dummies[9];
ParamInt   *cfg_band_tx_shift    = (ParamInt *)&dummies[10];
ParamInt   *cfg_xit              = (ParamInt *)&dummies[11];

static int qsk = 100;   /* the break-in delay set, ms */

int32_t param_i_get(const ParamInt *p) {
    if (p == cfg_key_speed) return 20;
    if (p == cfg_qsk_time) return qsk;
    return 0;
}

float param_f_get(const ParamFloat *p) {
    return p == cfg_key_ratio ? 3.0f : 0.0f;
}

static struct {
    Subject    *subj;
    observer_cb fn;
    void       *user_data;
} subs[16];
static int n_subs;

Observer *subject_subscribe(Subject *subj, observer_cb fn, void *user_data) {
    subs[n_subs].subj      = subj;
    subs[n_subs].fn        = fn;
    subs[n_subs].user_data = user_data;
    return (Observer *)&subs[n_subs++];
}

/* A setting changed: its observers, on the thread that changed it (Subject::notify) */
static void changed(void *subj) {
    for (int i = 0; i < n_subs; i++) {
        if (subs[i].subj == subj) {
            subs[i].fn(subs[i].subj, subs[i].user_data);
        }
    }
}

params_t params = {.cw_encoder_period = 10, .callsign = {"ON6URE"}};

const char *panel_last_call(void) {
    return "";
}

static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static char            said[128];

void msg_schedule_text_fmt(const char *fmt, ...) {
    va_list ap;

    pthread_mutex_lock(&mux);
    va_start(ap, fmt);
    vsnprintf(said, sizeof(said), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&mux);
}

/* ---- The key and the base --------------------------------------------------------------------------------------- */

#define KEYLOG_MAX 20000

static struct {
    uint32_t t;     /* ms from the test's start */
    bool     on;
} keylog[KEYLOG_MAX];
static int      n_key;
static bool     key_on;
static uint64_t key_up_ms;
static uint64_t t0;             /* the test's start, ms */
static uint32_t tail_ms = 100;  /* the base in TX after the key went up */
static bool     other_tx;       /* TX from elsewhere */
static uint32_t limit_ms;       /* radio_set_morse_key_limit */
static uint32_t limit_max;      /* the highest asked in the test */
static unsigned cuts;
static int      qsk_set[64];    /* radio_set_qsk_time, in order */
static int      n_qsk;

void radio_set_morse_key(bool on) {
    pthread_mutex_lock(&mux);
    if (on != key_on && n_key < KEYLOG_MAX) {
        keylog[n_key].t  = (uint32_t)(now_ms() - t0);
        keylog[n_key].on = on;
        n_key++;
    }
    if (key_on && !on) {
        key_up_ms = now_ms();
    }
    key_on = on;
    pthread_mutex_unlock(&mux);
}

radio_state_t radio_get_state(void) {
    return key_on || now_ms() - key_up_ms < tail_ms || other_tx ? RADIO_TX : RADIO_RX;
}

void radio_set_qsk_time(uint16_t ms) {
    if (n_qsk < 64) qsk_set[n_qsk++] = ms;
}

void radio_set_morse_key_limit(uint32_t ms) {
    limit_ms = ms;
    if (ms > limit_max) limit_max = ms;
}

unsigned radio_morse_key_cuts(void) {
    return cuts;
}

/* ---- Events at a time from the test's start, on the keyer's thread ---------------------------------------------- */

typedef enum { EV_FREQ = 1, EV_MODE, EV_SPLIT, EV_OTHER_TX, EV_CUT, EV_HOLD, EV_XIT, EV_CLOCK } ev_t;

static struct {
    uint32_t at;
    ev_t     what;
    bool     done;
} evs[8];
static int n_evs;

/* EV_HOLD: the keyer waits there until the test lets it go, the key as it was */
static atomic_bool held, hold_release;
static time_t      clock_set_to;    /* EV_CLOCK: the wall clock set to it (NTP, GPS, by hand) */

static void at(uint32_t ms, ev_t what) {
    evs[n_evs].at     = ms;
    evs[n_evs].what   = what;
    evs[n_evs++].done = false;
}

static void tick(void) {
    uint64_t t = now_ms() - t0;

    for (int i = 0; i < n_evs; i++) {
        if (evs[i].done || t < evs[i].at) {
            continue;
        }
        evs[i].done = true;
        switch (evs[i].what) {
            case EV_FREQ:       changed(cfg_band_vfoa_freq); break;
            case EV_MODE:       changed(cfg_band_vfoa_mode); break;
            case EV_SPLIT:      changed(cfg_band_split); break;
            case EV_XIT:        changed(cfg_xit); break;
            case EV_CLOCK:      wall_at(clock_set_to); break;
            case EV_OTHER_TX:   other_tx = true; break;
            case EV_CUT:        /* as radio.c's morse_limit_check */
                if (key_on) {
                    radio_set_morse_key(false);
                    cuts++;
                }
                break;
            case EV_HOLD:
                atomic_store(&held, true);
                while (!atomic_load(&hold_release)) {
                    struct timespec s = {0, 1000000};

                    __real_nanosleep(&s, NULL);
                }
                atomic_store(&held, false);
                break;
        }
    }
}

/* ---- Helpers ---------------------------------------------------------------------------------------------------- */

static void reset(void) {
    pthread_mutex_lock(&mux);
    n_key = 0;
    key_on = false;
    said[0] = 0;
    pthread_mutex_unlock(&mux);
    n_evs = 0;
    other_tx = false;
    tail_ms = 100;
    qsk = 100;
    limit_max = 0;
    n_qsk = 0;
    usleep_extra_us = 0;
    atomic_store(&hold_release, false);
    t0 = now_ms();
}

/* The keyer done by itself (a send, a stop it found), joined; false: still on after 5 s of real time */
static bool wait_idle(void) {
    for (int i = 0; i < 5000; i++) {
        if (cw_encoder_state() == CW_ENCODER_IDLE) {
            cw_encoder_stop();
            return true;
        }
        usleep(1000);
    }
    return false;
}

static bool said_is(const char *want) {
    pthread_mutex_lock(&mux);
    bool same = strcmp(said, want) == 0;
    pthread_mutex_unlock(&mux);
    if (!same) printf("        said \"%s\", want \"%s\"\n", said, want);
    return same;
}

/* The key log as elements: on, then off up to the next key down (0 after the last); false if it does not alternate
 * down, up, from down */
static int log_elements(cw_el_t *el, int max) {
    int n = 0;

    for (int i = 0; i + 1 < n_key && n < max; i += 2) {
        if (!keylog[i].on || keylog[i + 1].on) return -1;
        el[n].on_ms  = keylog[i + 1].t - keylog[i].t;
        el[n].off_ms = i + 2 < n_key ? keylog[i + 2].t - keylog[i + 1].t : 0;
        n++;
    }
    return n_key % 2 ? -1 : n;
}

static bool same_plan(const cw_el_t *a, int na, const cw_el_t *b, int nb) {
    if (na != nb) {
        printf("        %d elements, want %d\n", na, nb);
        return false;
    }
    for (int i = 0; i < na; i++) {
        if (a[i].on_ms != b[i].on_ms || a[i].off_ms != b[i].off_ms) {
            printf("        element %d: %u on %u off, want %u on %u off\n", i, a[i].on_ms, a[i].off_ms, b[i].on_ms,
                   b[i].off_ms);
            return false;
        }
    }
    return true;
}

static bool key_left_up(void) {
    return !key_on && limit_ms == 0 && !cw_encoder_qrss3();
}

/* ---- The rules alone ------------------------------------------------------------------------------------------- */

static bool plan_is(const char *text, const uint32_t *want, int n) {
    cw_el_t el[64], w[32];
    int     got = qrss3_plan(text, el, 64);

    for (int i = 0; i < n; i++) {
        w[i].on_ms  = want[2 * i];
        w[i].off_ms = want[2 * i + 1];
    }
    return same_plan(el, got, w, n);
}

static void test_plan(void) {
    puts("QRSS3 elements (cw_beacon.c)");

    static const uint32_t e[] = {3000, 0}, t[] = {9000, 0}, a[] = {3000, 3000, 9000, 0},
                          et[] = {3000, 9000, 9000, 0}, e_t[] = {3000, 21000, 9000, 0},
                          ar[] = {3000, 3000, 9000, 3000, 3000, 3000, 9000, 3000, 3000, 0};

    check(plan_is("E", e, 1), "E: a dot of 3 s");
    check(plan_is("T", t, 1), "T: a dash of 9 s");
    check(plan_is("A", a, 2), "A: 3 s between the elements of a character");
    check(plan_is("ET", et, 2), "ET: 9 s between characters");
    check(plan_is("E T", e_t, 2), "E T: 21 s between words");
    check(plan_is("  E   T  ", e_t, 2), "spaces at the start and the end not sent, a run of them one word space");
    check(plan_is("E#T", e_t, 2), "a character the table does not have: a word space");
    check(plan_is("e", e, 1), "lower case as upper");
    check(plan_is("<AR>", ar, 5), "the prosign <AR> as one character");
    check(qrss3_plan("", NULL, 0) == 0 && qrss3_plan("   ", NULL, 0) == 0 && qrss3_plan(NULL, NULL, 0) == 0,
          "nothing to send: no elements");

    cw_el_t two[2];
    check(qrss3_plan("TEST", two, 2) == 6 && two[0].on_ms == 9000 && two[0].off_ms == 9000 &&
          two[1].on_ms == 3000 && two[1].off_ms == 9000, "out too small: all counted, the first ones filled");

    cw_el_t el[512];
    int     n = qrss3_plan("PARIS", el, 512);
    check(cw_plan_ms(el, n) == 43 * QRSS3_DOT_MS, "PARIS takes 43 dots (the 50 of PARIS timing less its word space)");
    n = qrss3_plan("E E", el, 512);
    check(cw_plan_ms(el, n) == 27000, "E E: 3 s + 21 s + 3 s");

    /* Every character of the table, and the beacon text the issue thinks of */
    const char *text = "CQ CQ DE ON6URE ON6URE QRSS3 TEST 0123456789 .,?/=+-@!:;'\"$_ <AR> <SK> <BK> <KN> <SOS> K";
    bool        ok   = true;
    uint32_t    down = 0;

    n = qrss3_plan(text, el, 512);
    for (int i = 0; i < n; i++) {
        uint32_t off = el[i].off_ms;

        ok &= el[i].on_ms == QRSS3_DOT_MS || el[i].on_ms == QRSS3_DASH_MS;
        ok &= i + 1 < n ? off == QRSS3_DOT_MS || off == QRSS3_DASH_MS || off == QRSS3_WORD_MS : off == 0;
        if (el[i].on_ms > down) down = el[i].on_ms;
    }
    check(ok && down == QRSS3_DASH_MS && n > 100, "%d elements: the key never down more than 9 s, every gap 3, 9 or 21 s",
          n);
}

static void test_bands(void) {
    puts("QRSS3 bands and frequencies");
    check(qrss_band_of(10140050) == 0 && qrss_band_of(3569900) == 1, "10.140050 is 30 m, 3.569900 is 80 m");
    check(qrss_band_of(10100000) == 0 && qrss_band_of(10150000) == 0 && qrss_band_of(10150001) == -1 &&
          qrss_band_of(3499999) == -1 && qrss_band_of(7040000) == -1, "the band edges; 40 m none");
    check(qrss_freq(0, 0) == 10140050 && qrss_freq(1, 0) == 3569900, "none chosen: the window's middle");
    check(qrss_bands[0].freq_hz > qrss_bands[0].win_lo_hz && qrss_bands[0].freq_hz < qrss_bands[0].win_hi_hz &&
          qrss_bands[1].freq_hz > qrss_bands[1].win_lo_hz && qrss_bands[1].freq_hz < qrss_bands[1].win_hi_hz,
          "the presets inside their windows");
    check(qrss_freq(0, 10140080) == 10140080 && qrss_freq(0, 7040000) == 10140050,
          "a choice on the band kept, one off it gives the preset");
    check(qrss_freq_step(0, 0, 3) == 10140080 && qrss_freq_step(0, 10140080, -3) == 0,
          "MFK steps of 10 Hz; back on the preset it is 0 again");
    check(qrss_freq_step(1, 0, -1000000) == 3500000 && qrss_freq_step(1, 3999990, 5) == 4000000,
          "kept between the band's edges");
}

/* The radio thread's TX time limit (radio.c morse_limit_check): the key went down at down (made odd), now is when that
 * thread read its clock. A key down after that read, or in its millisecond, was cut at once: one in a few dozen QRSS3
 * elements, with the screen saying the key had been held 10 s */
static void test_key_limit(void) {
    puts("QRSS3's TX time limit (radio.c)");
    check(!key_limit_over(5000, 5000 | 1, 10000), "the key down in the millisecond the radio thread read: not held");
    check(!key_limit_over(5000, 5003, 10000), "the key down after the radio thread read its clock: not held");
    check(!key_limit_over(15001, 5001, 10000) && key_limit_over(15002, 5001, 10000),
          "held 10 s: not over; a millisecond more: over");
    check(!key_limit_over(0x00000800u, 0xFFFFF801u, 10000) && key_limit_over(0x00002800u, 0xFFFFF801u, 10000),
          "over the wrap of the 32-bit milliseconds: 4 s held not over, 12 s over");
    check(!key_limit_over(99999, 1, 0) && !key_limit_over(99999, 0, 10000), "no limit, or the key up: never over");
}

/* ---- The keyer, on its thread ---------------------------------------------------------------------------------- */

static void test_qrss_send(void) {
    puts("QRSS3 sent by cw_encoder.c");

    cw_el_t want[64], got[64];
    int     nw, ng;

    reset();
    nw = qrss3_plan("ET E", want, 64);
    check(cw_encoder_send_qrss3("ET E", false) && cw_encoder_qrss3(), "started");
    check(wait_idle(), "done by itself");
    ng = log_elements(got, 64);
    check(same_plan(got, ng, want, nw), "the key as planned: dot 3 s, 9 s, dash 9 s, 21 s, dot 3 s");
    check(said_is("") && key_left_up(), "nothing said, the key up, the TX time limit off");
    check(limit_max == QRSS3_DASH_MS + 1000, "the TX time limit while it sent: 10 s");

    bool qsk_kept = true;
    for (int i = 0; i < n_qsk; i++) qsk_kept &= qsk_set[i] == qsk;
    check(qsk_kept, "the break-in delay left as set: the radio receives in every gap");

    /* A long break-in delay, and the base's TX flag later still: its own TX after each element, not someone else's */
    reset();
    qsk     = 1000;
    tail_ms = 2400;
    nw = qrss3_plan("TEST", want, 64);
    cw_encoder_send_qrss3("TEST", false);
    check(wait_idle(), "TEST with a 1 s break-in delay, the base in TX 2.4 s after each element");
    ng = log_elements(got, 64);
    check(same_plan(got, ng, want, nw) && said_is(""), "the base's own TX after the key is not taken for another's");

    /* A beacon: the pause after each send, then again */
    reset();
    at(27000, EV_FREQ);
    cw_encoder_send_qrss3("E", true);
    check(wait_idle(), "a beacon, stopped by a change of frequency");
    check(n_key == 6 && keylog[0].t == 0 && keylog[1].t == 3000 && keylog[2].t == 13000 && keylog[3].t == 16000 &&
          keylog[4].t == 26000 && keylog[5].t <= 27050, "sends at 0, 13 and 26 s: the 10 s pause after each");
    check(said_is("QRSS3 stopped: tuned away") && key_left_up(), "said so, the key up within 50 ms of the change");
}

static void *stop_thread(void *arg) {
    (void)arg;
    cw_encoder_stop();
    return NULL;
}

static void test_qrss_stops(void) {
    puts("QRSS3 stops");

    reset();
    at(4000, EV_MODE);
    cw_encoder_send_qrss3("T", true);
    check(wait_idle() && n_key == 2 && keylog[1].t >= 4000 && keylog[1].t <= 4050,
          "mode changed during a dash: the key up within 50 ms");
    check(said_is("QRSS3 stopped: mode changed") && key_left_up(), "said so, nothing keyed after");

    reset();
    at(5000, EV_SPLIT);
    cw_encoder_send_qrss3("EE", false);
    check(wait_idle() && n_key == 2 && said_is("QRSS3 stopped: VFO changed") && key_left_up(),
          "split turned on in a gap: stopped before the next element");

    /* XIT turned during a dash: the base adds it to the carrier, which leaves the QRSS frequency */
    reset();
    at(6000, EV_XIT);
    cw_encoder_send_qrss3("T", true);
    check(wait_idle() && n_key == 2 && keylog[1].t >= 6000 && keylog[1].t <= 6050 &&
          said_is("QRSS3 stopped: tuned away") && key_left_up(), "XIT changed during a dash: the key up within 50 ms");

    /* The mic's PTT in the 9 s gap of EE (3-12 s): another TX, seen once the base's own tail is over */
    reset();
    at(5000, EV_OTHER_TX);
    cw_encoder_send_qrss3("EE", false);
    check(wait_idle() && n_key == 2 && said_is("QRSS3 stopped: other TX") && key_left_up(),
          "the radio in TX between elements, from elsewhere: stopped, no element after");

    /* The radio thread's TX time limit let go of the key during the dash */
    reset();
    at(4000, EV_CUT);
    cw_encoder_send_qrss3("TT", true);
    check(wait_idle() && n_key == 2 && keylog[1].t == 4000, "the key let go by the radio at 4 s, never down again");
    check(said_is("QRSS3 stopped: key held 10 s") && key_left_up(), "said so");

    /* The front PTT key on a beacon: it stops before PTT keys */
    reset();
    cw_encoder_send_qrss3("E", true);
    usleep(2000);
    cw_encoder_ptt_pressed();
    check(cw_encoder_state() == CW_ENCODER_IDLE && said_is("QRSS3 stopped: PTT") && key_left_up(),
          "the front PTT key: stopped, said so, the key up");

    /* ESC (the dialog closes) in the middle of a dash: the key goes up at once */
    pthread_t th;

    reset();
    at(4500, EV_HOLD);
    cw_encoder_send_qrss3("T", true);
    while (!atomic_load(&held)) usleep(1000);
    check(key_on, "held at 4.5 s with the key down");
    pthread_create(&th, NULL, stop_thread, NULL);
    usleep(20000);
    atomic_store(&hold_release, true);
    pthread_join(th, NULL);
    check(n_key == 2 && keylog[1].t == 4500 && said_is("") && key_left_up(),
          "cw_encoder_stop(): the key up at 4.5 s, nothing said");

    /* The knob turned on the GUI thread while the keyer sleeps in a dash: the base has the new frequency at once, so
     * the key goes up then, not at the keyer's next look */
    bool up_at_once;

    reset();
    at(4500, EV_HOLD);
    cw_encoder_send_qrss3("T", true);
    while (!atomic_load(&held)) usleep(1000);
    changed(cfg_band_vfoa_freq);
    pthread_mutex_lock(&mux);
    up_at_once = !key_on;
    pthread_mutex_unlock(&mux);
    atomic_store(&hold_release, true);
    check(wait_idle() && up_at_once && n_key == 2 && keylog[1].t == 4500 && said_is("QRSS3 stopped: tuned away") &&
          key_left_up(), "tuned away in the middle of a dash: the key up on the GUI thread, before the keyer looks");
}

/* ---- The schedule ---------------------------------------------------------------------------------------------- */

static time_t utc(int y, int mo, int d, int h, int mi, int sec) {
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = sec};

    return timegm(&tm);
}

static void zone_set(const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
}

#define HM(h, m) ((h) * 60 + (m))

static void test_sched_rules(void) {
    puts("The schedule's rules (cw_beacon.c)");

    zone_set("UTC0");
    time_t now = utc(2026, 10, 31, 19, 42, 30);

    check(bcn_next(now, HM(20, 0)) == utc(2026, 10, 31, 20, 0, 0), "19:42:30, start 20:00: today at 20:00");
    check(bcn_next(now, HM(19, 42)) == now, "start 19:42, its minute: at once");
    check(bcn_next(now, HM(19, 41)) == utc(2026, 11, 1, 19, 41, 0), "start 19:41, gone by: tomorrow");
    check(bcn_next(now, HM(0, 0)) == utc(2026, 11, 1, 0, 0, 0), "start 00:00: after midnight");

    bcn_sched_t at_once = {BCN_NOW, BCN_UNTIL_STOP, 10, BCN_NOW}, at20 = {HM(20, 0), BCN_UNTIL_TIME, 10, HM(21, 0)};
    time_t      t20     = utc(2026, 10, 31, 20, 0, 0);

    check(bcn_start_at(&at_once, now) == now && bcn_start_at(&at20, now) == t20, "the first send: now, or at the start");
    check(bcn_end_at(&at20, t20) == utc(2026, 10, 31, 21, 0, 0), "20:00 to 21:00: the end the same day");
    at20.end_min = HM(20, 0);
    check(bcn_end_at(&at20, t20) == utc(2026, 11, 1, 20, 0, 0), "an end at the start's minute: 24 hours on");
    at20.end_min = HM(1, 0);
    check(bcn_end_at(&at20, t20) == utc(2026, 11, 1, 1, 0, 0), "20:00 to 01:00: past midnight");
    check(bcn_end_days(&at20, t20, now) == 1, "20:00 to 01:00, at 19:42: the end tomorrow");
    bcn_sched_t now21 = {BCN_NOW, BCN_UNTIL_TIME, 10, HM(21, 0)}, late = {HM(19, 0), BCN_UNTIL_TIME, 10, HM(1, 0)};
    check(bcn_end_days(&now21, now, now) == 0, "Start Now at 19:42, until 21:00: today");
    check(bcn_end_days(&now21, utc(2026, 10, 31, 21, 5, 0), utc(2026, 10, 31, 21, 5, 0)) == 1,
          "Start Now at 21:05, until 21:00: tomorrow, about a day of beacon (#71)");
    check(bcn_end_days(&late, bcn_start_at(&late, now), now) == 2,
          "at 19:42, from 19:00 (tomorrow's) to 01:00: the day after tomorrow");
    at20.until = BCN_UNTIL_SENDS;
    check(bcn_end_at(&at20, t20) == 0, "no end time when it goes by sends");
    check(bcn_end_days(&at20, t20, now) == 0, "no end time, no day");

    bcn_sched_t three = {BCN_NOW, BCN_UNTIL_SENDS, 3, BCN_NOW}, to21 = {BCN_NOW, BCN_UNTIL_TIME, 10, HM(21, 0)};
    time_t      end   = utc(2026, 10, 31, 21, 0, 0);

    check(bcn_after_send(&three, 0, 1, now, 30) == now + 30 && bcn_after_send(&three, 0, 2, now, 30) == now + 30,
          "3 sends: after the 1st and the 2nd, the next after the pause");
    check(bcn_after_send(&three, 0, 3, now, 30) == 0, "3 sends: over after the 3rd, no pause");
    check(bcn_after_send(&to21, end, 5, utc(2026, 10, 31, 20, 59, 0), 30) == utc(2026, 10, 31, 20, 59, 30),
          "until 21:00: a send starting at 20:59:30 still goes");
    check(bcn_after_send(&to21, end, 5, utc(2026, 10, 31, 20, 59, 30), 30) == 0 &&
          bcn_after_send(&to21, end, 5, utc(2026, 10, 31, 20, 59, 45), 30) == 0,
          "until 21:00: none starts at 21:00 or after");
    check(bcn_after_send(&at_once, 0, 500, now, 120) == now + 120, "until stopped: again and again");
    check(bcn_may_start(&to21, end, end - 1) && !bcn_may_start(&to21, end, end) &&
          !bcn_may_start(&to21, end, end + 600), "until 21:00: a send may start at 20:59:59, not at 21:00 or after");
    check(bcn_may_start(&three, 0, end + 600) && bcn_may_start(&at_once, 0, end + 600),
          "by sends, or until stopped: any time");

    /* The screen clock in a time zone with summer time (Belgium) */
    zone_set("CET-1CEST,M3.5.0,M10.5.0/3");
    check(bcn_min_of(now) == HM(20, 42), "19:42 UTC is 20:42 on a CET clock");
    check(bcn_next(now, HM(21, 0)) == utc(2026, 10, 31, 20, 0, 0), "start 21:00 on the CET clock: 20:00 UTC");
    check(bcn_next(utc(2026, 3, 28, 11, 0, 0), HM(11, 30)) == utc(2026, 3, 29, 9, 30, 0),
          "11:30 gone by on the Saturday before summer time: Sunday 11:30 CEST, 23 hours on");
    check(bcn_next(utc(2026, 10, 24, 12, 0, 0), HM(12, 0)) == utc(2026, 10, 25, 11, 0, 0),
          "12:00 on the Saturday before winter time: Sunday 12:00 CET, 25 hours on");
    zone_set("UTC0");

    /* The MFK's steps */
    check(bcn_step_start(BCN_NOW, 1, HM(19, 42)) == HM(19, 45), "start: from Now one step on is the next 5 minutes");
    check(bcn_step_start(HM(19, 45), -1, HM(19, 42)) == BCN_NOW, "and one back is Now again");
    check(bcn_step_start(BCN_NOW, -1, HM(19, 42)) == HM(19, 40), "back from Now: the last time of the round");
    check(bcn_step_start(HM(23, 55), 1, HM(19, 42)) == HM(0, 0), "23:55 on: 00:00");
    check(bcn_step_start(BCN_NOW, 289, HM(19, 42)) == BCN_NOW, "Now and 288 times: a full round");
    check(bcn_step_end(BCN_NOW, 11, HM(20, 0)) == HM(21, 0), "end: an hour after a 20:00 start to begin with");
    check(bcn_step_end(HM(21, 0), -1, HM(20, 0)) == HM(20, 55) && bcn_step_end(HM(20, 5), -1, HM(20, 0)) == HM(20, 0),
          "end: 5 minutes back, round past the start");
    check(bcn_step_sends(10, 5) == 15 && bcn_step_sends(1, -3) == 1 && bcn_step_sends(98, 5) == BCN_SENDS_MAX,
          "sends: 1 to 99");
}

static void *ptt_thread(void *arg) {
    (void)arg;
    cw_encoder_ptt_pressed();
    return NULL;
}

static void test_sched_keyer(void) {
    puts("The schedule in cw_encoder.c");

    bcn_sched_t s;
    pthread_t   th;

    /* At 19:59:30 a beacon for 20:00: it waits, receiving, then sends */
    reset();
    wall_at(utc(2026, 10, 31, 19, 59, 30));
    at(10000, EV_HOLD);
    at(40000, EV_FREQ);
    s = (bcn_sched_t){HM(20, 0), BCN_UNTIL_STOP, 10, BCN_NOW};
    cw_encoder_schedule(&s);
    cw_encoder_send_qrss3("E", true);
    while (!atomic_load(&held)) usleep(1000);
    check(cw_encoder_state() == CW_ENCODER_BEACON_WAIT && cw_encoder_wait_left() == 20 && n_key == 0 && !key_on,
          "10 s in: waiting, 20 s to go, the key never down");
    atomic_store(&hold_release, true);
    check(wait_idle() && n_key >= 2 && keylog[0].t == 30000 && keylog[1].t == 33000,
          "the first send at 20:00:00, 30 s on");

    /* 3 sends, the pause 10 s */
    reset();
    s = (bcn_sched_t){BCN_NOW, BCN_UNTIL_SENDS, 3, BCN_NOW};
    cw_encoder_schedule(&s);
    cw_encoder_send_qrss3("E", true);
    check(wait_idle() && n_key == 6 && keylog[2].t == 13000 && keylog[4].t == 26000 && keylog[5].t == 29000,
          "3 sends at 0, 13 and 26 s, nothing after");
    check(said_is("Beacon over: 3 sends") && key_left_up() && cw_encoder_sends_done() == 3, "said so, the key up");

    /* The schedule was that beacon's only: the next one runs until stopped */
    reset();
    at(30000, EV_FREQ);
    cw_encoder_send_qrss3("E", true);
    check(wait_idle() && n_key == 6 && said_is("QRSS3 stopped: tuned away"),
          "the next beacon without a schedule: on until stopped (here by tuning away at 30 s)");

    /* Until 20:01, from 20:00:00, the pause 30 s: sends at 0 and 33 s; one at 66 s would be past the end */
    reset();
    wall_at(utc(2026, 10, 31, 20, 0, 0));
    s = (bcn_sched_t){BCN_NOW, BCN_UNTIL_TIME, 10, HM(20, 1)};
    params.cw_encoder_period = 30;
    cw_encoder_schedule(&s);
    cw_encoder_send_qrss3("E", true);
    check(wait_idle() && n_key == 4 && keylog[2].t == 33000 && said_is("Beacon over: 20:01") && key_left_up(),
          "until 20:01: sends at 20:00:00 and 20:00:33, none at 20:01:06");

    /* A send on the air at the end is finished: TT from 20:00:40 takes 27 s, past 20:01 */
    reset();
    wall_at(utc(2026, 10, 31, 20, 0, 40));
    params.cw_encoder_period = 10;
    cw_encoder_schedule(&s);
    cw_encoder_send_qrss3("TT", true);
    check(wait_idle() && n_key == 4 && keylog[3].t == 27000 && said_is("Beacon over: 20:01"),
          "the send on the air at 20:01 goes on to its end, at 20:01:07, and is the last");

    /* The pause runs a little long on the radio (1 ms on each of its 100 ms sleeps here): EEEE ends at 20:00:49.96,
     * the next would start at 20:00:59 by the clock's second but goes at 20:01:00.06. It is not sent */
    reset();
    wall_at(utc(2026, 10, 31, 20, 0, 49));
    usleep_extra_us = 1000;
    params.cw_encoder_period = 10;
    s = (bcn_sched_t){BCN_NOW, BCN_UNTIL_TIME, 10, HM(20, 1)};
    cw_encoder_schedule(&s);
    cw_encoder_send("EEEE", true);
    check(wait_idle() && n_key == 8 && said_is("Beacon over: 20:01") && !key_on,
          "until 20:01, a pause that ends at 20:01:00.06: no send starts after it");

    /* The clock set forward while the beacon waits (NTP at the WiFi's connect, a GPS fix, by hand) */
    reset();
    wall_at(utc(2026, 10, 31, 19, 59, 30));
    clock_set_to = utc(2026, 10, 31, 20, 45, 0);
    at(10000, EV_CLOCK);
    s = (bcn_sched_t){HM(20, 0), BCN_UNTIL_TIME, 10, HM(20, 30)};
    cw_encoder_schedule(&s);
    cw_encoder_send_qrss3("E", true);
    check(wait_idle() && n_key == 0 && said_is("Beacon over: 20:30") && key_left_up(),
          "20:00 to 20:30, the clock set from 19:59:40 to 20:45: over, nothing sent");

    reset();
    wall_at(utc(2026, 10, 31, 19, 59, 30));
    clock_set_to = utc(2026, 10, 31, 20, 10, 0);
    at(10000, EV_CLOCK);
    at(20000, EV_FREQ);
    cw_encoder_schedule(&s);
    cw_encoder_send_qrss3("E", true);
    check(wait_idle() && n_key >= 2 && keylog[0].t >= 10000 && keylog[0].t <= 10100,
          "20:00 to 20:30, the clock set from 19:59:40 to 20:10: it starts at once");

    /* At the keyer's speed too: 2 sends of a 60 ms dot, 10 s apart */
    reset();
    s = (bcn_sched_t){BCN_NOW, BCN_UNTIL_SENDS, 2, BCN_NOW};
    cw_encoder_schedule(&s);
    cw_encoder_send("E", true);
    check(wait_idle() && n_key == 4 && keylog[2].t >= 10000 && said_is("Beacon over: 2 sends") && !key_on,
          "a CW beacon of 2 sends: 2 dots, over");

    /* The front PTT key while a CW beacon waits for its start: stopped, never keyed */
    reset();
    wall_at(utc(2026, 10, 31, 19, 0, 0));
    at(5000, EV_HOLD);
    s = (bcn_sched_t){HM(20, 0), BCN_UNTIL_SENDS, 5, BCN_NOW};
    cw_encoder_schedule(&s);
    cw_encoder_send("E", true);
    while (!atomic_load(&held)) usleep(1000);
    pthread_create(&th, NULL, ptt_thread, NULL);
    usleep(20000);
    atomic_store(&hold_release, true);
    pthread_join(th, NULL);
    check(cw_encoder_state() == CW_ENCODER_IDLE && n_key == 0 && !key_on && said_is(""),
          "the front PTT key during the wait: stopped, the key never down");
}

/* The CW keyer's own path is as it was: a dot at the keyer's speed, the break-in delay held through the message */
static void test_cw(void) {
    puts("CW at the keyer's speed");
    reset();
    cw_encoder_send("E", false);
    check(wait_idle() && n_key == 2 && keylog[1].t - keylog[0].t == 60, "E at 20 WPM: a 60 ms dot");
    check(n_qsk >= 2 && qsk_set[0] == 7 * 60 + 150 && qsk_set[n_qsk - 1] == qsk,
          "the break-in delay held for a word space and 150 ms, then back");
    check(key_left_up() && limit_max == 0, "no TX time limit for CW");

    reset();
    cw_encoder_send("E", true);
    usleep(2000);
    cw_encoder_ptt_pressed();
    check(cw_encoder_state() == CW_ENCODER_IDLE && !key_on && said_is(""), "a CW beacon stops on the front PTT key");
}

int main(void) {
    main_thread = pthread_self();
    test_plan();
    test_bands();
    test_key_limit();
    test_qrss_send();
    test_qrss_stops();
    test_cw();
    test_sched_rules();
    test_sched_keyer();
    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}
