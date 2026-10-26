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
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* accept4 */

#include "winlink.h"

#include "audio.h"
#include "msg.h"
#include "radio.h"
#include "tx_player.h"

#include "lvgl/lvgl.h"

#include <liquid/liquid.h>

#include <errno.h>
#include <math.h>
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

    /* the worker's alone */
    msresamp_rrrf   rs;             /* 12000 -> 44100, its state kept from block to block */
    size_t          tail_len;       /* zeros that bring out what the filter still holds */
    float           out[OUT_MAX];
    int16_t         pend[OUT_MAX];  /* resampled, not yet given to tx_player */
    size_t          pend_pos, pend_len;
    uint64_t        key_ms;
} ses = {.fd = -1};

/* The worker's, kept from one connection to the next */
static uint64_t unkey_ms;           /* our last transmission ended */
static uint64_t swr_trip_ms;        /* the SWR protection stopped one, 0 never */

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
             * Once the last block was given, it is a key of its own after this one */
            if (ses.fill_done) {
                key_new();
            } else {
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

/* tx_player's abort, once a block: the client gone, or the key too long */
static bool tx_abort(void *ctx) {
    (void)ctx;
    bool quit  = atomic_load(&ses.quit);
    bool limit = now_ms() - ses.key_ms > KEY_LIMIT_MS;

    if (!quit && !limit)
        return false;
    if (limit) {
        LV_LOG_WARN("winlink: keyed for %d s, unkeyed", KEY_LIMIT_MS / 1000);
        msg_schedule_text_fmt("Winlink: TX stopped after %d s", KEY_LIMIT_MS / 1000);
    }
    pthread_mutex_lock(&ses.lock);
    ses.fill_done = true;
    pthread_mutex_unlock(&ses.lock);
    audio_play_flush();     /* this thread's stream: what tx_player plays into */
    return true;
}

/* Why ardopcf's key is not taken; NULL: it is */
static const char *tx_refused(void) {
    /* Our own transmission just ended: the base reports RX on its next packet */
    while (!radio_get_modem() && radio_get_state() != RADIO_RX && now_ms() - unkey_ms < TURN_MS &&
           !atomic_load(&ses.quit))
        usleep(10000);
    if (on_air())
        return "the radio is on the air";
    if (swr_trip_ms && now_ms() - swr_trip_ms < SWR_HOLD_MS)
        return "an SWR trip less than 5 minutes ago";
    return NULL;
}

/* One key on the air, the whole of it, on the worker */
static void transmit(void) {
    /* Opened on this thread, the one tx_player plays from: 100 ms queued instead of play_stm's 800 */
    bool     low   = audio_play_lowlat_begin();
    unsigned trips = radio_swr_trips();

    if (!low)
        LV_LOG_WARN("winlink: no low-latency stream, playing through the usual one");
    msresamp_rrrf_reset(ses.rs);
    ses.pend_pos = ses.pend_len = 0;
    ses.key_ms   = now_ms();

    tx_player_stream(tx_fill, NULL, TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), tx_abort, NULL);

    if (low)
        audio_play_lowlat_end();
    unkey_ms = now_ms();
    if (radio_swr_trips() != trips) {
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
            ses.key = ses.eos ? KEY_NONE : KEY_DISCARD;
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

    if (pthread_create(&th, NULL, ardop_thread, NULL) == 0)
        pthread_detach(th);
}
