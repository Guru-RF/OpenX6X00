/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GhostNet on the screen (see ghostnet.h): the reminder before a net, in the message box over whatever
 *  is open, and the RTTY shortcut.
 *
 *  After the GhostNet mode of the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico).
 */
#include "ghostnet.h"

#include "app_window.h"
#include "countries.h"
#include "msg_box.h"
#include "params/params.h"
#include "radio.h"
#include "radio_types.h"
#include "rtty.h"
#include "screen_main.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>

#define LOOK_MS        (20 * 1000)  /* how often the reminder looks at the clock */
#define REMIND_SHOW_MS 12000        /* in the message box */
#define RTTY_KHZ       7077         /* GhostNet's RTTY window: 7.077 MHz, 45.45 baud, 170 Hz */

static ghostnet_remind_t said;

/* The radio on the air, whoever keyed it: the reminder waits until it is back on receive */
static bool on_air(void) {
    return radio_get_modem() || radio_get_state() != RADIO_RX;
}

/* A reminder due, in the message box: in the lower middle of the screen, over the app open but clear of the bottom
 * buttons, never while transmitting. One the transmission held back comes after it, until its net starts */
static void remind_cb(lv_timer_t *t) {
    const ghostnet_conf_t *c = ghostnet_conf();
    ghostnet_slot_t        s[12];
    time_t                 now = time(NULL);
    char                   when[48];

    (void)t;
    if (!c->remind || on_air()) return;
    int n = ghostnet_upcoming(now, s, 12, 3, true);
    int i = ghostnet_remind_pick(&said, s, n, now, c->remind_min);
    if (i < 0) return;
    ghostnet_when(s[i].start, when, sizeof(when));
    int left = (int)((s[i].start - now + 59) / 60);
    /* short, to be read as it scrolls by: the net, the minutes left, the mode and frequency; the rest in its popup */
    if (s[i].khz > 0) {
        msg_schedule_text_ms(REMIND_SHOW_MS, "GhostNet %s in %d min, %s %d.%03d", s[i].name, left, s[i].mode,
                             s[i].khz / 1000, s[i].khz % 1000);
    } else {
        msg_schedule_text_ms(REMIND_SHOW_MS, "GhostNet %s in %d min, %s", s[i].name, left, s[i].mode);
    }
    LV_LOG_USER("GhostNet reminder: %s (%s) at %s", s[i].name, s[i].net, when);
}

void ghostnet_init(void) {
    ghostnet_conf();
    lv_timer_create(remind_cb, LOOK_MS, NULL);
}

/* The radio's country for the regional group: the country setting (Settings > General > STATION > Country, or the
 * first-start guide's), "" while none is set; the group typed on the web page GhostNet goes before it
 * (ghostnet_region_group). A buffer per thread, so a caller on another thread does not change this one's */
const char *ghostnet_country_iso2(void) {
    static __thread char cc[3];

    return country_code(cc);
}

void ghostnet_region_tag(char *out, size_t size) {
    const ghostnet_conf_t *c = ghostnet_conf();

    ghostnet_region_group(ghostnet_country_iso2(), c->dept, c->region, out, size);
}

/* ---- RTTY ---- */

static void rtty_go(void *arg) {
    (void)arg;
    /* 45.45 baud, 170 Hz, the mark on the higher frequency as amateur RTTY: for this opening only, RTTY's own
     * rate, shift and reverse come back when it closes */
    rtty_open_with(4545, 170, false);
    /* RTTY opens where its memory says, after saving where the radio is: the radio goes back there on its close */
    app_mem_set("RTTY", RTTY_KHZ * 1000, radio_mode_usb);
    main_screen_action(ACTION_APP_RTTY);
    msg_schedule_text_fmt("GhostNet RTTY: 7.077 MHz, 45.45 baud, 170 Hz");
}

void ghostnet_rtty_open(void) {
    lv_async_call(rtty_go, NULL);   /* once the app that asked has closed */
}
