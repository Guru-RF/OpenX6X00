/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - spoken feedback (accessibility)
 *
 *  From Jimr9's OpenX6X00 Speech Access (github.com/Jimr9/OpenX6X00-Speech-Access) and his X6100 Accessibility fork
 *  (github.com/Jimr9/X6100-Accessibility-Testing), whose engine came from R1CBU's x6100_gui (Copyright (c) 2022-2023
 *  Belousov Oleg aka R1CBU).
 *
 *  Off by default (Settings > Voice > Voice mode, or the DFN key held). Off, and on a card image without the
 *  synthesizer, every call below returns at once and says nothing.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICES_NUM 4

typedef enum {
    VOICE_OFF = 0,
    VOICE_LCD,      /* only while the screen is dark */
    VOICE_ALWAYS
} voice_mode_t;

typedef struct {
    const char *name;
    const char *label;
    const char *welcome;
} voice_item_t;

extern voice_item_t voice_item[VOICES_NUM];

/* What is said in the next second and a half is spoken whatever the mode (the voice settings themselves) */
void voice_sure();
/* Off, LCD, Always, Off... said and saved */
void voice_change_mode();

/* Immediately (a key press) */
void voice_say_text_fmt(const char *fmt, ...);
/* After the debounce delay, superseding one still waiting (a knob turning) */
void voice_delay_say_text_fmt(const char *fmt, ...);
/* "prompt value": after a few repeats of the same prompt only the value is said */
void voice_say_prompted_fmt(const char *prompt, const char *fmt, ...);
void voice_say_freq(uint64_t freq);         /* VFO knob: its own, longer debounce */
void voice_say_freq_now(uint64_t freq);     /* a jump: at once */
void voice_say_prompted_freq_now(const char *prompt, uint64_t freq);

void voice_say_bool(const char *prompt, bool x);
void voice_say_int(const char *prompt, int32_t x);
void voice_say_float(const char *prompt, float x);
void voice_say_float2(const char *prompt, float x);
void voice_say_text(const char *prompt, const char *x);
void voice_say_lang();

/* Every attempt to speak counts: a caller can tell whether what it ran said anything */
uint32_t voice_say_count();
void voice_count_bump();
/* The app speech helper: focus changes, pop-ups and silent F-key presses spoken (voice_focus.c) */
void voice_auto_set(bool on);
bool voice_auto_active();
/* Cuts any announcement off and takes back one still waiting: before a transmission, a recording. Any thread;
 * returns once the audio has stopped, 250 ms at most, at once when nothing plays */
void voice_stop();

/* Speech not Off in Settings > Voice: then a key or a knob turn that wakes the screen also acts, as a blind
 * operator cannot see that the screen had gone dark (screen_light.c, front_keys.c) */
bool voice_is_on();

/* The card image has the synthesizer (speech_engine.h): Settings > Voice says so when it has not */
bool voice_available();

/* An announcement is waiting for its delay or playing */
bool voice_busy();

/* Said in front of the next announcement if one comes within 400 ms, else alone ("Closed": a window closing
 * loads its F-key page, which is announced) */
void voice_say_prefix(const char *text);

/* A digital mode's text made sayable: each word with letters and digits mixed (a callsign, a grid, RR73) spelled
 * out, "K1ABC FN42" as "K 1 A B C, F N 4 2"; other words as they are */
void voice_spell_calls(const char *in, char *out, size_t size);

/* "192 dot 168 dot 5 dot 80": the radio's WiFi address as said, "" when it has none */
void voice_ip_text(char *out, size_t size);

/* Test builds: a line in /tmp/voice.log */
void voice_trace(const char *fmt, ...);
/* Screen text made speakable: colour codes, symbols, line breaks out */
void voice_clean_text(const char *in, char *out, size_t size);
void voice_say_clean(const char *text, bool delayed);
/* A knob's "Name: value" line (controls_encoder_update()): debounced, and after a few turns of the
 * same control only its value is said */
void voice_say_ctrl(const char *line);

/* A knob pressed: "VOL, choose, RF gain, 100" or "M F K, change, Zoom, x2", the name always said (the press
 * otherwise sounded like one more turn, its name dropped as a repeat) */
void voice_say_knob_state(const char *knob, bool choosing, const char *line);
/* The message box (msg_box.c) reads each message out as it is shown, except while transmitting and
 * those posted between voice_msg_hold(true) and (false) on the same thread: said another way already */
void voice_msg_hold(bool on);
bool voice_msg_held();
void voice_say_msg(const char *text);
/* "14.074" as said for a frequency */
void voice_freq_text(uint64_t freq, char *out, size_t size);
/* A mode's short name ("USB-D", "CW-R", "NFM") as it is said: "U S B digital", "C W reverse", "N F M" */
void voice_mode_text(const char *name, char *out, size_t size);

#ifdef __cplusplus
}
#endif
