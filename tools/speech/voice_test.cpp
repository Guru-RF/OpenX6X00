/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the spoken feedback's rules (src/voice.cpp), the GUI stood in for by stubs/
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 *
 *  Without arguments (no synthesizer on this PC, as a card image without speech): nothing speaks whatever the mode,
 *  the mode key says so and leaves the mode, no call waits, and the words are made as they should ("14.074", "U S B
 *  digital", a callsign spelled). With SPEECH_LIB, SPEECH_DATA and SPEECH_CONFIG (see the Makefile) the announcements
 *  are synthesized, counted and paced as if they played: Off says nothing and takes no speaker; a call to speak
 *  returns at once while the voices load; a knob's thirty values make one announcement; a newer one cuts the one
 *  playing; voice_stop() silences within its 250 ms and gives the speaker back; nothing on the air, with the
 *  recorder on, or (screen off only) with the screen lit; a setting of the speech itself speaks with speech Off;
 *  without interrupting a second one is dropped; a name is said once and then its values.
 */
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "stubs.h"

#include "speech_engine.h"
#include "voice.h"

/* ---- the GUI, stood in for ---- */

static ParamInt p_mode{0}, p_lang{0}, p_rate{100}, p_pitch{100}, p_volume{100}, p_interrupt{1}, p_read{1}, p_repeat{5},
    p_delay{100}, p_fdelay{200};

ParamInt *cfg_voice_mode = &p_mode, *cfg_voice_lang = &p_lang, *cfg_voice_rate = &p_rate, *cfg_voice_pitch = &p_pitch,
         *cfg_voice_volume = &p_volume, *cfg_voice_interrupt = &p_interrupt, *cfg_voice_read_text = &p_read,
         *cfg_voice_repeat_count = &p_repeat, *cfg_voice_interrupt_delay_ms = &p_delay,
         *cfg_voice_freq_interrupt_delay_ms = &p_fdelay;

static int           claims, releases;
static radio_state_t radio_state = RADIO_RX;
static bool          recording, dark;
static char          shown[128];

int32_t param_i_get(const ParamInt *p) { return p->v; }
void    param_i_set(ParamInt *p, int32_t v) { p->v = v; }

void split_freq(int32_t freq, uint16_t *mhz, uint16_t *khz, uint16_t *hz) {
    *mhz = freq / 1000000;
    *khz = (freq / 1000) % 1000;
    *hz  = freq % 1000;
}

static uint32_t ms_now() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32_t    lv_tick_get(void) { return ms_now(); }
uint32_t    lv_tick_elaps(uint32_t prev) { return ms_now() - prev; }
lv_timer_t *lv_timer_create(lv_timer_cb_t, uint32_t, void *) { return NULL; }      /* none is run here */
void        lv_timer_set_repeat_count(lv_timer_t *, int32_t) {}
void        lv_timer_del(lv_timer_t *) {}

void audio_user_claim(audio_user_t) { claims++; }
void audio_user_release(audio_user_t) { releases++; }
bool btaudio_hp_sink(char *, size_t) { return false; }

void msg_update_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(shown, sizeof(shown), fmt, ap);
    va_end(ap);
}

radio_state_t     radio_get_state(void) { return radio_state; }
bool              recorder_is_on(void) { return recording; }
bool              backlight_is_dark(void) { return dark; }
msg_voice_state_t dialog_msg_voice_get_state(void) { return MSG_VOICE_OFF; }

/* ---- */

static int fails;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* Until nothing waits or speaks; false when that takes longer than ms */
static bool wait_idle(int ms) {
    uint32_t t0 = ms_now();

    while (voice_busy()) {
        if (ms_now() - t0 > (uint32_t)ms) return false;
        sleep_ms(2);
    }
    sleep_ms(5);    /* the speaker given back just after */
    return true;
}

static void words(void) {
    char out[128];

    voice_freq_text(14074000, out, sizeof(out));
    CHECK(!strcmp(out, "14.074"));
    voice_freq_text(7000000, out, sizeof(out));
    CHECK(!strcmp(out, "7"));
    voice_freq_text(14074500, out, sizeof(out));
    CHECK(!strcmp(out, "14.074.500"));
    voice_freq_text(3500000, out, sizeof(out));
    CHECK(!strcmp(out, "3.500"));
    voice_mode_text("USB-D", out, sizeof(out));
    CHECK(!strcmp(out, "U S B digital"));
    voice_mode_text("CW-R", out, sizeof(out));
    CHECK(!strcmp(out, "C W reverse"));
    voice_mode_text("NFM", out, sizeof(out));
    CHECK(!strcmp(out, "N F M"));
    voice_spell_calls("K1ABC FN42", out, sizeof(out));
    CHECK(!strcmp(out, "K 1 A B C, F N 4 2"));
    voice_spell_calls("CQ ON4/K1ABC", out, sizeof(out));
    CHECK(!strcmp(out, "CQ, O N 4 stroke K 1 A B C"));
    voice_clean_text("#ff0000 TX stopped#\nSWR 3.1 & more...", out, sizeof(out));
    CHECK(!strcmp(out, "TX stopped, SWR 3.1 and more"));
    voice_clean_text("21 \xC2\xB0""C", out, sizeof(out));
    CHECK(!strcmp(out, "21 degrees C"));
}

int main(void) {
    const char *l = getenv("SPEECH_LIB"), *data = getenv("SPEECH_DATA"), *config = getenv("SPEECH_CONFIG");
    const char *line = "14.074, U S B digital, VFO A, 20 meters, 10:42, battery 80 percent";
    uint32_t    t0;

    speech_engine_test_mute(true, true);
    voice_auto_set(true);
    words();

    if (!l || !l[0]) {
        speech_engine_test_paths("/nonexistent/libRHVoice.so.5", NULL, NULL);
        p_mode.v = VOICE_ALWAYS;
        CHECK(!voice_available());
        CHECK(!voice_is_on());
        CHECK(!voice_auto_active());
        voice_say_text_fmt("%s", line);
        CHECK(!voice_busy());
        voice_change_mode();
        CHECK(p_mode.v == VOICE_ALWAYS);
        CHECK(strstr(shown, "not on this card image") != NULL);
        t0 = ms_now();
        voice_stop();
        CHECK(ms_now() - t0 < 5);
        CHECK(claims == 0 && releases == 0);
        printf("voice: no synthesizer, nothing speaks; the words: %s\n", fails ? "FAILED" : "ok");
        return fails ? 1 : 0;
    }
    speech_engine_test_paths(l, data, config);

    /* Off: nothing, and the mode key turns it on a step at a time */
    p_mode.v = VOICE_OFF;
    CHECK(!voice_is_on() && !voice_auto_active());
    voice_say_text_fmt("%s", line);
    voice_say_freq(14074000);
    CHECK(!voice_busy());
    CHECK(claims == 0);

    /* a setting of the speech itself is said with speech Off, for a second and a half */
    voice_sure();
    voice_say_text_fmt("Speech off");
    CHECK(voice_busy());
    CHECK(wait_idle(8000));     /* the first announcement loads the voices */
    CHECK(claims == 1 && releases == 1);
    CHECK(speech_engine_test_samples() > 4800);
    sleep_ms(1600);
    voice_say_text_fmt("Speech off");
    CHECK(!voice_busy());

    /* Always: the call returns at once, the announcement is said to its end, the speaker taken and given back */
    p_mode.v = VOICE_ALWAYS;
    CHECK(voice_is_on() && voice_auto_active());
    t0 = ms_now();
    voice_say_text_fmt("%s", line);
    CHECK(ms_now() - t0 < 10);
    CHECK(voice_busy());
    sleep_ms(200);
    CHECK(voice_busy());        /* paced: seconds of speech */
    CHECK(wait_idle(15000));
    CHECK(claims == 2 && releases == 2);

    uint32_t full = speech_engine_test_samples();

    CHECK(full > 3 * 24000);

    /* A knob: thirty values 5 ms apart, each debounced 100 ms: one announcement, of the last */
    t0 = ms_now();
    for (int i = 0; i < 30; i++) {
        voice_say_int("Volume,", i);
        sleep_ms(5);
    }
    CHECK(ms_now() - t0 < 30 * 5 + 100);    /* no call waited for the one before */
    CHECK(wait_idle(5000));
    CHECK(claims == 3 && releases == 3);

    /* the same name again: its value alone, shorter */
    uint32_t named = speech_engine_test_samples();

    voice_say_int("Volume,", 30);
    CHECK(wait_idle(5000));
    CHECK(speech_engine_test_samples() < named * 8 / 10);
    CHECK(claims == 4 && releases == 4);

    /* A newer announcement cuts the one playing: the call returns at once, the first is not said to its end */
    voice_say_text_fmt("%s", line);
    sleep_ms(300);
    t0 = ms_now();
    voice_say_text_fmt("Mode locked by the app");
    CHECK(ms_now() - t0 < 10);
    CHECK(wait_idle(5000));
    CHECK(claims == 6 && releases == 6);
    CHECK(speech_engine_test_samples() < full / 2);     /* the second one's, a short line */

    /* voice_stop(): silent within its 250 ms (a piece of audio is 20 ms), the one waiting taken back too */
    voice_say_text_fmt("%s", line);
    sleep_ms(300);
    t0 = ms_now();
    voice_stop();
    CHECK(ms_now() - t0 < 100);
    CHECK(speech_engine_test_samples() < full / 2);
    CHECK(wait_idle(200));
    CHECK(claims == 7 && releases == 7);
    voice_delay_say_text_fmt("%s", line);       /* waiting its 100 ms */
    voice_stop();
    CHECK(wait_idle(50));
    sleep_ms(200);
    CHECK(claims == 7);

    /* Nothing on the air, nothing into a recording */
    radio_state = RADIO_TX;
    CHECK(!voice_auto_active());
    voice_say_text_fmt("%s", line);
    CHECK(!voice_busy());
    radio_state = RADIO_ATU_RUN;
    voice_say_text_fmt("%s", line);
    CHECK(!voice_busy());
    radio_state = RADIO_RX;
    recording   = true;
    voice_say_text_fmt("%s", line);
    CHECK(!voice_busy());
    recording = false;

    /* Screen off only: silent with the screen lit */
    p_mode.v = VOICE_LCD;
    dark     = false;
    voice_say_text_fmt("Band locked by the app");
    CHECK(!voice_busy());
    dark = true;
    voice_say_text_fmt("Band locked by the app");
    CHECK(voice_busy());
    CHECK(wait_idle(5000));
    CHECK(claims == 8 && releases == 8);
    p_mode.v = VOICE_ALWAYS;

    /* Without interrupting: a second announcement is dropped while one speaks */
    p_interrupt.v = 0;
    voice_say_text_fmt("%s", line);
    sleep_ms(300);
    voice_say_text_fmt("Mode locked by the app");
    CHECK(wait_idle(15000));
    CHECK(speech_engine_test_samples() == full);
    CHECK(claims == 9 && releases == 9);
    p_interrupt.v = 1;

    /* The mode key: always, off (said), screen off only */
    voice_change_mode();
    CHECK(p_mode.v == VOICE_OFF);
    CHECK(voice_busy());        /* "Speech off. Hold D F N for speech again" */
    CHECK(wait_idle(8000));
    voice_change_mode();
    CHECK(p_mode.v == VOICE_LCD);
    CHECK(wait_idle(8000));

    printf("voice: off, at once, debounced, cut, stopped, never on the air: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
