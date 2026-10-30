/* Issue #56: what the main screen does with the hand mic keys the X6200's H-22 has besides the X6100 mic's. run.py
 * cuts ab_key(), the VOL+/VOL- repeat and mic_key_cb() out of the production src/screen_main.c into
 * mic_screen_keys.c, and they run here with what they call stood in for: F3's own actions next to F1's and F2's,
 * VOL+/VOL- a step a press and on by themselves while held (after MIC_VOL_DELAY_MS, every MIC_VOL_REPEAT_MS) until
 * let go, also when the release went to another screen, A/B as the front A/B key, and the BAND codes.
 * Ends "N of N cases as they should be". */
#include "src/cfg/encoder_bind_types.h"
#include "src/gui_events.h"
#include "src/radio_types.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char did[256];

static void note(const char *fmt, ...) {
    char    b[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    snprintf(did + strlen(did), sizeof(did) - strlen(did), "%s%s", *did ? ", " : "", b);
}

/* The screen's state and the calls of the code cut out */
enum { cfg_band_current_vfo, cfg_cur_mode };

static bool         ab_lock, band_lock, mode_lock, knobs_shown;
static cfg_ctrl_t   vol_ctrl = CTRL_VOL;
static int32_t      vfo      = RADIO_VFO_A, mode = radio_mode_usb, volume = 30;
static uint32_t     down;               /* the mic key mic_keys.c has down (hkey_down) */
static event_hkey_t ev;
static lv_obj_t    *obj;
static lv_timer_t   timer;
static struct {
    bool run;
} apps[7];
static struct {
    uint8_t press_f1, press_f2, press_f3, long_f1, long_f2, long_f3;
} params = {1, 2, 3, 11, 12, 13};

#define dialog_wefax    (&apps[0])
#define dialog_navtex   (&apps[1])
#define dialog_sstv     (&apps[2])
#define dialog_js8      (&apps[3])
#define dialog_psk      (&apps[4])
#define dialog_olivia   (&apps[5])
#define dialog_jtty     (&apps[6])
#define dialog_channels NULL

void *lv_event_get_param(lv_event_t *e) { return &ev; }

void _lv_log_add(lv_log_level_t level, const char *file, int line, const char *func, const char *format, ...) {
    note("log \"%s\"", format);
}

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *data) {
    note("a timer of %u ms", (unsigned)period);
    return &timer;
}

void lv_timer_del(lv_timer_t *t) { note("timer deleted"); }
void lv_timer_set_period(lv_timer_t *t, uint32_t period) { note("every %u ms", (unsigned)period); }

static bool hkey_down(uint32_t key) { return key == down; }
static int32_t param_i_get(int p) { return vfo; }
static int32_t cparam_i_get(int p) { return mode; }
static void cparam_i_set(int p, int32_t v) { note("mode %d", (int)v); }
static uint32_t radio_caps(void) { return HW_CAP_SAM_WFM; }
static bool knobs_visible(void) { return knobs_shown; }
static cfg_ctrl_t vol_get_ctrl(void) { return vol_ctrl; }

static uint16_t radio_change_vol(int16_t df) {
    volume += df;
    note("volume %d", (int)volume);
    return volume;
}

static void msg_update_text_fmt(const char *fmt, ...) {
    char    b[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    note("\"%s\"", b);
}

static void radio_toggle_vfo(void) {
    vfo = vfo == RADIO_VFO_A ? RADIO_VFO_B : RADIO_VFO_A;
    note("VFO %s", vfo == RADIO_VFO_A ? "A" : "B");
}

static void spectrum_clear(void) { note("spectrum cleared"); }
static void tiny_vfo(void) { note("VFO shown"); }
static void cfg_band_vfo_copy(void) { note("VFO copied"); }
static void band_step(bool up) { note("band %s", up ? "up" : "down"); }
static void controls_set_band_m(int32_t m) { note("%d m", (int)m); }
static void mem_load(uint16_t id) { note("memory %u", id); }
static void toggle_freq_lock(void) { note("lock"); }
static bool toggle_atu(void) { note("ATU"); return true; }
static void radio_start_atu(void) { note("ATU tune"); }
static bool dialog_channels_recall_relative(int d) { note("channel %+d", d); return false; }
static const char *dialog_channels_last_recalled_name(void) { return ""; }
static void channel_overlay_show(const char *name) {}
static void dialog_construct(void *d, lv_obj_t *parent) { note("CHANNELS"); }
static bool page_key(keypad_key_t key, bool tap, bool hold) { note("DFL"); return true; }
static void mode_key(keypad_key_t key, bool hold) { note("DIG"); }
static void swl_gene(void) { note("SWL"); }
static void swl_save_channel(void) { note("NW"); }
static void main_screen_action(int action) { note("action %d", action); }

#include "mic_screen_keys.c"

static int cases, good;

static void check(const char *what, const char *want) {
    const char *got = *did ? did : "nothing";
    bool        ok  = strcmp(got, want) == 0;

    cases++;
    good += ok;
    printf("%s: %s%s\n", what, got, ok ? "" : "   <-- not as it should be");
}

/* A key's event as mic_keys.c posts it to the main screen */
static void key(const char *what, hkey_t k, hkey_state_t state, const char *want) {
    *did = 0;
    ev   = (event_hkey_t){.key = k, .state = state};
    if (state == HKEY_PRESS || state == HKEY_LONG) {
        down = k;
    } else {
        down = HKEY_UNKNOWN;
    }
    mic_key_cb(NULL);
    check(what, want);
}

/* The VOL repeat's timer coming due, while the key is still down or not */
static void due(const char *what, bool held, const char *want) {
    *did = 0;
    if (!held) {
        down = HKEY_UNKNOWN;
    }
    if (mic_vol_timer) {
        mic_vol_repeat_cb(mic_vol_timer);
    } else {
        note("no timer");
    }
    check(what, want);
}

int main(void) {
    /* F1, F2 as before, F3 with its own actions; a press acts on its release, a hold on the long press */
    key("F1 pressed", HKEY_F1, HKEY_PRESS, "nothing");
    key("F1 let go", HKEY_F1, HKEY_RELEASE, "action 1");
    key("F1 held", HKEY_F1, HKEY_LONG, "action 11");
    key("F2 let go", HKEY_F2, HKEY_RELEASE, "action 2");
    key("F2 held", HKEY_F2, HKEY_LONG, "action 12");
    key("F3 pressed", HKEY_F3, HKEY_PRESS, "nothing");
    key("F3 let go", HKEY_F3, HKEY_RELEASE, "action 3");
    key("F3 held", HKEY_F3, HKEY_LONG, "action 13");
    key("F3 let go after the hold", HKEY_F3, HKEY_LONG_RELEASE, "nothing");

    /* VOL+ and VOL-: a step on the press, then on by themselves while held, until let go */
    key("VOL+ pressed", HKEY_VOL_UP, HKEY_PRESS, "volume 31, \"Volume: 31\", a timer of 400 ms");
    key("VOL+ let go", HKEY_VOL_UP, HKEY_RELEASE, "timer deleted");
    due("VOL+ let go, the timer's time", true, "no timer");
    key("VOL- pressed", HKEY_VOL_DOWN, HKEY_PRESS, "volume 30, \"Volume: 30\", a timer of 400 ms");
    key("VOL- let go", HKEY_VOL_DOWN, HKEY_RELEASE, "timer deleted");
    key("VOL+ held", HKEY_VOL_UP, HKEY_PRESS, "volume 31, \"Volume: 31\", a timer of 400 ms");
    due("VOL+ held 0.4 s", true, "volume 32, \"Volume: 32\", every 100 ms");
    due("VOL+ held 0.5 s", true, "volume 33, \"Volume: 33\", every 100 ms");
    key("VOL+ held 1 s, its long press", HKEY_VOL_UP, HKEY_LONG, "nothing");
    due("VOL+ held 1.1 s", true, "volume 34, \"Volume: 34\", every 100 ms");
    key("VOL+ let go after the long press", HKEY_VOL_UP, HKEY_LONG_RELEASE, "timer deleted");
    key("VOL+ held again", HKEY_VOL_UP, HKEY_PRESS, "volume 35, \"Volume: 35\", a timer of 400 ms");
    due("VOL+ let go, the release to another screen (walkie mode)", false, "timer deleted");
    key("VOL+ held", HKEY_VOL_UP, HKEY_PRESS, "volume 36, \"Volume: 36\", a timer of 400 ms");
    key("VOL- pressed with VOL+'s steps on", HKEY_VOL_DOWN, HKEY_PRESS,
        "timer deleted, volume 35, \"Volume: 35\", a timer of 400 ms");
    due("VOL- held 0.4 s", true, "volume 34, \"Volume: 34\", every 100 ms");
    key("VOL- let go", HKEY_VOL_DOWN, HKEY_RELEASE, "timer deleted");
    knobs_shown = true;
    key("VOL+ pressed, the VOL knob's line on the volume", HKEY_VOL_UP, HKEY_PRESS, "volume 35, a timer of 400 ms");
    key("VOL+ let go", HKEY_VOL_UP, HKEY_RELEASE, "timer deleted");
    vol_ctrl = CTRL_SQL;
    key("VOL+ pressed, the VOL knob's line on another control", HKEY_VOL_UP, HKEY_PRESS,
        "volume 36, \"Volume: 36\", a timer of 400 ms");
    key("VOL+ let go", HKEY_VOL_UP, HKEY_RELEASE, "timer deleted");

    /* A/B as the front A/B key: VFO A/B, held this VFO copied to the other one, neither while an app holds them */
    key("A/B pressed", HKEY_AB, HKEY_PRESS, "nothing");
    key("A/B let go", HKEY_AB, HKEY_RELEASE, "VFO B, spectrum cleared, VFO shown");
    key("A/B held", HKEY_AB, HKEY_LONG, "VFO copied, \"Clone VFO B->A\"");
    key("A/B let go after the hold", HKEY_AB, HKEY_LONG_RELEASE, "nothing");
    ab_lock = true;
    key("A/B let go, an app holding the VFOs", HKEY_AB, HKEY_RELEASE, "nothing");
    key("A/B held, an app holding the VFOs", HKEY_AB, HKEY_LONG, "nothing");
    ab_lock = false;

    /* The band up and down codes of Xiegu's app, as the BAND keys */
    key("BAND UP let go", HKEY_BAND_UP, HKEY_RELEASE, "band up");
    key("BAND DOWN let go", HKEY_BAND_DOWN, HKEY_RELEASE, "band down");
    key("BAND UP held", HKEY_BAND_UP, HKEY_LONG, "nothing");

    /* The digits as before: the band, held a memory */
    key("3 let go", HKEY_3, HKEY_RELEASE, "40 m");
    key("3 held", HKEY_3, HKEY_LONG, "memory 3");
    key("0 let go", HKEY_0, HKEY_RELEASE, "6 m");

    printf("%d of %d cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
