/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what src/voice.cpp needs of the GUI, for its host test (voice_test.cpp): the settings, the radio's
 *  state, the speaker's claims, LVGL's tick and timers. Every header voice.cpp includes is this one here.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* cfg/cfg_api.h */
typedef struct ParamInt {
    int32_t v;
} ParamInt;

extern ParamInt *cfg_voice_mode, *cfg_voice_lang, *cfg_voice_rate, *cfg_voice_pitch, *cfg_voice_volume,
    *cfg_voice_interrupt, *cfg_voice_read_text, *cfg_voice_repeat_count, *cfg_voice_interrupt_delay_ms,
    *cfg_voice_freq_interrupt_delay_ms;

int32_t param_i_get(const ParamInt *p);
void    param_i_set(ParamInt *p, int32_t v);

/* util.h */
void split_freq(int32_t freq, uint16_t *mhz, uint16_t *khz, uint16_t *hz);

/* lvgl/lvgl.h */
typedef struct lv_timer_t lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *);

uint32_t    lv_tick_get(void);
uint32_t    lv_tick_elaps(uint32_t prev);
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data);
void        lv_timer_set_repeat_count(lv_timer_t *t, int32_t n);
void        lv_timer_del(lv_timer_t *t);

/* audio.h, btaudio.h */
typedef enum { AUDIO_USER_SPEECH = 4 } audio_user_t;

void audio_user_claim(audio_user_t u);
void audio_user_release(audio_user_t u);
bool btaudio_hp_sink(char *buf, size_t size);

/* msg.h (msg_box.h) */
void msg_update_text_fmt(const char *fmt, ...);

/* radio.h */
typedef enum { RADIO_RX = 0, RADIO_TX, RADIO_ATU_START, RADIO_ATU_WAIT, RADIO_ATU_RUN, RADIO_SWRSCAN } radio_state_t;

radio_state_t radio_get_state(void);

/* recorder.h, screen_light.h, voice_messages.h */
typedef enum { MSG_VOICE_OFF, MSG_VOICE_RECORD, MSG_VOICE_PLAY } msg_voice_state_t;

bool              recorder_is_on(void);
bool              backlight_is_dark(void);
msg_voice_state_t dialog_msg_voice_get_state(void);

#ifdef __cplusplus
}
#endif
