/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's session: rootfs/usr/sbin/x6100-zello on a channel, started from the app (dialog_zello.c)
 *  and ended when the app closes, unless the Simplex repeater is on; nothing starts it at power on. What the channel
 *  says plays on the speaker at the radio's volume (on the Bluetooth headphones while they stand in for it). The
 *  user's messages: the hand mic without its PTT (the base sends it to Linux while it receives, AUDIO_PLAY_VOICE_REC)
 *  or a Bluetooth headset's mic (the helper records it).
 *
 *  What keys the radio here is the Simplex repeater alone (zello_link.h), off by default, under main's TX rules: the
 *  key through tx_player's claim (no other app's slot meanwhile) and radio_set_modem, which refuses where the radio may
 *  not transmit (TX ranges, the amateur bands, TX outside them, the WR12, no data from the base); the SWR protection
 *  and a band stop end it and stop Zello to RF; the radio's power setting at most the apps' cap (tx_player's 7 W) and
 *  the radio's top; never while anyone else transmits; the operator's PTT ends it at once; a time limit and a pause
 *  between transmissions; the CW ID. Everything else is over the internet only:
 *  - with the app open the PTT key talks on Zello and never reaches the radio's PTT (zello_ptt.h, screen_main.c);
 *  - while the radio transmits, whatever keyed it, and half a second after, the channel's audio is dropped and the
 *    base's path is the radio's own (zello_path); before a key from the GUI it goes at once, on the keying thread
 *    (radio_set_tx_prepare_fn), so none of it goes out with the TX audio. A message being sent ends.
 *
 *  Four threads: the GUI's, the play thread (the helper's socket, the base's path, the speaker, the link's rules), the
 *  link's transmission (one at a time) and the audio thread (the hand mic, the receiver's audio).
 *
 *  After the CX8ANM edition's Zello (condoruy/OpenX6X00-CX8ANM 2cfc9264, bec4fb36, 1aa72f4a): the helper with its
 *  own audio thread, its quick reconnect and its log, the screen that starts it again by itself, the hand mic in
 *  record mode, the speaker's audio in pieces it has room for.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "zello.h"

#include "app_window.h"
#include "audio.h"
#include "cfg/cfg_api.h"
#include "cfg/computed_api.h"
#include "cfg/parameter_api.h"
#include "cfg/settings_manager_api.h"
#include "cw_encoder.h"
#include "fonts/font_select.h"
#include "gui_styles.h"
#include "gui_events.h"
#include "i18n.h"
#include "lvgl/lvgl.h"
#include "msg_box.h"
#include "olivia_resamp.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "theme.h"
#include "tx_player.h"
#include "util.h"
#include "zello_link.h"
#include "zello_ptt.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

#define ZRATE         16000         /* Hz: the helper's audio, both ways */
#define PIECE         1024          /* samples a write to the speaker (audio_user_write) */
#define ROOM_WAIT_MS  3000          /* the speaker taking nothing for this long: the rest of the block is dropped */
#define MIC_SKIP_MS   150           /* the capture's first ms on the hand mic: still the receiver's */
#define MIC_QUIET_MS  3000          /* a message with nothing from the hand mic this long: said once */
#define MIC_QUIET     330           /* the loudest under this: no hand mic (as the voice messages tell it) */
#define MIC_BLOCK     1024          /* capture samples resampled at a time */
#define REC_MAX       8192          /* bytes of a record from the helper */
#define RELAUNCH_MS   2000          /* the helper gone by itself: started again after this */
#define RELAUNCH_MAX  20
#define STEP_US       50000         /* the helper's end: looked at every 50 ms */
#define TERM_STEPS    20            /* SIGTERM after 1 s */
#define KILL_STEPS    40            /* SIGKILL after 2 s */
#define KEY_HOLD_MS   200           /* after a key from the GUI: off until the radio's own flags say TX */
#define PLAY_HOLD_MS  600           /* after the channel's last audio: still its path (the gaps between packets) */
#define RING_N        (ZRATE * 12)  /* the channel's audio waiting to go on the air: 12 s */
#define TX_FRAME      320           /* 20 ms at ZRATE, the transmission's step */
#define LEAD_MS       150           /* keyed, before the audio: the base's TX settles */
#define ID_WPM        20
#define ID_HZ         800
#define ID_AMP        0.25f
#define SQL_BLOCK     (AUDIO_CAPTURE_RATE / 50)     /* 20 ms of the receiver's audio a squelch reading */
#define TIMER_MS      250
#define TX_HANG_MS    30000         /* a transmission this long past the time limit: hung (an ID takes 2-6 s) */

extern char **environ;

/* What the helper said, from the play thread to the screen */
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    zello_conn_t state;
    char         reason[96];
    int          retry_s;
    int          users;
    char         talker[64];
    char         notice[112];   /* said once */
} st;

/* GUI thread */
static char        cur_channel[64];
static bool        thread_live;             /* the play thread has not ended yet */
static bool        want_on;                 /* on a channel, by the user's choice */
static bool        restart_pending;         /* another channel chosen: started once the last one has ended */
static bool        window_open;             /* the app's window (zello_connect from it, zello_window_closed) */
static int         relaunches;
static bool        talk_by_ptt;
static bool        quiet_told;
static bool        prepared;                /* radio_set_tx_prepare_fn, the resamplers: once */
static int         via_sent = -1;           /* the talkvia last sent to this helper: 1 bt, 0 mic, -1 none yet */
static zello_ptt_t ptt;
static lv_timer_t *timer;
static lv_obj_t   *badge;                   /* "ZELLO LINK" over the main screen while the link is on */

/* The helper's socket: its sends from three threads, its end */
static pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER;
static int             helper_fd = -1;

static atomic_bool running;         /* the play thread is to go on */
static atomic_bool connected;       /* the channel online */
static atomic_bool talk_want;       /* the user's message wanted: Talk, or the PTT key held */
static atomic_int  talk_pending;    /* the user's talk start sent, its talk on or talk refused not seen yet */
static atomic_bool helper_error;    /* it gave up ("error ..."): not started again by itself */
static atomic_bool btmic_on;        /* the helper sees a Bluetooth headset's mic (talkvia bt) */
static atomic_bool talk_bt;         /* the user's message is from the Bluetooth mic, not the hand mic */
static atomic_bool talker_on;       /* someone talks on the channel */

/* The base's audio path and the speaker's stream: the play thread, and tx_prepare on a thread that keys */
static pthread_mutex_t  gate_lock = PTHREAD_MUTEX_INITIALIZER;
static zello_path_t     applied = ZELLO_PATH_OFF;
static atomic_int       path_now;   /* applied, for the audio thread */
static _Atomic uint64_t keyed_ms;   /* when the GUI last went to key */
static uint64_t         heard_ms;   /* the channel's last audio (play thread) */

/* The hand mic while the user's message from it is sent: the audio thread */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

static atomic_bool      mic_on;
static atomic_int       mic_in_put;
static _Atomic uint64_t mic_from_ms;
static _Atomic uint64_t mic_since_ms;
static atomic_int       mic_peak;
static olivia_resamp_t  mic_rs;
static bool             mic_rs_ok;
static biquad_t         mic_filter[3];

/* The Simplex repeater. Its rules on the play thread (link, zello_link_step) */
static zello_link_t     zlink;
static atomic_bool      link_on;        /* a direction on, for the badge and the window */
static atomic_int       link_show;      /* zello_link_show_t */
static atomic_bool      link_stopped;   /* Zello to RF stopped by the radio */
static atomic_bool      rearm_rf;       /* switched on again: armed */
static atomic_bool      ptt_pressed;    /* a PTT press since the play thread last looked: the operator */
static atomic_bool      rx_tap;         /* RF to Zello on: the audio thread reads the receiver */
static atomic_bool      link_rx;        /* the receiver to the channel now */
static atomic_bool      rx_ended;       /* the helper ended or refused that message */
static atomic_bool      sql_open;       /* the squelch as the receiver's audio shows it */
static atomic_int       rx_in_put;
static olivia_resamp_t  rx_rs;
static bool             rx_rs_ok;
static atomic_bool      rx_rs_reset;

/* The channel's audio waiting to go on the air (the play thread puts, the transmission takes) */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t         ring[RING_N];
static size_t          ring_r, ring_n;

/* The link's transmission: one thread at a time */
static atomic_bool tx_live;         /* its thread runs */
static atomic_int  tx_stop;         /* 0 go on; 1 what is queued played out, 2 dropped, then the ID if asked for and
                                     * unkey; 3 unkey now, nothing more (the operator) */
static atomic_bool tx_with_id;      /* the ID before unkeying, set before tx_stop */
static atomic_int  tx_result;       /* zello_tx_result_t, once tx_live has gone false */
static char        tx_call[32];     /* the callsign for its ID, set before the thread starts */
static uint64_t    tx_began_ms;     /* its start (play thread) */
static bool        tx_forced;       /* the play thread unkeyed it: it hung (play thread) */

static void play_ended(void *arg);
static void timer_cb(lv_timer_t *t);

/* ---------- the helper ---------- */

static void send_line(const char *line) {
    pthread_mutex_lock(&fd_lock);
    if (helper_fd >= 0) send(helper_fd, line, strlen(line), MSG_DONTWAIT | MSG_NOSIGNAL);
    pthread_mutex_unlock(&fd_lock);
}

static void send_audio(const int16_t *pcm, int m) {
    uint8_t rec[1 + 2 * 1024];

    if (m <= 0) return;
    if (m > 1024) m = 1024;
    rec[0] = 0;
    memcpy(rec + 1, pcm, 2 * (size_t)m);
    pthread_mutex_lock(&fd_lock);
    if (helper_fd >= 0) send(helper_fd, rec, 1 + 2 * (size_t)m, MSG_DONTWAIT | MSG_NOSIGNAL);
    pthread_mutex_unlock(&fd_lock);
}

static void notice(const char *text) {
    pthread_mutex_lock(&st_lock);
    snprintf(st.notice, sizeof(st.notice), "%s", text);
    pthread_mutex_unlock(&st_lock);
}

/* A line of the helper's (play thread) */
static void on_line(char *line) {
    line[strcspn(line, "\n")] = 0;
    pthread_mutex_lock(&st_lock);
    if (!strcmp(line, "state connecting")) {
        st.state = ZELLO_CONNECTING;
    } else if (!strcmp(line, "state online")) {
        st.state = ZELLO_ONLINE;
    } else if (!strncmp(line, "state offline ", 14)) {
        st.state = ZELLO_OFFLINE;
        snprintf(st.reason, sizeof(st.reason), "%s", line + 14);
    } else if (!strncmp(line, "state retry ", 12)) {
        char *why = NULL;

        st.state   = ZELLO_RETRY;
        st.retry_s = (int)strtol(line + 12, &why, 10);
        snprintf(st.reason, sizeof(st.reason), "%s", why && *why == ' ' ? why + 1 : "");
    } else if (!strncmp(line, "error ", 6)) {
        st.state = ZELLO_ERROR;
        snprintf(st.reason, sizeof(st.reason), "%s", line + 6);
        atomic_store(&helper_error, true);
    } else if (!strncmp(line, "channel ", 8)) {
        st.users = line[8] == '-' ? -1 : atoi(line + 8);
    } else if (!strncmp(line, "talker", 6)) {
        snprintf(st.talker, sizeof(st.talker), "%s", line[6] == ' ' ? line + 7 : "");
        atomic_store(&talker_on, st.talker[0] != 0);
    } else if (!strcmp(line, "btmic on") || !strcmp(line, "btmic off")) {
        atomic_store(&btmic_on, line[7] == 'n');
    } else if (!strcmp(line, "talk on")) {
        if (atomic_load(&talk_pending) > 0) atomic_fetch_sub(&talk_pending, 1);
    } else if (!strcmp(line, "talk off") || !strncmp(line, "talk refused ", 13)) {
        bool refused = line[5] == 'r';

        if (atomic_load(&talk_pending) > 0) {
            /* the answer to the user's talk start, or the end of the message before it */
            if (refused) {
                atomic_fetch_sub(&talk_pending, 1);
                atomic_store(&talk_want, false);
                snprintf(st.notice, sizeof(st.notice), "%s: %s", _("Zello: not sent"), line + 13);
            }
        } else if (atomic_load(&link_rx)) {
            atomic_store(&link_rx, false);      /* the link's message to Zello: ended by the helper or refused */
            atomic_store(&rx_ended, true);
            if (refused) snprintf(st.notice, sizeof(st.notice), "%s: %s", _("RF to Zello: not sent"), line + 13);
        } else {
            atomic_store(&talk_want, false);    /* the user's: the helper ended it (two minutes, the line lost) */
            if (refused) snprintf(st.notice, sizeof(st.notice), "%s: %s", _("Zello: not sent"), line + 13);
        }
    }
    atomic_store(&connected, st.state == ZELLO_ONLINE);
    pthread_mutex_unlock(&st_lock);
}

/* The radio transmits or the GUI keys it, or went to a moment ago */
static bool on_air(uint64_t now) {
    return radio_get_state() != RADIO_RX || radio_get_ptt() || radio_get_modem() ||
           now - atomic_load(&keyed_ms) < KEY_HOLD_MS;
}

/* RBJ cookbook at the capture rate: 0 high pass, 2 notch */
static void biquad_set(biquad_t *f, int kind, float hz, float q) {
    float w = 2.0f * (float)M_PI * hz / AUDIO_CAPTURE_RATE, c = cosf(w), a = sinf(w) / (2.0f * q);
    float a0 = 1.0f + a;

    if (kind == 0) {
        f->b0 = (1.0f + c) / 2.0f; f->b1 = -(1.0f + c); f->b2 = f->b0;
    } else {
        f->b0 = 1.0f; f->b1 = -2.0f * c; f->b2 = 1.0f;
    }
    f->b0 /= a0; f->b1 /= a0; f->b2 /= a0;
    f->a1 = -2.0f * c / a0;
    f->a2 = (1.0f - a) / a0;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_run(biquad_t *f, float x) {
    float y = f->b0 * x + f->z1;

    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* The hand mic for a new message: filters and resampler from the start, the first ms skipped. mic_on is off */
static void mic_start(void) {
    uint64_t now = get_time();

    while (atomic_load(&mic_in_put)) usleep(500);
    /* The hand mic input carries the radio's hum (multiples of 33-40 Hz) and a whistle at 781 Hz (voice_messages.c):
     * out with 250 Hz and a narrow notch; the resampler cuts above 7 kHz */
    biquad_set(&mic_filter[0], 0, 250.0f, 0.707f);
    biquad_set(&mic_filter[1], 0, 250.0f, 0.707f);
    biquad_set(&mic_filter[2], 2, 781.0f, 10.0f);
    if (mic_rs_ok) olivia_resamp_reset(&mic_rs);
    atomic_store(&mic_peak, 0);
    atomic_store(&mic_from_ms, now + MIC_SKIP_MS);
    atomic_store(&mic_since_ms, now);
}

/* The path left, with gate_lock held: what waits in its own stream dropped (a station's stays), its claim on the
 * speaker or its Rec given back (audio_owner.h: the speaker the receiver's once nobody claims it) */
static void leave_path(void) {
    if (applied == ZELLO_PATH_LISTEN) {
        audio_user_flush(AUDIO_USER_ZELLO);
        audio_user_release(AUDIO_USER_ZELLO);
    } else if (applied == ZELLO_PATH_TALK) {
        audio_user_rec(AUDIO_USER_ZELLO, false);
    }
}

/* To path, with gate_lock held. LISTEN claims the speaker (or the headphones standing in for it): a message from the
 * channel over an Internet radio station, which goes on after it; TALK records the hand mic */
static void apply_path(zello_path_t to) {
    if (to == applied) return;
    leave_path();
    atomic_store(&mic_on, false);
    switch (to) {
        case ZELLO_PATH_OFF:
            break;
        case ZELLO_PATH_LISTEN:
            audio_user_claim(AUDIO_USER_ZELLO);
            break;
        case ZELLO_PATH_TALK:
            mic_start();
            audio_user_rec(AUDIO_USER_ZELLO, true);
            atomic_store(&mic_on, true);
            break;
    }
    applied = to;
    atomic_store(&path_now, to);
}

/* Before the GUI keys, on the thread that keys (radio_set_tx_prepare_fn): the channel's audio out of the codec's
 * stream and the path the radio's own, before the key goes down. The link's own key too: its audio comes after */
static void tx_prepare(void) {
    pthread_mutex_lock(&gate_lock);
    atomic_store(&keyed_ms, get_time());
    if (applied != ZELLO_PATH_OFF) {
        atomic_store(&mic_on, false);
        leave_path();
        applied = ZELLO_PATH_OFF;
        atomic_store(&path_now, ZELLO_PATH_OFF);
    }
    pthread_mutex_unlock(&gate_lock);
}

/* The user's message cut by the radio's TX or the end of the channel (play thread) */
static void talk_cut(const char *why) {
    if (!atomic_exchange(&talk_want, false)) return;
    send_line("talk stop\n");
    notice(why);
}

/* ---------- the link ---------- */

static void ring_put(const uint8_t *pcm, size_t n) {
    pthread_mutex_lock(&ring_lock);
    for (size_t i = 0; i < n; i++) {
        int16_t v;

        memcpy(&v, pcm + 2 * i, 2);
        if (ring_n == RING_N) {     /* full: the oldest goes */
            ring_r = (ring_r + 1) % RING_N;
            ring_n--;
        }
        ring[(ring_r + ring_n) % RING_N] = v;
        ring_n++;
    }
    pthread_mutex_unlock(&ring_lock);
}

static size_t ring_take(int16_t *out, size_t max) {
    size_t n;

    pthread_mutex_lock(&ring_lock);
    n = ring_n < max ? ring_n : max;
    for (size_t i = 0; i < n; i++) out[i] = ring[(ring_r + i) % RING_N];
    ring_r = (ring_r + n) % RING_N;
    ring_n -= n;
    pthread_mutex_unlock(&ring_lock);
    return n;
}

static size_t ring_count(void) {
    size_t n;

    pthread_mutex_lock(&ring_lock);
    n = ring_n;
    pthread_mutex_unlock(&ring_lock);
    return n;
}

static void ring_clear(void) {
    pthread_mutex_lock(&ring_lock);
    ring_r = ring_n = 0;
    pthread_mutex_unlock(&ring_lock);
}

static void link_cfg(zello_link_cfg_t *c) {
    c->to_rf        = param_i_get(cfg_zello_to_rf);
    c->to_zello     = param_i_get(cfg_zello_to_zello) && param_i_get(cfg_squelch) > 0;
    c->id_on        = param_i_get(cfg_zello_id);
    c->id_period_ms = (uint32_t)param_i_get(cfg_zello_id_min) * 60000u;
    c->limit_ms     = (uint32_t)param_i_get(cfg_zello_limit) * 1000u;
    c->pause_ms     = (uint32_t)param_i_get(cfg_zello_pause) * 1000u;
}

/* A voice mode, a callsign for the ID, the squelch to tell a busy frequency */
static bool voice_mode(void) {
    switch (cparam_i_get(cfg_cur_mode)) {
        case radio_mode_lsb:
        case radio_mode_usb:
        case radio_mode_am:
        case radio_mode_nfm:
            return true;
        default:
            return false;
    }
}

static bool can_key(const zello_link_cfg_t *c) {
    return voice_mode() && (!c->id_on || params.callsign.x[0]) && param_i_get(cfg_squelch) > 0;
}

/* Someone else on the air or about to be: the radio's TX that is not ours, the GUI's PTT, a transmission holding the
 * player, the keyer (a beacon), the user talking on Zello */
static bool radio_busy(void) {
    bool ours = atomic_load(&tx_live);

    return (!ours && (radio_get_state() != RADIO_RX || radio_get_modem() || tx_player_busy())) || radio_get_ptt() ||
           cw_encoder_state() != CW_ENCODER_IDLE || atomic_load(&talk_want);
}

/* The channel's audio is the link's: on the air, or dropped while its message was cut; not on the speaker */
static bool relaying(const zello_link_cfg_t *c) {
    return c->to_rf && !zlink.to_rf_stopped && can_key(c);
}

/* The link's transmission: keyed as a voice message is sent (radio_set_modem, the audio from the GUI), held by
 * tx_player's claim so that no app's slot keys meanwhile; the power at most the apps' cap */
static void *tx_fn(void *arg) {
    bool              id_only = (bool)(intptr_t)arg;
    zello_tx_result_t result  = ZELLO_TX_NOT_KEYED;
    olivia_resamp_t   rs;
    bool              rs_ok   = olivia_resamp_init(&rs, ZRATE, AUDIO_PLAY_RATE) == 0;
    int16_t           in16[TX_FRAME], out16[TX_FRAME * 3 + 8], zeros[AUDIO_PLAY_RATE / 50];
    float             inf[TX_FRAME], outf[TX_FRAME * 3 + 8];
    int16_t          *id = NULL;
    bool              claimed = false, pwr_set = false;
    unsigned          swr0, stops0;

    memset(zeros, 0, sizeof(zeros));
    if (!rs_ok || !(claimed = tx_player_claim())) goto out;
    if (radio_get_state() != RADIO_RX || radio_get_ptt() || radio_get_modem()) goto out;

    float set = param_f_get(cfg_pwr);
    float w   = LV_MIN(LV_MIN(set, TX_PLAYER_MAX_PWR_W), cfg_pwr_max_get());

    pwr_set = w != set;
    if (pwr_set) radio_set_pwr(w);
    swr0   = radio_swr_trips();
    stops0 = radio_band_stops();

    radio_set_modem(true);
    if (!radio_get_modem()) {
        result = ZELLO_TX_STOPPED;      /* refused where the radio may not transmit: it said why */
        goto out;
    }
    result = ZELLO_TX_DONE;
    for (int t = 0; t < LEAD_MS / 20 && radio_get_modem(); t++) audio_play(zeros, sizeof(zeros) / sizeof(zeros[0]));

    while (!id_only && atomic_load(&tx_stop) == 0 && radio_get_modem() && !radio_get_ptt()) {
        size_t n = ring_take(in16, TX_FRAME);

        if (n == 0) {
            audio_play(zeros, sizeof(zeros) / sizeof(zeros[0]));    /* a gap in the channel's audio */
            continue;
        }
        for (size_t i = 0; i < n; i++) inf[i] = in16[i];
        int m = olivia_resamp_process(&rs, inf, (int)n, outf);
        for (int i = 0; i < m; i++) out16[i] = (int16_t)LV_CLAMP(-32767.0f, outf[i], 32767.0f);
        int gain = param_i_get(cfg_voice_msg_gain);     /* the voice messages' TX level */
        if (gain) audio_gain_db(out16, (size_t)m, (float)gain, out16);
        audio_play(out16, (size_t)m);
    }

    if (atomic_load(&tx_stop) >= 2) audio_play_flush();       /* the rest of the message not sent */
    if (radio_get_modem() && !radio_get_ptt() &&
        (id_only || (atomic_load(&tx_with_id) && atomic_load(&tx_stop) != 3))) {
        size_t max = (size_t)zello_morse_ms(tx_call, ID_WPM) * AUDIO_PLAY_RATE / 1000 + 16;

        if (tx_call[0] && (id = malloc(max * sizeof(int16_t)))) {
            size_t n    = zello_morse_render(tx_call, ID_WPM, ID_HZ, AUDIO_PLAY_RATE, ID_AMP, id, max);
            int    gain = param_i_get(cfg_voice_msg_gain);     /* at the messages' level */

            if (gain) audio_gain_db(id, n, (float)gain, id);

            audio_play(zeros, sizeof(zeros) / sizeof(zeros[0]));
            for (size_t i = 0; i < n && atomic_load(&tx_stop) != 3 && radio_get_modem() && !radio_get_ptt();) {
                size_t k = n - i > 2048 ? 2048 : n - i;

                audio_play(id + i, k);
                i += k;
            }
        }
    }
    if (atomic_load(&tx_stop) == 3 || radio_get_ptt() || !radio_get_modem()) {
        audio_play_flush();     /* the operator, or the radio ended it: none of it left */
    }
    else audio_play_wait();
    radio_set_modem(false);
    if (radio_swr_trips() != swr0 || radio_band_stops() != stops0) result = ZELLO_TX_STOPPED;

out:
    if (pwr_set) radio_set_pwr(param_f_get(cfg_pwr));
    if (claimed) tx_player_release();
    free(id);
    if (rs_ok) olivia_resamp_free(&rs);
    atomic_store(&tx_result, result);
    atomic_store(&tx_live, false);
    return NULL;
}

static void tx_start(bool id_only) {
    pthread_t t;

    snprintf(tx_call, sizeof(tx_call), "%s", params.callsign.x);
    tx_began_ms = get_time();
    tx_forced   = false;
    atomic_store(&tx_stop, 0);
    atomic_store(&tx_with_id, false);
    atomic_store(&tx_live, true);
    if (pthread_create(&t, NULL, tx_fn, (void *)(intptr_t)id_only) != 0) {
        atomic_store(&tx_result, ZELLO_TX_NOT_KEYED);
        atomic_store(&tx_live, false);
        return;
    }
    pthread_detach(t);
}

/* The link's rules for now, and what they say done (play thread) */
static void link_tick(void) {
    zello_link_cfg_t c;

    link_cfg(&c);
    /* Zello to RF switched off after transmitting: the ID it owes goes now, the last thing the link sends */
    if (!c.to_rf && zlink.tx_since_id) c.id_period_ms = 0;
    if (atomic_exchange(&rearm_rf, false)) zello_link_rearm(&zlink);
    atomic_store(&rx_tap, c.to_zello || (c.to_rf && param_i_get(cfg_squelch) > 0));
    atomic_store(&link_on, (c.to_rf || param_i_get(cfg_zello_to_zello)));
    atomic_store(&link_stopped, zlink.to_rf_stopped);
    if (!c.to_rf && !c.to_zello && zlink.state == ZELLO_LINK_IDLE && !zlink.tx_since_id && !atomic_load(&tx_live)) {
        atomic_store(&link_show, ZELLO_LINK_SHOW_IDLE);
        atomic_exchange(&ptt_pressed, false);
        return;
    }

    bool            keyed = atomic_load(&tx_live);
    zello_link_in_t in    = {
        .now_ms       = get_time(),
        .connected    = atomic_load(&connected),
        .zello_msg    = c.to_rf && (atomic_load(&talker_on) || ring_count() > 0),
        .squelch_open = atomic_load(&sql_open),
        .radio_busy   = radio_busy(),
        .can_key      = can_key(&c),
        .operator_key = atomic_exchange(&ptt_pressed, false) || (keyed && radio_get_ptt()),
        .keyed        = keyed,
        .result       = (zello_tx_result_t)atomic_load(&tx_result),
        .rx_ended     = atomic_exchange(&rx_ended, false),
    };
    zello_link_out_t out = zello_link_step(&zlink, &c, &in);

    /* A transmission still on well past the time limit and the ID: its thread hangs (the audio stream stalled), the
     * radio unkeyed from here. The link keys nothing more while that thread lives */
    if (keyed && !tx_forced && in.now_ms - tx_began_ms > c.limit_ms + TX_HANG_MS && radio_get_modem()) {
        tx_forced = true;
        radio_set_modem(false);
        msg_schedule_text_fmt("Zello link: the transmission hung, unkeyed");
    }

    if (out.key) {
        tx_start(out.id);
    }
    if (out.unkey) {
        atomic_store(&tx_with_id, out.id);
        atomic_store(&tx_stop, !out.now ? 1 : out.id ? 2 : 3);
    }
    if (out.talk_start) {
        atomic_store(&rx_rs_reset, true);
        atomic_store(&link_rx, true);
        send_line("talk start\n");
    }
    if (out.talk_stop) {
        atomic_store(&link_rx, false);
        send_line("talk stop\n");
    }
    if (out.why) {
        msg_schedule_text_fmt("%s", out.why);
    }
    if (zlink.cut || !c.to_rf) {
        ring_clear();       /* a message cut, or not to be sent: its audio goes */
    }
    switch (zlink.state) {
        case ZELLO_LINK_RELAY:    atomic_store(&link_show, ZELLO_LINK_SHOW_TX); break;
        case ZELLO_LINK_ID:       atomic_store(&link_show, ZELLO_LINK_SHOW_ID); break;
        case ZELLO_LINK_ENDING:   atomic_store(&link_show, zlink.id_pending ? ZELLO_LINK_SHOW_ID : ZELLO_LINK_SHOW_TX); break;
        case ZELLO_LINK_TO_ZELLO: atomic_store(&link_show, ZELLO_LINK_SHOW_TO_ZELLO); break;
        default:                  atomic_store(&link_show, ZELLO_LINK_SHOW_IDLE); break;
    }
}

/* ---------- the play thread ---------- */

/* The path for now (play thread) */
static void path_tick(zello_gate_t *g) {
    zello_link_cfg_t c;
    uint64_t         now  = get_time();
    bool             air  = on_air(now);
    bool             talk = atomic_load(&talk_want);

    link_cfg(&c);
    bool play = !relaying(&c) && (atomic_load(&talker_on) || now - heard_ms < PLAY_HOLD_MS);
    /* A message from the Bluetooth mic needs no path of the base's: the radio's own meanwhile */
    zello_path_t to = zello_path(g, atomic_load(&connected), talk && !atomic_load(&talk_bt), play, air, now);

    pthread_mutex_lock(&gate_lock);
    apply_path(to);
    pthread_mutex_unlock(&gate_lock);
    if (talk && air) talk_cut(_("Zello: message ended, the radio transmits"));
}

/* Into the speaker's stream while the path is LISTEN, a piece at a time as there is room; dropped once it is not */
static void play_out(zello_gate_t *g, int16_t *buf, size_t n) {
    int waited = 0;

    while (n && atomic_load(&running)) {
        size_t took = 0;
        bool   listen;

        path_tick(g);
        pthread_mutex_lock(&gate_lock);
        listen = applied == ZELLO_PATH_LISTEN;
        if (listen) took = audio_user_write(AUDIO_USER_ZELLO, buf, n > PIECE ? PIECE : n);
        pthread_mutex_unlock(&gate_lock);
        if (!listen) return;
        if (took) {
            buf += took;
            n -= took;
            waited = 0;
        } else if (waited >= ROOM_WAIT_MS) {
            return;
        } else {
            usleep(2000);
            waited += 2;
        }
    }
}

/* A block of the channel's audio, 16 kHz, to the speaker's 44.1 kHz */
static void play_block(zello_gate_t *g, olivia_resamp_t *rs, const uint8_t *pcm, size_t bytes) {
    float   in[MIC_BLOCK], out[MIC_BLOCK * 3 + 8];
    int16_t s16[MIC_BLOCK * 3 + 8];

    while (bytes >= 2) {
        size_t n = bytes / 2 > MIC_BLOCK ? MIC_BLOCK : bytes / 2;

        for (size_t i = 0; i < n; i++) {
            int16_t v;

            memcpy(&v, pcm + 2 * i, 2);         /* after the record's first byte: not aligned */
            in[i] = v;
        }
        pcm += 2 * n;
        bytes -= 2 * n;

        int m = olivia_resamp_process(rs, in, (int)n, out);

        for (int i = 0; i < m; i++) s16[i] = (int16_t)LV_CLAMP(-32767.0f, out[i], 32767.0f);
        play_out(g, s16, (size_t)m);
    }
}

static void reap(pid_t p) {
    for (int i = 0; i < KILL_STEPS; i++) {
        pid_t r = waitpid(p, NULL, WNOHANG);

        if (r == p || (r < 0 && errno == ECHILD)) return;
        if (i == TERM_STEPS) kill(p, SIGTERM);
        usleep(STEP_US);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

/* The helper's socket: its lines and audio; the base's path; the speaker; the link. Until the helper goes or the
 * session ends */
static void *play_fn(void *arg) {
    pid_t           pid = (pid_t)(intptr_t)arg;
    int             fd;
    uint8_t         rec[REC_MAX];
    zello_gate_t    gate = {0};
    olivia_resamp_t rs;
    bool            rs_ok = olivia_resamp_init(&rs, ZRATE, AUDIO_PLAY_RATE) == 0;
    /* Its own stream: on the headphones while they stand in for the speaker */
    bool            heard = audio_user_open(AUDIO_USER_ZELLO) >= 0;

    pthread_mutex_lock(&fd_lock);
    fd = helper_fd;
    pthread_mutex_unlock(&fd_lock);
    if (!heard) LV_LOG_ERROR("Zello: no stream to play the channel on");

    while (atomic_load(&running) && fd >= 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           r = poll(&p, 1, 40);

        path_tick(&gate);
        link_tick();
        if (r <= 0) continue;
        ssize_t n = recv(fd, rec, sizeof(rec) - 1, 0);
        if (n <= 0) break;                  /* the helper has gone, or the session closed the socket */
        if (rec[0] == 0) {
            zello_link_cfg_t c;

            link_cfg(&c);
            heard_ms = get_time();
            if (relaying(&c)) {
                if (!zlink.cut) ring_put(rec + 1, ((size_t)n - 1) / 2);
            } else if (rs_ok) {
                play_block(&gate, &rs, rec + 1, (size_t)n - 1);
            }
        } else {
            rec[n] = 0;
            on_line((char *)rec);
        }
        if (!atomic_load(&connected) && atomic_load(&talk_want)) talk_cut(_("Zello: message ended, off the channel"));
    }

    /* The link's transmission ends first, at once */
    atomic_store(&tx_stop, 3);
    for (int i = 0; i < 100 && atomic_load(&tx_live); i++) usleep(30000);
    atomic_store(&link_rx, false);
    atomic_store(&connected, false);
    atomic_store(&talker_on, false);
    atomic_store(&sql_open, false);
    atomic_store(&link_show, ZELLO_LINK_SHOW_IDLE);
    ring_clear();
    zello_link_init(&zlink);
    pthread_mutex_lock(&gate_lock);
    apply_path(ZELLO_PATH_OFF);
    pthread_mutex_unlock(&gate_lock);
    audio_user_close(AUDIO_USER_ZELLO);
    if (rs_ok) olivia_resamp_free(&rs);

    /* The socket closed: the helper's stdin ends, it says goodbye to Zello and exits */
    pthread_mutex_lock(&fd_lock);
    if (helper_fd >= 0) close(helper_fd);
    helper_fd = -1;
    pthread_mutex_unlock(&fd_lock);
    reap(pid);
    while (!scheduler_put_noargs(play_ended)) usleep(20000);
    return NULL;
}

/* The helper started on cur_channel, with the play thread. False: it could not be */
static bool launch(void) {
    int                        sv[2];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    sigset_t                   none, dfl;
    pthread_t                  t;
    char                      *argv[] = {X6100_SBIN "/x6100-zello", "run", cur_channel, NULL};

    if (thread_live) {
        restart_pending = true;
        return true;
    }
    if (!cur_channel[0]) return false;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return false;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGINT);
    sigaddset(&dfl, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dfl);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t p;
    int   r = posix_spawn(&p, argv[0], &fa, &attr, argv, environ);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(sv[1]);
    if (r != 0) {
        close(sv[0]);
        return false;
    }

    pthread_mutex_lock(&st_lock);
    st.state     = ZELLO_CONNECTING;
    st.reason[0] = st.talker[0] = st.notice[0] = 0;
    st.users     = -1;
    pthread_mutex_unlock(&st_lock);
    atomic_store(&helper_error, false);
    atomic_store(&btmic_on, false);
    via_sent = -1;
    atomic_store(&connected, false);
    atomic_store(&talk_want, false);
    atomic_store(&talk_pending, 0);
    atomic_store(&rx_ended, false);
    zello_link_init(&zlink);

    pthread_mutex_lock(&fd_lock);
    helper_fd = sv[0];
    pthread_mutex_unlock(&fd_lock);
    atomic_store(&running, true);
    if (pthread_create(&t, NULL, play_fn, (void *)(intptr_t)p) != 0) {
        atomic_store(&running, false);
        pthread_mutex_lock(&fd_lock);
        close(helper_fd);
        helper_fd = -1;
        pthread_mutex_unlock(&fd_lock);
        kill(p, SIGKILL);
        waitpid(p, NULL, 0);
        return false;
    }
    pthread_detach(t);
    thread_live = true;
    if (!timer) timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    return true;
}

/* The play thread to its end: the socket shut, which the helper sees as its stdin's end */
static void stop_run(void) {
    atomic_store(&talk_want, false);
    atomic_store(&running, false);
    pthread_mutex_lock(&fd_lock);
    if (helper_fd >= 0) shutdown(helper_fd, SHUT_RDWR);
    pthread_mutex_unlock(&fd_lock);
}

static void relaunch_cb(lv_timer_t *t) {
    (void)t;
    if (want_on && !thread_live) launch();
}

/* GUI thread, after the play thread */
static void play_ended(void *arg) {
    (void)arg;
    thread_live = false;
    if (restart_pending && want_on) {
        restart_pending = false;
        if (!launch()) want_on = false;
    } else if (want_on && !atomic_load(&helper_error) && relaunches < RELAUNCH_MAX) {
        /* Gone by itself, not given up (it tries the network again itself): started again */
        relaunches++;
        lv_timer_t *t = lv_timer_create(relaunch_cb, RELAUNCH_MS, NULL);
        lv_timer_set_repeat_count(t, 1);
    } else {
        want_on         = false;
        restart_pending = false;
    }
}

/* ---------- the main screen's badge ---------- */

static void badge_show(bool show, const char *text, bool tx) {
    if (!show) {
        if (badge && lv_obj_is_valid(badge)) lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!badge || !lv_obj_is_valid(badge)) {
        badge = lv_label_create(lv_layer_top());     /* as the screen recording's mark */
        lv_obj_set_style_text_font(badge, &sony_22, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(badge, 2, 0);
        lv_obj_set_style_radius(badge, 6, 0);
        lv_obj_set_style_pad_hor(badge, 8, 0);
        lv_obj_set_style_pad_ver(badge, 2, 0);
        lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 8, 176);    /* the spectrum's top left, under the band plan */
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    }
    if (!strcmp(lv_label_get_text(badge), text) && !lv_obj_has_flag(badge, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_set_style_bg_color(badge, tx ? TC(tx_bg) : TC(warn_bg), 0);
    lv_obj_set_style_border_color(badge, tx ? TC(tx) : TC(warn), 0);
    lv_obj_set_style_text_color(badge, tx ? TC(text) : TC(warn), 0);     /* as the CW keyboard's ON AIR */
    lv_label_set_text(badge, text);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN);
}

/* The session's timer (GUI thread): the helper told which mic, the badge */
static void timer_cb(lv_timer_t *t) {
    (void)t;
    if (thread_live && via_sent != param_i_get(cfg_zello_talk_bt)) {
        via_sent = param_i_get(cfg_zello_talk_bt);
        send_line(via_sent ? "talkvia bt\n" : "talkvia mic\n");
    }

    bool on = (want_on || thread_live) && atomic_load(&link_on);

    if (on && !window_open && !dialog_is_run()) {      /* the main screen's: not over a window */
        char              text[96];
        zello_link_show_t s = (zello_link_show_t)atomic_load(&link_show);
        const char       *what = s == ZELLO_LINK_SHOW_TX ? " [TX]" : s == ZELLO_LINK_SHOW_ID ? " [ID]"
                               : s == ZELLO_LINK_SHOW_TO_ZELLO ? " [RX>Z]" : "";

        snprintf(text, sizeof(text), "%s%s%s%s%s", _("ZELLO LINK"),
                 !param_i_get(cfg_zello_to_rf) ? ""
                 : atomic_load(&link_stopped)  ? " Z>RF(off)"
                 : param_i_get(cfg_squelch) == 0 ? " Z>RF(no SQL)"
                                                 : " Z>RF",
                 !param_i_get(cfg_zello_to_zello) ? ""
                 : param_i_get(cfg_squelch) == 0  ? " RF>Z(no SQL)"
                                                  : " RF>Z",
                 atomic_load(&connected) ? "" : " ...", what);
        badge_show(true, text, s == ZELLO_LINK_SHOW_TX || s == ZELLO_LINK_SHOW_ID);
    } else {
        badge_show(false, NULL, false);
    }
    if (!want_on && !thread_live && timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
}

/* ---------- the API ---------- */

void zello_status(zello_status_t *out) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&st_lock);
    out->state   = want_on || thread_live ? st.state : ZELLO_OFF;
    out->users   = st.users;
    out->retry_s = st.retry_s;
    snprintf(out->reason, sizeof(out->reason), "%s", st.reason);
    snprintf(out->talker, sizeof(out->talker), "%s", st.talker);
    if (st.notice[0]) {
        msg_update_text_fmt("%s", st.notice);
        st.notice[0] = 0;
    }
    pthread_mutex_unlock(&st_lock);
    snprintf(out->channel, sizeof(out->channel), "%s", cur_channel);
    out->on            = want_on || thread_live;
    out->talking       = atomic_load(&talk_want);
    out->talk_bt       = atomic_load(&talk_bt);
    out->btmic         = atomic_load(&btmic_on);
    out->link          = out->on && atomic_load(&link_on);
    out->to_rf_stopped = atomic_load(&link_stopped);
    out->link_show     = (zello_link_show_t)atomic_load(&link_show);
}

static void prepare(void) {
    if (prepared) return;
    prepared  = true;
    mic_rs_ok = olivia_resamp_init(&mic_rs, AUDIO_CAPTURE_RATE, ZRATE) == 0;
    rx_rs_ok  = olivia_resamp_init(&rx_rs, AUDIO_CAPTURE_RATE, ZRATE) == 0;
    radio_set_tx_prepare_fn(tx_prepare);
}

bool zello_connect(const char *channel) {
    prepare();
    window_open = true;
    snprintf(cur_channel, sizeof(cur_channel), "%s", channel);
    want_on    = true;
    relaunches = 0;
    if (thread_live) {
        restart_pending = true;
        stop_run();
        return true;
    }
    if (!launch()) {
        want_on = false;
        return false;
    }
    if (param_i_get(cfg_zello_to_rf) || param_i_get(cfg_zello_to_zello)) {
        msg_schedule_text_fmt("Simplex repeater on: %s%s", param_i_get(cfg_zello_to_rf) ? "Zello to RF " : "",
                              param_i_get(cfg_zello_to_zello) ? "RF to Zello" : "");
    }
    return true;
}

void zello_disconnect(void) {
    want_on         = false;
    restart_pending = false;
    stop_run();
}

void zello_window_opened(void) {
    window_open = true;
    prepare();
    pthread_mutex_lock(&st_lock);
    st.notice[0] = 0;       /* one from while the window was closed: stale */
    pthread_mutex_unlock(&st_lock);
}

void zello_window_closed(void) {
    window_open = false;
    zello_talk_stop();
    if (!param_i_get(cfg_zello_to_rf) && !param_i_get(cfg_zello_to_zello)) {
        zello_disconnect();
    } else if (want_on || thread_live) {
        msg_update_text_fmt("Zello link stays on");
    }
}

void zello_talk_start(bool by_ptt) {
    if (atomic_load(&talk_want)) return;
    if (!atomic_load(&connected)) {
        msg_update_text_fmt("Zello: not on a channel yet");
        return;
    }
    if (atomic_load(&tx_live)) {
        atomic_store(&ptt_pressed, true);   /* the operator first: the link's transmission ends */
        msg_update_text_fmt("Zello link unkeyed: press again to talk");
        return;
    }
    if (radio_get_state() != RADIO_RX || radio_get_ptt() || radio_get_modem()) {
        msg_update_text_fmt("Zello: not while the radio transmits");
        return;
    }

    bool bt = param_i_get(cfg_zello_talk_bt);

    if (bt && !atomic_load(&btmic_on)) {
        msg_update_text_fmt("Zello: no Bluetooth mic connected");
        return;
    }
    /* The link's message from the receiver gives way to the user's */
    if (atomic_exchange(&link_rx, false)) {
        atomic_store(&rx_ended, true);
        send_line("talk stop\n");
    }
    talk_by_ptt = by_ptt;
    quiet_told  = false;
    atomic_store(&talk_bt, bt);
    atomic_fetch_add(&talk_pending, 1);
    atomic_store(&talk_want, true);
    send_line(bt ? "talk start bt\n" : "talk start\n");
}

void zello_talk_stop(void) {
    if (!atomic_exchange(&talk_want, false)) return;
    send_line("talk stop\n");
}

bool zello_talk_by_ptt(void) {
    return talk_by_ptt;
}

bool zello_ptt_key(int state, bool app_open) {
    zello_key_t to;

    if (state == KEYPAD_PRESS) {
        if (thread_live) atomic_store(&ptt_pressed, true);  /* the operator: the link's transmission ends */
        to = zello_ptt_press(&ptt, app_open);
        if (to == ZELLO_KEY_ZELLO) zello_talk_start(true);
    } else if (state == KEYPAD_RELEASE || state == KEYPAD_LONG_RELEASE) {
        to = zello_ptt_release(&ptt);
        /* After the app closed its message ended with it: the release has nothing left to do */
        if (to == ZELLO_KEY_ZELLO && app_open && talk_by_ptt) zello_talk_stop();
    } else {
        to = ptt.down ? ptt.owner : ZELLO_KEY_RADIO;    /* the long press in between */
    }
    return to == ZELLO_KEY_ZELLO;
}

void zello_link_rearm_rf(void) {
    atomic_store(&rearm_rf, true);
}

bool zello_mic_quiet(void) {
    if (quiet_told || !atomic_load(&talk_want) || atomic_load(&talk_bt) || !atomic_load(&mic_on)) return false;
    if (get_time() - atomic_load(&mic_since_ms) > MIC_QUIET_MS && atomic_load(&mic_peak) < MIC_QUIET) {
        quiet_told = true;
        return true;
    }
    return false;
}

bool zello_mic(void) {
    return atomic_load(&mic_on);
}

/* Audio thread: the hand mic filtered, at 16 kHz, to the helper; a record that does not fit is dropped, never waited
 * for */
static void put_mic(size_t n, const int16_t *samples) {
    float   in[MIC_BLOCK], out[MIC_BLOCK / 2 + 8];
    int16_t pcm[MIC_BLOCK / 2 + 8];
    int     peak = atomic_load(&mic_peak);

    while (n) {
        size_t k = n > MIC_BLOCK ? MIC_BLOCK : n;

        for (size_t i = 0; i < k; i++) {
            float y = samples[i];

            for (int f = 0; f < 3; f++) y = biquad_run(&mic_filter[f], y);
            in[i] = y;
            if (fabsf(y) > peak) peak = (int)fabsf(y);
        }
        samples += k;
        n -= k;

        int m = olivia_resamp_process(&mic_rs, in, (int)k, out);

        for (int i = 0; i < m; i++) pcm[i] = (int16_t)LV_CLAMP(-32767.0f, out[i], 32767.0f);
        send_audio(pcm, m);
    }
    atomic_store(&mic_peak, peak);
}

void zello_put_mic(size_t nsamples, const int16_t *samples) {
    if (!atomic_load(&mic_on) || !mic_rs_ok) return;
    atomic_fetch_add(&mic_in_put, 1);
    if (atomic_load(&mic_on) && get_time() >= atomic_load(&mic_from_ms)) put_mic(nsamples, samples);
    atomic_fetch_sub(&mic_in_put, 1);
}

/* Audio thread: the receiver's audio. Its level is the squelch (the base mutes it with the squelch closed), read only
 * while it is the receiver's (the path the radio's own, the radio receiving); while the link sends the receiver to
 * the channel, it goes to the helper at 16 kHz */
void zello_put_rx(size_t n, const int16_t *samples) {
    static zello_sql_t sql;
    static double      sum;
    static int         count;
    float              in[MIC_BLOCK], out[MIC_BLOCK / 2 + 8];
    int16_t            pcm[MIC_BLOCK / 2 + 8];

    if (!atomic_load(&rx_tap)) {
        if (sql.open || atomic_load(&sql_open)) {
            memset(&sql, 0, sizeof(sql));
            atomic_store(&sql_open, false);
        }
        return;
    }
    bool valid = atomic_load(&path_now) == ZELLO_PATH_OFF && radio_get_state() == RADIO_RX && !radio_get_modem() &&
                 !radio_get_ptt();

    for (size_t i = 0; i < n; i++) {
        sum += (double)samples[i] * samples[i];
        if (++count == SQL_BLOCK) {
            int rms = (int)sqrt(sum / count);

            if (valid) {
                atomic_store(&sql_open, zello_sql_step(&sql, rms, 20, ZELLO_LINK_HANG_MS));
            } else {
                memset(&sql, 0, sizeof(sql));
                atomic_store(&sql_open, false);
            }
            sum   = 0.0;
            count = 0;
        }
    }

    if (!atomic_load(&link_rx) || !valid || !rx_rs_ok) return;
    if (atomic_exchange(&rx_rs_reset, false)) olivia_resamp_reset(&rx_rs);
    while (n) {
        size_t k = n > MIC_BLOCK ? MIC_BLOCK : n;

        for (size_t i = 0; i < k; i++) in[i] = samples[i];
        samples += k;
        n -= k;

        int m = olivia_resamp_process(&rx_rs, in, (int)k, out);

        for (int i = 0; i < m; i++) pcm[i] = (int16_t)LV_CLAMP(-32767.0f, out[i], 32767.0f);
        send_audio(pcm, m);
    }
}
