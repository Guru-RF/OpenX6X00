/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - TCI server (tci_server.h)
 *
 *  One thread owns the sockets: it accepts the clients, reads their frames, parses the
 *  commands and keeps the state lines flowing. The radio is never touched from it: a
 *  command becomes a cmd_t handed to the LVGL thread through the scheduler, and the thread
 *  reads the radio's state (the settings' getters are safe from any thread) every 20 ms and
 *  tells the clients what changed, so a change made on the radio itself reaches them too.
 *
 *  Audio: a capture thread reads the receiver from PulseAudio at 24 kHz, mono, and hands it to
 *  every client that asked (audio_start) in the format that client asked for: TCI's defaults,
 *  48 kHz float32 stereo in frames of 2048, until it asks for another (tci_audio.c has the
 *  conversions). A client keyed with "trx:0,true,tci" sends TX_AUDIO frames; they go to the
 *  codec as the voice keyer's audio does, with the modem bit keying the base. The server paces
 *  it with a TX_CHRONO frame every 21.3 ms (512 samples at 24 kHz), and unkeys after 500 ms
 *  without audio or 3 minutes on the air.
 *
 *  A TCI client has no pairing: /mnt/tci.conf says what it may do (trust=operate, the default,
 *  as before: it tunes, changes the mode and keys; trust=view: it only hears and follows, its
 *  commands are answered with the state as it is). An operating client takes control of the
 *  radio (control_owner.h) as the knob always did, unless someone is on the air; a browser
 *  that held it sees "TCI client active" and takes it back on purpose (R3).
 *
 *  Over Tailscale Funnel (#58) only the browser Panel's own sound and microphone: a connection
 *  tailscaled marks as one from the internet gets no greeting until its first text, "auth TOKEN",
 *  is the token of a browser paired at home (remote_sessions.h), within 5 s; then only the audio
 *  commands, and keying with its audio for a session that may transmit (funnel_cmd_ok). One at
 *  a time, closed when its session is revoked; a wrong token counts towards Funnel's pause.
 *  TCI programs cannot pair and do not come over Funnel.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* accept4, pipe2 */

#include "tci_server.h"
#include "tci_audio.h"
#include "tci_ws.h"
#include "control_owner.h"
#include "remote_sessions.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "screen_main.h"
#include "smeter.h"
#include "tx_meters.h"

#include "lvgl/lvgl.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS   4
#define IN_SIZE       32768         /* a client's frame: commands, or TX audio (WSJT-X's: 64 + 2048 * 8 bytes) */
#define OUT_SIZE      262144        /* queued output; RX audio takes up to audio_budget() of it */
#define URG_SIZE      2048          /* queued TX_CHRONO frames: 0.6 s of them, longer than TX_SILENCE_MS */
#define NOTSENT_LOWAT 16384         /* what the kernel holds unsent for a client: the rest waits in out */
#define TEXT_MAX      2048          /* a text message, fragments joined */
#define IDLE_MS       60000         /* a client silent this long is gone (the knob pings every 2 s) */
#define PING_MS       20000         /* a client silent this long is pinged: a browser page (the remote's audio) in a
                                       background tab has its timers throttled, the browser answers a PING itself */
#define STATE_MS      20            /* the radio's state read this often */
#define SMETER_MS     100
#define TXSENS_MS     200
#define CONF_MS       2000
#define AUDIO_RATE    24000         /* the capture's, and the RX audio's for a client that asks for it */
#define AUDIO_FRAMES  512           /* a capture read, and a TX_CHRONO's worth at 24 kHz: 21.3 ms */
#define ACC_MAX       (2048 + 2 * AUDIO_FRAMES)   /* a client's RX audio short of a frame, then a read more */
#define FUNNEL_AUTH_MS  5000        /* over Funnel: the token within this, or the connection goes */
#define FUNNEL_CHECK_MS 2000        /* over Funnel: its session looked for this often (revoked: it goes) */
#define TX_SILENCE_MS 500           /* keyed with TCI audio: unkeyed without frames this long */
#define TX_LIMIT_MS   (180 * 1000)
#define DRIVE_SPAN    (BOARD_PWR_MAX - 0.1f)   /* drive 0..100 %: 0.1 W to the top of PWR (10 W, the X6200 8 W) */

/* The TCI audio frame header, 64 bytes, little endian. length counts the samples of all the
 * channels (TCI's "real samples"), so a frame holds length / channels samples of each */
enum { AT_IQ = 0, AT_RX = 1, AT_TX = 2, AT_CHRONO = 3 };

typedef struct {
    uint32_t receiver, sample_rate, format, codec, crc, length, type, channels;
    uint32_t reserved[8];
} audio_hdr_t;

typedef struct {
    int             fd;
    bool            ws;             /* handshake done: frames are read */
    bool            up;             /* and it is sent to: at once, or over Funnel once its token is good */
    bool            funnel;         /* from the internet through Tailscale Funnel: the Panel's sound and mic (#58) */
    uint64_t        open_ms;        /* when the handshake was done */
    uint64_t        check_ms;       /* over Funnel: when its session was last found still there */
    char            token[33];      /* over Funnel: the paired browser's token it gave */
    int             role;           /* and that session's role (0 view, 1 operate, 2 admin) */
    bool            tx_ok;          /* and whether it may transmit */
    char            ip[48];         /* over Funnel the browser's address, from tailscaled's X-Forwarded-For */
    uint8_t         in[IN_SIZE];
    size_t          in_len;
    /* Output, under lock (the capture thread writes here too). out is a ring: out_len bytes from
     * out_head, whole frames; urg holds the TX_CHRONO frames, which go out before out's next
     * frame (client_flush) */
    uint8_t         out[OUT_SIZE];
    size_t          out_head, out_len;
    size_t          out_cut;        /* what is left to send of the frame at out_head (0: it starts there) */
    uint8_t         urg[URG_SIZE];
    size_t          urg_len;
    pthread_mutex_t lock;
    char            text[TEXT_MAX];
    size_t          text_len;
    bool            rx_sensors, tx_sensors;
    bool            audio_on;
    /* The audio format the client set (see audio_fmt): written by the socket thread under the
     * lock, read by the capture thread under it */
    uint32_t        fmt_rate, fmt_type;
    uint32_t        fmt_ch, fmt_samples;    /* 0: not set */
    bool            fmt_rate_asked;         /* it sent audio_samplerate (a rate the server has) */
    uint32_t        fmt_gen;                /* moves with every change: the capture thread starts the frame over */
    /* The capture thread's alone: the client's RX audio at its rate, short of a whole frame */
    float           acc[ACC_MAX];
    size_t          acc_n;
    uint32_t        acc_gen;
    uint64_t        last_ms;
    uint64_t        ping_ms;        /* the last PING we sent */
} client_t;

/* What the clients are told; read from the radio every STATE_MS */
typedef struct {
    int32_t freq, mode, flo, fhi, agc, rit;
    int     drive;
    bool    tx;
} rstate_t;

/* A command for the LVGL thread */
typedef enum { C_FREQ, C_MODE, C_FILTER, C_AGC, C_RIT, C_DRIVE, C_KEY, C_UNKEY } cmd_id_t;

typedef struct {
    cmd_id_t id;
    int32_t  a, b;      /* C_KEY: a is the start's number (key_gen); C_UNKEY of the PTT: b is */
    bool     tci;
} cmd_t;

static client_t        clients[MAX_CLIENTS];
static atomic_bool     enabled = true;    /* on until /mnt/tci.conf says enabled=0 */
static atomic_int      port = TCI_DEFAULT_PORT;
static atomic_bool     trust_operate = true;   /* trust=operate (default) or view */
static atomic_int      nclients;
static int             wake_fd[2] = {-1, -1};
static atomic_bool     cap_wanted;      /* a client wants RX audio: set by the socket thread alone (cap_update) */
/* The drive a client set, with the power the setting took from it: reported as long as the
 * setting still holds that power. A change from the front panel or another client then shows
 * as the conversion of the actual power again (audit A17). One word: written on the GUI
 * thread, read on the socket thread */
static atomic_uint_fast64_t drive_set;   /* drive + 1 in the high half (0: none), the power's float bits low */

static void drive_set_store(int drive, float pwr) {
    uint32_t bits;

    memcpy(&bits, &pwr, sizeof(bits));
    atomic_store(&drive_set, ((uint64_t)(drive + 1) << 32) | bits);
}

/* The drive to report for the actual power */
static int drive_of(float pwr) {
    uint64_t v = atomic_load(&drive_set);
    uint32_t bits;

    memcpy(&bits, &pwr, sizeof(bits));
    if ((v >> 32) && (uint32_t)v == bits)
        return (int)(v >> 32) - 1;
    return (int)lroundf((pwr - 0.1f) * 100.0f / DRIVE_SPAN);
}
/* A start is its client's from the moment its key is queued, and a second start is refused until
 * the first is over: a second one taking the owner while the first key was on its way to the LVGL
 * thread left the radio keyed for the first client, which could no longer unkey it (audit C01).
 * The LVGL thread says what became of a start by its number, so a refusal ends that start alone */
static uint32_t        key_gen;          /* the last start queued (socket thread) */
static atomic_uint     key_keyed;        /* the last start the LVGL thread keyed */
static atomic_uint     key_refused;      /* the last start the LVGL thread refused */
static atomic_uint     key_cancelled;    /* the last start ended before its key ran: that key keys nothing */
static int             ptt_owner = -1;   /* the client the radio's own PTT is keyed for (or about to be), -1 none */
static bool            ptt_on_air;       /* its key took: the radio was seen on the air since */
static uint64_t        ptt_keyed_ms;     /* when the socket thread saw its key run, 0 not yet */
static atomic_uint     ptt_unkey_due;    /* the start whose PTT unkey is queued, 0 none: resent until it ran */
static uint64_t        ptt_unkey_ms;
static uint64_t        ptt_ms;

/* The TX audio session: one client at a time */
static struct {
    atomic_bool keyed;      /* the base keyed by us through the modem bit */
    int         owner;      /* client index */
    uint64_t    start_ms, last_audio_ms;
    uint64_t    chrono_t0;  /* us, when TX_CHRONO 0 went out (0: not yet) */
    uint64_t    chrono_n;   /* TX_CHRONO frames since */
    uint32_t    chrono_gen; /* the client's format they were asked in */
    uint32_t    rate;       /* of the client's last TX frame */
    double      pos;
    int16_t     prev;
} txs;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ---- The audio format of a client --------------------------------------------------- */

typedef struct {
    uint32_t rate, type, ch;
    uint32_t samples;       /* of each channel in an RX frame */
} afmt_t;

/* What the client gets: what it set, TCI's defaults for the rest (48 kHz, float32, 2 channels,
 * 2048 samples a frame at 48 kHz, 1024 at 24 kHz), which WSJT-X and JTDX take without asking.
 * One exception keeps the clients written for this server before working: a client that asks
 * for 24 kHz and says nothing of the channels gets one channel and 512 samples a frame, as the
 * server always served it. That is the VFO-Knob (audio_samplerate:24000, int16, nothing more;
 * it plays every sample of a frame as one channel). Socket thread, or under the lock */
static afmt_t audio_fmt(const client_t *c) {
    bool   legacy = c->fmt_rate_asked && c->fmt_rate == AUDIO_RATE && !c->fmt_ch;
    afmt_t f      = {.rate = c->fmt_rate, .type = c->fmt_type};

    f.ch      = c->fmt_ch ? c->fmt_ch : legacy ? 1 : 2;
    f.samples = c->fmt_samples ? c->fmt_samples : legacy ? AUDIO_FRAMES : c->fmt_rate == 48000 ? 2048 : 1024;
    return f;
}

/* RX audio queued for a client up to 0.6 s of its stream (64 kB at least, as before), so a slow
 * link loses whole frames and never the state lines behind them */
static size_t audio_budget(const afmt_t *f) {
    size_t b = (size_t)f->rate * f->ch * tci_sample_bytes(f->type) * 6 / 10;
    return b < 65536 ? 65536 : b > OUT_SIZE - 16384 ? OUT_SIZE - 16384 : b;
}

/* ---- Configuration ------------------------------------------------------------------ */

static time_t conf_mtime(void) {
    struct stat st;
    return stat(TCI_CONF, &st) == 0 ? st.st_mtime : 0;
}

static void conf_read(void) {
    FILE *f = fopen(TCI_CONF, "r");
    char  line[64];
    bool  en = true, op = true;
    int   p  = TCI_DEFAULT_PORT;

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "enabled=", 8) == 0)
            en = atoi(line + 8) != 0;
        else if (strncmp(line, "port=", 5) == 0 && atoi(line + 5) > 0 && atoi(line + 5) < 65536)
            p = atoi(line + 5);
        else if (strncmp(line, "trust=", 6) == 0)
            op = strncmp(line + 6, "view", 4) != 0;
    }
    fclose(f);
    atomic_store(&enabled, en);
    atomic_store(&port, p);
    atomic_store(&trust_operate, op);
}

static void conf_write(void) {
    char  tmp[64];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", TCI_CONF);
    if (!(f = fopen(tmp, "w")))
        return;
    fprintf(f, "enabled=%d\nport=%d\ntrust=%s\n", atomic_load(&enabled) ? 1 : 0, atomic_load(&port),
            atomic_load(&trust_operate) ? "operate" : "view");
    fclose(f);
    rename(tmp, TCI_CONF);
}

static void wake(void) {
    if (wake_fd[1] >= 0 && write(wake_fd[1], "w", 1) < 0) {}
}

/* ---- Clients ------------------------------------------------------------------------ */

/* The frame at out_head, header and payload: its length, from its header (ws_frame_head's) */
static size_t out_frame_size(const client_t *c) {
    uint8_t h[WS_HEAD_MAX] = {0};
    size_t  len;

    for (size_t i = 0; i < sizeof(h) && i < c->out_len; i++)
        h[i] = c->out[(c->out_head + i) % OUT_SIZE];
    len = h[1] & 0x7f;
    if (len == 126)
        return 4 + ((size_t)h[2] << 8 | h[3]);
    if (len == 127) {
        len = 0;
        for (int i = 2; i < 10; i++)
            len = len << 8 | h[i];
        return 10 + len;
    }
    return 2 + len;
}

/* n bytes of out sent: the head moves on, and out_cut follows the frames it crossed */
static void out_sent(client_t *c, size_t n) {
    while (n) {
        if (!c->out_cut)
            c->out_cut = out_frame_size(c);
        size_t k = n < c->out_cut ? n : c->out_cut;
        c->out_cut -= k;
        c->out_head = (c->out_head + k) % OUT_SIZE;
        c->out_len -= k;
        n -= k;
    }
    if (!c->out_len)
        c->out_head = 0;
}

/* What the socket takes, without waiting; under lock. A TX_CHRONO waiting in urg goes before
 * the rest of out, as soon as the frame being sent from out is whole on the wire: behind RX
 * audio queued for a link slower than the stream (WSJT-X's 385 kB/s) it came later than the
 * 500 ms TX_SILENCE_MS, and the over was unkeyed. NOTSENT_LOWAT keeps that queue here and
 * not in the kernel, where nothing can pass it. out is a ring: nothing is moved */
static void client_flush(client_t *c) {
    while (c->fd >= 0 && (c->urg_len || c->out_len)) {
        struct iovec  iov[2];
        struct msghdr m   = {.msg_iov = iov, .msg_iovlen = 1};
        bool          urg = c->urg_len && !c->out_cut;
        size_t        n;

        if (urg) {
            n      = c->urg_len;
            iov[0] = (struct iovec){c->urg, n};
        } else {
            n = c->urg_len ? c->out_cut : c->out_len;   /* urg waits: the frame begun, no more */
            size_t first = OUT_SIZE - c->out_head;
            iov[0] = (struct iovec){c->out + c->out_head, n < first ? n : first};
            if (n > first) {
                iov[1]       = (struct iovec){c->out, n - first};
                m.msg_iovlen = 2;
            }
        }
        ssize_t k = sendmsg(c->fd, &m, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k <= 0)
            return;
        if (urg) {
            c->urg_len -= k;
            memmove(c->urg, c->urg + k, c->urg_len);
        } else {
            out_sent(c, k);
        }
        if ((size_t)k < n)
            return;     /* the socket is full: POLLOUT brings us back */
    }
}

/* Bytes after what waits in out; the room was checked */
static void out_put(client_t *c, const void *p, size_t n) {
    size_t at    = (c->out_head + c->out_len) % OUT_SIZE;
    size_t first = OUT_SIZE - at < n ? OUT_SIZE - at : n;

    memcpy(c->out + at, p, first);
    memcpy(c->out, (const uint8_t *)p + first, n - first);
    c->out_len += n;
}

/* A whole frame (header, then one or two payload parts) or nothing: a slow client loses
 * frames, never gets a cut one. The frame is queued while the output stays within limit.
 * Any thread. */
static void client_frame_within(client_t *c, size_t limit, uint8_t opcode, const void *p1, size_t l1,
                                const void *p2, size_t l2) {
    uint8_t head[WS_HEAD_MAX];
    size_t  hl = ws_frame_head(head, opcode, l1 + l2);

    pthread_mutex_lock(&c->lock);
    if (c->fd >= 0 && c->up && c->out_len + hl + l1 + l2 <= limit) {
        out_put(c, head, hl);
        if (l1)
            out_put(c, p1, l1);
        if (l2)
            out_put(c, p2, l2);
        client_flush(c);
    }
    pthread_mutex_unlock(&c->lock);
}

/* A TX_CHRONO: through urg, ahead of what waits in out (client_flush) */
static void client_chrono(client_t *c, const audio_hdr_t *h) {
    uint8_t head[WS_HEAD_MAX];
    size_t  hl = ws_frame_head(head, WS_OP_BINARY, sizeof(*h));

    pthread_mutex_lock(&c->lock);
    if (c->fd >= 0 && c->up && c->urg_len + hl + sizeof(*h) <= URG_SIZE) {
        memcpy(c->urg + c->urg_len, head, hl);
        memcpy(c->urg + c->urg_len + hl, h, sizeof(*h));
        c->urg_len += hl + sizeof(*h);
        client_flush(c);
    }
    pthread_mutex_unlock(&c->lock);
}

static void client_frame(client_t *c, uint8_t opcode, const void *p1, size_t l1, const void *p2, size_t l2) {
    client_frame_within(c, OUT_SIZE, opcode, p1, l1, p2, l2);
}

static void client_text(client_t *c, const char *s) {
    client_frame(c, WS_OP_TEXT, s, strlen(s), NULL, 0);
}

static void client_textf(client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void client_textf(client_t *c, const char *fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof(buf))
        client_text(c, buf);
}

static void broadcast(const char *s) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].up)
            client_text(&clients[i], s);
}

static void broadcastf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void broadcastf(const char *fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof(buf))
        broadcast(buf);
}

/* Socket thread: the capture runs while a client wants audio. Only this thread decides, so
 * an audio_start cannot be undone by the capture thread finding no listener a moment
 * earlier (audit A16) */
static void cap_update(void) {
    bool any = false;

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].audio_on)
            any = true;
    atomic_store(&cap_wanted, any);
}

static void client_close(client_t *c) {
    ctl_release(CTL_TCI, (int)(c - clients));
    pthread_mutex_lock(&c->lock);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        atomic_fetch_sub(&nclients, 1);
    }
    c->ws = c->up = c->funnel = false;
    c->token[0] = 0;
    c->out_head = c->out_len = c->out_cut = c->urg_len = c->in_len = c->text_len = 0;
    c->audio_on = c->rx_sensors = c->tx_sensors = false;
    pthread_mutex_unlock(&c->lock);
    cap_update();
}

/* ---- The radio, on the LVGL thread -------------------------------------------------- */

/* The modes TCI audio keys the radio in: all but CW and CW-R. In DIGL and DIGU the modem bit takes
 * the codec's audio too: the radio's own FT8, PSK, Olivia and Winlink transmit so in USB-D (#17) */
static bool takes_tx_audio(int32_t m) {
    return m != radio_mode_cw && m != radio_mode_cwr;
}

/* tci: the TCI audio session (if there is one); else the radio's own PTT */
static void tx_unkey(bool tci, bool drain) {
    if (tci) {
        if (!atomic_load(&txs.keyed))
            return;
        if (drain)
            audio_play_wait();
        else
            audio_play_flush();
        radio_set_modem(false);
        audio_play_lowlat_end();
        atomic_store(&txs.keyed, false);
    } else {
        radio_set_ptt(false);
    }
    radio_set_tci_mute(false);
}

/* The PTT unkey of start gen is the one due: no longer due, true once (LVGL thread) */
static bool ptt_unkey_take(uint32_t gen) {
    return atomic_compare_exchange_strong(&ptt_unkey_due, &gen, 0);
}

static void apply_cmd(void *arg) {
    cmd_t *c = (cmd_t *)arg;

    switch (c->id) {
        case C_FREQ:
            backlight_tick();   /* tuning from the knob or a program: the screen wakes as for a key */
            if (!main_screen_freq_locked() && cfg_is_valid_hw_freq(c->a))
                main_screen_set_freq((uint64_t)c->a);
            break;
        case C_MODE:
            if (!main_screen_mode_locked())
                cparam_i_set(cfg_cur_mode, c->a);
            break;
        case C_FILTER:
            /* low then high, or the other way round when the band moves up: each edge is
             * checked against the other as it is set */
            if (c->b > cparam_i_get(cfg_cur_filter_high)) {
                cparam_i_set(cfg_cur_filter_high, c->b);
                cparam_i_set(cfg_cur_filter_low, c->a);
            } else {
                cparam_i_set(cfg_cur_filter_low, c->a);
                cparam_i_set(cfg_cur_filter_high, c->b);
            }
            break;
        case C_AGC:
            cparam_i_set(cfg_cur_agc, c->a);
            break;
        case C_RIT:
            param_i_set(cfg_rit, c->a);
            break;
        case C_DRIVE:
            cfg_pwr_set(0.1f + c->a * DRIVE_SPAN / 100.0f);   /* saved, the radio follows the setting */
            drive_set_store(c->a, param_f_get(cfg_pwr)); /* as the setting kept it */
            break;
        case C_KEY:
            /* keyed or refused, the socket thread learns it by the start's number (state_follow).
             * A start that ended before its key got here keys nothing: no one would hold it (C01) */
            if ((uint32_t)c->a == atomic_load(&key_cancelled) || radio_get_state() != RADIO_RX || radio_get_modem() ||
                atomic_load(&txs.keyed)) {
                atomic_store(&key_refused, (uint32_t)c->a);
                break;
            }
            if (c->tci && takes_tx_audio(cparam_i_get(cfg_cur_mode))) {
                if (!audio_play_lowlat_begin()) {
                    atomic_store(&key_refused, (uint32_t)c->a);
                    break;
                }
                txs.pos  = 0;
                txs.prev = 0;
                atomic_store(&txs.keyed, true);
                radio_set_modem(true);
            } else if (c->tci) {
                /* CW or CW-R: the audio has no way in, and the radio's own PTT would have been
                 * keyed with nothing in the session to release it (audit A01) */
                atomic_store(&key_refused, (uint32_t)c->a);
                break;
            } else {
                radio_set_ptt(true);
            }
            radio_set_tci_mute(true);   /* the operator is at the other end: nothing from the speaker */
            atomic_store(&key_keyed, (uint32_t)c->a);
            break;
        case C_UNKEY:
            /* The radio's PTT is the operator's too (the PTT key, the browser's PTT): a start's
             * unkey releases it only if that start's key set it, and once, while it is the one
             * due: a copy resent before it ran then does nothing. A refused start keys nothing
             * and unkeys nothing (C01) */
            if (!c->tci && (!ptt_unkey_take((uint32_t)c->b) || (uint32_t)c->b != atomic_load(&key_keyed)))
                break;
            tx_unkey(c->tci, c->a != 0);
            break;
    }
}

/* An unkey takes the scheduler's reserve: a flood of tuning must not keep the radio on the air.
 * False: the queue was full, the command is lost */
static bool put_cmd(cmd_id_t id, int32_t a, int32_t b, bool tci) {
    cmd_t c = {.id = id, .a = a, .b = b, .tci = tci};
    if (id == C_UNKEY)
        return scheduler_put_urgent(apply_cmd, &c, sizeof(c));
    return scheduler_put(apply_cmd, &c, sizeof(c));
}

/* ---- The state, read from any thread ------------------------------------------------ */

static const char *const mode_names[] = {"lsb", "digl", "usb", "digu", "cw", "cwr", "am", "nfm", "sam", "wfm"};
static const char *const agc_names[]  = {"off", "slow", "fast", "auto"};

static void state_read(rstate_t *s) {
    s->freq  = cparam_i_get(cfg_fg_freq);
    s->mode  = cparam_i_get(cfg_cur_mode);
    s->flo   = cparam_i_get(cfg_cur_filter_low);
    s->fhi   = cparam_i_get(cfg_cur_filter_high);
    s->agc   = cparam_i_get(cfg_cur_agc);
    s->rit   = param_i_get(cfg_rit);
    s->drive = drive_of(param_f_get(cfg_pwr));
    s->tx    = radio_get_state() != RADIO_RX || radio_get_modem();
}

static const char *mode_name(int32_t m) {
    return m >= 0 && m <= radio_mode_last ? mode_names[m] : "usb";
}

static const char *agc_name(int32_t a) {
    return a >= 0 && a < 4 ? agc_names[a] : "auto";
}

/* The passband as TCI tells it: below the carrier negative (lsb, digl, cwr) */
static void filter_line(char *buf, size_t n, const rstate_t *s) {
    bool neg = s->mode == radio_mode_lsb || s->mode == radio_mode_lsb_dig || s->mode == radio_mode_cwr;
    if (neg)
        snprintf(buf, n, "rx_filter_band:0,%d,%d;", -s->fhi, -s->flo);
    else
        snprintf(buf, n, "rx_filter_band:0,%d,%d;", s->flo, s->fhi);
}

static void send_greeting(client_t *c, const rstate_t *s) {
    char fb[64];

    filter_line(fb, sizeof(fb), s);
    client_text(c, "protocol:ExpertSDR3,1.9;");
    client_text(c, "device:OpenX6X00;");
    client_text(c, "receive_only:false;");
    client_text(c, "trx_count:1;");
    client_text(c, "channels_count:1;");
    client_text(c, "vfo_limits:100000,55000000;");
    client_text(c, "if_limits:-48000,48000;");
    /* the list and the parser agree: sam and wfm where the base has them (audit B15) */
    client_textf(c, "modulations_list:lsb,usb,digl,digu,cw,cwr,am,nfm%s;", radio_caps() & HW_CAP_SAM_WFM ? ",sam,wfm" : "");
    /* the client's audio format: TCI's defaults, until it asks */
    afmt_t f = audio_fmt(c);
    client_textf(c, "audio_samplerate:%u;", f.rate);
    client_textf(c, "audio_stream_sample_type:%s;", tci_sample_type_name(f.type));
    client_textf(c, "audio_stream_channels:%u;", f.ch);
    client_textf(c, "audio_stream_samples:%u;", f.samples);
    client_textf(c, "vfo:0,0,%d;", s->freq);
    client_textf(c, "modulation:0,%s;", mode_name(s->mode));
    client_text(c, fb);
    client_textf(c, "agc_mode:0,%s;", agc_name(s->agc));
    client_textf(c, "rit_enable:0,%s;", s->rit ? "true" : "false");
    client_textf(c, "rit_offset:0,%d;", s->rit);
    client_text(c, "rx_enable:0,true;");
    client_text(c, "tx_enable:0,true;");
    client_text(c, "lock:0,false;");
    client_text(c, "split_enable:0,false;");
    client_textf(c, "drive:0,%d;", s->drive);
    client_textf(c, "trx:0,%s;", s->tx ? "true" : "false");
    /* The receiver runs whenever the server does. ExpertSDR3 and Thetis end the state with
     * start; (stop; for an SDR switched off), and WSJT-X's and JTDX's TCI client take a
     * greeting without it as a radio switched off: "TCI SDR is not switched on" (#17) */
    client_text(c, "start;");
    client_text(c, "ready;");
}

static void tx_session_end(int ci, bool drain);

/* What changed since the last look goes to everyone. What became of the last start is read
 * before the state: a transmission seen in the state then came after its key */
static void state_follow(rstate_t *last, bool first) {
    uint32_t keyed = atomic_load(&key_keyed), refused = atomic_load(&key_refused);
    rstate_t s;
    char     fb[64];

    state_read(&s);
    if (first) {
        *last = s;
        return;
    }
    if (s.freq != last->freq)
        broadcastf("vfo:0,0,%d;", s.freq);
    if (s.mode != last->mode)
        broadcastf("modulation:0,%s;", mode_name(s.mode));
    if (s.flo != last->flo || s.fhi != last->fhi || s.mode != last->mode) {
        filter_line(fb, sizeof(fb), &s);
        broadcast(fb);
    }
    if (s.agc != last->agc)
        broadcastf("agc_mode:0,%s;", agc_name(s.agc));
    if (s.rit != last->rit) {
        if ((s.rit != 0) != (last->rit != 0))
            broadcastf("rit_enable:0,%s;", s.rit ? "true" : "false");
        broadcastf("rit_offset:0,%d;", s.rit);
    }
    if (s.drive != last->drive)
        broadcastf("drive:0,%d;", s.drive);
    if (s.tx != last->tx)
        broadcastf("trx:0,%s;", s.tx ? "true" : "false");
    /* The last start refused: its client holds nothing and hears so (audit A01). The refusal of
     * a start cancelled since is no one's: it neither ends nor tells the start after it (C01) */
    if (refused == key_gen && (txs.owner >= 0 || ptt_owner >= 0)) {
        client_text(&clients[txs.owner >= 0 ? txs.owner : ptt_owner], "trx:0,false;");
        txs.owner = ptt_owner = -1;
    }
    /* The radio's PTT is its client's until the radio is back on receive after the key took; the
     * session ends then, its PTT released with it, never left set with no one to end it. An end
     * seen before the key took is another transmission's (C01) */
    if (ptt_owner >= 0 && keyed == key_gen && s.tx)
        ptt_on_air = true;
    if (ptt_owner >= 0 && ptt_on_air && !s.tx)
        tx_session_end(ptt_owner, false);
    *last = s;
}

/* ---- Commands ----------------------------------------------------------------------- */

static bool parse_bool(const char *s) {
    return strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0;
}

static int mode_index(const char *name) {
    int n = radio_caps() & HW_CAP_SAM_WFM ? 10 : 8;

    for (int i = 0; i < n; i++)
        if (strcasecmp(name, mode_names[i]) == 0)
            return i;
    if (strcasecmp(name, "fm") == 0 || strcasecmp(name, "wfm") == 0) return radio_mode_nfm;
    if (strcasecmp(name, "sam") == 0 || strcasecmp(name, "dsb") == 0) return radio_mode_am;
    return -1;
}

static int agc_index(const char *name) {
    for (int i = 0; i < 4; i++)
        if (strcasecmp(name, agc_names[i]) == 0)
            return i;
    if (strcasecmp(name, "med") == 0 || strcasecmp(name, "medium") == 0) return radio_agc_auto;
    return -1;
}

/* "name:a,b,c" split in place; returns the argument count */
static int split_args(char *cmd, char **name, char *args[], int max) {
    char *colon = strchr(cmd, ':');
    int   n     = 0;

    *name = cmd;
    if (!colon)
        return 0;
    *colon = 0;
    char *p = colon + 1;
    while (n < max && p) {
        args[n++] = p;
        p = strchr(p, ',');
        if (p)
            *p++ = 0;
    }
    return n;
}

/* A whole number, nothing after it */
static bool parse_int(const char *s, int *v) {
    char *end;
    long  l = strtol(s, &end, 10);

    if (end == s || *end || l < -1000000 || l > 1000000)
        return false;
    *v = (int)l;
    return true;
}

/* audio_samplerate, audio_stream_sample_type, audio_stream_channels, audio_stream_samples:
 * set (a value the server has not got is ignored, as TCI says: 24 and 48 kHz; int16, int24,
 * int32, float32; channels limited to 1-2, samples to 100-2048), answered with the value as it
 * is, then with every other one that moved with it: the samples follow the rate unless set, and
 * the knob's exception in audio_fmt comes and goes with the rate and the channels */
static void audio_fmt_cmd(client_t *c, const char *name, const char *arg) {
    afmt_t was = audio_fmt(c), now;
    int    v;

    pthread_mutex_lock(&c->lock);
    if (arg && !strcmp(name, "audio_samplerate")) {
        if (parse_int(arg, &v) && (v == AUDIO_RATE || v == 48000)) {
            c->fmt_rate       = v;
            c->fmt_rate_asked = true;
        }
    } else if (arg && !strcmp(name, "audio_stream_sample_type")) {
        if ((v = tci_sample_type(arg)) >= 0)
            c->fmt_type = v;
    } else if (arg && !strcmp(name, "audio_stream_channels")) {
        if (parse_int(arg, &v))
            c->fmt_ch = LV_CLAMP(1, v, 2);
    } else if (arg) {
        if (parse_int(arg, &v))
            c->fmt_samples = LV_CLAMP(100, v, 2048);
    }
    now = audio_fmt(c);
    if (memcmp(&was, &now, sizeof(now)))
        c->fmt_gen++;
    pthread_mutex_unlock(&c->lock);

    bool rate = !strcmp(name, "audio_samplerate"), type = !strcmp(name, "audio_stream_sample_type");
    bool ch = !strcmp(name, "audio_stream_channels"), samples = !strcmp(name, "audio_stream_samples");
    if (rate || was.rate != now.rate)
        client_textf(c, "audio_samplerate:%u;", now.rate);
    if (type || was.type != now.type)
        client_textf(c, "audio_stream_sample_type:%s;", tci_sample_type_name(now.type));
    if (ch || was.ch != now.ch)
        client_textf(c, "audio_stream_channels:%u;", now.ch);
    if (samples || was.samples != now.samples)
        client_textf(c, "audio_stream_samples:%u;", now.samples);
}

/* The client's transmission ends, whichever way it keyed: from the socket thread. A key still
 * on its way to the LVGL thread is cancelled; one that ran meanwhile is undone by the unkey
 * queued after it. */
static void tx_session_end(int ci, bool drain) {
    if ((txs.owner == ci || ptt_owner == ci) && atomic_load(&key_keyed) != key_gen &&
        atomic_load(&key_refused) != key_gen)
        atomic_store(&key_cancelled, key_gen);
    if (txs.owner == ci) {
        put_cmd(C_UNKEY, drain, 0, true);
        txs.owner = -1;
    }
    if (ptt_owner == ci) {
        /* due before it is queued, where it may run at once; the queue can drop it: tx_session_tick resends */
        atomic_store(&ptt_unkey_due, key_gen);
        ptt_unkey_ms = now_ms();
        put_cmd(C_UNKEY, 0, (int32_t)key_gen, false);
        ptt_owner    = -1;
    }
}

/* A start may be queued: nobody is on the air, no start is on its way to it (audit C01), and
 * nothing a start left is still to be undone. One start at a time, from its request to its unkey:
 * a key queued behind a cancelled start was undone by that start's unkey or the unkey's resend,
 * or its session ended when the cancelled start's modem bit dropped */
static bool tx_free(const rstate_t *s) {
    bool handled = atomic_load(&key_keyed) == key_gen || atomic_load(&key_refused) == key_gen;

    return !s->tx && txs.owner < 0 && ptt_owner < 0 && handled && !atomic_load(&txs.keyed) &&
           !atomic_load(&ptt_unkey_due);
}

/* Over Funnel the Panel's own sound and microphone, nothing more (#58): the audio format, audio on and off, the
 * TX state, an unkey, and a key with the page's audio ("trx:0,true,tci") for a session that may transmit, looked
 * up again as it keys (a session revoked meanwhile keys nothing). A key with the radio's own microphone is answered
 * trx:0,false; anything else is left unanswered. The TX state asked, or an unkey from a client whose transmission
 * it is not, is answered with the state here: handle_cmd would take control of the radio for it as for a TCI
 * program, and a session that may only listen, or not transmit, takes no control on the browser remote either.
 * TCI programs do not come over Funnel: they cannot pair */
static bool funnel_cmd_ok(client_t *c, const char *name, char **a, int n) {
    int ci = (int)(c - clients);

    if (!strcmp(name, "audio_samplerate") || !strcmp(name, "audio_stream_sample_type") ||
        !strcmp(name, "audio_stream_channels") || !strcmp(name, "audio_stream_samples") ||
        !strcmp(name, "audio_start") || !strcmp(name, "audio_stop"))
        return true;
    if (strcmp(name, "trx") != 0)
        return false;
    if (n < 2 || !parse_bool(a[1])) {
        if (n >= 2 && (txs.owner == ci || ptt_owner == ci))
            return true;                    /* its own transmission's end, never refused (audit B01) */
        rstate_t s;
        state_read(&s);
        client_textf(c, "trx:0,%s;", s.tx ? "true" : "false");
        return false;
    }
    if (n >= 3 && !strcasecmp(a[2], "tci") && remote_server_session_check(c->token, &c->role, &c->tx_ok, NULL, 0) &&
        c->role >= 1 && c->tx_ok)
        return true;
    client_text(c, "trx:0,false;");
    return false;
}

static void handle_cmd(int ci, char *line) {
    client_t *c = &clients[ci];
    char     *name, *a[4];
    int       n = split_args(line, &name, a, 4);
    rstate_t  s;
    char      fb[64];

    while (*name == ' ' || *name == '\n' || *name == '\r') name++;
    if (!*name)
        return;
    if (c->funnel && !funnel_cmd_ok(c, name, a, n))
        return;
    state_read(&s);

    /* What touches the RF side: a trusted client takes control of the radio as the knob always
     * did, unless someone is on the air; a client trusted to view only is answered with the
     * state as it is, which is what the echo below does when nothing changes */
    bool rf = !strcmp(name, "vfo") || !strcmp(name, "modulation") || !strcmp(name, "rx_filter_band") ||
              !strcmp(name, "agc_mode") || !strcmp(name, "rit_enable") || !strcmp(name, "rit_offset") ||
              !strcmp(name, "drive") || !strcmp(name, "trx");
    /* Ending its own transmission is never refused: the client that keyed unkeys whatever happened
     * to control (a knob took it) or to its trust (Listen only) since; refusing left the radio on
     * the air until the 180 s limit (audit B01) */
    bool release = !strcmp(name, "trx") && n >= 2 && !parse_bool(a[1]) && (txs.owner == ci || ptt_owner == ci);
    /* A start that cannot be had is answered trx:0,false below and takes nothing: control stays
     * where it is, with the client whose start is on its way or on the air, which then still
     * tunes during its over (C01). A client trusted to view only is answered with the state */
    bool refused = !strcmp(name, "trx") && n >= 2 && parse_bool(a[1]) && !tx_free(&s);
    bool claim   = !refused && !(n < 2 && strcmp(name, "trx"));
    bool may     = !rf || release || (atomic_load(&trust_operate) && (!claim || ctl_claim(CTL_TCI, ci, "TCI", true)));
    if (rf && !may && n >= 2) {
        /* the state as it is, as for a command that changes nothing */
        if (!strcmp(name, "vfo")) client_textf(c, "vfo:0,0,%d;", s.freq);
        else if (!strcmp(name, "modulation")) client_textf(c, "modulation:0,%s;", mode_name(s.mode));
        else if (!strcmp(name, "rx_filter_band")) { filter_line(fb, sizeof(fb), &s); client_text(c, fb); }
        else if (!strcmp(name, "agc_mode")) client_textf(c, "agc_mode:0,%s;", agc_name(s.agc));
        else if (!strcmp(name, "rit_enable")) client_textf(c, "rit_enable:0,%s;", s.rit ? "true" : "false");
        else if (!strcmp(name, "rit_offset")) client_textf(c, "rit_offset:0,%d;", s.rit);
        else if (!strcmp(name, "drive")) client_textf(c, "drive:0,%d;", s.drive);
        else client_textf(c, "trx:0,%s;", s.tx ? "true" : "false");
        return;
    }

    if (!strcmp(name, "vfo")) {
        if (n >= 3 && atoi(a[0]) == 0 && atoi(a[1]) == 0) {
            int32_t f = (int32_t)strtoll(a[2], NULL, 10);
            if (f == s.freq)
                client_textf(c, "vfo:0,0,%d;", s.freq);   /* nothing will change: the echo now */
            else
                put_cmd(C_FREQ, f, 0, false);
        } else {
            client_textf(c, "vfo:0,0,%d;", s.freq);
        }
    } else if (!strcmp(name, "modulation")) {
        int m = n >= 2 ? mode_index(a[1]) : -1;
        if (m < 0 || m == s.mode)
            client_textf(c, "modulation:0,%s;", mode_name(s.mode));
        else
            put_cmd(C_MODE, m, 0, false);
    } else if (!strcmp(name, "rx_filter_band")) {
        if (n >= 3) {
            int32_t lo = abs(atoi(a[1])), hi = abs(atoi(a[2]));
            if (lo > hi) { int32_t t = lo; lo = hi; hi = t; }
            if (lo == s.flo && hi == s.fhi) {
                filter_line(fb, sizeof(fb), &s);
                client_text(c, fb);
            } else {
                put_cmd(C_FILTER, lo, hi, false);
            }
        } else {
            filter_line(fb, sizeof(fb), &s);
            client_text(c, fb);
        }
    } else if (!strcmp(name, "agc_mode")) {
        int g = n >= 2 ? agc_index(a[1]) : -1;
        if (g < 0 || g == s.agc)
            client_textf(c, "agc_mode:0,%s;", agc_name(s.agc));
        else
            put_cmd(C_AGC, g, 0, false);
    } else if (!strcmp(name, "rit_enable")) {
        if (n >= 2 && !parse_bool(a[1]) && s.rit != 0)
            put_cmd(C_RIT, 0, 0, false);
        else
            client_textf(c, "rit_enable:0,%s;", s.rit ? "true" : "false");
    } else if (!strcmp(name, "rit_offset")) {
        int32_t r = n >= 2 ? atoi(a[1]) : s.rit;
        if (n < 2 || r == s.rit)
            client_textf(c, "rit_offset:0,%d;", s.rit);
        else
            put_cmd(C_RIT, LV_CLAMP(-1500, r, 1500), 0, false);
    } else if (!strcmp(name, "drive")) {
        int d = n >= 2 ? atoi(a[1]) : s.drive;
        if (n < 2 || d == s.drive)
            client_textf(c, "drive:0,%d;", s.drive);
        else
            put_cmd(C_DRIVE, LV_CLAMP(0, d, 100), 0, false);
    } else if (!strcmp(name, "trx")) {
        if (n < 2) {
            client_textf(c, "trx:0,%s;", s.tx ? "true" : "false");
        } else if (parse_bool(a[1])) {
            bool tci = n >= 3 && strcasecmp(a[2], "tci") == 0;
            if (refused) {      /* as decided above: control was not claimed, even if tx_free holds by now */
                client_text(c, "trx:0,false;");
            } else {
                key_gen++;
                if (tci) {
                    txs.owner         = ci;
                    txs.start_ms      = now_ms();
                    txs.last_audio_ms = txs.start_ms;
                    txs.chrono_t0     = 0;
                    txs.chrono_n      = 0;
                    txs.rate          = audio_fmt(c).rate;
                } else {
                    ptt_owner    = ci;
                    ptt_on_air   = false;
                    ptt_keyed_ms = 0;
                    ptt_ms       = now_ms();
                }
                if (!put_cmd(C_KEY, (int32_t)key_gen, 0, tci))
                    atomic_store(&key_refused, key_gen);    /* lost: refused, as the LVGL thread would */
            }
        } else {
            if (txs.owner == ci || ptt_owner == ci)
                tx_session_end(ci, true);
            else
                client_text(c, "trx:0,false;");     /* not this client's transmission */
        }
    } else if (!strcmp(name, "rx_sensors_enable")) {
        if (n >= 1) c->rx_sensors = parse_bool(a[0]);
        client_textf(c, "rx_sensors_enable:%s;", c->rx_sensors ? "true" : "false");
    } else if (!strcmp(name, "tx_sensors_enable")) {
        if (n >= 1) c->tx_sensors = parse_bool(a[0]);
        client_textf(c, "tx_sensors_enable:%s;", c->tx_sensors ? "true" : "false");
    } else if (!strcmp(name, "audio_samplerate") || !strcmp(name, "audio_stream_sample_type") ||
               !strcmp(name, "audio_stream_channels") || !strcmp(name, "audio_stream_samples")) {
        audio_fmt_cmd(c, name, n >= 1 ? a[0] : NULL);
    } else if (!strcmp(name, "audio_start")) {
        pthread_mutex_lock(&c->lock);
        c->audio_on = true;
        c->fmt_gen++;               /* the stream starts with a whole frame of now */
        pthread_mutex_unlock(&c->lock);
        cap_update();
        client_text(c, "audio_start:0;");
    } else if (!strcmp(name, "audio_stop")) {
        pthread_mutex_lock(&c->lock);
        c->audio_on = false;
        pthread_mutex_unlock(&c->lock);
        cap_update();
        client_text(c, "audio_stop:0;");
    } else if (!strcmp(name, "tx_enable")) {
        client_text(c, "tx_enable:0,true;");
    } else if (!strcmp(name, "rx_enable")) {
        client_text(c, "rx_enable:0,true;");
    } else if (!strcmp(name, "lock") || !strcmp(name, "split_enable") || !strcmp(name, "xit_enable") ||
               !strcmp(name, "tune") || !strcmp(name, "mute") || !strcmp(name, "rx_mute")) {
        client_textf(c, "%s:0,false;", name);
    } else if (!strcmp(name, "xit_offset")) {
        client_text(c, "xit_offset:0,0;");
    } else if (!strcmp(name, "volume") || !strcmp(name, "mon_volume")) {
        client_textf(c, "%s:0;", name);
    } else if (!strcmp(name, "rx_volume") || !strcmp(name, "rx_balance")) {
        client_textf(c, "%s:0,0,0;", name);
    } else if (!strcmp(name, "start") || !strcmp(name, "stop")) {
        /* A client cannot switch the radio on or off: the answer is the state as it is, running */
        client_text(c, "start;");
    }
    /* spot, tune_drive, the DSP switches and what we don't know: nothing to say */
}

static void handle_text(int ci, char *text) {
    char *p = text, *semi;

    while ((semi = strchr(p, ';'))) {
        *semi = 0;
        handle_cmd(ci, p);
        p = semi + 1;
    }
    if (*p)
        handle_cmd(ci, p);    /* a last command without its ';' */
}

/* ---- TX audio from the client ------------------------------------------------------- */

/* Linear interpolation from in_rate to AUDIO_PLAY_RATE, carried over between frames. 48 kHz
 * (WSJT-X, a browser's microphone) goes in as it is: simulated, a halfband down to 24 kHz first
 * changed nothing in the passband, where the worst product is this interpolation's own (-48 dBc
 * for a 2.9 kHz tone, the same from the knob's 24 kHz), and left 15 dB more above it */
static size_t resample(const int16_t *in, size_t n, uint32_t in_rate, int16_t *out, size_t out_max) {
    double step = (double)in_rate / AUDIO_PLAY_RATE;
    size_t k    = 0;

    while (k < out_max) {
        double  p  = txs.pos;
        size_t  i  = (size_t)p;
        if (i >= n)
            break;
        double  fr = p - i;
        int16_t a  = i == 0 ? txs.prev : in[i - 1];
        int16_t b  = in[i];
        /* p lies between in[i-1] (a) and in[i] (b); prev is the last sample of the previous frame */
        out[k++]   = (int16_t)(a + (b - a) * fr);
        txs.pos += step;
    }
    txs.pos -= n;
    if (txs.pos < 0)
        txs.pos = 0;
    txs.prev = in[n - 1];
    return k;
}

/* A TX frame as TCI lays it out: length samples of all the channels, in the header's type.
 * Two shapes are taken:
 *  - the payload is length samples (TCI's, the knob's, the browser microphone's); the
 *    channels are the header's, or the client's format's when that word is not 1 or 2;
 *  - the payload is twice that, its first half the audio: WSJT-X and JTDX size the frame for
 *    length pairs, fill it with length samples (length / 2 pairs, left and right the same)
 *    and leave the channel word unwritten, so their frame is stereo whatever it says.
 * A frame of another shape is no audio the radio can read (played as it came it was noise at
 * the set drive): it ends the transmission before any of it is played, the radio unkeys and
 * the client hears trx:0,false. The first channel goes on the air. Each bound checked on its
 * own: a product of the header's words unchecked wrapped on 32 bits and read past the frame */
static void handle_tx_audio(int ci, const uint8_t *frame, size_t len) {
    audio_hdr_t h;
    int16_t     pcm[2048];
    int16_t     out[2048 * 6 + 2];

    if (len < sizeof(h) || txs.owner != ci || !atomic_load(&txs.keyed))
        return;
    memcpy(&h, frame, sizeof(h));
    if (h.type != AT_TX)
        return;
    size_t   n   = h.length;
    size_t   bps = tci_sample_bytes(h.format);
    size_t   p   = len - sizeof(h);
    uint32_t ch  = 0;
    if (bps && n && n <= 2048) {
        if (p == n * bps)
            ch = h.channels == 1 || h.channels == 2 ? h.channels : audio_fmt(&clients[ci]).ch;
        else if (p == 2 * n * bps)
            ch = 2;
    }
    if (!ch || n / ch == 0) {
        tx_session_end(ci, false);
        return;
    }
    size_t frames = n / ch;
    tci_decode_first(frame + sizeof(h), frames, ch, h.format, pcm);
    if (h.sample_rate >= 8000 && h.sample_rate <= 48000)
        txs.rate = h.sample_rate;
    txs.last_audio_ms = now_ms();
    size_t k = resample(pcm, frames, txs.rate, out, sizeof(out) / sizeof(out[0]));
    if (k)
        audio_play_if_room(out, k);    /* full: dropped, the delay stays short */
}

/* Keyed with TCI audio: the pacing frames, and the watchdog. The radio's own PTT keyed from
 * here has the time limit too. */
static void tx_session_tick(uint64_t now) {
    if (ptt_owner >= 0 && (clients[ptt_owner].fd < 0 || now - ptt_ms > TX_LIMIT_MS))
        tx_session_end(ptt_owner, false);
    /* A key that never took (no TX on this frequency, the base did not follow) holds nothing:
     * the start is over, as a TCI audio session's below, and its client hears so. It would
     * keep every other start out (audit C01). The second counts from when its key ran: a key
     * still queued behind a busy LVGL thread is on its way, not refused */
    if (ptt_owner >= 0 && !ptt_on_air && atomic_load(&key_keyed) == key_gen) {
        if (!ptt_keyed_ms) {
            ptt_keyed_ms = now;
        } else if (now - ptt_keyed_ms > 1000) {
            client_text(&clients[ptt_owner], "trx:0,false;");
            tx_session_end(ptt_owner, false);
        }
    }
    /* The unkey of the radio's PTT is one entry in the scheduler's queue, which drops when full
     * (a flooding client): until the LVGL thread ran it, it goes again every 300 ms */
    uint32_t due = atomic_load(&ptt_unkey_due);
    if (due && now - ptt_unkey_ms > 300) {
        ptt_unkey_ms = now;
        put_cmd(C_UNKEY, 0, (int32_t)due, false);
    }
    if (txs.owner < 0) {
        if (atomic_load(&txs.keyed))            /* keyed for a client that has gone */
            put_cmd(C_UNKEY, 0, 0, true);
        return;
    }
    client_t *c = &clients[txs.owner];
    if (c->fd < 0 || !atomic_load(&txs.keyed)) {
        if (c->fd < 0 || now - txs.start_ms > 1000)   /* gone, or the key never took */
            tx_session_end(txs.owner, false);
        return;
    }
    if (!radio_get_modem()) {               /* the SWR guard or an app took the modem bit */
        tx_session_end(txs.owner, false);
        return;
    }
    if (now - txs.last_audio_ms > TX_SILENCE_MS || now - txs.start_ms > TX_LIMIT_MS) {
        tx_session_end(txs.owner, false);
        return;
    }
    /* TX_CHRONO asks for 21.3 ms of audio in the client's format (512 samples at 24 kHz, 1024
     * at 48 kHz; length counts every channel's), on time to the microsecond: a pace rounded to
     * 21 ms asked for 1.6 % more than the air takes, and the play stream dropped the rest. A
     * late tick sends what is due; a stall, or a new format, starts the count over */
    afmt_t   f      = audio_fmt(c);
    uint64_t frames = (uint64_t)f.rate * AUDIO_FRAMES / AUDIO_RATE;
    uint64_t us     = now_us();
    if (!txs.chrono_t0 || txs.chrono_gen != c->fmt_gen) {
        txs.chrono_t0  = us;
        txs.chrono_n   = 0;
        txs.chrono_gen = c->fmt_gen;
    }
    for (int i = 0; i < 2; i++) {
        uint64_t due = txs.chrono_t0 + txs.chrono_n * frames * 1000000 / f.rate;
        if (us < due)
            break;
        if (us - due > 200000) {
            txs.chrono_t0 = us;
            txs.chrono_n  = 0;
        }
        audio_hdr_t h = {.receiver = 0, .sample_rate = f.rate, .format = f.type, .length = (uint32_t)frames * f.ch,
                         .type = AT_CHRONO, .channels = f.ch};
        client_chrono(c, &h);
        txs.chrono_n++;
    }
}

/* ---- RX audio to the clients -------------------------------------------------------- */

/* Captured once at AUDIO_RATE, mono; each client gets it in its format (audio_fmt): at 48 kHz
 * through the halfband (run once a read for all of them), the one channel in each of the
 * client's, in frames of the client's size, what is short of a frame kept for the next read.
 * The header says what the frame holds. */
static void *capture_thread(void *arg) {
    static uint8_t pkt[2048 * 2 * 4];   /* the largest frame: 2048 float32 pairs */
    pa_simple     *s = NULL;
    int16_t        pcm[AUDIO_FRAMES];
    float          x24[AUDIO_FRAMES], x48[2 * AUDIO_FRAMES];
    tci_up2_t      up = {0};
    (void)arg;

    while (true) {
        if (!atomic_load(&cap_wanted)) {
            if (s) {
                pa_simple_free(s);
                s = NULL;
            }
            usleep(50000);
            continue;
        }
        if (!s) {
            pa_sample_spec spec = {.format = PA_SAMPLE_S16LE, .rate = AUDIO_RATE, .channels = 1};
            pa_buffer_attr attr = {.maxlength = (uint32_t)-1, .fragsize = AUDIO_FRAMES * 2};
            s = pa_simple_new(NULL, "OpenX6X00 TCI", PA_STREAM_RECORD, "alsa_input.platform-sound.stereo-fallback",
                              "TCI RX", &spec, NULL, &attr, NULL);
            if (!s) {
                LV_LOG_ERROR("tci: no capture stream");
                sleep(1);
                continue;
            }
        }
        if (pa_simple_read(s, pcm, sizeof(pcm), NULL) < 0) {
            pa_simple_free(s);
            s = NULL;
            continue;
        }

        bool   on[MAX_CLIENTS], any48 = false;
        afmt_t fmt[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            uint32_t  gen;

            /* one look at the client's wishes per read: the header and the samples agree */
            pthread_mutex_lock(&c->lock);
            on[i]  = c->fd >= 0 && c->up && c->audio_on;
            fmt[i] = audio_fmt(c);
            gen    = c->fmt_gen;
            pthread_mutex_unlock(&c->lock);
            if (gen != c->acc_gen || !on[i]) {
                c->acc_gen = gen;
                c->acc_n   = 0;
            }
            any48 = any48 || (on[i] && fmt[i].rate == 48000);
        }
        for (int k = 0; k < AUDIO_FRAMES; k++)
            x24[k] = pcm[k] / 32768.0f;
        tci_up2(&up, x24, AUDIO_FRAMES, any48 ? x48 : NULL);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t     *c  = &clients[i];
            const afmt_t *f  = &fmt[i];
            bool          hi = f->rate == 48000;
            size_t        n  = hi ? 2 * AUDIO_FRAMES : AUDIO_FRAMES;

            if (!on[i])
                continue;
            memcpy(c->acc + c->acc_n, hi ? x48 : x24, n * sizeof(float));
            c->acc_n += n;
            while (c->acc_n >= f->samples) {
                audio_hdr_t h = {.receiver = 0, .sample_rate = f->rate, .format = f->type,
                                 .length = f->samples * f->ch, .type = AT_RX, .channels = f->ch};
                size_t bytes = tci_encode(c->acc, f->samples, f->ch, f->type, pkt);
                client_frame_within(c, audio_budget(f), WS_OP_BINARY, &h, sizeof(h), pkt, bytes);
                c->acc_n -= f->samples;
                memmove(c->acc, c->acc + f->samples, c->acc_n * sizeof(float));
            }
        }
    }
    return NULL;
}

/* ---- The socket thread -------------------------------------------------------------- */

/* A connection over Tailscale Funnel (#58), its handshake done: the Panel's sound and microphone, one at a time,
 * and none while Funnel is paused after wrong keys. One that has not given its token yet makes room for it. A
 * funnel made by hand ("tailscale funnel 50001") put TCI, which has no login, on the internet: nothing comes of
 * it now without the token of a browser paired at home */
static bool funnel_take(client_t *c) {
    if (remote_server_funnel_paused())
        return false;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (&clients[i] != c && clients[i].fd >= 0 && clients[i].funnel && clients[i].up)
            return false;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (&clients[i] != c && clients[i].fd >= 0 && clients[i].ws && clients[i].funnel)
            client_close(&clients[i]);
    return true;
}

/* The first text over Funnel: "auth TOKEN" (or TCI's "auth:TOKEN;"), the token of a browser paired at home, as
 * the browser remote takes it. A good one: the greeting, then funnel_cmd_ok's commands. A wrong one counts towards
 * Funnel's pause; it, or anything else first, closes the connection */
static void funnel_auth(int ci, const char *text) {
    client_t *c = &clients[ci];
    char      tok[sizeof(c->token)] = "";
    char      name[32];
    size_t    l;

    if (!strncmp(text, "auth:", 5) || !strncmp(text, "auth ", 5)) {
        l = strcspn(text + 5, ";\r\n");
        if (l == sizeof(tok) - 1) {         /* a token's 32 characters; anything else is a wrong key */
            memcpy(tok, text + 5, l);
            tok[l] = 0;
        }
    } else {
        client_close(c);
        return;
    }
    if (!remote_server_session_check(tok, &c->role, &c->tx_ok, name, sizeof(name))) {
        remote_server_funnel_refused(c->ip);
        LV_LOG_USER("tci: a wrong key over Tailscale Funnel from %s", c->ip);
        client_close(c);
        return;
    }
    snprintf(c->token, sizeof(c->token), "%s", tok);
    c->check_ms = now_ms();
    pthread_mutex_lock(&c->lock);
    c->up = true;
    pthread_mutex_unlock(&c->lock);
    rstate_t s;
    state_read(&s);
    send_greeting(c, &s);
    LV_LOG_USER("tci: %s's sound over Tailscale Funnel from %s", name, c->ip);
}

static void client_read(int ci) {
    client_t *c = &clients[ci];
    ssize_t   n = recv(c->fd, c->in + c->in_len, IN_SIZE - c->in_len, MSG_DONTWAIT);

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        tx_session_end(ci, false);
        client_close(c);
        return;
    }
    if (n < 0)
        return;
    c->in_len += n;
    c->last_ms = now_ms();

    if (!c->ws) {
        char      resp[256];
        ws_peer_t peer;
        int       r = ws_handshake((const char *)c->in, c->in_len, resp, sizeof(resp), &peer);
        if (r == 0) {
            if (c->in_len == IN_SIZE)
                client_close(c);
            return;
        }
        if (r > 0 && peer.funnel && !funnel_take(c)) {
            LV_LOG_USER("tci: refused %s over Tailscale Funnel (%s)", peer.addr[0] ? peer.addr : "a client",
                        remote_server_funnel_paused() ? "paused after wrong keys" : "one at a time");
            client_close(c);
            return;
        }
        if (r < 0 || send(c->fd, resp, strlen(resp), MSG_NOSIGNAL) < 0) {
            client_close(c);
            return;
        }
        c->in_len  = 0;
        c->ws      = true;
        c->open_ms = now_ms();
        if (peer.funnel) {      /* no greeting before its token (funnel_auth, FUNNEL_AUTH_MS) */
            c->funnel = true;
            snprintf(c->ip, sizeof(c->ip), "%s", peer.addr[0] ? peer.addr : "-");
            return;
        }
        pthread_mutex_lock(&c->lock);
        c->up = true;
        pthread_mutex_unlock(&c->lock);
        rstate_t s;
        state_read(&s);
        send_greeting(c, &s);
        LV_LOG_USER("tci: client %s", c->ip);
        return;
    }

    size_t pos = 0;
    while (c->fd >= 0) {
        ws_frame_t f;
        int        used = ws_frame_parse(c->in + pos, c->in_len - pos, IN_SIZE - 16, &f);
        if (used == 0)
            break;
        if (used < 0) {
            tx_session_end(ci, false);
            client_close(c);
            return;
        }
        pos += used;
        switch (f.opcode) {
            case WS_OP_TEXT:
            case WS_OP_CONT:
                if (f.opcode == WS_OP_TEXT)
                    c->text_len = 0;
                if (c->text_len + f.len < TEXT_MAX) {
                    memcpy(c->text + c->text_len, f.payload, f.len);
                    c->text_len += f.len;
                } else {
                    c->text_len = 0;   /* too long for a command: dropped */
                    break;
                }
                if (f.fin) {
                    c->text[c->text_len] = 0;
                    c->text_len = 0;
                    if (c->up)
                        handle_text(ci, c->text);
                    else
                        funnel_auth(ci, c->text);   /* over Funnel, before its token */
                }
                break;
            case WS_OP_BINARY:
                if (!c->up)
                    client_close(c);                /* over Funnel, audio before its token */
                else if (f.fin)
                    handle_tx_audio(ci, f.payload, f.len);
                break;
            case WS_OP_PING:
                client_frame(c, WS_OP_PONG, f.payload, f.len, NULL, 0);
                break;
            case WS_OP_CLOSE:
                client_frame(c, WS_OP_CLOSE, f.payload, f.len > 2 ? 2 : f.len, NULL, 0);
                tx_session_end(ci, false);
                client_close(c);
                return;
            default:
                break;
        }
    }
    if (c->fd < 0)      /* a command that closed it: nothing left to keep */
        return;
    if (pos) {
        c->in_len -= pos;
        memmove(c->in, c->in + pos, c->in_len);
    }
    if (c->in_len == IN_SIZE)
        client_close(c);   /* a frame that never fits */
}

static int listen_open(int p) {
    int                fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a  = {.sin_family = AF_INET, .sin_port = htons((uint16_t)p), .sin_addr.s_addr = htonl(INADDR_ANY)};
    int                one = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
        LV_LOG_ERROR("tci: can't listen on port %d", p);
        close(fd);
        return -1;
    }
    LV_LOG_USER("tci: listening on port %d", p);
    return fd;
}

static void accept_client(int listen_fd) {
    struct sockaddr_in peer;
    socklen_t          pl = sizeof(peer);
    int                fd = accept4(listen_fd, (struct sockaddr *)&peer, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int                one = 1;

    if (fd < 0)
        return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        if (c->fd >= 0)
            continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_NOTSENT_LOWAT
        setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &(int){NOTSENT_LOWAT}, sizeof(int));
#endif
        pthread_mutex_lock(&c->lock);
        c->fd          = fd;
        c->ws = c->up = c->funnel = false;
        c->token[0]    = 0;
        c->in_len = c->text_len = 0;
        c->out_head = c->out_len = c->out_cut = c->urg_len = 0;
        c->rx_sensors = c->tx_sensors = c->audio_on = false;
        c->fmt_rate    = 48000;         /* TCI's defaults until it asks (audio_fmt) */
        c->fmt_type    = TCI_FLOAT32;
        c->fmt_ch = c->fmt_samples = 0;
        c->fmt_rate_asked = false;
        c->fmt_gen++;
        c->last_ms = c->ping_ms = now_ms();
        inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof(c->ip));
        pthread_mutex_unlock(&c->lock);
        atomic_fetch_add(&nclients, 1);
        return;
    }
    close(fd);
}

static void telemetry(uint64_t now, const rstate_t *s) {
    static uint64_t smeter_ms, txsens_ms;
    static uint8_t  seen;
    static float    alc;

    if (!s->tx && now - smeter_ms >= SMETER_MS) {
        smeter_ms = now;
        int db = meter_get_raw_db();
        broadcastf("rx_smeter:0,0,%d;", db);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && clients[i].up && clients[i].rx_sensors)
                client_textf(&clients[i], "rx_channel_sensors:0,0,%.1f;", (double)db);
    }
    if (s->tx && now - txsens_ms >= TXSENS_MS) {
        txsens_ms = now;
        float pwr, swr;
        tx_info_get(&pwr, &swr);
        tx_info_refresh(&seen, &alc, NULL, NULL);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && clients[i].up && clients[i].tx_sensors)
                client_textf(&clients[i], "tx_sensors:0,%.1f,%.1f,%.1f,%.2f,%.1f;", -20.0, (double)pwr, (double)pwr,
                             (double)swr, (double)alc);
    }
}

static void *server_thread(void *arg) {
    int      listen_fd = -1, listen_port = 0;
    rstate_t last;
    uint64_t state_ms = 0, conf_ms = 0;
    time_t   mtime    = conf_mtime();
    (void)arg;

    state_follow(&last, true);
    while (true) {
        bool en = atomic_load(&enabled);
        int  p  = atomic_load(&port);

        if ((!en || p != listen_port) && listen_fd >= 0) {
            close(listen_fd);
            listen_fd = -1;
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].fd >= 0) {
                    tx_session_end(i, false);
                    client_close(&clients[i]);
                }
        }
        if (en && listen_fd < 0) {
            listen_fd   = listen_open(p);
            listen_port = p;
        }

        struct pollfd pfd[2 + MAX_CLIENTS];
        int           slot[2 + MAX_CLIENTS];
        int           n = 0;
        pfd[n++] = (struct pollfd){.fd = wake_fd[0], .events = POLLIN};
        pfd[n++] = (struct pollfd){.fd = listen_fd, .events = listen_fd >= 0 ? POLLIN : 0};
        bool busy = false;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0)
                continue;
            pthread_mutex_lock(&c->lock);
            short ev = POLLIN | (c->out_len || c->urg_len ? POLLOUT : 0);
            pthread_mutex_unlock(&c->lock);
            slot[n]  = i;
            pfd[n++] = (struct pollfd){.fd = c->fd, .events = ev};
            busy = true;
        }
        /* The TCI audio session paces its client every 21 ms; a sleeping server wakes for a
         * client, the wake pipe or the clock */
        int timeout = txs.owner >= 0 ? 5 : busy ? STATE_MS : (listen_fd >= 0 ? 500 : 1000);
        if (poll(pfd, n, timeout) < 0 && errno != EINTR) {
            usleep(100000);
            continue;
        }
        if (pfd[0].revents & POLLIN) {
            char buf[64];
            while (read(wake_fd[0], buf, sizeof(buf)) > 0) {}
        }
        if (listen_fd >= 0 && (pfd[1].revents & POLLIN))
            accept_client(listen_fd);
        for (int k = 2; k < n; k++) {
            client_t *c = &clients[slot[k]];
            if (c->fd < 0)
                continue;
            if (pfd[k].revents & POLLOUT) {
                pthread_mutex_lock(&c->lock);
                client_flush(c);
                pthread_mutex_unlock(&c->lock);
            }
            if (c->fd >= 0 && (pfd[k].revents & POLLIN))
                client_read(slot[k]);
            else if (c->fd >= 0 && (pfd[k].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                tx_session_end(slot[k], false);
                client_close(c);
            }
        }

        uint64_t now = now_ms();
        tx_session_tick(now);
        if (now - state_ms >= STATE_MS) {
            state_ms = now;
            state_follow(&last, false);
            telemetry(now, &last);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &clients[i];
                if (c->fd < 0)
                    continue;
                if (now - c->last_ms > IDLE_MS) {
                    tx_session_end(i, false);
                    client_close(c);
                } else if (c->funnel && !c->up && now - c->open_ms > FUNNEL_AUTH_MS) {
                    LV_LOG_USER("tci: %s over Tailscale Funnel gave no key in time", c->ip);
                    client_close(c);
                } else if (c->funnel && c->up && now - c->check_ms > FUNNEL_CHECK_MS) {
                    /* the browser's session revoked at home (Sessions, Forget all): its sound goes too */
                    int  role;
                    bool tx;
                    c->check_ms = now;
                    if (!remote_server_session_check(c->token, &role, &tx, NULL, 0)) {
                        LV_LOG_USER("tci: %s over Tailscale Funnel: its session was revoked", c->ip);
                        tx_session_end(i, false);
                        client_close(c);
                    }
                } else if (c->up && now - c->last_ms > PING_MS && now - c->ping_ms > PING_MS) {
                    c->ping_ms = now;
                    client_frame(c, WS_OP_PING, NULL, 0, NULL, 0);
                }
            }
        }
        if (now - conf_ms >= CONF_MS) {
            conf_ms = now;
            time_t m = conf_mtime();
            if (m != mtime) {       /* the web configurator wrote it */
                mtime = m;
                conf_read();
            }
        }
    }
    return NULL;
}

/* ---- Public ------------------------------------------------------------------------- */

void tci_server_init(void) {
    pthread_t th;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        pthread_mutex_init(&clients[i].lock, NULL);
    }
    txs.owner = -1;
    conf_read();
    if (pipe2(wake_fd, O_NONBLOCK | O_CLOEXEC) < 0) {
        LV_LOG_ERROR("tci: no wake pipe");
        return;
    }
    pthread_create(&th, NULL, capture_thread, NULL);
    pthread_detach(th);
    pthread_create(&th, NULL, server_thread, NULL);
    pthread_detach(th);
}

bool tci_server_get_enabled(void) {
    return atomic_load(&enabled);
}

void tci_server_set_enabled(bool on) {
    atomic_store(&enabled, on);
    conf_write();
    wake();
}

int tci_server_port(void) {
    return atomic_load(&port);
}

bool tci_server_get_trust_operate(void) {
    return atomic_load(&trust_operate);
}

void tci_server_set_trust_operate(bool on) {
    atomic_store(&trust_operate, on);
    conf_write();
}

/* Those that are up: one over Funnel that has not given its token yet is no client */
int tci_server_clients(void) {
    int n = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
        n += clients[i].fd >= 0 && clients[i].up;
    return n;
}

const char *tci_server_client(void) {
    static char buf[64];

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        pthread_mutex_lock(&c->lock);
        bool up = c->fd >= 0 && c->up;
        if (up)
            snprintf(buf, sizeof(buf), "%s%s", c->ip, c->funnel ? " via Funnel" : "");
        pthread_mutex_unlock(&c->lock);
        if (up)
            return buf;
    }
    return NULL;
}
