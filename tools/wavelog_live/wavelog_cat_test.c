/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/wavelog_cat.c, the GUI's side of Wavelog's live link (#245), with control_owner.c and the quick
 *  QSO log's modes (src/field/flog.c) as they are and the rest stood in for (stub_include):
 *
 *  1. The modes a spot is tuned in, by name: the eight of the radio's HF modes, in any case; SAM, WFM and any other
 *     name refused.
 *  2. What the radio is on (radio_get): the dial, where it transmits (the other VFO in split), the mode by name, the
 *     quick log's mode (an app's, FT8 by the dial, SSB, none in a data mode it can't name), the power at most the
 *     digital apps' 7 W while one is open, and whether it transmits.
 *  3. A tune: frequency and mode set, the display says it, control comes to the operator at the radio. Refused while
 *     the radio transmits (nothing set, control stays with whoever has it), where it does not receive now, while the
 *     frequency is locked, and for a mode it has no name for; with the mode held by an app the frequency only.
 *  No PTT and no modem are in the shims: wavelog_cat.c does not link if it ever keys the radio.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "cfg/cfg_api.h"
#include "control_owner.h"
#include "msg_box.h"
#include "radio.h"
#include "screen_light.h"
#include "screen_main.h"
#include "wavelog_cat.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct {
    int32_t     freq, mode, tx_freq;
    float       pwr;
    bool        digi, transmitting, valid, locked, mode_locked, rx_only;
    const char *app;
    int         set_freq_calls, set_mode_calls;
    uint64_t    set_freq;
    char        note[160];
} r;

/* the parameters: one dummy object each, told apart by address */
static char        pwr_obj, freq_obj, mode_obj;
ParamFloat        *cfg_pwr     = (ParamFloat *)&pwr_obj;
ComputedParamInt  *cfg_fg_freq = (ComputedParamInt *)&freq_obj, *cfg_cur_mode = (ComputedParamInt *)&mode_obj;

float param_f_get(const ParamFloat *p) {
    return p == cfg_pwr ? r.pwr : 0.0f;
}

int32_t cparam_i_get(const ComputedParamInt *p) {
    return p == cfg_fg_freq ? r.freq : p == cfg_cur_mode ? r.mode : 0;
}

void cparam_i_set(ComputedParamInt *p, int32_t value) {
    if (p == cfg_cur_mode) {
        r.mode = value;
        r.set_mode_calls++;
    }
}

bool cfg_is_valid_hw_freq(int32_t freq) {
    (void)freq;
    return r.valid;
}

bool cfg_is_rx_only_freq(int32_t freq) {
    (void)freq;
    return r.rx_only;
}

radio_state_t radio_get_state(void) {
    return r.transmitting ? RADIO_TX : RADIO_RX;
}

int32_t radio_tx_freq(void) {
    return r.tx_freq ? r.tx_freq : r.freq;
}

bool radio_digi_cap(void) {
    return r.digi;
}

bool main_screen_freq_locked(void) {
    return r.locked;
}

bool main_screen_mode_locked(void) {
    return r.mode_locked;
}

void main_screen_set_freq(uint64_t freq) {
    r.set_freq = freq;
    r.freq     = (int32_t)freq;
    r.set_freq_calls++;
}

const char *main_screen_digital_app(void) {
    return r.app;
}

bool backlight_tick(void) {
    return true;
}

void msg_update_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(r.note, sizeof(r.note), fmt, ap);
    va_end(ap);
}

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static void reset(void) {
    memset(&r, 0, sizeof(r));
    r.freq  = 14074000;
    r.mode  = radio_mode_usb_dig;
    r.pwr   = 5.0f;
    r.valid = true;
    ctl_init();
}

static void test_modes(void) {
    static const struct {
        const char *name;
        int         mode;
    } cases[] = {
        {"LSB", radio_mode_lsb}, {"lsb-d", radio_mode_lsb_dig}, {"USB", radio_mode_usb}, {"Usb-D", radio_mode_usb_dig},
        {"CW", radio_mode_cw},   {"CWR", radio_mode_cwr},       {"AM", radio_mode_am},   {"nfm", radio_mode_nfm},
        {"SAM", -1},             {"WFM", -1},                    {"PKTUSB", -1},          {"FM", -1},
        {"", -1},                {"USB-D ", -1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(wavelog_cat_mode(cases[i].name) == cases[i].mode, "mode %s: %d", cases[i].name,
              wavelog_cat_mode(cases[i].name));
    CHECK(wavelog_cat_mode(NULL) == -1, "no name");
}

static void state(char *buf, size_t size) {
    int n = wavelog_cat_state(buf, size);

    CHECK(n > 0 && (size_t)n < size && buf[n - 1] == '\n', "one line: %s", buf);
}

static void test_state(void) {
    char buf[512];

    reset();
    r.app  = "FT8";
    r.digi = true;
    r.pwr  = 10.0f;
    state(buf, sizeof(buf));
    CHECK(!strcmp(buf, "{\"t\":\"radio\",\"hz\":14074000,\"tx_hz\":14074000,\"mode\":\"USB-D\",\"log_mode\":\"FT8\","
                       "\"app\":\"FT8\",\"pwr\":7.0,\"tx\":0}\n"),
          "FT8 app, power capped: %s", buf);

    reset();
    r.digi = false;
    r.pwr  = 10.0f;
    r.freq = 7040000;   /* USB-D away from FT8 and FT4, no app: a program on TCI, the mode not known */
    state(buf, sizeof(buf));
    CHECK(strstr(buf, "\"log_mode\":\"\",\"app\":\"\",\"pwr\":10.0") != NULL, "data mode, no app: %s", buf);

    reset();
    r.mode    = radio_mode_usb;
    r.freq    = 14200000;
    r.tx_freq = 14205000;   /* split */
    state(buf, sizeof(buf));
    CHECK(strstr(buf, "\"hz\":14200000,\"tx_hz\":14205000,\"mode\":\"USB\",\"log_mode\":\"SSB\"") != NULL,
          "split, voice: %s", buf);

    reset();
    r.mode = radio_mode_cwr;
    r.app  = "RTTY";   /* CW is CW, app or not */
    r.transmitting = true;
    state(buf, sizeof(buf));
    CHECK(strstr(buf, "\"mode\":\"CWR\",\"log_mode\":\"CW\"") && strstr(buf, "\"tx\":1}"), "CW-R on the air: %s", buf);

    reset();
    r.mode = 42;
    state(buf, sizeof(buf));
    CHECK(strstr(buf, "\"mode\":\"USB\"") != NULL, "a mode out of range reads as USB: %s", buf);

    reset();
    r.mode = radio_mode_wfm;
    state(buf, sizeof(buf));
    CHECK(strstr(buf, "\"mode\":\"WFM\",\"log_mode\":\"FM\"") != NULL, "the X6200's WFM: %s", buf);
}

static void test_tune(void) {
    ctl_info_t o;

    /* tuned */
    reset();
    r.mode = radio_mode_usb;
    ctl_claim(CTL_TCI, 3, "TCI", true);
    CHECK(wavelog_cat_tune(7074000, radio_mode_usb_dig) == NULL, "tune refused");
    CHECK(r.set_freq_calls == 1 && r.set_freq == 7074000, "frequency: %d calls, %llu", r.set_freq_calls,
          (unsigned long long)r.set_freq);
    CHECK(r.set_mode_calls == 1 && r.mode == radio_mode_usb_dig, "mode: %d calls, %d", r.set_mode_calls, r.mode);
    CHECK(!strcmp(r.note, "Wavelog: 7.074.0 USB-D"), "the display: %s", r.note);
    ctl_get(&o);
    CHECK(o.kind == CTL_LOCAL, "control to the operator at the radio: %d", o.kind);

    reset();
    r.rx_only = true;
    CHECK(wavelog_cat_tune(145500000, radio_mode_nfm) == NULL, "RX only refused");
    CHECK(!strcmp(r.note, "Wavelog: 145.500.0 NFM - RX only"), "the display: %s", r.note);

    /* on the air: nothing set, control where it was */
    reset();
    ctl_claim(CTL_TCI, 3, "TCI", true);
    r.transmitting = true;
    const char *why = wavelog_cat_tune(7074000, radio_mode_cw);
    CHECK(why && !strcmp(why, "The radio is transmitting"), "on the air: %s", why ? why : "(tuned)");
    CHECK(r.set_freq_calls == 0 && r.set_mode_calls == 0 && !r.note[0], "on the air, set: %d %d '%s'",
          r.set_freq_calls, r.set_mode_calls, r.note);
    ctl_get(&o);
    CHECK(o.kind == CTL_TCI && o.id == 3, "control stays with the TCI client: %d", o.kind);

    /* where the radio does not receive now */
    reset();
    r.valid = false;
    why     = wavelog_cat_tune(144300000, radio_mode_usb);
    CHECK(why && strstr(why, "does not receive"), "not received: %s", why ? why : "(tuned)");
    CHECK(r.set_freq_calls == 0 && r.set_mode_calls == 0, "not received, set");

    /* locked: the user's lock, an app on the air */
    reset();
    r.locked = true;
    why      = wavelog_cat_tune(14074000, radio_mode_usb);
    CHECK(why && strstr(why, "locked"), "locked: %s", why ? why : "(tuned)");
    CHECK(r.set_freq_calls == 0 && r.set_mode_calls == 0, "locked, set");

    /* the mode held by an app: the frequency only, the display says the mode the radio is in */
    reset();
    r.mode_locked = true;
    CHECK(wavelog_cat_tune(14080000, radio_mode_cw) == NULL, "mode held refused");
    CHECK(r.set_freq_calls == 1 && r.set_mode_calls == 0 && r.mode == radio_mode_usb_dig, "mode held: %d %d",
          r.set_freq_calls, r.set_mode_calls);
    CHECK(!strcmp(r.note, "Wavelog: 14.080.0 USB-D"), "the display: %s", r.note);

    /* modes it has no name for */
    reset();
    CHECK(wavelog_cat_tune(14074000, -1) != NULL && wavelog_cat_tune(14074000, radio_mode_sam) != NULL &&
              wavelog_cat_tune(14074000, radio_mode_wfm) != NULL && wavelog_cat_tune(14074000, 99) != NULL,
          "a mode with no name tuned");
    CHECK(r.set_freq_calls == 0 && r.set_mode_calls == 0, "no name, set");
}

int main(void) {
    test_modes();
    test_state();
    test_tune();
    if (failures) {
        printf("wavelog_cat_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("wavelog_cat_test: all passed\n");
    return 0;
}
