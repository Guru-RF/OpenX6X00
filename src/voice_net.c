/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the internet voice apps' session (voice_net.h): the app's helper on its network, started from the app
 *  (dialog_zello.c, dialog_svx.c) and ended when the app closes, unless its Simplex repeater is on; nothing starts it
 *  at power on. One at a time: another app's session ends first. What the network says plays on the speaker at the
 *  radio's volume (on the Bluetooth headphones while they stand in for it). The user's messages: the hand mic without
 *  its PTT (the base sends it to Linux while it receives, AUDIO_PLAY_VOICE_REC) or a Bluetooth headset's mic (the
 *  helper records it, or the app does and sends it with vnet_send_audio).
 *
 *  What keys the radio here is the Simplex repeater alone (voice_link.h), off by default, under main's TX rules: the
 *  key through tx_player's claim (no other app's slot meanwhile) and radio_set_modem, which refuses where the radio may
 *  not transmit (TX ranges, the amateur bands, TX outside them, the WR12, no data from the base); on top of those only
 *  inside the amateur bands or a transverter's range, whatever TX outside them says (the CB and HF Air Band apps tune
 *  outside); the SWR protection and a band stop end it and stop network to RF; the radio's power setting at most the
 *  apps' cap (tx_player's 7 W) and the radio's top; never while anyone else transmits; the operator's PTT ends it at
 *  once; a time limit and a pause between transmissions; the CW ID. Its RF to the network: what is received inside
 *  the amateur bands or a transverter's range goes to the network; the CB, airband or broadcast frequencies the radio
 *  may listen to only with TX outside ham bands on (MARS, CAP), and then with a word of warning. Everything else is
 *  over the internet only:
 *  - with the app open the PTT key talks on the network and never reaches the radio's PTT (voice_ptt.h,
 *    screen_main.c);
 *  - while the radio transmits, whatever keyed it, and half a second after, the network's audio is dropped and the
 *    base's path is the radio's own (vpath); before a key from the GUI it goes at once, on the keying thread
 *    (radio_set_tx_prepare_fn), so none of it goes out with the TX audio. A message being sent ends.
 *
 *  The speaker: one owner for all its users (audio_owner.h), the app's user id its vnet_app_t's (AUDIO_USER_ZELLO,
 *  AUDIO_USER_SVX). The play thread opens the app's own stream (audio_user_open: on the Bluetooth headphones while
 *  they stand in for the speaker) and closes it at its end; the network's audio goes there only through play_out
 *  (audio_user_write) while the path is LISTEN, and the path is set only by apply_path and tx_prepare: LISTEN holds a
 *  claim on the speaker (a message over an Internet radio station, which goes on after it), TALK the hand mic's Rec,
 *  each given back with what waits in the app's own stream when the path is left (leave_path). One place for both
 *  apps. The link's transmission (tx_fn) writes the codec's TX audio, not the speaker's.
 *
 *  Four threads: the GUI's, the play thread (the helper's socket, the base's path, the speaker, the link's rules), the
 *  link's transmission (one at a time) and the audio thread (the hand mic, the receiver's audio).
 *
 *  After the CX8ANM edition's Zello (condoruy/OpenX6X00-CX8ANM 2cfc9264, bec4fb36, 1aa72f4a): the helper with its
 *  own audio thread, its quick reconnect and its log, the screen that starts it again by itself, the hand mic in
 *  record mode, the speaker's audio in pieces it has room for. Zello's session until SVXConnect came to share it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "voice_net.h"

#include "app_window.h"
#include "audio.h"
#include "cfg/cfg_api.h"
#include "cfg/computed_api.h"
#include "cfg/ham_bands.h"
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
#include "ctcss.h"
#include "util.h"
#include "voice_link.h"
#include "voice_ptt.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
#define PLAY_HOLD_MS  600           /* after the network's last audio: still its path (the gaps between packets) */
#define RING_N        (ZRATE * 12)  /* the network's audio waiting to go on the air: 12 s */
#define TX_FRAME      320           /* 20 ms at ZRATE, the transmission's step */
#define LEAD_MS       150           /* keyed, before the audio: the base's TX settles */
#define ID_WPM        20
#define ID_HZ         800
#define ID_AMP        0.25f
#define SQL_BLOCK     (AUDIO_CAPTURE_RATE / 50)     /* 20 ms of the receiver's audio a squelch reading */
#define TIMER_MS      250
#define TX_HANG_MS    30000         /* a transmission this long past the time limit: hung (an ID takes 2-6 s) */
#define LOG_MAX       (256 * 1024)  /* the helper's log started again from empty above this */

extern char **environ;

/* Said once on the screen, from any thread */
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static char            st_notice[112];

/* GUI thread */
static const vnet_app_t *want;              /* the app whose session is wanted, by the user's choice; NULL none */
static const vnet_app_t *run_app;           /* the app of the helper and the play thread (play thread too) */
static const vnet_app_t *window_app;        /* the app whose window is open */
static bool              thread_live;       /* the play thread has not ended yet */
static bool              restart_pending;   /* another channel or app chosen: started once the last one has ended */
static int               relaunches;
static bool              talk_by_ptt;
static bool              quiet_told;
static bool              prepared;          /* radio_set_tx_prepare_fn, the resamplers: once */
static vptt_t            ptt;
static lv_timer_t       *timer;
static lv_obj_t         *badge;             /* "ZELLO LINK" over the main screen while the link is on */

/* The helper's socket: its sends from three threads, its end */
static pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER;
static int             helper_fd = -1;

static atomic_bool running;         /* the play thread is to go on */
static atomic_bool connected;       /* on the network: the channel online, the reflector logged in */
static atomic_bool talk_want;       /* the user's message wanted: Talk, or the PTT key held */
static atomic_int  talk_pending;    /* the user's talk start sent, its talk on or talk refused not seen yet */
static atomic_bool helper_error;    /* it gave up ("error ..."): not started again by itself */
static atomic_bool btmic_on;        /* a Bluetooth headset's mic is there */
static atomic_bool talk_bt;         /* the user's message is from the Bluetooth mic, not the hand mic */
static atomic_bool talker_on;       /* someone talks on the network */

/* The base's audio path and the speaker's stream: the play thread, and tx_prepare on a thread that keys */
static pthread_mutex_t  gate_lock = PTHREAD_MUTEX_INITIALIZER;
static vpath_t          applied = VPATH_OFF;
static atomic_int       path_now;   /* applied, for the audio thread */
static audio_user_t     play_user = AUDIO_USER_ZELLO;   /* the app's stream and claims (play thread's app) */
static _Atomic uint64_t keyed_ms;   /* when the GUI last went to key */
static uint64_t         heard_ms;   /* the network's last audio (play thread) */

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

/* The Simplex repeater. Its rules on the play thread (link_tick, vlink_step) */
static vlink_t          rules;
static atomic_bool      link_on;        /* a direction on, for the badge and the window */
static atomic_int       link_show;      /* vnet_show_t */
static atomic_bool      link_stopped;   /* network to RF stopped by the radio */
static atomic_bool      off_band;       /* outside the amateur bands: the link keys nothing */
static atomic_bool      rx_off_band;    /* the receiver outside them: nothing of it to the network */
static atomic_bool      rx_outside;     /* ... but TX outside ham bands is on: it goes, with a warning */
static atomic_bool      rearm_rf;       /* switched on again: armed */
static atomic_bool      ptt_pressed;    /* a PTT press since the play thread last looked: the operator */
static atomic_bool      rx_tap;         /* RF to the network on: the audio thread reads the receiver */
static atomic_bool      link_rx;        /* the receiver to the network now */
static atomic_bool      rx_ended;       /* the helper ended or refused that message */
static atomic_bool      sql_open;       /* the squelch as the receiver's audio shows it */
static atomic_int       rx_in_put;
static olivia_resamp_t  rx_rs;
static bool             rx_rs_ok;
static atomic_bool      rx_rs_reset;

/* The network's audio waiting to go on the air (the play thread puts, the transmission takes) */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t         ring[RING_N];
static size_t          ring_r, ring_n;

/* The link's transmission: one thread at a time */
static atomic_bool tx_live;         /* its thread runs */
static atomic_int  tx_stop;         /* 0 go on; 1 what is queued played out, 2 dropped, then the ID if asked for and
                                     * unkey; 3 unkey now, nothing more (the operator) */
static atomic_bool tx_with_id;      /* the ID before unkeying, set before tx_stop */
static atomic_int  tx_result;       /* vlink_tx_result_t, once tx_live has gone false */
static char        tx_call[32];     /* the callsign for its ID, set before the thread starts */
static uint64_t    tx_began_ms;     /* its start (play thread) */
static bool        tx_forced;       /* the play thread unkeyed it: it hung (play thread) */

static void play_ended(void *arg);
static void timer_cb(lv_timer_t *t);

/* ---------- the helper ---------- */

void vnet_send(const char *line) {
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

void vnet_send_audio(const int16_t *pcm, int n) {
    if (!atomic_load(&talk_want) || !atomic_load(&talk_bt)) return;   /* the user's message from it only */
    while (n > 0) {
        int k = n > 1024 ? 1024 : n;

        send_audio(pcm, k);
        pcm += k;
        n -= k;
    }
}

void vnet_notice(const char *text) {
    pthread_mutex_lock(&st_lock);
    snprintf(st_notice, sizeof(st_notice), "%s", text);
    pthread_mutex_unlock(&st_lock);
}

static void notice_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notice_fmt(const char *fmt, ...) {
    char    text[sizeof(st_notice)];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    vnet_notice(text);
}

/* The app of the session, from any thread: NULL once it has ended */
static const vnet_app_t *app_now(void) {
    return __atomic_load_n(&run_app, __ATOMIC_ACQUIRE);
}

void vnet_set_connected(bool on) {
    atomic_store(&connected, on);
}

void vnet_set_talker(bool on) {
    atomic_store(&talker_on, on);
}

void vnet_set_btmic(bool on) {
    atomic_store(&btmic_on, on);
}

void vnet_gave_up(void) {
    atomic_store(&helper_error, true);
}

/* What the helper said of a message: the user's, or the link's from the receiver */
void vnet_talk_event(vnet_talk_t what, const char *why) {
    const vnet_app_t *a       = app_now();
    const char       *name    = a ? a->name : "";
    const char       *net     = a ? a->net : "";
    bool              refused = what == VNET_TALK_REFUSED;

    if (!why) why = "";
    if (what == VNET_TALK_ON) {
        if (atomic_load(&talk_pending) > 0) atomic_fetch_sub(&talk_pending, 1);
        return;
    }
    if (atomic_load(&talk_pending) > 0) {
        /* the answer to the user's talk start, or the end of the message before it */
        if (refused) {
            atomic_fetch_sub(&talk_pending, 1);
            atomic_store(&talk_want, false);
            notice_fmt("%s: %s: %s", name, _("not sent"), why);
        }
    } else if (atomic_load(&link_rx)) {
        atomic_store(&link_rx, false);      /* the link's message to the network: ended by the helper or refused */
        atomic_store(&rx_ended, true);
        if (refused) notice_fmt("RF to %s: %s: %s", net, _("not sent"), why);
    } else {
        atomic_store(&talk_want, false);    /* the user's: the helper ended it (its time out, the line lost) */
        if (refused) notice_fmt("%s: %s: %s", name, _("not sent"), why);
    }
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

/* The path left, with gate_lock held: what waits in the app's own stream dropped (a station's stays), its claim on
 * the speaker or its Rec given back (audio_owner.h: the speaker the receiver's once nobody claims it) */
static void leave_path(void) {
    if (applied == VPATH_LISTEN) {
        audio_user_flush(play_user);
        audio_user_release(play_user);
    } else if (applied == VPATH_TALK) {
        audio_user_rec(play_user, false);
    }
}

/* To path, with gate_lock held. LISTEN claims the speaker (or the headphones standing in for it): a message from the
 * network over an Internet radio station, which goes on after it; TALK records the hand mic */
static void apply_path(vpath_t to) {
    if (to == applied) return;
    leave_path();
    atomic_store(&mic_on, false);
    switch (to) {
        case VPATH_OFF:
            break;
        case VPATH_LISTEN:
            audio_user_claim(play_user);
            break;
        case VPATH_TALK:
            mic_start();
            audio_user_rec(play_user, true);
            atomic_store(&mic_on, true);
            break;
    }
    applied = to;
    atomic_store(&path_now, to);
}

/* Before the GUI keys, on the thread that keys (radio_set_tx_prepare_fn): the network's audio out of the app's stream
 * and its claim or Rec given back, so the path is the radio's own before the key goes down. The link's own key too:
 * its audio comes after */
static void tx_prepare(void) {
    pthread_mutex_lock(&gate_lock);
    atomic_store(&keyed_ms, get_time());
    if (applied != VPATH_OFF) {
        atomic_store(&mic_on, false);
        leave_path();
        applied = VPATH_OFF;
        atomic_store(&path_now, VPATH_OFF);
    }
    pthread_mutex_unlock(&gate_lock);
}

/* The user's message cut by the radio's TX or the end of the connection (play thread) */
static void talk_cut(const vnet_app_t *a, const char *why) {
    if (!atomic_exchange(&talk_want, false)) return;
    a->talk(false, atomic_load(&talk_bt), false);
    notice_fmt("%s: %s", a->name, why);
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

#define P(a, f) param_i_get(*(a)->f)

static void link_cfg(const vnet_app_t *a, vlink_cfg_t *c) {
    c->to_rf        = P(a, to_rf);
    c->to_net       = P(a, to_net) && param_i_get(cfg_squelch) > 0;
    c->id_on        = P(a, id);
    c->id_period_ms = (uint32_t)P(a, id_min) * 60000u;
    c->limit_ms     = (uint32_t)P(a, limit) * 1000u;
    c->pause_ms     = (uint32_t)P(a, pause) * 1000u;
    c->net          = a->net;
    c->tr           = i18n_tr;
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

static bool can_key(const vlink_cfg_t *c) {
    return voice_mode() && (!c->id_on || params.callsign.x[0]) && param_i_get(cfg_squelch) > 0;
}

/* The link keys only inside the amateur bands (ham_bands.h) or a transverter's range, whatever Settings > TX Cal > TX
 * outside ham bands says and whatever the TX ranges allow outside them: the CB and HF Air Band apps tune outside the
 * bands, and the link would relay the internet there unattended. Both the frequency it transmits on and the one the
 * base keys (the dial, in split on the X6200) */
static bool ham_freq(int32_t f) {
    return ham_tx_check(f, cfg_is_tx_freq(f), cfg_transverter_shift_for(f) != 0, false) == HAM_TX_OK;
}

static bool in_ham_band(void) {
    return ham_freq(radio_tx_freq()) && ham_freq(radio_key_freq());
}

/* RF to the network: the frequency the receiver is on (the VFO in use, as radio.c reads it) inside the amateur bands
 * or a transverter's range (vlink_rx_band); outside them TX outside ham bands decides (vlink_in_t.rx_outside_ok) */
static bool rx_in_ham_band(void) {
    bool    a = param_i_get(cfg_band_current_vfo) == RADIO_VFO_A;
    int32_t f = param_i_get(a ? cfg_band_vfoa_freq : cfg_band_vfob_freq);

    return vlink_rx_band(f, cfg_transverter_shift_for(f) != 0);
}

/* Someone else on the air or about to be: the radio's TX that is not ours, the GUI's PTT, a transmission holding the
 * player, the keyer (a beacon), the user talking on the network */
static bool radio_busy(void) {
    bool ours = atomic_load(&tx_live);

    return (!ours && (radio_get_state() != RADIO_RX || radio_get_modem() || tx_player_busy())) || radio_get_ptt() ||
           cw_encoder_state() != CW_ENCODER_IDLE || atomic_load(&talk_want);
}

/* The network's audio is the link's: on the air, or dropped while its message was cut; not on the speaker */
static bool relaying(const vlink_cfg_t *c) {
    return c->to_rf && !rules.to_rf_stopped && can_key(c);
}

/* The link's audio to the transmitter, the CTCSS tone under it (none: as it is) */
static void tx_play(ctcss_t *tone, int16_t *buf, size_t n) {
    ctcss_mix(tone, buf, n);
    audio_play(buf, n);
}

/* 20 ms of silence to the transmitter: the tone alone, when there is one */
static void tx_play_silence(ctcss_t *tone, int16_t *zeros, size_t n) {
    memset(zeros, 0, n * sizeof(zeros[0]));
    tx_play(tone, zeros, n);
}

/* The link's transmission: keyed as a voice message is sent (radio_set_modem, the audio from the GUI), held by
 * tx_player's claim so that no app's slot keys meanwhile; the power at most the apps' cap */
static void *tx_fn(void *arg) {
    bool              id_only = (bool)(intptr_t)arg;
    vlink_tx_result_t result  = VLINK_TX_NOT_KEYED;
    olivia_resamp_t   rs;
    bool              rs_ok   = olivia_resamp_init(&rs, ZRATE, AUDIO_PLAY_RATE) == 0;
    int16_t           in16[TX_FRAME], out16[TX_FRAME * 3 + 8], zeros[AUDIO_PLAY_RATE / 50];
    float             inf[TX_FRAME], outf[TX_FRAME * 3 + 8];
    int16_t          *id = NULL;
    bool              claimed = false, pwr_set = false;
    unsigned          swr0, stops0;
    ctcss_t           tone;

    /* The CTCSS tone (#141), in FM only: under all of the transmission, the lead, the gaps and the ID too, so a
     * receiver's tone squelch stays open to its end */
    bool fm = cparam_i_get(cfg_cur_mode) == radio_mode_nfm;

    ctcss_init(&tone, fm ? ctcss_valid(param_i_get(cfg_link_ctcss)) : CTCSS_OFF, AUDIO_PLAY_RATE);
    if (!rs_ok || !(claimed = tx_player_claim())) goto out;
    if (radio_get_state() != RADIO_RX || radio_get_ptt() || radio_get_modem()) goto out;
    if (!in_ham_band()) goto out;       /* looked at again under the claim: nothing keyed */

    float set = param_f_get(cfg_pwr);
    float w   = LV_MIN(LV_MIN(set, TX_PLAYER_MAX_PWR_W), cfg_pwr_max_get());

    pwr_set = w != set;
    if (pwr_set) radio_set_pwr(w);
    swr0   = radio_swr_trips();
    stops0 = radio_band_stops();

    radio_set_modem(true);
    if (!radio_get_modem()) {
        result = VLINK_TX_STOPPED;      /* refused where the radio may not transmit: it said why */
        goto out;
    }
    result = VLINK_TX_DONE;
    for (int t = 0; t < LEAD_MS / 20 && radio_get_modem(); t++) {
        tx_play_silence(&tone, zeros, sizeof(zeros) / sizeof(zeros[0]));
    }

    while (!id_only && atomic_load(&tx_stop) == 0 && radio_get_modem() && !radio_get_ptt()) {
        size_t n = ring_take(in16, TX_FRAME);

        if (n == 0) {
            tx_play_silence(&tone, zeros, sizeof(zeros) / sizeof(zeros[0]));    /* a gap in the network's audio */
            continue;
        }
        for (size_t i = 0; i < n; i++) inf[i] = in16[i];
        int m = olivia_resamp_process(&rs, inf, (int)n, outf);
        for (int i = 0; i < m; i++) out16[i] = (int16_t)LV_CLAMP(-32767.0f, outf[i], 32767.0f);
        int gain = param_i_get(cfg_voice_msg_gain);     /* the voice messages' TX level */
        if (gain) audio_gain_db(out16, (size_t)m, (float)gain, out16);
        tx_play(&tone, out16, (size_t)m);
    }

    if (atomic_load(&tx_stop) >= 2) audio_play_flush();       /* the rest of the message not sent */
    if (radio_get_modem() && !radio_get_ptt() &&
        (id_only || (atomic_load(&tx_with_id) && atomic_load(&tx_stop) != 3))) {
        size_t max = (size_t)vlink_morse_ms(tx_call, ID_WPM) * AUDIO_PLAY_RATE / 1000 + 16;

        if (tx_call[0] && (id = malloc(max * sizeof(int16_t)))) {
            size_t n    = vlink_morse_render(tx_call, ID_WPM, ID_HZ, AUDIO_PLAY_RATE, ID_AMP, id, max);
            int    gain = param_i_get(cfg_voice_msg_gain);     /* at the messages' level */

            if (gain) audio_gain_db(id, n, (float)gain, id);

            tx_play_silence(&tone, zeros, sizeof(zeros) / sizeof(zeros[0]));
            for (size_t i = 0; i < n && atomic_load(&tx_stop) != 3 && radio_get_modem() && !radio_get_ptt();) {
                size_t k = n - i > 2048 ? 2048 : n - i;

                tx_play(&tone, id + i, k);
                i += k;
            }
        }
    }
    if (atomic_load(&tx_stop) == 3 || radio_get_ptt() || !radio_get_modem()) {
        audio_play_flush();     /* the operator, or the radio ended it: none of it left */
    }
    else audio_play_wait();
    radio_set_modem(false);
    if (radio_swr_trips() != swr0 || radio_band_stops() != stops0) result = VLINK_TX_STOPPED;

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
        atomic_store(&tx_result, VLINK_TX_NOT_KEYED);
        atomic_store(&tx_live, false);
        return;
    }
    pthread_detach(t);
}

/* The link's rules for now, and what they say done (play thread) */
static void link_tick(const vnet_app_t *a) {
    vlink_cfg_t c;

    link_cfg(a, &c);
    /* Network to RF switched off after transmitting: the ID it owes goes now, the last thing the link sends */
    if (!c.to_rf && rules.tx_since_id) c.id_period_ms = 0;
    if (atomic_exchange(&rearm_rf, false)) vlink_rearm(&rules);
    atomic_store(&rx_tap, c.to_net || (c.to_rf && param_i_get(cfg_squelch) > 0));
    atomic_store(&link_on, (c.to_rf || P(a, to_net)));
    atomic_store(&link_stopped, rules.to_rf_stopped);
    if (!c.to_rf && !c.to_net && rules.state == VLINK_IDLE && !rules.tx_since_id && !atomic_load(&tx_live)) {
        atomic_store(&link_show, VNET_SHOW_IDLE);
        atomic_store(&off_band, false);
        atomic_store(&rx_off_band, false);
        atomic_store(&rx_outside, false);
        atomic_exchange(&ptt_pressed, false);
        rules.rx_band_told    = false;  /* not stepped while off: said again when RF to the network is on again */
        rules.rx_outside_told = false;
        return;
    }

    bool       keyed   = atomic_load(&tx_live);
    bool       band    = in_ham_band();
    bool       rx_band = rx_in_ham_band();
    bool       mars    = param_i_get(cfg_tx_outside_bands) != 0;
    vlink_in_t in    = {
        .now_ms        = get_time(),
        .connected     = atomic_load(&connected),
        .net_msg       = c.to_rf && (atomic_load(&talker_on) || ring_count() > 0),
        .squelch_open  = atomic_load(&sql_open),
        .radio_busy    = radio_busy(),
        .can_key       = can_key(&c),
        .ham_band      = band,
        .rx_ham_band   = rx_band,
        .rx_outside_ok = mars,
        .operator_key  = atomic_exchange(&ptt_pressed, false) || (keyed && radio_get_ptt()),
        .keyed         = keyed,
        .result        = (vlink_tx_result_t)atomic_load(&tx_result),
        .rx_ended      = atomic_exchange(&rx_ended, false),
    };
    vlink_out_t out = vlink_step(&rules, &c, &in);

    atomic_store(&off_band, c.to_rf && !band);
    atomic_store(&rx_off_band, c.to_net && !rx_band && !mars);
    atomic_store(&rx_outside, c.to_net && !rx_band && mars);
    /* A transmission still on well past the time limit and the ID: its thread hangs (the audio stream stalled), the
     * radio unkeyed from here. The link keys nothing more while that thread lives */
    if (keyed && !tx_forced && in.now_ms - tx_began_ms > c.limit_ms + TX_HANG_MS && radio_get_modem()) {
        tx_forced = true;
        radio_set_modem(false);
        msg_schedule_text_fmt("%s link: the transmission hung, unkeyed", a->name);
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
        a->talk(true, false, true);
    }
    if (out.talk_stop) {
        atomic_store(&link_rx, false);
        a->talk(false, false, true);
    }
    if (out.why) {
        msg_schedule_text_fmt("%s", out.why);
    }
    if (rules.cut || !c.to_rf) {
        ring_clear();       /* a message cut, or not to be sent: its audio goes */
    }
    switch (rules.state) {
        case VLINK_RELAY:  atomic_store(&link_show, VNET_SHOW_TX); break;
        case VLINK_ID:     atomic_store(&link_show, VNET_SHOW_ID); break;
        case VLINK_ENDING: atomic_store(&link_show, rules.id_pending ? VNET_SHOW_ID : VNET_SHOW_TX); break;
        case VLINK_TO_NET: atomic_store(&link_show, VNET_SHOW_TO_NET); break;
        default:           atomic_store(&link_show, VNET_SHOW_IDLE); break;
    }
}

/* ---------- the play thread ---------- */

/* The path for now (play thread) */
static void path_tick(const vnet_app_t *a, vgate_t *g) {
    vlink_cfg_t c;
    uint64_t    now  = get_time();
    bool        air  = on_air(now);
    bool        talk = atomic_load(&talk_want);

    link_cfg(a, &c);
    bool play = !relaying(&c) && (atomic_load(&talker_on) || now - heard_ms < PLAY_HOLD_MS);
    /* A message from the Bluetooth mic needs no path of the base's: the radio's own meanwhile */
    vpath_t to = vpath(g, atomic_load(&connected), talk && !atomic_load(&talk_bt), play, air, now);

    pthread_mutex_lock(&gate_lock);
    apply_path(to);
    pthread_mutex_unlock(&gate_lock);
    if (talk && air) talk_cut(a, _("message ended, the radio transmits"));
}

/* Into the speaker's stream while the path is LISTEN, a piece at a time as there is room; dropped once it is not */
static void play_out(const vnet_app_t *a, vgate_t *g, int16_t *buf, size_t n) {
    int waited = 0;

    while (n && atomic_load(&running)) {
        size_t took = 0;
        bool   listen;

        path_tick(a, g);
        pthread_mutex_lock(&gate_lock);
        listen = applied == VPATH_LISTEN;
        if (listen) took = audio_user_write(play_user, buf, n > PIECE ? PIECE : n);
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

/* A block of the network's audio, 16 kHz, to the speaker's 44.1 kHz */
static void play_block(const vnet_app_t *a, vgate_t *g, olivia_resamp_t *rs, const uint8_t *pcm, size_t bytes) {
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
        play_out(a, g, s16, (size_t)m);
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
    pid_t             pid = (pid_t)(intptr_t)arg;
    const vnet_app_t *a   = app_now();
    int               fd;
    uint8_t           rec[REC_MAX];
    vgate_t           gate = {0};
    olivia_resamp_t   rs;
    bool              rs_ok = olivia_resamp_init(&rs, ZRATE, AUDIO_PLAY_RATE) == 0;

    /* The app's own stream: on the headphones while they stand in for the speaker. The path is OFF here, so nothing
     * of the last app's is claimed under the id that changes */
    pthread_mutex_lock(&gate_lock);
    play_user = a->user;
    pthread_mutex_unlock(&gate_lock);
    if (audio_user_open(a->user) < 0) LV_LOG_ERROR("%s: no stream to play the network on", a->name);

    pthread_mutex_lock(&fd_lock);
    fd = helper_fd;
    pthread_mutex_unlock(&fd_lock);

    while (atomic_load(&running) && fd >= 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           r = poll(&p, 1, 40);

        path_tick(a, &gate);
        link_tick(a);
        if (r <= 0) continue;
        ssize_t n = recv(fd, rec, sizeof(rec) - 1, 0);
        if (n <= 0) break;                  /* the helper has gone, or the session closed the socket */
        if (rec[0] == 0) {
            vlink_cfg_t c;

            link_cfg(a, &c);
            heard_ms = get_time();
            if (relaying(&c)) {
                if (!rules.cut) ring_put(rec + 1, ((size_t)n - 1) / 2);
            } else if (rs_ok) {
                play_block(a, &gate, &rs, rec + 1, (size_t)n - 1);
            }
        } else {
            char *save = NULL;

            rec[n] = 0;
            for (char *l = strtok_r((char *)rec, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) a->line(l);
        }
        if (!atomic_load(&connected) && atomic_load(&talk_want)) talk_cut(a, _("message ended, disconnected"));
    }

    /* The link's transmission ends first, at once */
    atomic_store(&tx_stop, 3);
    for (int i = 0; i < 100 && atomic_load(&tx_live); i++) usleep(30000);
    atomic_store(&link_rx, false);
    atomic_store(&connected, false);
    atomic_store(&talker_on, false);
    atomic_store(&sql_open, false);
    atomic_store(&link_show, VNET_SHOW_IDLE);
    atomic_store(&off_band, false);
    atomic_store(&rx_off_band, false);
    atomic_store(&rx_outside, false);
    ring_clear();
    vlink_init(&rules);
    pthread_mutex_lock(&gate_lock);
    apply_path(VPATH_OFF);
    pthread_mutex_unlock(&gate_lock);
    audio_user_close(a->user);
    if (rs_ok) olivia_resamp_free(&rs);

    /* The socket closed: the helper's stdin ends, it says goodbye to its network and exits */
    pthread_mutex_lock(&fd_lock);
    if (helper_fd >= 0) close(helper_fd);
    helper_fd = -1;
    pthread_mutex_unlock(&fd_lock);
    reap(pid);
    while (!scheduler_put_noargs(play_ended)) usleep(20000);
    return NULL;
}

/* The helper's stderr: its log, started again from empty once it is over LOG_MAX (its folder made if need be); else,
 * or when it cannot be opened, nowhere. The descriptor to close after the start, or -1 */
static int open_log(posix_spawn_file_actions_t *fa, const char *path) {
    struct stat sb;
    char        dir[256];
    char       *slash;
    int         flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, fd = -1;

    if (path) {
        snprintf(dir, sizeof(dir), "%s", path);
        if ((slash = strrchr(dir, '/')) && slash != dir) {
            *slash = 0;
            mkdir(dir, 0755);
        }
        if (stat(path, &sb) == 0 && sb.st_size > LOG_MAX) flags |= O_TRUNC;
        fd = open(path, flags, 0644);
    }
    if (fd >= 0) posix_spawn_file_actions_adddup2(fa, fd, 2);
    else posix_spawn_file_actions_addopen(fa, 2, "/dev/null", O_WRONLY, 0);
    return fd;
}

/* The wanted app's helper started, with the play thread. False: it could not be */
static bool launch(void) {
    int                        sv[2];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    sigset_t                   none, dfl;
    pthread_t                  t;
    const vnet_app_t          *a = want;
    char *const               *argv;

    if (thread_live) {
        restart_pending = true;
        return true;
    }
    if (!a || !(argv = a->argv())) return false;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return false;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
    int log_fd = open_log(&fa, a->log);
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
    if (log_fd >= 0) close(log_fd);
    if (r != 0) {
        close(sv[0]);
        return false;
    }

    pthread_mutex_lock(&st_lock);
    st_notice[0] = 0;
    pthread_mutex_unlock(&st_lock);
    if (a->reset) a->reset();
    atomic_store(&helper_error, false);
    atomic_store(&btmic_on, false);
    atomic_store(&connected, false);
    atomic_store(&talker_on, false);
    atomic_store(&talk_want, false);
    atomic_store(&talk_pending, 0);
    atomic_store(&rx_ended, false);
    vlink_init(&rules);
    __atomic_store_n(&run_app, a, __ATOMIC_RELEASE);

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
        __atomic_store_n(&run_app, NULL, __ATOMIC_RELEASE);
        return false;
    }
    pthread_detach(t);
    thread_live = true;
    if (!timer) timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    return true;
}

/* The play thread to its end: the socket shut, which the helper sees as its stdin's end */
static void stop_run(void) {
    const vnet_app_t *a = app_now();

    if (a && atomic_exchange(&talk_want, false)) a->talk(false, atomic_load(&talk_bt), false);
    atomic_store(&running, false);
    pthread_mutex_lock(&fd_lock);
    if (helper_fd >= 0) shutdown(helper_fd, SHUT_RDWR);
    pthread_mutex_unlock(&fd_lock);
}

static void relaunch_cb(lv_timer_t *t) {
    (void)t;
    if (want && !thread_live) launch();
}

/* GUI thread, after the play thread */
static void play_ended(void *arg) {
    (void)arg;
    thread_live = false;
    __atomic_store_n(&run_app, NULL, __ATOMIC_RELEASE);
    if (restart_pending && want) {
        restart_pending = false;
        if (!launch()) want = NULL;
    } else if (want && !atomic_load(&helper_error) && relaunches < RELAUNCH_MAX) {
        /* Gone by itself, not given up (it tries the network again itself): started again */
        relaunches++;
        lv_timer_t *t = lv_timer_create(relaunch_cb, RELAUNCH_MS, NULL);
        lv_timer_set_repeat_count(t, 1);
    } else {
        want            = NULL;
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

/* The session's timer (GUI thread): the app's own, the badge */
static void timer_cb(lv_timer_t *t) {
    (void)t;
    const vnet_app_t *a = want ? want : app_now();

    if (thread_live && a && a->tick) a->tick();

    bool on = a && (want || thread_live) && atomic_load(&link_on);

    if (on && !window_app && !dialog_is_run()) {       /* the main screen's: not over a window */
        char        text[112], rf[48] = "", rx[48] = "";
        vnet_show_t s    = (vnet_show_t)atomic_load(&link_show);
        const char *what = s == VNET_SHOW_TX ? " [TX]" : s == VNET_SHOW_ID ? " [ID]" : "";

        if (P(a, to_rf)) {
            snprintf(rf, sizeof(rf), " %s>RF%s", a->tag,
                     atomic_load(&link_stopped)     ? _("(off)")
                     : param_i_get(cfg_squelch) == 0 ? _("(no SQL)")
                     : atomic_load(&off_band)        ? _("(no ham band)")
                                                     : "");
        }
        if (P(a, to_net)) {
            snprintf(rx, sizeof(rx), " RF>%s%s", a->tag,
                     param_i_get(cfg_squelch) == 0  ? _("(no SQL)")
                     : atomic_load(&rx_off_band)    ? _("(no ham band)")
                     : atomic_load(&rx_outside)     ? _("(outside bands)")
                                                    : "");
        }
        if (s == VNET_SHOW_TO_NET) {
            snprintf(text, sizeof(text), "%s%s%s%s [RX>%s]", _(a->badge), rf, rx,
                     atomic_load(&connected) ? "" : " ...", a->tag);
        } else {
            snprintf(text, sizeof(text), "%s%s%s%s%s", _(a->badge), rf, rx, atomic_load(&connected) ? "" : " ...",
                     what);
        }
        badge_show(true, text, s == VNET_SHOW_TX || s == VNET_SHOW_ID);
    } else {
        badge_show(false, NULL, false);
    }
    if (!want && !thread_live && timer) {
        lv_timer_del(timer);
        timer = NULL;
        badge_show(false, NULL, false);
    }
}

/* ---------- the API ---------- */

const vnet_app_t *vnet_current(void) {
    return want ? want : thread_live ? app_now() : NULL;
}

/* This app's session on: wanted, or its helper still running */
static bool is_on(const vnet_app_t *a) {
    return want == a || (thread_live && app_now() == a && !want);
}

void vnet_status(const vnet_app_t *a, vnet_status_t *out) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&st_lock);
    if (st_notice[0] && (window_app == a || app_now() == a)) {
        msg_update_text_fmt("%s", st_notice);
        st_notice[0] = 0;
    }
    pthread_mutex_unlock(&st_lock);
    out->on = is_on(a);
    if (!out->on) return;
    out->connected     = app_now() == a && atomic_load(&connected);
    out->talking       = atomic_load(&talk_want);
    out->talk_bt       = atomic_load(&talk_bt);
    out->btmic         = atomic_load(&btmic_on);
    out->link          = atomic_load(&link_on);
    out->to_rf_stopped = atomic_load(&link_stopped);
    out->off_band      = atomic_load(&off_band);
    out->rx_off_band   = atomic_load(&rx_off_band);
    out->rx_outside    = atomic_load(&rx_outside);
    out->show          = (vnet_show_t)atomic_load(&link_show);
}

static void prepare(void) {
    if (prepared) return;
    prepared  = true;
    mic_rs_ok = olivia_resamp_init(&mic_rs, AUDIO_CAPTURE_RATE, ZRATE) == 0;
    rx_rs_ok  = olivia_resamp_init(&rx_rs, AUDIO_CAPTURE_RATE, ZRATE) == 0;
    radio_set_tx_prepare_fn(tx_prepare);
}

const char *vnet_link_taken(const vnet_app_t *a, const vnet_app_t *other) {
    (void)a;
    return other && (P(other, to_rf) || P(other, to_net)) ? other->name : NULL;
}

bool vnet_connect(const vnet_app_t *a) {
    const vnet_app_t *was = vnet_current();

    prepare();
    window_app = a;
    if (was && was != a) {
        msg_schedule_text_fmt("%s %s, %s %s", was->name, _("off"), a->name, _("on"));   /* one session at a time */
    }
    want       = a;
    relaunches = 0;
    if (thread_live) {
        restart_pending = true;
        stop_run();
        return true;
    }
    if (!launch()) {
        want = NULL;
        return false;
    }
    if (P(a, to_rf) || P(a, to_net)) {
        msg_schedule_text_fmt("%s: %s%s%s%s", _("Simplex repeater on"), P(a, to_rf) ? a->net : "",
                              P(a, to_rf) ? _(" to RF ") : "", P(a, to_net) ? _("RF to ") : "", P(a, to_net) ? a->net : "");
    }
    return true;
}

void vnet_disconnect(const vnet_app_t *a) {
    if (vnet_current() != a) return;
    want            = NULL;
    restart_pending = false;
    stop_run();
}

void vnet_window_opened(const vnet_app_t *a) {
    window_app = a;
    prepare();
    pthread_mutex_lock(&st_lock);
    st_notice[0] = 0;       /* one from while the window was closed: stale */
    pthread_mutex_unlock(&st_lock);
}

void vnet_window_closed(const vnet_app_t *a) {
    if (window_app == a) window_app = NULL;
    if (vnet_current() != a) return;
    vnet_talk_stop(a);
    if (!P(a, to_rf) && !P(a, to_net)) {
        vnet_disconnect(a);
    } else if (want || thread_live) {
        msg_update_text_fmt("%s %s", a->name, _("link stays on"));
    }
}

void vnet_talk_start(const vnet_app_t *a, bool by_ptt) {
    if (atomic_load(&talk_want) || vnet_current() != a) return;
    if (app_now() != a || !atomic_load(&connected)) {
        msg_update_text_fmt("%s: %s", a->name, _(a->not_yet));
        return;
    }
    if (atomic_load(&tx_live)) {
        atomic_store(&ptt_pressed, true);   /* the operator first: the link's transmission ends */
        msg_update_text_fmt("%s %s", a->name, _("link unkeyed: press again to talk"));
        return;
    }
    if (radio_get_state() != RADIO_RX || radio_get_ptt() || radio_get_modem()) {
        msg_update_text_fmt("%s: %s", a->name, _("not while the radio transmits"));
        return;
    }

    bool bt = P(a, talk_bt);

    if (bt && !atomic_load(&btmic_on)) {
        msg_update_text_fmt("%s: %s", a->name, _("no Bluetooth mic connected"));
        return;
    }
    /* The link's message from the receiver gives way to the user's */
    if (atomic_exchange(&link_rx, false)) {
        atomic_store(&rx_ended, true);
        a->talk(false, false, true);
    }
    talk_by_ptt = by_ptt;
    quiet_told  = false;
    atomic_store(&talk_bt, bt);
    atomic_fetch_add(&talk_pending, 1);
    atomic_store(&talk_want, true);
    a->talk(true, bt, false);
}

void vnet_talk_stop(const vnet_app_t *a) {
    if (app_now() != a || !atomic_exchange(&talk_want, false)) return;
    a->talk(false, atomic_load(&talk_bt), false);
}

bool vnet_talk_by_ptt(void) {
    return talk_by_ptt;
}

bool vnet_ptt_key(int state) {
    const vnet_app_t *a = window_app;
    vptt_key_t        to;

    if (state == KEYPAD_PRESS) {
        if (thread_live) atomic_store(&ptt_pressed, true);  /* the operator: the link's transmission ends */
        to = vptt_press(&ptt, a != NULL);
        if (to == VPTT_APP) vnet_talk_start(a, true);
    } else if (state == KEYPAD_RELEASE || state == KEYPAD_LONG_RELEASE) {
        to = vptt_release(&ptt);
        /* After the app closed its message ended with it: the release has nothing left to do */
        if (to == VPTT_APP && a && talk_by_ptt) vnet_talk_stop(a);
    } else {
        to = ptt.down ? ptt.owner : VPTT_RADIO;     /* the long press in between */
    }
    return to == VPTT_APP;
}

void vnet_link_rearm_rf(const vnet_app_t *a) {
    if (vnet_current() == a) atomic_store(&rearm_rf, true);
}

bool vnet_mic_quiet(const vnet_app_t *a) {
    if (app_now() != a) return false;
    if (quiet_told || !atomic_load(&talk_want) || atomic_load(&talk_bt) || !atomic_load(&mic_on)) return false;
    if (get_time() - atomic_load(&mic_since_ms) > MIC_QUIET_MS && atomic_load(&mic_peak) < MIC_QUIET) {
        quiet_told = true;
        return true;
    }
    return false;
}

bool vnet_mic(void) {
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

void vnet_put_mic(size_t nsamples, const int16_t *samples) {
    if (!atomic_load(&mic_on) || !mic_rs_ok) return;
    atomic_fetch_add(&mic_in_put, 1);
    if (atomic_load(&mic_on) && get_time() >= atomic_load(&mic_from_ms)) put_mic(nsamples, samples);
    atomic_fetch_sub(&mic_in_put, 1);
}

/* Audio thread: the receiver's audio. Its level is the squelch (the base mutes it with the squelch closed), read only
 * while it is the receiver's (the path the radio's own, the radio receiving); while the link sends the receiver to
 * the network, it goes to the helper at 16 kHz */
void vnet_put_rx(size_t n, const int16_t *samples) {
    static vlink_sql_t sql;
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
    bool valid = atomic_load(&path_now) == VPATH_OFF && radio_get_state() == RADIO_RX && !radio_get_modem() &&
                 !radio_get_ptt();

    for (size_t i = 0; i < n; i++) {
        sum += (double)samples[i] * samples[i];
        if (++count == SQL_BLOCK) {
            int rms = (int)sqrt(sum / count);

            if (valid) {
                atomic_store(&sql_open, vlink_sql_step(&sql, rms, 20, VLINK_HANG_MS));
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
