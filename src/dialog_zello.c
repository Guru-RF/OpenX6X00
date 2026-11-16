/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Zello: the radio on a channel of Zello, the push-to-talk voice service on the internet. A channel
 *  chosen here starts rootfs/usr/sbin/x6100-zello, which logs on to zello.io and stays on the channel while the app is
 *  open; closing the app ends it, and nothing connects otherwise. What the channel says plays on the speaker at the
 *  radio's volume (on the Bluetooth headphones while they stand in for it). To talk: the radio's PTT key or Talk, and
 *  the voice from the hand mic without its own PTT, the way a voice message is recorded: the base sends the hand mic
 *  to Linux while it receives (AUDIO_PLAY_VOICE_REC). The built-in mic can't: the base keeps it off on receive.
 *
 *  Nothing here keys the radio or changes a TX setting:
 *  - with the app open the PTT key talks on Zello and never reaches the radio's PTT (zello_ptt.h, screen_main.c);
 *  - while the radio transmits, whatever keyed it (the hand mic's PTT, the keyboard's, the browser's, CAT, TCI), and
 *    half a second after, Zello's audio is dropped and the base's path is the radio's own (zello_path); before a key
 *    from the GUI it goes at once, on the keying thread (radio_set_tx_prepare_fn), so none of it goes out with the TX
 *    audio. A message being sent ends.
 *
 *  The account, the developer key and the channels are set on the web page's Zello page (webserver apps.py); the GUI
 *  reads the channel list only. Three threads: the GUI's, the play thread (the helper's socket, the base's path, the
 *  speaker) and the audio thread (the hand mic, dialog_zello_put_mic).
 *
 *  After the CX8ANM edition's Zello (condoruy/OpenX6X00-CX8ANM 2cfc9264, bec4fb36, 1aa72f4a): the helper with its
 *  own audio thread, its quick reconnect and its log, the screen that starts it again by itself, the hand mic in
 *  record mode, the speaker's audio in pieces it has room for.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dialog_zello.h"

#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "events.h"
#include "fonts/font_select.h"
#include "keyboard.h"
#include "msg_box.h"
#include "olivia_resamp.h"
#include "radio.h"
#include "scheduler.h"
#include "styles.h"
#include "theme.h"
#include "util.h"
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

#define CHANNELS_FILE BOARD_DATA "/zello/channels.txt"
#define MAX_CH        20
#define CH_LEN        64
#define ZRATE         16000         /* Hz: the helper's audio, both ways */
#define PIECE         1024          /* samples a write to the speaker (audio_play_room) */
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

extern char **environ;

typedef enum { ST_OFF, ST_CONNECTING, ST_ONLINE, ST_OFFLINE, ST_RETRY, ST_ERROR } conn_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void conn_cb(button_data_t *b);
static void talk_cb(button_data_t *b);
static void reload_cb(button_data_t *b);
static const char *conn_label(void);
static const char *talk_label(void);

static button_data_t btn_conn   = {.type = BTN_TEXT_FN, .label_fn = conn_label, .press = conn_cb};
static button_data_t btn_talk   = {.type = BTN_TEXT_FN, .label_fn = talk_label, .press = talk_cb};
static button_data_t btn_reload = {.type = BTN_TEXT, .label = "Reload\nchannels", .press = reload_cb};

static buttons_page_t page = {{&btn_conn, &btn_talk, &btn_reload}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
};

dialog_t *dialog_zello = &dialog;

/* What the helper said, from the play thread to the screen */
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    conn_t state;
    char   reason[96];
    int    retry_s;
    int    users;           /* -1: not known */
    char   talker[64];
    char   notice[112];     /* said once on the message line */
    bool   talk_on;         /* the helper sends a message now */
} st;

/* GUI thread */
static char        channels[MAX_CH][CH_LEN];
static int         channels_n;
static char        cur_channel[CH_LEN];     /* kept while the GUI runs */
static bool        thread_live;             /* the play thread has not ended yet */
static bool        want_on;                 /* on a channel, by the user's choice */
static bool        restart_pending;         /* another channel chosen: started once the last one has ended */
static int         relaunches;
static bool        talk_by_ptt;
static bool        quiet_told;
static bool        prepare_set;
static zello_ptt_t ptt;
static lv_obj_t   *head_lbl, *state_lbl, *talk_lbl, *table;
static lv_timer_t *status_timer;

/* The helper's socket: its sends from three threads, its end */
static pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER;
static int             helper_fd = -1;

static atomic_bool running;         /* the play thread is to go on */
static atomic_bool connected;       /* the channel online */
static atomic_bool talk_want;       /* a message wanted: Talk, or the PTT key held */
static atomic_int  talk_pending;    /* talk start sent, its talk on or talk refused not seen yet */
static atomic_bool helper_error;    /* it gave up ("error ..."): not started again by itself */

/* The base's audio path and the speaker's stream: the play thread, and tx_prepare on a thread that keys */
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static zello_path_t    applied = ZELLO_PATH_OFF;
static bool            play_bt;     /* the play thread plays on the Bluetooth headphones */
static _Atomic uint64_t keyed_ms;   /* when the GUI last went to key */
static uint64_t         heard_ms;   /* the channel's last audio (play thread) */
static atomic_bool      talker_on;  /* someone talks on the channel */

/* The hand mic while a message is sent: the audio thread */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

static atomic_bool      mic_on;
static atomic_int       mic_in_put;
static _Atomic uint64_t mic_from_ms;
static _Atomic uint64_t mic_since_ms;
static atomic_int       mic_peak;
static olivia_resamp_t  mic_rs;
static bool             mic_rs_ok;
static biquad_t         mic_filter[3];

/* ---------- the channels ---------- */

static void load_channels(void) {
    FILE *f = fopen(CHANNELS_FILE, "r");
    char  line[256];

    channels_n = 0;
    if (!f) return;
    while (channels_n < MAX_CH && fgets(line, sizeof(line), f)) {
        char *s = line;
        size_t n;

        line[strcspn(line, "\r\n")] = 0;
        while (*s == ' ' || *s == '\t') s++;
        n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
        if (n && n < CH_LEN) snprintf(channels[channels_n++], CH_LEN, "%s", s);
    }
    fclose(f);
}

/* ---------- the helper ---------- */

static void send_line(const char *line) {
    pthread_mutex_lock(&fd_lock);
    if (helper_fd >= 0) send(helper_fd, line, strlen(line), MSG_DONTWAIT | MSG_NOSIGNAL);
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
        st.state = ST_CONNECTING;
    } else if (!strcmp(line, "state online")) {
        st.state = ST_ONLINE;
    } else if (!strncmp(line, "state offline ", 14)) {
        st.state = ST_OFFLINE;
        snprintf(st.reason, sizeof(st.reason), "%s", line + 14);
    } else if (!strncmp(line, "state retry ", 12)) {
        char *why = NULL;

        st.state   = ST_RETRY;
        st.retry_s = (int)strtol(line + 12, &why, 10);
        snprintf(st.reason, sizeof(st.reason), "%s", why && *why == ' ' ? why + 1 : "");
    } else if (!strncmp(line, "error ", 6)) {
        st.state = ST_ERROR;
        snprintf(st.reason, sizeof(st.reason), "%s", line + 6);
        atomic_store(&helper_error, true);
    } else if (!strncmp(line, "channel ", 8)) {
        st.users = line[8] == '-' ? -1 : atoi(line + 8);
    } else if (!strncmp(line, "talker", 6)) {
        snprintf(st.talker, sizeof(st.talker), "%s", line[6] == ' ' ? line + 7 : "");
        atomic_store(&talker_on, st.talker[0] != 0);
    } else if (!strcmp(line, "talk on")) {
        st.talk_on = true;
        if (atomic_load(&talk_pending) > 0) atomic_fetch_sub(&talk_pending, 1);
    } else if (!strcmp(line, "talk off")) {
        st.talk_on = false;
        /* The helper ended it (two minutes, the connection lost); an earlier message's end while a new one is asked
         * for leaves that one */
        if (atomic_load(&talk_pending) == 0) atomic_store(&talk_want, false);
    } else if (!strncmp(line, "talk refused ", 13)) {
        st.talk_on = false;
        if (atomic_load(&talk_pending) > 0) atomic_fetch_sub(&talk_pending, 1);
        atomic_store(&talk_want, false);
        snprintf(st.notice, sizeof(st.notice), "%s: %s", _("Zello: not sent"), line + 13);
    }
    atomic_store(&connected, st.state == ST_ONLINE);
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

/* To path, with gate_lock held. Leaving LISTEN drops what waits in the stream */
static void apply_path(zello_path_t to) {
    if (to == applied) return;
    if (applied == ZELLO_PATH_LISTEN) audio_play_flush();
    atomic_store(&mic_on, false);
    switch (to) {
        case ZELLO_PATH_OFF:
            radio_set_play_mode(AUDIO_PLAY_OFF);
            break;
        case ZELLO_PATH_LISTEN:
            radio_set_play_mode(play_bt ? AUDIO_PLAY_OFF : AUDIO_PLAY_ON);
            break;
        case ZELLO_PATH_TALK:
            mic_start();
            radio_set_play_mode(AUDIO_PLAY_VOICE_REC);
            atomic_store(&mic_on, true);
            break;
    }
    applied = to;
}

/* Before the GUI keys, on the thread that keys (radio_set_tx_prepare_fn): Zello's audio out of the codec's stream and
 * the path the radio's own, before the key goes down */
static void tx_prepare(void) {
    pthread_mutex_lock(&gate_lock);
    atomic_store(&keyed_ms, get_time());
    if (applied != ZELLO_PATH_OFF) {
        atomic_store(&mic_on, false);
        if (applied == ZELLO_PATH_LISTEN && !play_bt) audio_play_flush_speaker();
        radio_set_play_mode(AUDIO_PLAY_OFF);
        applied = ZELLO_PATH_OFF;
    }
    pthread_mutex_unlock(&gate_lock);
}

/* A message cut by the radio's own TX or the end of the channel (play thread) */
static void talk_cut(const char *why) {
    if (!atomic_exchange(&talk_want, false)) return;
    send_line("talk stop\n");
    notice(why);
}

/* The path for now (play thread) */
static void path_tick(zello_gate_t *g) {
    uint64_t     now  = get_time();
    bool         air  = on_air(now);
    bool         talk = atomic_load(&talk_want);
    bool         play = atomic_load(&talker_on) || now - heard_ms < PLAY_HOLD_MS;
    zello_path_t to   = zello_path(g, atomic_load(&connected), talk, play, air, now);

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
        if (listen) took = audio_play_room(buf, n > PIECE ? PIECE : n);
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

static void play_ended(void *arg);

/* The helper's socket: its lines and audio; the base's path; the speaker. Until the helper goes or the app closes */
static void *play_fn(void *arg) {
    pid_t           pid = (pid_t)(intptr_t)arg;
    int             fd;
    uint8_t         rec[REC_MAX];
    zello_gate_t    gate = {0};
    olivia_resamp_t rs;
    bool            rs_ok = olivia_resamp_init(&rs, ZRATE, AUDIO_PLAY_RATE) == 0;
    bool            bt    = audio_play_bt_begin();      /* the headphones, while they stand in for the speaker */

    pthread_mutex_lock(&fd_lock);
    fd = helper_fd;
    pthread_mutex_unlock(&fd_lock);
    pthread_mutex_lock(&gate_lock);
    play_bt = bt;
    pthread_mutex_unlock(&gate_lock);

    while (atomic_load(&running) && fd >= 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           r = poll(&p, 1, 40);

        path_tick(&gate);
        if (r <= 0) continue;
        ssize_t n = recv(fd, rec, sizeof(rec) - 1, 0);
        if (n <= 0) break;                  /* the helper has gone, or the app closed the socket */
        if (rec[0] == 0) {
            heard_ms = get_time();
            if (rs_ok) play_block(&gate, &rs, rec + 1, (size_t)n - 1);
        } else {
            rec[n] = 0;
            on_line((char *)rec);
        }
        if (!atomic_load(&connected) && atomic_load(&talk_want)) talk_cut(_("Zello: message ended, off the channel"));
    }

    atomic_store(&connected, false);
    atomic_store(&talker_on, false);
    pthread_mutex_lock(&gate_lock);
    apply_path(ZELLO_PATH_OFF);
    play_bt = false;
    pthread_mutex_unlock(&gate_lock);
    if (bt) audio_play_bt_end();
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

static void show_all(void);

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
    st.state     = ST_CONNECTING;
    st.reason[0] = st.talker[0] = st.notice[0] = 0;
    st.users     = -1;
    st.talk_on   = false;
    pthread_mutex_unlock(&st_lock);
    atomic_store(&helper_error, false);
    atomic_store(&connected, false);
    atomic_store(&talk_want, false);
    atomic_store(&talk_pending, 0);

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
    if (dialog.run && want_on && !thread_live) launch();
    show_all();
}

/* GUI thread, after the play thread */
static void play_ended(void *arg) {
    (void)arg;
    thread_live = false;
    if (restart_pending && dialog.run && want_on) {
        restart_pending = false;
        if (!launch()) want_on = false;
    } else if (dialog.run && want_on && !atomic_load(&helper_error) && relaunches < RELAUNCH_MAX) {
        /* Gone by itself, not given up (it tries the network again itself): started again */
        relaunches++;
        lv_timer_t *t = lv_timer_create(relaunch_cb, RELAUNCH_MS, NULL);
        lv_timer_set_repeat_count(t, 1);
    } else {
        want_on         = false;
        restart_pending = false;
    }
    if (dialog.run) show_all();
}

static void connect_to(const char *channel) {
    snprintf(cur_channel, sizeof(cur_channel), "%s", channel);
    want_on    = true;
    relaunches = 0;
    if (thread_live) {
        restart_pending = true;
        stop_run();
    } else if (!launch()) {
        want_on = false;
        msg_update_text_fmt("Zello: the helper did not start");
    }
    show_all();
}

static void disconnect(void) {
    want_on         = false;
    restart_pending = false;
    stop_run();
    show_all();
}

/* ---------- talking ---------- */

static void talk_start(bool by_ptt) {
    if (atomic_load(&talk_want)) return;
    if (!atomic_load(&connected)) {
        msg_update_text_fmt("Zello: not on a channel yet");
        return;
    }
    if (radio_get_state() != RADIO_RX || radio_get_ptt() || radio_get_modem()) {
        msg_update_text_fmt("Zello: not while the radio transmits");
        return;
    }
    talk_by_ptt = by_ptt;
    quiet_told  = false;
    atomic_fetch_add(&talk_pending, 1);
    atomic_store(&talk_want, true);
    send_line("talk start\n");
    buttons_refresh(&btn_talk);
}

static void talk_stop(void) {
    if (!atomic_exchange(&talk_want, false)) return;
    send_line("talk stop\n");
    buttons_refresh(&btn_talk);
}

bool dialog_zello_ptt_key(int state) {
    zello_key_t to;

    if (state == KEYPAD_PRESS) {
        to = zello_ptt_press(&ptt, dialog.run);
        if (to == ZELLO_KEY_ZELLO) talk_start(true);
    } else if (state == KEYPAD_RELEASE || state == KEYPAD_LONG_RELEASE) {
        to = zello_ptt_release(&ptt);
        /* After the app closed its message ended with it: the release has nothing left to do */
        if (to == ZELLO_KEY_ZELLO && dialog.run && talk_by_ptt) talk_stop();
    } else {
        to = ptt.down ? ptt.owner : ZELLO_KEY_RADIO;    /* the long press in between */
    }
    return to == ZELLO_KEY_ZELLO;
}

bool dialog_zello_mic(void) {
    return atomic_load(&mic_on);
}

/* Audio thread: the hand mic filtered, at 16 kHz, to the helper; a record that does not fit is dropped, never waited
 * for */
static void put_mic(size_t n, const int16_t *samples) {
    float   in[MIC_BLOCK], out[MIC_BLOCK / 2 + 8];
    uint8_t rec[1 + 2 * (MIC_BLOCK / 2 + 8)];
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

        rec[0] = 0;
        for (int i = 0; i < m; i++) {
            int16_t v = (int16_t)LV_CLAMP(-32767.0f, out[i], 32767.0f);

            memcpy(rec + 1 + 2 * i, &v, 2);
        }
        if (m > 0) {
            pthread_mutex_lock(&fd_lock);
            if (helper_fd >= 0) send(helper_fd, rec, 1 + 2 * (size_t)m, MSG_DONTWAIT | MSG_NOSIGNAL);
            pthread_mutex_unlock(&fd_lock);
        }
    }
    atomic_store(&mic_peak, peak);
}

void dialog_zello_put_mic(size_t nsamples, const int16_t *samples) {
    if (!atomic_load(&mic_on) || !mic_rs_ok) return;
    atomic_fetch_add(&mic_in_put, 1);
    if (atomic_load(&mic_on) && get_time() >= atomic_load(&mic_from_ms)) put_mic(nsamples, samples);
    atomic_fetch_sub(&mic_in_put, 1);
}

/* ---------- the screen ---------- */

static const char *conn_label(void) {
    return want_on || thread_live ? "Disconnect" : "Connect";
}

static const char *talk_label(void) {
    return atomic_load(&talk_want) ? "Stop\ntalking" : "Talk";
}

/* The two buttons again when what they say has changed: a message ended by the helper or the radio's TX, the helper
 * gone */
static void refresh_buttons(void) {
    static const char *conn_shown, *talk_shown;

    if (conn_label() != conn_shown) {
        conn_shown = conn_label();
        buttons_refresh(&btn_conn);
    }
    if (talk_label() != talk_shown) {
        talk_shown = talk_label();
        buttons_refresh(&btn_talk);
    }
}

static void show_all(void) {
    if (!table) return;
    lv_table_set_row_cnt(table, channels_n ? channels_n : 1);
    if (!channels_n) lv_table_set_cell_value(table, 0, 0, "No channels: add them on the radio's web page, Zello");
    for (int i = 0; i < channels_n; i++) {
        bool cur = want_on && !strcmp(channels[i], cur_channel);

        lv_table_set_cell_value_fmt(table, i, 0, "%s %s", cur ? ">" : "  ", channels[i]);
    }
    refresh_buttons();
}

static void status_cb(lv_timer_t *t) {
    (void)t;
    conn_t state;
    char   reason[96], talker[64], note[112];
    int    users, retry_s;

    if (!state_lbl) return;
    pthread_mutex_lock(&st_lock);
    state   = st.state;
    users   = st.users;
    retry_s = st.retry_s;
    snprintf(reason, sizeof(reason), "%s", st.reason);
    snprintf(talker, sizeof(talker), "%s", st.talker);
    snprintf(note, sizeof(note), "%s", st.notice);
    st.notice[0] = 0;
    pthread_mutex_unlock(&st_lock);

    if (note[0]) msg_update_text_fmt("%s", note);

    if (state == ST_ERROR) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_err), reason);
    } else if (!want_on && !thread_live) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("Not connected: choose a channel and press the MFK"));
    } else if (state == ST_ONLINE) {
        if (users >= 0)
            lv_label_set_text_fmt(state_lbl, "#%06x %s#  #%06x %d %s#", theme_hex(TC_ok), cur_channel,
                                  theme_hex(TC_text_dim), users, _("online"));
        else
            lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_ok), cur_channel);
    } else if (state == ST_OFFLINE) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s: %s#", theme_hex(TC_warn), cur_channel, reason);
    } else if (state == ST_RETRY) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s (%s %d s)#", theme_hex(TC_warn), reason, _("again in"), retry_s);
    } else {
        lv_label_set_text_fmt(state_lbl, "#%06x %s %s#", theme_hex(TC_text_dim), _("Connecting to"), cur_channel);
    }

    if (atomic_load(&talk_want)) {
        uint64_t since = atomic_load(&mic_since_ms);

        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_tx), _("Talking on Zello: speak into the hand mic"));
        if (!quiet_told && atomic_load(&mic_on) && get_time() - since > MIC_QUIET_MS &&
            atomic_load(&mic_peak) < MIC_QUIET) {
            quiet_told = true;
            msg_update_text_fmt("No audio: talking on Zello uses the hand mic");
        }
    } else if (state == ST_ONLINE && talker[0]) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s# #%06x %s#", theme_hex(TC_text_dim), _("Talking now:"),
                              theme_hex(TC_accent), talker);
    } else if (state == ST_ONLINE) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("PTT or Talk: speak into the hand mic, without its own PTT"));
    } else {
        lv_label_set_text(talk_lbl, "");
    }
    refresh_buttons();
}

static int selected(void) {
    uint16_t row = LV_TABLE_CELL_NONE, col = 0;

    if (!table || !channels_n) return -1;
    lv_table_get_selected_cell(table, &row, &col);
    return row == LV_TABLE_CELL_NONE || row >= channels_n ? -1 : row;
}

static void choose(int i) {
    if (i < 0) return;
    if (want_on && !strcmp(channels[i], cur_channel)) disconnect();
    else connect_to(channels[i]);
}

static void conn_cb(button_data_t *b) {
    (void)b;
    if (want_on || thread_live) {
        disconnect();
        return;
    }
    int i = selected();
    if (i < 0) {
        msg_update_text_fmt("Zello: add channels on the radio's web page first");
        return;
    }
    connect_to(channels[i]);
}

static void talk_cb(button_data_t *b) {
    (void)b;
    if (atomic_load(&talk_want)) talk_stop();
    else talk_start(false);
}

static void reload_cb(button_data_t *b) {
    (void)b;
    load_channels();
    show_all();
    msg_update_text_fmt("Zello channels: %d", channels_n);
}

static void pressed_cb(lv_event_t *e) {
    (void)e;
    choose(selected());
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

static lv_obj_t *text(lv_coord_t y, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(dialog.obj);

    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 770);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_obj_set_pos(l, 14, y);
    lv_label_set_text(l, "");
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    buttons_load_page(&page);

    if (!prepare_set) {
        prepare_set = true;
        mic_rs_ok   = olivia_resamp_init(&mic_rs, AUDIO_CAPTURE_RATE, ZRATE) == 0;
        radio_set_tx_prepare_fn(tx_prepare);
    }

    head_lbl  = text(6, &sony_22);
    state_lbl = text(36, &sony_24);
    talk_lbl  = text(68, &sony_22);
    lv_label_set_text_fmt(head_lbl, "#%06x ZELLO#  #%06x %s#", theme_hex(TC_text_head), theme_hex(TC_text_dim),
                          _("voice channels on the internet, never on the air"));

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_pos(table, 10, 100);
    lv_obj_set_size(table, 780, DIALOG_H - 108);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, 770);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(sel_text), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_color(table, TC(sel_bg), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_group_add_obj(keyboard_group, table);
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);

    load_channels();
    if (!cur_channel[0] && channels_n) snprintf(cur_channel, sizeof(cur_channel), "%s", channels[0]);
    show_all();
    for (int i = 0; i < channels_n; i++) {
        if (!strcmp(channels[i], cur_channel)) {
            for (int k = 0; k < i; k++) lv_group_send_data(keyboard_group, LV_KEY_DOWN);
            break;
        }
    }

    status_timer = lv_timer_create(status_cb, 250, NULL);
    status_cb(NULL);
}

static void destruct_cb(void) {
    want_on         = false;
    restart_pending = false;
    stop_run();
    if (status_timer) lv_timer_del(status_timer);
    status_timer = NULL;
    lv_group_remove_obj(table);
    head_lbl = state_lbl = talk_lbl = table = NULL;
}
