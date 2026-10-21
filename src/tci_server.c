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
 *  Audio: a capture thread reads the receiver from PulseAudio at 24 kHz, mono, and hands
 *  512-sample frames to every client that asked (audio_start). A client keyed with
 *  "trx:0,true,tci" sends TX_AUDIO frames; they go to the codec as the voice keyer's audio
 *  does, with the modem bit keying the base. The server paces it with a TX_CHRONO frame every
 *  512 samples, and unkeys after 500 ms without audio or 3 minutes on the air.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* accept4, pipe2 */

#include "tci_server.h"
#include "tci_ws.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "scheduler.h"
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
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS   4
#define IN_SIZE       16384         /* a client's frame: commands, or 512 TX samples as float32 */
#define OUT_SIZE      65536         /* queued output: ~1.3 s of RX audio, dropped when full */
#define TEXT_MAX      2048          /* a text message, fragments joined */
#define IDLE_MS       60000         /* a client silent this long is gone (the knob pings every 2 s) */
#define STATE_MS      20            /* the radio's state read this often */
#define SMETER_MS     100
#define TXSENS_MS     200
#define CONF_MS       2000
#define AUDIO_RATE    24000         /* the RX audio to the clients, whatever rate they ask (see below) */
#define AUDIO_FRAMES  512
#define TX_SILENCE_MS 500           /* keyed with TCI audio: unkeyed without frames this long */
#define TX_LIMIT_MS   (180 * 1000)

/* The TCI audio frame header, 64 bytes, little endian */
enum { AF_INT16 = 0, AF_FLOAT32 = 3 };
enum { AT_IQ = 0, AT_RX = 1, AT_TX = 2, AT_CHRONO = 3 };

typedef struct {
    uint32_t receiver, sample_rate, format, codec, crc, length, type, channels;
    uint32_t reserved[8];
} audio_hdr_t;

typedef struct {
    int             fd;
    bool            up;             /* handshake done */
    char            ip[48];
    uint8_t         in[IN_SIZE];
    size_t          in_len;
    uint8_t         out[OUT_SIZE];  /* under lock: the capture thread writes here too */
    size_t          out_len;
    pthread_mutex_t lock;
    char            text[TEXT_MAX];
    size_t          text_len;
    bool            rx_sensors, tx_sensors;
    bool            audio_on, audio_float;
    uint32_t        audio_rate;
    uint64_t        last_ms;
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
    int32_t  a, b;
    bool     tci;
} cmd_t;

static client_t        clients[MAX_CLIENTS];
static atomic_bool     enabled = true;    /* on until /mnt/tci.conf says enabled=0 */
static atomic_int      port = TCI_DEFAULT_PORT;
static atomic_int      nclients;
static int             wake_fd[2] = {-1, -1};
static atomic_bool     cap_wanted;
static int             drive_set = -1;   /* the drive a client set; the base holds it, the settings keep theirs */
static int             ptt_owner = -1;   /* the client that keyed the radio's own PTT, -1 none */
static uint64_t        ptt_ms;

/* The TX audio session: one client at a time */
static struct {
    atomic_bool keyed;      /* the base keyed by us through the modem bit */
    atomic_bool refused;    /* the last key was refused: say trx false */
    int         owner;      /* client index */
    uint64_t    start_ms, last_audio_ms, chrono_ms;
    uint32_t    rate;
    double      pos;
    int16_t     prev;
} txs;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

/* ---- Configuration ------------------------------------------------------------------ */

static time_t conf_mtime(void) {
    struct stat st;
    return stat(TCI_CONF, &st) == 0 ? st.st_mtime : 0;
}

static void conf_read(void) {
    FILE *f = fopen(TCI_CONF, "r");
    char  line[64];
    bool  en = true;
    int   p  = TCI_DEFAULT_PORT;

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "enabled=", 8) == 0)
            en = atoi(line + 8) != 0;
        else if (strncmp(line, "port=", 5) == 0 && atoi(line + 5) > 0 && atoi(line + 5) < 65536)
            p = atoi(line + 5);
    }
    fclose(f);
    atomic_store(&enabled, en);
    atomic_store(&port, p);
}

static void conf_write(void) {
    char  tmp[64];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", TCI_CONF);
    if (!(f = fopen(tmp, "w")))
        return;
    fprintf(f, "enabled=%d\nport=%d\n", atomic_load(&enabled) ? 1 : 0, atomic_load(&port));
    fclose(f);
    rename(tmp, TCI_CONF);
}

static void wake(void) {
    if (wake_fd[1] >= 0 && write(wake_fd[1], "w", 1) < 0) {}
}

/* ---- Clients ------------------------------------------------------------------------ */

static void client_flush(client_t *c) {
    if (c->fd < 0 || !c->out_len)
        return;
    ssize_t n = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
        c->out_len -= n;
        memmove(c->out, c->out + n, c->out_len);
    }
}

/* A whole frame (header, then one or two payload parts) or nothing: a slow client loses
 * frames, never gets a cut one. Any thread. */
static void client_frame(client_t *c, uint8_t opcode, const void *p1, size_t l1, const void *p2, size_t l2) {
    uint8_t head[WS_HEAD_MAX];
    size_t  hl = ws_frame_head(head, opcode, l1 + l2);

    pthread_mutex_lock(&c->lock);
    if (c->fd >= 0 && c->up && c->out_len + hl + l1 + l2 <= OUT_SIZE) {
        memcpy(c->out + c->out_len, head, hl);
        c->out_len += hl;
        if (l1) {
            memcpy(c->out + c->out_len, p1, l1);
            c->out_len += l1;
        }
        if (l2) {
            memcpy(c->out + c->out_len, p2, l2);
            c->out_len += l2;
        }
        client_flush(c);
    }
    pthread_mutex_unlock(&c->lock);
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

static void client_close(client_t *c) {
    pthread_mutex_lock(&c->lock);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        atomic_fetch_sub(&nclients, 1);
    }
    c->up = false;
    c->out_len = c->in_len = c->text_len = 0;
    c->audio_on = c->rx_sensors = c->tx_sensors = false;
    pthread_mutex_unlock(&c->lock);
}

/* ---- The radio, on the LVGL thread -------------------------------------------------- */

static bool voice_mode(int32_t m) {
    return m == x6100_mode_lsb || m == x6100_mode_usb || m == x6100_mode_am || m == x6100_mode_nfm;
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
}

static void apply_cmd(void *arg) {
    cmd_t *c = (cmd_t *)arg;

    switch (c->id) {
        case C_FREQ:
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
            drive_set = c->a;
            cfg_pwr_set(0.1f + c->a * 9.9f / 100.0f);   /* saved, the radio follows the setting */
            break;
        case C_KEY:
            if (radio_get_state() != RADIO_RX || radio_get_modem() || atomic_load(&txs.keyed)) {
                atomic_store(&txs.refused, true);
                break;
            }
            if (c->tci && voice_mode(cparam_i_get(cfg_cur_mode))) {
                if (!audio_play_lowlat_begin()) {
                    atomic_store(&txs.refused, true);
                    break;
                }
                txs.pos  = 0;
                txs.prev = 0;
                atomic_store(&txs.keyed, true);
                radio_set_modem(true);
            } else {
                radio_set_ptt(true);
            }
            break;
        case C_UNKEY:
            tx_unkey(c->tci, c->a != 0);
            break;
    }
}

static void put_cmd(cmd_id_t id, int32_t a, int32_t b, bool tci) {
    cmd_t c = {.id = id, .a = a, .b = b, .tci = tci};
    scheduler_put(apply_cmd, &c, sizeof(c));
}

/* ---- The state, read from any thread ------------------------------------------------ */

static const char *const mode_names[] = {"lsb", "digl", "usb", "digu", "cw", "cwr", "am", "nfm"};
static const char *const agc_names[]  = {"off", "slow", "fast", "auto"};

static void state_read(rstate_t *s) {
    s->freq  = cparam_i_get(cfg_fg_freq);
    s->mode  = cparam_i_get(cfg_cur_mode);
    s->flo   = cparam_i_get(cfg_cur_filter_low);
    s->fhi   = cparam_i_get(cfg_cur_filter_high);
    s->agc   = cparam_i_get(cfg_cur_agc);
    s->rit   = param_i_get(cfg_rit);
    s->drive = drive_set >= 0 ? drive_set : (int)lroundf((param_f_get(cfg_pwr) - 0.1f) * 100.0f / 9.9f);
    s->tx    = radio_get_state() != RADIO_RX || radio_get_modem();
}

static const char *mode_name(int32_t m) {
    return m >= 0 && m < 8 ? mode_names[m] : "usb";
}

static const char *agc_name(int32_t a) {
    return a >= 0 && a < 4 ? agc_names[a] : "auto";
}

/* The passband as TCI tells it: below the carrier negative (lsb, digl, cwr) */
static void filter_line(char *buf, size_t n, const rstate_t *s) {
    bool neg = s->mode == x6100_mode_lsb || s->mode == x6100_mode_lsb_dig || s->mode == x6100_mode_cwr;
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
    client_text(c, "modulations_list:lsb,usb,digl,digu,cw,cwr,am,nfm;");
    client_textf(c, "audio_samplerate:%d;", AUDIO_RATE);
    client_text(c, "audio_stream_sample_type:int16;");
    client_text(c, "audio_stream_channels:1;");
    client_textf(c, "audio_stream_samples:%d;", AUDIO_FRAMES);
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
    client_text(c, "ready;");
}

/* What changed since the last look goes to everyone */
static void state_follow(rstate_t *last, bool first) {
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
    if (s.tx != last->tx) {
        broadcastf("trx:0,%s;", s.tx ? "true" : "false");
        if (!s.tx)
            ptt_owner = -1;
    }
    if (atomic_exchange(&txs.refused, false))
        broadcast("trx:0,false;");
    *last = s;
}

/* ---- Commands ----------------------------------------------------------------------- */

static bool parse_bool(const char *s) {
    return strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0;
}

static int mode_index(const char *name) {
    for (int i = 0; i < 8; i++)
        if (strcasecmp(name, mode_names[i]) == 0)
            return i;
    if (strcasecmp(name, "fm") == 0) return x6100_mode_nfm;
    if (strcasecmp(name, "sam") == 0 || strcasecmp(name, "dsb") == 0) return x6100_mode_am;
    return -1;
}

static int agc_index(const char *name) {
    for (int i = 0; i < 4; i++)
        if (strcasecmp(name, agc_names[i]) == 0)
            return i;
    if (strcasecmp(name, "med") == 0 || strcasecmp(name, "medium") == 0) return x6100_agc_auto;
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

/* The client's transmission ends, whichever way it keyed: from the socket thread. A key still
 * on its way to the LVGL thread is undone by the unkey queued after it. */
static void tx_session_end(int ci, bool drain) {
    if (txs.owner == ci) {
        put_cmd(C_UNKEY, drain, 0, true);
        txs.owner = -1;
    }
    if (ptt_owner == ci) {
        put_cmd(C_UNKEY, 0, 0, false);
        ptt_owner = -1;
    }
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
    state_read(&s);

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
            if (s.tx || txs.owner >= 0) {
                client_text(c, "trx:0,false;");     /* someone is on the air already */
            } else {
                if (tci) {
                    txs.owner         = ci;
                    txs.start_ms      = now_ms();
                    txs.last_audio_ms = txs.start_ms;
                    txs.chrono_ms     = 0;
                    txs.rate          = AUDIO_RATE;
                } else {
                    ptt_owner = ci;
                    ptt_ms    = now_ms();
                }
                put_cmd(C_KEY, 0, 0, tci);
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
    } else if (!strcmp(name, "audio_samplerate")) {
        if (n >= 1) {
            int r = atoi(a[0]);
            if (r == 8000 || r == 12000 || r == 24000 || r == 48000)
                c->audio_rate = r;
        }
        client_textf(c, "audio_samplerate:%u;", c->audio_rate);
    } else if (!strcmp(name, "audio_stream_sample_type")) {
        if (n >= 1) c->audio_float = strcasecmp(a[0], "float32") == 0;
        client_textf(c, "audio_stream_sample_type:%s;", c->audio_float ? "float32" : "int16");
    } else if (!strcmp(name, "audio_start")) {
        c->audio_on = true;
        atomic_store(&cap_wanted, true);
        client_text(c, "audio_start:0;");
    } else if (!strcmp(name, "audio_stop")) {
        c->audio_on = false;
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
        client_textf(c, "%s;", name);
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

/* Linear interpolation from in_rate to AUDIO_PLAY_RATE, carried over between frames */
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
    uint32_t ch  = h.channels ? h.channels : 1;
    size_t   bps = h.format == AF_FLOAT32 ? 4 : 2;
    /* Each bound checked on its own: n * ch * bps with the header's channel count unchecked
     * wrapped on 32 bits and read past the frame */
    if (n == 0 || n > 2048 || ch > 2 || len - sizeof(h) < n * ch * bps)
        return;
    const uint8_t *d = frame + sizeof(h);
    for (size_t i = 0; i < n; i++) {
        if (h.format == AF_FLOAT32) {
            float f;
            memcpy(&f, d + i * ch * 4, 4);
            pcm[i] = (int16_t)(LV_CLAMP(-1.0f, f, 1.0f) * 32767.0f);
        } else {
            pcm[i] = (int16_t)(d[i * ch * 2] | d[i * ch * 2 + 1] << 8);
        }
    }
    if (h.sample_rate >= 8000 && h.sample_rate <= 48000)
        txs.rate = h.sample_rate;
    txs.last_audio_ms = now_ms();
    size_t k = resample(pcm, n, txs.rate, out, sizeof(out) / sizeof(out[0]));
    if (k)
        audio_play_if_room(out, k);    /* full: dropped, the delay stays short */
}

/* Keyed with TCI audio: the pacing frames, and the watchdog. The radio's own PTT keyed from
 * here has the time limit too. */
static void tx_session_tick(uint64_t now) {
    if (ptt_owner >= 0 && (clients[ptt_owner].fd < 0 || now - ptt_ms > TX_LIMIT_MS))
        tx_session_end(ptt_owner, false);
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
    uint64_t period = AUDIO_FRAMES * 1000 / txs.rate;   /* 21 ms at 24 kHz */
    if (now - txs.chrono_ms >= period) {
        audio_hdr_t h = {.receiver = 0, .sample_rate = txs.rate, .format = AF_INT16, .length = AUDIO_FRAMES,
                         .type = AT_CHRONO, .channels = 1};
        txs.chrono_ms = txs.chrono_ms ? txs.chrono_ms + period : now;
        client_frame(c, WS_OP_BINARY, &h, sizeof(h), NULL, 0);
    }
}

/* ---- RX audio to the clients -------------------------------------------------------- */

/* Capture at AUDIO_RATE, mono: every client gets that rate, whatever it asked (the header
 * says so, and the knob plays what the header says) */
static void *capture_thread(void *arg) {
    pa_simple *s = NULL;
    int16_t    pcm[AUDIO_FRAMES];
    float      fl[AUDIO_FRAMES];
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
        bool any = false;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0 || !c->audio_on)
                continue;
            any = true;
            audio_hdr_t h = {.receiver = 0, .sample_rate = AUDIO_RATE, .format = c->audio_float ? AF_FLOAT32 : AF_INT16,
                             .length = AUDIO_FRAMES, .type = AT_RX, .channels = 1};
            if (c->audio_float) {
                for (int k = 0; k < AUDIO_FRAMES; k++)
                    fl[k] = pcm[k] / 32768.0f;
                client_frame(c, WS_OP_BINARY, &h, sizeof(h), fl, sizeof(fl));
            } else {
                client_frame(c, WS_OP_BINARY, &h, sizeof(h), pcm, sizeof(pcm));
            }
        }
        if (!any)
            atomic_store(&cap_wanted, false);
    }
    return NULL;
}

/* ---- The socket thread -------------------------------------------------------------- */

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

    if (!c->up) {
        char resp[256];
        int  r = ws_handshake((const char *)c->in, c->in_len, resp, sizeof(resp));
        if (r == 0) {
            if (c->in_len == IN_SIZE)
                client_close(c);
            return;
        }
        if (r < 0 || send(c->fd, resp, strlen(resp), MSG_NOSIGNAL) < 0) {
            client_close(c);
            return;
        }
        c->in_len = 0;
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
                    handle_text(ci, c->text);
                }
                break;
            case WS_OP_BINARY:
                if (f.fin)
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
        pthread_mutex_lock(&c->lock);
        c->fd          = fd;
        c->up          = false;
        c->in_len = c->out_len = c->text_len = 0;
        c->rx_sensors = c->tx_sensors = c->audio_on = c->audio_float = false;
        c->audio_rate  = AUDIO_RATE;
        c->last_ms     = now_ms();
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
            short ev = POLLIN | (c->out_len ? POLLOUT : 0);
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
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].fd >= 0 && now - clients[i].last_ms > IDLE_MS) {
                    tx_session_end(i, false);
                    client_close(&clients[i]);
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

int tci_server_clients(void) {
    return atomic_load(&nclients);
}

const char *tci_server_client(void) {
    static char buf[48];

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        pthread_mutex_lock(&c->lock);
        bool up = c->fd >= 0 && c->up;
        if (up)
            snprintf(buf, sizeof(buf), "%s", c->ip);
        pthread_mutex_unlock(&c->lock);
        if (up)
            return buf;
    }
    return NULL;
}
