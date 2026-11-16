/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the radio's buttons (src/front_keys.c), issue #233: what the screen gets of a press, a hold and a
 *  release, and when. LVGL's timers are run here as LVGL 8.3 runs them, the newest first, on a clock of the test's
 *  own (1 ms a step). A key comes either as keypad_button() does it (the F-slots on the screen) or through a device
 *  (a FIFO standing in for /dev/input/eventN), read by the indev timer every 10 ms as LVGL reads it.
 *  - BAND UP and DOWN: the press at once, held a REPEAT after 500 ms and every 300 ms after the end of the step
 *    before, no long press. The others: the long press after the hold time, or after the app's (JS8), POWER always
 *    after 1 s.
 *  - A GUI held up past the hold time with the release already waiting on the device: the release is read first,
 *    the press stays a tap (a band key stepped once, an F key not its long action). Before, the timer made at the
 *    press ran before the read and sent the long press: the tap was lost.
 */

#include "front_keys.h"
#include "main.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint32_t    EVENT_KEYPAD = 1;
lv_group_t *keyboard_group;
static encoder_t mfk_dev;
encoder_t  *mfk = &mfk_dev;

bool backlight_tick() {
    return false;
}

void ctl_local_input(void) {}

/* ---- LVGL's timers ---- */

#define MAX_TIMERS 16

static lv_timer_t *timers[MAX_TIMERS];  /* [0] the newest: lv_timer_create puts it at the list's head */
static int         n_timers;
static bool        list_changed;        /* a timer made or deleted: the handler starts again from the head */
static uint32_t    now;

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data) {
    lv_timer_t *t = calloc(1, sizeof(*t));

    *t = (lv_timer_t){.period = period, .last_run = now, .timer_cb = cb, .user_data = user_data, .repeat_count = -1};
    memmove(timers + 1, timers, n_timers * sizeof(*timers));
    timers[0] = t;
    n_timers++;
    list_changed = true;
    return t;
}

void lv_timer_del(lv_timer_t *t) {
    for (int i = 0; i < n_timers; i++) {
        if (timers[i] == t) {
            memmove(timers + i, timers + i + 1, (n_timers - i - 1) * sizeof(*timers));
            n_timers--;
            free(t);
            list_changed = true;
            return;
        }
    }
}

void lv_timer_set_period(lv_timer_t *t, uint32_t period) {
    t->period = period;
}

void lv_timer_set_repeat_count(lv_timer_t *t, int32_t count) {
    t->repeat_count = count;
}

void lv_timer_reset(lv_timer_t *t) {
    t->last_run = now;
}

/* lv_timer_handler: each timer due, from the head; again from the head after a timer was made or deleted */
static void handler(void) {
again:
    list_changed = false;
    for (int i = 0; i < n_timers; i++) {
        lv_timer_t *t = timers[i];

        if (now - t->last_run < t->period)
            continue;
        int32_t count = t->repeat_count;

        if (count > 0)
            t->repeat_count--;
        t->last_run = now;
        if (count != 0)
            t->timer_cb(t);
        if (list_changed)
            goto again;
        if (t->repeat_count == 0) {
            lv_timer_del(t);
            goto again;
        }
    }
}

/* The clock on to ms, the handler run every ms */
static void run_to(uint32_t ms) {
    while (now < ms) {
        now++;
        handler();
    }
}

/* The GUI held up until ms (a band change, an app closing): the handler runs once then */
static void stall_to(uint32_t ms) {
    now = ms;
    handler();
}

/* ---- LVGL's input devices: the keypad's read every 10 ms ---- */

static lv_indev_t indev;

void lv_indev_drv_init(lv_indev_drv_t *drv) {
    memset(drv, 0, sizeof(*drv));
}

static void indev_read(lv_timer_t *t) {
    lv_indev_drv_t *drv = t->user_data;
    lv_indev_data_t data;

    do {
        memset(&data, 0, sizeof(data));
        drv->read_cb(drv, &data);
    } while (data.continue_reading);
}

lv_indev_t *lv_indev_drv_register(lv_indev_drv_t *drv) {
    indev.driver = drv;
    lv_timer_create(indev_read, 10, drv);
    return &indev;
}

void lv_indev_set_group(lv_indev_t *i, lv_group_t *group) {
    (void)i, (void)group;
}

/* ---- the screen: what it got, and when ---- */

static char     got[1024];
static uint32_t step_ms;    /* what a band step costs the GUI */

lv_obj_t *lv_scr_act(void) {
    return NULL;
}

int lv_event_send(lv_obj_t *obj, lv_event_code_t code, void *param) {
    static const char *names[] = {"PRESS", "RELEASE", "LONG", "LONG_RELEASE", "REPEAT"};
    event_keypad_t    *ev      = param;
    size_t             n       = strlen(got);

    (void)obj;
    if (code != EVENT_KEYPAD)
        return 0;
    snprintf(got + n, sizeof(got) - n, "%s%s%s@%u", n ? " " : "",
             ev->key == KEYPAD_BAND_UP ? "" : ev->key == KEYPAD_F1 ? "F1:" : ev->key == KEYPAD_POWER ? "POWER:" : "?:",
             names[ev->state], now);
    if (ev->key == KEYPAD_BAND_UP && (ev->state == KEYPAD_PRESS || ev->state == KEYPAD_REPEAT))
        now += step_ms;     /* the step's own work, the GUI busy meanwhile */
    return 0;
}

/* ---- the cases ---- */

static int bad, cases;

static void check(const char *want, const char *what) {
    cases++;
    if (strcmp(got, want)) {
        printf("WRONG %s\n  want %s\n  got  %s\n", what, want, got);
        bad++;
    }
}

/* A fresh start: no key down, no timer but the device's read, the clock at 0 */
static void start(void) {
    for (int i = n_timers - 1; i >= 0; i--)
        if (timers[i]->timer_cb != indev_read)
            lv_timer_del(timers[i]);
    for (int i = 0; i < n_timers; i++)
        timers[i]->last_run = 0;
    now     = 0;
    step_ms = 0;
    got[0]  = 0;
}

/* keypad_button(): BAND UP tapped, held, stepping slowly; F1 and POWER held, with an app's shorter hold */
static void buttons(void) {
    start();
    keypad_button(KEYPAD_BAND_UP, 1);
    run_to(120);
    keypad_button(KEYPAD_BAND_UP, 0);
    run_to(2000);
    check("PRESS@0 RELEASE@120", "BAND UP tapped: one step, on the press");

    start();
    keypad_button(KEYPAD_BAND_UP, 1);
    run_to(1450);
    keypad_button(KEYPAD_BAND_UP, 0);
    run_to(3000);
    check("PRESS@0 REPEAT@500 REPEAT@800 REPEAT@1100 REPEAT@1400 RELEASE@1450",
          "BAND UP held 1.45 s: the press, after 500 ms a step every 300 ms, no long press");

    start();
    step_ms = 200;
    keypad_button(KEYPAD_BAND_UP, 1);
    run_to(1300);
    keypad_button(KEYPAD_BAND_UP, 0);
    run_to(3000);
    check("PRESS@0 REPEAT@700 REPEAT@1200 RELEASE@1400",
          "BAND UP held, a step taking 200 ms: the next one 500 then 300 ms after a step's end");

    start();
    keypad_button(KEYPAD_F1, 1);
    run_to(1200);
    keypad_button(KEYPAD_F1, 0);
    check("F1:PRESS@0 F1:LONG@1000 F1:LONG_RELEASE@1200", "F1 held: the long press at 1 s");

    start();
    keypad_button(KEYPAD_F1, 1);
    run_to(300);
    keypad_button(KEYPAD_F1, 0);
    run_to(2000);
    check("F1:PRESS@0 F1:RELEASE@300", "F1 tapped");

    start();
    keypad_set_long_time(400);
    keypad_button(KEYPAD_F1, 1);
    keypad_button(KEYPAD_POWER, 1);
    keypad_button(KEYPAD_BAND_UP, 1);
    run_to(1050);
    keypad_button(KEYPAD_F1, 0);
    keypad_button(KEYPAD_POWER, 0);
    keypad_button(KEYPAD_BAND_UP, 0);
    keypad_set_long_time(0);
    check("F1:PRESS@0 POWER:PRESS@0 PRESS@0 F1:LONG@400 REPEAT@500 REPEAT@800 POWER:LONG@1000 F1:LONG_RELEASE@1050 "
          "POWER:LONG_RELEASE@1050 RELEASE@1050",
          "an app's 400 ms hold (JS8): F1's; POWER keeps 1 s, BAND UP its repeat, each key on its own");
}

static int fifo;

static void key(uint16_t code, int value) {
    struct input_event in[2] = {{.type = EV_KEY, .code = code, .value = value}, {.type = EV_SYN, .code = SYN_REPORT}};

    if (write(fifo, in, sizeof(in)) != sizeof(in))
        printf("FIFO not written\n");
}

/* Through the device, the X6100's codes: BAND UP is BTN_TRIGGER_HAPPY6, F1 BTN_TRIGGER_HAPPY13 */
static void device(void) {
    start();
    key(BTN_TRIGGER_HAPPY6, 1);
    run_to(100);
    key(BTN_TRIGGER_HAPPY6, 0);
    run_to(1000);
    check("PRESS@10 RELEASE@110", "BAND UP tapped on the device: read within 10 ms");

    start();
    key(BTN_TRIGGER_HAPPY13, 1);
    run_to(10);
    key(BTN_TRIGGER_HAPPY13, 0);   /* let go at 10 ms; the GUI busy until 1.5 s */
    stall_to(1500);
    run_to(2000);
    check("F1:PRESS@10 F1:RELEASE@1500", "F1 tapped while the GUI was busy past the hold time: a tap");

    start();
    key(BTN_TRIGGER_HAPPY6, 1);
    run_to(10);
    key(BTN_TRIGGER_HAPPY6, 0);
    stall_to(900);
    run_to(2000);
    check("PRESS@10 RELEASE@900", "BAND UP tapped while the GUI was busy past the repeat: one step");

    start();
    key(BTN_TRIGGER_HAPPY6, 1);
    run_to(10);
    stall_to(900);      /* still held, nothing waiting: the repeat at once */
    run_to(1000);
    key(BTN_TRIGGER_HAPPY6, 0);
    run_to(1500);
    check("PRESS@10 REPEAT@900 RELEASE@1010", "BAND UP held through a busy GUI: the repeat when it is free");
}

int main(void) {
    const char *path = "build/keypad.fifo";
    keypad_t   *pad;

    buttons();
    unlink(path);
    if (mkfifo(path, 0600) != 0 || !(pad = keypad_init((char *)path))) {
        printf("no FIFO for the keypad at %s\n", path);
        return 1;
    }
    fifo = pad->fd;     /* O_RDWR: the test writes the events the GUI reads */
    device();
    unlink(path);
    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
