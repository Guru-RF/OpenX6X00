/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Winlink: the radio's side of ARDOP and Pat (winlink.h)
 *
 *  The ARDOP bridge. ardopcf connects to WINLINK_ARDOP_SOCK (SOCK_SEQPACKET) and every packet is
 *  one message: a 4-byte header {type, arg, n} (host byte order), then n samples, mono, signed
 *  16 bits, 12000 Hz.
 *    H  ardopcf -> GUI  hello on each connect, arg = the protocol version (1)
 *    R  GUI -> ardopcf  the receiver, 240 samples (20 ms) a message; none while the radio
 *                       transmits, and dropped when the socket is full
 *    T  ardopcf -> GUI  audio to send, n <= 1200
 *    P  ardopcf -> GUI  arg 1: key now; arg 0: the end, every T before it is played out, then
 *                       the radio unkeys. A P 0 without a P 1 is ardopcf's insurance unkey
 *  One client at a time: a new connection replaces the old one. While one is there a thread reads
 *  the receiver from PulseAudio at 12 kHz (as the TCI server's capture does, the GUI's own capture
 *  and the decoders untouched) and a worker sends ardopcf's audio through tx_player, which keys
 *  the radio, caps the power and steers the drive as for the other digital modes. The client
 *  going ends everything: the stream stops, a transmission is cut and the radio unkeyed.
 *  Winlink is simplex: a key goes out only where the radio listens (radio_tx_freq() is the dial, no
 *  XIT, no RIT), on the frequency of the connection under way (Pat's last tune or the last key's,
 *  for REF_MS; then the dial again), and not while a QSY Pat was told was made could not be
 *  (missed_hz). Checked as it begins (tx_off_freq) and at each block of it (tx_abort). A frame
 *  ardopcf keys before the last one is out goes out in the same key, unless Pat asked a QSY
 *  meanwhile: then it is a key of its own, after the QSY.
 *
 *  The rigctld server, for Pat's hamlib rig on 127.0.0.1:WINLINK_RIG_PORT: one command a line
 *  (CR LF or LF), at most one line back. \get_freq (f), \set_freq (F: tunes as TCI does and sets
 *  USB-D unless the mode is USB or USB-D), \get_ptt (t), \set_ptt (T: answered only, ardopcf keys
 *  through the socket; Pat runs with ptt_ctrl false), \chk_vfo, \quit (q); anything else RPRT -11.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* accept4 */

#include "winlink.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "msg.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "screen_main.h"
#include "tx_player.h"

#include "lvgl/lvgl.h"

#include <liquid/liquid.h>

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ARDOP_VERSION   1
#define ARDOP_RATE      12000
#define RX_BLOCK        240             /* 20 ms, ardopcf's own block */
#define T_MAX           1200            /* samples in a T message at most */
#define RING_SIZE       (4 * ARDOP_RATE)    /* TX audio waiting: ardopcf sends 100 ms ahead of the clock */
#define IN_BLOCK        120             /* 10 ms of ardopcf's audio a block: 441 samples to play */
#define OUT_MAX         1024            /* the resampler's output for IN_BLOCK, with room (1 + 2 r n) */
#define WAIT_MS         10              /* the worker looks at the ring this often while it waits */
#define SILENCE_MS      500             /* keyed without audio this long: unkeyed */
#define KEY_LIMIT_MS    (30 * 1000)     /* one key at most */
#define SWR_HOLD_MS     (5 * 60 * 1000) /* no keying this long after an SWR trip in a Winlink TX */
#define TURN_MS         500             /* after our own TX the base may still report it this long */

#define RIG_CLIENTS     4
#define RIG_LINE        256
#define TUNE_WAIT_MS    700             /* \set_freq waits this long for the LVGL thread (Pat reads for 1 s) */
#define PENDING_MS      (30 * 1000)     /* a QSY asked on the air is done at the end of it, if within this */
#define REF_MS          (60 * 1000)     /* Pat's tune or ardopcf's key this recent: a connection under way */

typedef struct {
    uint8_t  type;
    uint8_t  arg;
    uint16_t n;         /* samples after the header */
} hdr_t;

_Static_assert(sizeof(hdr_t) == 4, "the socket header is 4 bytes");

/* Where ardopcf's key stands, between the socket thread and the worker */
typedef enum {
    KEY_NONE,           /* receiving */
    KEY_WANTED,         /* a P 1 for the worker: its audio collects in the ring */
    KEY_ON,             /* on the air through tx_player */
    KEY_DISCARD,        /* refused, or ended before its P 0: the audio is thrown away until then */
} key_state_t;

/* The ardopcf connection: one at a time, so one of these */
static struct {
    int             fd;             /* -1: none */
    atomic_bool     quit;           /* the connection ends: its threads stop */
    pthread_t       rx_th, tx_th;
    bool            rx_up, tx_up;

    pthread_mutex_t lock;           /* the key and the ring */
    pthread_cond_t  cond;           /* CLOCK_MONOTONIC */
    key_state_t     key;
    bool            eos;            /* the P 0 of this key came */
    bool            fill_done;      /* the last block of this key is given: later audio is not its */
    uint64_t        audio_ms;       /* the last T of this key, or the key itself */
    int16_t         ring[RING_SIZE];
    size_t          head, len;      /* head: the oldest sample */
    bool            cut_on;         /* a frame keyed with Pat's QSY pending: this key ends after `cut` more */
    size_t          cut;            /* samples, the frame before it (in the ring) */

    /* the worker's alone */
    msresamp_rrrf   rs;             /* 12000 -> 44100, its state kept from block to block */
    size_t          tail_len;       /* zeros that bring out what the filter still holds */
    float           out[OUT_MAX];
    int16_t         pend[OUT_MAX];  /* resampled, not yet given to tx_player */
    size_t          pend_pos, pend_len;
    uint64_t        key_ms;
    int32_t         key_hz;         /* where this key goes out: the frequency the radio listens on */
} ses = {.fd = -1};

/* The worker's, kept from one connection to the next */
static uint64_t unkey_ms;           /* our last transmission ended */
static uint64_t swr_trip_ms;        /* the SWR protection stopped one, 0 never */
static unsigned key_trips;          /* radio_swr_trips() when our last key began */
static atomic_bool keying;          /* the worker transmits: on_air() may not show it yet, or no more */

static void rig_pending_tune(void);
static atomic_int    pending_hz;    /* a \set_freq asked on the air, done once it is over; 0 none */
static atomic_int    missed_hz;     /* a QSY Pat was told was done and that could not be (rig_pending_tune); 0 none */
/* The frequency of the connection under way: Pat's last tune, or where ardopcf last keyed; 0 none. Written by
 * the rig thread under tune_lock, and by the worker in tx_off_freq after its own rig_pending_tune: a \set_freq
 * from then on is kept for later (keying), so never both at once */
static atomic_int    ref_hz;
static atomic_ullong ref_ms;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

/* The radio on the air, whoever keyed it */
static bool on_air(void) {
    return radio_get_modem() || radio_get_state() != RADIO_RX;
}

/* ---- The TX audio ring, under ses.lock ---------------------------------------------- */

static void ring_clear(void) {
    ses.head = ses.len = 0;
}

static void ring_push(const int16_t *s, size_t n) {
    static uint64_t said;

    if (n > RING_SIZE - ses.len) {
        if (now_ms() - said > 5000) {
            said = now_ms();
            LV_LOG_WARN("winlink: TX audio overflow, %zu samples dropped", n - (RING_SIZE - ses.len));
        }
        n = RING_SIZE - ses.len;
    }
    for (size_t i = 0, at = (ses.head + ses.len) % RING_SIZE; i < n; i++, at = (at + 1) % RING_SIZE)
        ses.ring[at] = s[i];
    ses.len += n;
}

/* Up to n samples out of the ring, as floats */
static size_t ring_take(float *out, size_t n) {
    if (n > ses.len)
        n = ses.len;
    for (size_t i = 0; i < n; i++) {
        out[i] = ses.ring[ses.head] / 32768.0f;
        ses.head = (ses.head + 1) % RING_SIZE;
    }
    ses.len -= n;
    return n;
}

static void wait_ms(int ms) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&ses.cond, &ses.lock, &ts);
}

/* ---- ardopcf's messages, on the socket thread --------------------------------------- */

/* Under ses.lock: a new key, its audio from now on */
static void key_new(void) {
    ses.key      = KEY_WANTED;
    ses.eos      = false;
    ses.audio_ms = now_ms();
    ses.cut_on   = false;
    ring_clear();
}

/* P 1, under ses.lock */
static void key_on(void) {
    switch (ses.key) {
        case KEY_NONE:
        case KEY_DISCARD:   /* a P 0 lost on the way: a new key all the same */
            key_new();
            break;
        case KEY_WANTED:
            break;
        case KEY_ON:
            /* The next frame before this one is out: one transmission, the radio stays keyed.
             * Once the last block was given, it is a key of its own after this one. So it is when Pat
             * asked a QSY during this one: the next frame is for the new frequency, and the QSY is made
             * before a key (tx_off_freq), so this key ends where this frame does (next_block) */
            if (ses.fill_done) {
                key_new();
            } else {
                if (ses.eos && atomic_load(&pending_hz) && !ses.cut_on) {
                    ses.cut_on = true;
                    ses.cut    = ses.len;
                }
                ses.eos      = false;
                ses.audio_ms = now_ms();
            }
            break;
    }
}

/* P 0, under ses.lock */
static void key_off(void) {
    switch (ses.key) {
        case KEY_NONE:      /* ardopcf's insurance unkey */
            break;
        case KEY_DISCARD:
            ses.key = KEY_NONE;
            break;
        case KEY_WANTED:
        case KEY_ON:
            ses.eos = true;
            break;
    }
}

/* One message. False: a client we do not speak with */
static bool ardop_msg(const hdr_t *h, const int16_t *samples, ssize_t len) {
    if (len < (ssize_t)sizeof(*h) || len != (ssize_t)(sizeof(*h) + h->n * sizeof(int16_t)))
        return true;    /* cut or padded: not ours to guess */

    switch (h->type) {
        case 'H':
            if (h->arg != ARDOP_VERSION) {
                LV_LOG_ERROR("winlink: ardopcf speaks protocol %u, the GUI %u", h->arg, ARDOP_VERSION);
                return false;
            }
            LV_LOG_USER("winlink: ardopcf connected");
            break;
        case 'T':
            if (h->n > T_MAX)
                break;
            pthread_mutex_lock(&ses.lock);
            if (ses.key == KEY_WANTED || (ses.key == KEY_ON && !ses.fill_done)) {
                ring_push(samples, h->n);
                ses.audio_ms = now_ms();
                pthread_cond_broadcast(&ses.cond);
            }
            pthread_mutex_unlock(&ses.lock);
            break;
        case 'P':
            pthread_mutex_lock(&ses.lock);
            if (h->arg)
                key_on();
            else
                key_off();
            pthread_cond_broadcast(&ses.cond);
            pthread_mutex_unlock(&ses.lock);
            break;
        default:
            break;
    }
    return true;
}

/* ---- The receiver to ardopcf -------------------------------------------------------- */

/* While ardopcf is there: its own record stream at 12 kHz in 20 ms fragments, PulseAudio
 * converts the codec's 44.1 kHz. Never blocks on the socket: a full one drops the message */
static void *rx_thread(void *arg) {
    int        fd = *(int *)arg;
    pa_simple *pa = NULL;
    bool       said = false;
    struct {
        hdr_t   h;
        int16_t pcm[RX_BLOCK];
    } m = {.h = {.type = 'R', .arg = 0, .n = RX_BLOCK}};

    while (!atomic_load(&ses.quit)) {
        if (!pa) {
            pa_sample_spec spec = {.format = PA_SAMPLE_S16NE, .rate = ARDOP_RATE, .channels = 1};
            /* 0.5 s kept at most: a stall drops audio, it never grows the delay */
            pa_buffer_attr attr = {.maxlength = ARDOP_RATE, .tlength = (uint32_t)-1, .prebuf = (uint32_t)-1,
                                   .minreq = (uint32_t)-1, .fragsize = sizeof(m.pcm)};
            int            err  = 0;

            pa = pa_simple_new(NULL, "OpenX6X00 ARDOP", PA_STREAM_RECORD, BOARD_PA_SOURCE, "ARDOP RX", &spec, NULL,
                               &attr, &err);
            if (!pa) {
                if (!said)
                    LV_LOG_ERROR("winlink: no capture stream: %s", pa_strerror(err));
                said = true;
                for (int i = 0; i < 20 && !atomic_load(&ses.quit); i++)
                    usleep(50000);
                continue;
            }
        }
        if (pa_simple_read(pa, m.pcm, sizeof(m.pcm), NULL) < 0) {
            pa_simple_free(pa);
            pa = NULL;
            usleep(100000);     /* a stream that fails at once does not spin */
            continue;
        }
        if (on_air())
            continue;   /* the capture is not the receiver now; ardopcf does not listen either */
        /* full: dropped, ardopcf catches up; gone: the socket thread ends the connection */
        send(fd, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    if (pa)
        pa_simple_free(pa);
    return NULL;
}

/* ---- ardopcf's audio on the air ----------------------------------------------------- */

/* The next block into pend; false at the end of the key: its P 0 and every sample played, no
 * audio for SILENCE_MS, or the client gone */
static bool next_block(void) {
    float    in[IN_BLOCK];
    size_t   k    = 0;
    bool     end  = false, tail = false;
    unsigned ny   = 0;

    pthread_mutex_lock(&ses.lock);
    for (;;) {
        if (ses.fill_done || atomic_load(&ses.quit)) {
            end = true;
            break;
        }
        if (ses.cut_on) {
            /* The frame before Pat's QSY to its end, then this key's; the next one waits in the ring for
             * the worker, which keys it once the QSY is made */
            if (ses.cut) {
                k = ring_take(in, LV_MIN((size_t)IN_BLOCK, ses.cut));
                ses.cut -= k;
            } else {
                tail       = true;
                ses.cut_on = false;
                ses.key    = KEY_WANTED;
            }
            break;
        }
        if (ses.len >= IN_BLOCK || (ses.eos && ses.len)) {
            k = ring_take(in, IN_BLOCK);
            break;
        }
        if (ses.eos) {
            tail = true;    /* all of it in: what the filter still holds, then the end */
            break;
        }
        if (now_ms() - ses.audio_ms > SILENCE_MS) {
            LV_LOG_WARN("winlink: no audio from ardopcf for %d ms, unkeyed", SILENCE_MS);
            end = true;
            break;
        }
        wait_ms(WAIT_MS);
    }
    if (end || tail)
        ses.fill_done = true;
    pthread_mutex_unlock(&ses.lock);

    if (end) {
        if (atomic_load(&ses.quit))
            audio_play_flush();     /* ardopcf gone: off the air now, not after the queue */
        return false;
    }
    if (tail) {
        k = ses.tail_len;
        memset(in, 0, k * sizeof(float));
    }
    msresamp_rrrf_execute(ses.rs, in, (unsigned)k, ses.out, &ny);
    for (unsigned i = 0; i < ny; i++)
        ses.pend[i] = (int16_t)lrintf(LV_CLAMP(-32767.0f, ses.out[i] * 32768.0f, 32767.0f));
    ses.pend_pos = 0;
    ses.pend_len = ny;
    return ny > 0 || !tail;
}

/* tx_player's fill: at most a block (10 ms) a call, the low-latency stream takes no more at once */
static size_t tx_fill(int16_t *buf, size_t n, void *ctx) {
    (void)ctx;
    while (ses.pend_pos == ses.pend_len) {
        if (!next_block())
            return 0;
    }
    size_t k = LV_MIN(n, ses.pend_len - ses.pend_pos);

    memcpy(buf, ses.pend + ses.pend_pos, k * sizeof(int16_t));
    ses.pend_pos += k;
    return k;
}

/* 14105000 as "14.105.000" */
static const char *mhz(char *s, size_t size, int32_t hz) {
    snprintf(s, size, "%d.%03d.%03d", (int)(hz / 1000000), (int)(hz / 1000 % 1000), (int)(hz % 1000));
    return s;
}

/* tx_player's abort, once a block, before it is played: the client gone, the key too long, or its frequency
 * left. Split turned on, a shift, XIT, the other VFO, the dial turned: the base sends the rest of it elsewhere */
static bool tx_abort(void *ctx) {
    (void)ctx;
    bool    quit  = atomic_load(&ses.quit);
    bool    limit = now_ms() - ses.key_ms > KEY_LIMIT_MS;
    int32_t tx    = radio_tx_freq() + param_i_get(cfg_xit);
    bool    moved = tx != ses.key_hz || cparam_i_get(cfg_fg_freq) != ses.key_hz;

    if (!quit && !limit && !moved)
        return false;
    if (limit) {
        LV_LOG_WARN("winlink: keyed for %d s, unkeyed", KEY_LIMIT_MS / 1000);
        msg_schedule_text_fmt("Winlink: TX stopped after %d s", KEY_LIMIT_MS / 1000);
    } else if (moved && !quit) {
        char f[16];

        LV_LOG_WARN("winlink: the TX frequency left %d Hz, unkeyed", (int)ses.key_hz);
        msg_schedule_text_fmt("Winlink: TX stopped, it left %s MHz", mhz(f, sizeof(f), ses.key_hz));
    }
    pthread_mutex_lock(&ses.lock);
    ses.fill_done = true;
    pthread_mutex_unlock(&ses.lock);
    audio_play_flush();     /* this thread's stream: what tx_player plays into */
    return true;
}

/* Why ardopcf's key is not taken, the radio's state; NULL: it is, if tx_off_freq agrees */
static const char *tx_refused(void) {
    /* Our own transmission just ended: the base reports RX on its next packet */
    while (!radio_get_modem() && radio_get_state() != RADIO_RX && now_ms() - unkey_ms < TURN_MS &&
           !atomic_load(&ses.quit))
        usleep(10000);
    if (on_air())
        return "the radio is on the air";
    /* A trip the guard reported after our last key ended (its callback runs on the LVGL thread,
     * maybe after tx_player returned) counts for that key */
    if (unkey_ms && radio_swr_trips() != key_trips) {
        key_trips   = radio_swr_trips();    /* counted once */
        swr_trip_ms = unkey_ms;
        LV_LOG_WARN("winlink: SWR trip, no TX for %d minutes", SWR_HOLD_MS / 60000);
    }
    if (swr_trip_ms && now_ms() - swr_trip_ms < SWR_HOLD_MS)
        return "an SWR trip less than 5 minutes ago";
    return NULL;
}

/* Why ardopcf's key would not go out where the radio listens, on the connection's frequency; NULL: it would, on
 * ses.key_hz. Winlink is simplex, the gateway answers on the frequency it is called on. The key goes out on
 * radio_tx_freq() (tx_player), in split the other VFO or the shifted one, and the X6100's base adds the XIT
 * (lo = fg + xit); with RIT the radio, and ARDOP, listen beside the dial. Only a TX shift was refused: plain split
 * with B elsewhere called the gateway from B (audit C07). Checked here, on what the radio is set to as it keys,
 * after the QSY Pat asked during the last key: it can bring another band's split.
 * The connection's frequency: Pat's last tune (made, or not made: missed_hz), or where ardopcf keyed, while either
 * is less than REF_MS old; the dial once it is older, so a connection made without a QSY keys where the radio
 * listens. A dial turned, or the other VFO chosen, during a connection keys no more there */
static const char *tx_off_freq(void) {
    static char why[96];    /* the worker's */
    char        a[16], b[16];
    int32_t     rx, tx, xit, rit, missed, ref;
    uint64_t    now;

    rig_pending_tune();     /* a QSY Pat asked during the last key: this one goes out there */
    rx     = cparam_i_get(cfg_fg_freq);
    tx     = radio_tx_freq();
    xit    = param_i_get(cfg_xit);
    rit    = param_i_get(cfg_rit);
    missed = atomic_load(&missed_hz);
    if (missed) {
        if (missed != rx) {
            snprintf(why, sizeof(why), "the radio could not go to Pat's %s MHz", mhz(a, sizeof(a), missed));
            return why;
        }
        atomic_compare_exchange_strong(&missed_hz, &missed, 0);    /* there after all */
    }
    now = now_ms();
    ref = atomic_load(&ref_hz);
    if (!ref || now - atomic_load(&ref_ms) >= REF_MS)
        ref = rx;
    atomic_store(&ref_hz, ref);
    atomic_store(&ref_ms, now);     /* ardopcf keys, keyed or not: its connection goes on */
    if (rx != ref) {
        snprintf(why, sizeof(why), "the radio left %s MHz", mhz(a, sizeof(a), ref));
        return why;
    }
    if (xit) {
        snprintf(why, sizeof(why), "XIT is set (%+d Hz)", (int)xit);
        return why;
    }
    if (rit) {
        snprintf(why, sizeof(why), "RIT is set (%+d Hz)", (int)rit);
        return why;
    }
    if (tx != rx) {
        if (param_i_get(cfg_band_tx_shift))
            return "a TX shift is set on this band";
        snprintf(why, sizeof(why), "split would send on %s, not %s", mhz(a, sizeof(a), tx), mhz(b, sizeof(b), rx));
        return why;
    }
    ses.key_hz = rx;
    return NULL;
}

/* One key on the air, the whole of it, on the worker */
static void transmit(void) {
    /* Opened on this thread, the one tx_player plays from: 100 ms queued instead of play_stm's 800 */
    bool     low   = audio_play_lowlat_begin();
    unsigned trips = radio_swr_trips();

    key_trips = trips;

    if (!low)
        LV_LOG_WARN("winlink: no low-latency stream, playing through the usual one");
    msresamp_rrrf_reset(ses.rs);
    ses.pend_pos = ses.pend_len = 0;
    ses.key_ms   = now_ms();

    tx_player_stream(tx_fill, NULL, TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), tx_abort, NULL);
    atomic_store(&keying, false);

    if (low)
        audio_play_lowlat_end();
    unkey_ms = now_ms();
    if (radio_swr_trips() != trips) {
        key_trips   = radio_swr_trips();
        swr_trip_ms = unkey_ms;
        LV_LOG_WARN("winlink: SWR trip, no TX for %d minutes", SWR_HOLD_MS / 60000);
    }
}

/* The worker: one per connection, it waits for ardopcf's keys */
static void *tx_thread(void *arg) {
    static uint64_t said;
    (void)arg;

    pthread_mutex_lock(&ses.lock);
    while (!atomic_load(&ses.quit)) {
        if (ses.key != KEY_WANTED) {
            pthread_cond_wait(&ses.cond, &ses.lock);
            continue;
        }
        pthread_mutex_unlock(&ses.lock);
        const char *no = tx_refused();

        if (!no) {
            atomic_store(&keying, true);    /* a QSY Pat asks from now on waits for the end of this key */
            if ((no = tx_off_freq()))
                atomic_store(&keying, false);
        }
        pthread_mutex_lock(&ses.lock);

        if (no) {
            /* ardopcf believes it sends: its audio is thrown away until its P 0 */
            ses.key = ses.eos ? KEY_NONE : KEY_DISCARD;
            ring_clear();
            pthread_mutex_unlock(&ses.lock);
            LV_LOG_WARN("winlink: not keyed, %s", no);
            if (now_ms() - said > 3000) {   /* once, not for every retry */
                said = now_ms();
                msg_schedule_text_fmt("Winlink: no TX, %s", no);
            }
            pthread_mutex_lock(&ses.lock);
            continue;
        }
        ses.key       = KEY_ON;
        ses.fill_done = false;
        pthread_mutex_unlock(&ses.lock);

        transmit();

        pthread_mutex_lock(&ses.lock);
        if (ses.key == KEY_ON) {    /* else ardopcf keyed again meanwhile: that key is next */
            ses.key    = ses.eos ? KEY_NONE : KEY_DISCARD;
            ses.cut_on = false;
            ring_clear();
        }
    }
    pthread_mutex_unlock(&ses.lock);
    return NULL;
}

/* ---- The connection, on the socket thread ------------------------------------------- */

static void session_start(int fd) {
    pthread_mutex_lock(&ses.lock);
    ses.fd        = fd;
    ses.key       = KEY_NONE;
    ses.eos       = false;
    ses.fill_done = false;
    ses.cut_on    = false;
    ring_clear();
    pthread_mutex_unlock(&ses.lock);
    atomic_store(&ses.quit, false);
    ses.rx_up = pthread_create(&ses.rx_th, NULL, rx_thread, &ses.fd) == 0;
    ses.tx_up = pthread_create(&ses.tx_th, NULL, tx_thread, NULL) == 0;
    if (!ses.rx_up || !ses.tx_up)
        LV_LOG_ERROR("winlink: no thread for ardopcf");
}

/* The client gone or replaced: a transmission cut and unkeyed, the capture stopped */
static void session_end(void) {
    if (ses.fd < 0)
        return;
    atomic_store(&ses.quit, true);
    pthread_mutex_lock(&ses.lock);
    pthread_cond_broadcast(&ses.cond);
    pthread_mutex_unlock(&ses.lock);
    if (ses.tx_up)
        pthread_join(ses.tx_th, NULL);
    if (ses.rx_up)
        pthread_join(ses.rx_th, NULL);
    ses.rx_up = ses.tx_up = false;
    close(ses.fd);
    ses.fd = -1;
}

static int ardop_listen(void) {
    static bool        said;
    struct sockaddr_un a   = {.sun_family = AF_UNIX};
    size_t             len = strlen(WINLINK_ARDOP_SOCK);
    int                fd;

    if (len >= sizeof(a.sun_path)) {    /* a PC build deep in the tree: cut, it would be another file */
        if (!said)
            LV_LOG_ERROR("winlink: %s is too long for a socket", WINLINK_ARDOP_SOCK);
        said = true;
        return -1;
    }
    if ((fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) < 0)
        return -1;
    memcpy(a.sun_path, WINLINK_ARDOP_SOCK, len + 1);
    unlink(a.sun_path);     /* the last run's */
    /* 0600 before listen(): nobody connects to it before */
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || chmod(a.sun_path, 0600) < 0 || listen(fd, 2) < 0) {
        if (!said)
            LV_LOG_ERROR("winlink: can't listen on %s: %s", a.sun_path, strerror(errno));
        said = true;
        close(fd);
        return -1;
    }
    LV_LOG_USER("winlink: ARDOP audio on %s", a.sun_path);
    return fd;
}

static void *ardop_thread(void *arg) {
    int lfd = -1;
    union {
        hdr_t   h;
        int16_t s[2 + T_MAX];   /* the header is two samples long */
    } m;
    (void)arg;

    for (;;) {
        if (lfd < 0 && (lfd = ardop_listen()) < 0) {
            sleep(5);
            continue;
        }
        struct pollfd pfd[2] = {{.fd = lfd, .events = POLLIN}, {.fd = ses.fd, .events = POLLIN}};
        nfds_t        n      = ses.fd >= 0 ? 2 : 1;

        if (poll(pfd, n, 1000) < 0) {
            if (errno != EINTR)
                usleep(100000);
            continue;
        }
        if (pfd[0].revents & POLLIN) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

            if (fd >= 0) {
                if (ses.fd >= 0)
                    LV_LOG_USER("winlink: a new ardopcf connection replaces the old one");
                session_end();
                session_start(fd);
            }
            continue;
        }
        if (n < 2 || !pfd[1].revents)
            continue;
        /* What has come, 64 messages at most: a flood keeps no new client waiting */
        for (int i = 0; i < 64 && ses.fd >= 0; i++) {
            ssize_t len = recv(ses.fd, &m, sizeof(m), MSG_DONTWAIT);

            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                break;
            if (len <= 0) {
                LV_LOG_USER("winlink: ardopcf gone");
                session_end();
                break;
            }
            if (!ardop_msg(&m.h, m.s + 2, len)) {
                session_end();
                break;
            }
        }
    }
    return NULL;
}

/* ---- rigctld for Pat ---------------------------------------------------------------- */

typedef enum {
    RIG_NONE,           /* an empty line: no answer */
    RIG_GET_FREQ,
    RIG_SET_FREQ,
    RIG_GET_PTT,
    RIG_SET_PTT,
    RIG_CHK_VFO,
    RIG_QUIT,
    RIG_BAD_ARG,        /* a command we know without the number it needs */
    RIG_UNKNOWN,
} rig_cmd_t;

/* One line, its CR LF cut: the command, with its number in *arg */
static rig_cmd_t rig_parse(const char *line, double *arg) {
    static const struct {
        const char *brief, *full;
        rig_cmd_t  cmd;
        bool       number;
    } cmds[] = {
        {"f", "\\get_freq", RIG_GET_FREQ, false},
        {"F", "\\set_freq", RIG_SET_FREQ, true},
        {"t", "\\get_ptt", RIG_GET_PTT, false},
        {"T", "\\set_ptt", RIG_SET_PTT, true},
        {NULL, "\\chk_vfo", RIG_CHK_VFO, false},
        {"q", "\\quit", RIG_QUIT, false},
        {"Q", NULL, RIG_QUIT, false},
    };
    char   word[32];
    size_t n;

    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line)
        return RIG_NONE;
    n = strcspn(line, " \t");
    if (n >= sizeof(word))
        return RIG_UNKNOWN;
    memcpy(word, line, n);
    word[n] = 0;
    line += n;

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (!(cmds[i].brief && !strcmp(word, cmds[i].brief)) && !(cmds[i].full && !strcmp(word, cmds[i].full)))
            continue;
        if (cmds[i].number) {
            char *end;

            *arg = strtod(line, &end);
            while (*end == ' ' || *end == '\t')
                end++;
            if (end == line || *end || !isfinite(*arg))
                return RIG_BAD_ARG;
        }
        return cmds[i].cmd;
    }
    return RIG_UNKNOWN;
}

/* One tune at a time, the rig thread's or the worker's, each waited for by its own number. With a count of the
 * tunes made shared by all, one given up on after TUNE_WAIT_MS was still made when the LVGL thread came to it,
 * after Pat had been told it timed out, and its count woke the next one's wait, whose answer was then decided on
 * the frequency the late one had left */
typedef struct {
    int32_t  hz;
    unsigned id;
} tune_t;

static pthread_mutex_t tune_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint     tune_queued;     /* the tune in the LVGL queue that is still wanted; 0 none */
static atomic_uint     tune_done;       /* the last one tune_cb made */

/* LVGL thread: Pat's QSY as TCI tunes, then USB-D unless the mode is USB or USB-D already. Not one rig_do_tune
 * took back */
static void tune_cb(void *arg) {
    const tune_t *t  = arg;
    unsigned      id = t->id;
    int32_t       f  = t->hz;

    if (!atomic_compare_exchange_strong(&tune_queued, &id, 0))
        return;
    if (!on_air() && !main_screen_freq_locked() && cfg_is_valid_hw_freq(f)) {
        int32_t mode;

        backlight_tick();   /* tuned from a program: the screen wakes as for a key */
        if (f != cparam_i_get(cfg_fg_freq))
            main_screen_set_freq((uint64_t)f);
        mode = cparam_i_get(cfg_cur_mode);
        if (mode != radio_mode_usb && mode != radio_mode_usb_dig && !main_screen_mode_locked())
            cparam_i_set(cfg_cur_mode, radio_mode_usb_dig);
    }
    atomic_store(&tune_done, t->id);
}

static atomic_ullong pending_ms;    /* when pending_hz was asked */

/* The tune on the LVGL thread, waited for: hamlib's code, 0 when the radio is there. Under tune_lock */
static int rig_do_tune(int32_t f) {
    static unsigned last;
    tune_t          t = {.hz = f};

    if (!++last)
        last = 1;       /* 0 is none */
    t.id = last;
    atomic_store(&tune_queued, t.id);
    if (!scheduler_put(tune_cb, &t, sizeof(t))) {
        atomic_store(&tune_queued, 0);
        return -9;      /* RIG_ERJCTED */
    }
    for (int ms = 0; atomic_load(&tune_done) != t.id; ms += 5) {
        unsigned id = t.id;

        /* Not begun: taken back, the LVGL thread passes it by. Begun: its end is a moment away */
        if (ms >= TUNE_WAIT_MS && atomic_compare_exchange_strong(&tune_queued, &id, 0))
            return -5;  /* RIG_ETIMEOUT: the LVGL thread busy */
        usleep(5000);
    }
    return cparam_i_get(cfg_fg_freq) == f ? 0 : -9;
}

/* A QSY kept while on the air, now that it is over (the rig thread) or before the next key (the
 * worker): whichever comes first takes it. Pat was told it was done: one that cannot be (an app holds
 * the frequency, the LVGL thread busy, too late) leaves the radio where Pat does not think it is, and
 * ardopcf does not key until it is there or Pat tunes again (missed_hz) */
static void rig_pending_tune(void) {
    int32_t f;

    /* The worker also waits out a QSY of the rig thread's under way: its key is checked where that leaves the radio */
    pthread_mutex_lock(&tune_lock);
    f = atomic_load(&pending_hz);
    if (f && !(on_air() && !atomic_load(&keying))) {
        atomic_store(&pending_hz, 0);
        if (now_ms() - atomic_load(&pending_ms) >= PENDING_MS || rig_do_tune(f) != 0)
            atomic_store(&missed_hz, f);
        atomic_store(&ref_hz, f);   /* where Pat waits, made or not */
        atomic_store(&ref_ms, now_ms());
    }
    pthread_mutex_unlock(&tune_lock);
}

/* \set_freq: hamlib's code. On the air it is answered at once and done at the end of the
 * transmission: tx_player puts its own dial back as it unkeys, and Pat tunes back a second after
 * its session, while ardopcf may still send its ID. Not on a frequency an app holds */
static int rig_tune(double hz) {
    int32_t f;
    int     ret = 0;

    if (!(hz >= 1.0 && hz < 2147483647.0))
        return -1;      /* RIG_EINVAL */
    f = (int32_t)llround(hz);
    if (!cfg_is_valid_hw_freq(f))
        return -1;
    pthread_mutex_lock(&tune_lock);     /* kept or made with no tune of the worker's in between */
    atomic_store(&missed_hz, 0);    /* Pat's QSY before this one is not where Pat waits any more */
    if (on_air() || atomic_load(&keying)) {
        atomic_store(&pending_ms, now_ms());
        atomic_store(&pending_hz, f);
    } else {
        atomic_store(&pending_hz, 0);   /* asked again since: the newest wins */
        ret = rig_do_tune(f);
        atomic_store(&ref_hz, ret == 0 ? f : 0);    /* not made: Pat knows, and waits nowhere */
        atomic_store(&ref_ms, now_ms());
    }
    pthread_mutex_unlock(&tune_lock);
    return ret;
}

/* The answer to one line into reply, "" for none. False: the client says goodbye */
static bool rig_answer(const char *line, char *reply, size_t size) {
    double arg = 0;

    reply[0] = 0;
    switch (rig_parse(line, &arg)) {
        case RIG_NONE:
            break;
        case RIG_GET_FREQ:
            snprintf(reply, size, "%d\n", (int)cparam_i_get(cfg_fg_freq));
            break;
        case RIG_SET_FREQ:
            snprintf(reply, size, "RPRT %d\n", rig_tune(arg));
            break;
        case RIG_GET_PTT:
            snprintf(reply, size, "%d\n", on_air() ? 1 : 0);
            break;
        case RIG_SET_PTT:
            snprintf(reply, size, "RPRT 0\n");      /* ardopcf keys through the socket */
            break;
        case RIG_CHK_VFO:
            snprintf(reply, size, "0\n");
            break;
        case RIG_QUIT:
            return false;
        case RIG_BAD_ARG:
            snprintf(reply, size, "RPRT -1\n");     /* RIG_EINVAL */
            break;
        case RIG_UNKNOWN:
            snprintf(reply, size, "RPRT -11\n");    /* RIG_ENAVAIL */
            break;
    }
    return true;
}

typedef struct {
    int    fd;
    char   in[RIG_LINE];
    size_t len;
} rig_client_t;

static rig_client_t rig_clients[RIG_CLIENTS];

static void rig_close(rig_client_t *c) {
    close(c->fd);
    c->fd  = -1;
    c->len = 0;
}

static void rig_read(rig_client_t *c) {
    ssize_t n = recv(c->fd, c->in + c->len, sizeof(c->in) - 1 - c->len, MSG_DONTWAIT);
    char   *p, *nl;

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        rig_close(c);
        return;
    }
    if (n < 0)
        return;
    c->len += n;
    p = c->in;
    while ((nl = memchr(p, '\n', c->len - (size_t)(p - c->in)))) {
        char reply[64];
        bool stay;

        *nl = 0;
        if (nl > p && nl[-1] == '\r')
            nl[-1] = 0;
        stay = rig_answer(p, reply, sizeof(reply));
        if (reply[0])
            send(c->fd, reply, strlen(reply), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (!stay) {
            rig_close(c);
            return;
        }
        p = nl + 1;
    }
    c->len -= (size_t)(p - c->in);
    memmove(c->in, p, c->len);
    if (c->len == sizeof(c->in) - 1)
        rig_close(c);   /* a line that never ends */
}

static int rig_listen(void) {
    static bool        said;
    struct sockaddr_in a   = {.sin_family = AF_INET, .sin_port = htons(WINLINK_RIG_PORT),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int                fd  = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int                one = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
        if (!said)
            LV_LOG_ERROR("winlink: can't listen on 127.0.0.1:%d: %s", WINLINK_RIG_PORT, strerror(errno));
        said = true;
        close(fd);
        return -1;
    }
    LV_LOG_USER("winlink: rigctld on 127.0.0.1:%d", WINLINK_RIG_PORT);
    return fd;
}

static void *rig_thread(void *arg) {
    int lfd = -1;
    (void)arg;

    for (;;) {
        if (lfd < 0 && (lfd = rig_listen()) < 0) {
            sleep(5);
            continue;
        }
        struct pollfd pfd[1 + RIG_CLIENTS];
        int           slot[1 + RIG_CLIENTS];
        nfds_t        n = 0;

        pfd[n++] = (struct pollfd){.fd = lfd, .events = POLLIN};
        for (int i = 0; i < RIG_CLIENTS; i++) {
            if (rig_clients[i].fd < 0)
                continue;
            slot[n]  = i;
            pfd[n++] = (struct pollfd){.fd = rig_clients[i].fd, .events = POLLIN};
        }
        if (poll(pfd, n, atomic_load(&pending_hz) ? 100 : -1) < 0) {
            if (errno != EINTR)
                usleep(100000);
            continue;
        }
        if (atomic_load(&pending_hz) && !on_air() && !atomic_load(&keying))
            rig_pending_tune();
        for (nfds_t k = 1; k < n; k++) {
            if (pfd[k].revents)
                rig_read(&rig_clients[slot[k]]);
        }
        if (pfd[0].revents & POLLIN) {
            int fd  = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            int one = 1;

            if (fd < 0)
                continue;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            for (int i = 0; i < RIG_CLIENTS && fd >= 0; i++) {
                if (rig_clients[i].fd < 0) {
                    rig_clients[i].fd  = fd;
                    rig_clients[i].len = 0;
                    fd = -1;
                }
            }
            if (fd >= 0)
                close(fd);  /* all taken */
        }
    }
    return NULL;
}

/* ---- Public ------------------------------------------------------------------------- */

void winlink_init(void) {
    pthread_condattr_t ca;
    pthread_t          th;
    float              rate = (float)AUDIO_PLAY_RATE / ARDOP_RATE;

    pthread_mutex_init(&ses.lock, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&ses.cond, &ca);
    pthread_condattr_destroy(&ca);

    /* One resampler for the life of the GUI, reset at each key; 60 dB as the other apps' */
    ses.rs       = msresamp_rrrf_create(rate, 60.0f);
    ses.tail_len = LV_MIN((size_t)ceilf(msresamp_rrrf_get_delay(ses.rs) / rate) + 2, (size_t)IN_BLOCK);

    for (int i = 0; i < RIG_CLIENTS; i++)
        rig_clients[i].fd = -1;

    if (pthread_create(&th, NULL, ardop_thread, NULL) == 0)
        pthread_detach(th);
    if (pthread_create(&th, NULL, rig_thread, NULL) == 0)
        pthread_detach(th);
}
