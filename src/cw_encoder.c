/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */
#include "cw_encoder.h"

#include "cw_decoder.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "panel.h"

#include "lvgl/lvgl.h"
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sched.h>


static cw_encoder_state_t   state = CW_ENCODER_IDLE;
static pthread_t            thread;
static bool                 thread_started;

static char                 *current_msg = NULL;
static char                 *current_char = NULL;

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

/* Keys the text (again after a pause, as a beacon) until done or stopped.
 * Stopping is by flag: the key is always released by this thread. */
static void * endecode_thread(void *arg) {
    long dit_nsec         = 20000000L / (param_i_get(cfg_key_speed)) * 60;
    long dah_nsec         = dit_nsec * param_f_get(cfg_key_ratio);
    long world_space_nsec = dit_nsec * 7;

    hold_tx(world_space_nsec, true);

    while (!stop_req) {
        char    *morse;
        uint8_t len;

        if (*current_char == ' ') {
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
            state = CW_ENCODER_BEACON_IDLE;
            hold_tx(world_space_nsec, false);   /* receive between beacons */
            for (int t = params.cw_encoder_period * 10; t > 0 && !stop_req; t--) {
                pause_left = (t + 9) / 10;
                usleep(100000);
            }
            hold_tx(world_space_nsec, true);
            state        = CW_ENCODER_BEACON;
            current_char = current_msg;
        }
    }
    radio_set_morse_key(false);
    hold_tx(world_space_nsec, false);
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

void cw_encoder_send(const char *text, bool beacon) {
    cw_encoder_stop();

    if (!text || !*text) {
        return;
    }
    if (current_msg) {
        free(current_msg);
    }

    current_msg = strdup(text);
    current_char = current_msg;
    state = beacon ? CW_ENCODER_BEACON : CW_ENCODER_SEND;
    stop_req = false;

    pthread_attr_t attr;
    struct sched_param param;
    pthread_attr_init(&attr);
    pthread_attr_getschedparam(&attr, &param);
    param.sched_priority += 5;
    pthread_attr_setschedparam(&attr, &param);

    thread_started = pthread_create(&thread, &attr, endecode_thread, NULL) == 0;
    if (!thread_started) {
        state = CW_ENCODER_IDLE;
    }
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
