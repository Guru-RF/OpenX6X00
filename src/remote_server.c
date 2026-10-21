/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Browser remote. A WebSocket server (port 50002) for the web configurator's Remote
 *  page: the radio's screen goes out as JPEG frames, the page sends the front keys and the knobs
 *  back. With it a phone, a tablet or a PC works the radio as if standing in front of it; RX and
 *  TX audio travel over the TCI server next door.
 *
 *  Threads. LVGL thread: screen_rec_flush() hands every flushed area to remote_flush(), kept in a
 *  shadow of the screen while someone is watching; a timer copies the shadow into a frame (the
 *  waterfall plane composed under it) up to ten times a second. Encoder thread: JPEG, then the
 *  frame to every viewer whose socket has room; an unchanged screen is sent once a second. Socket
 *  thread: accept, handshake, the commands. The keys and knobs are written into the input devices
 *  the GUI reads (the same path as the real keys: long presses, the screen waking, all of it).
 *
 *  Frames: binary, 8 bytes header {u16 width, u16 height, u32 sequence} little endian, then JPEG.
 *  Commands (text): "key NAME 1|0" (press, release), "knob main|vol|mfk N" (detents, signed),
 *  "ping". Answers: "hello <version>", "size W H", "pong", "err <text>".
 *
 *  The Origin check of ws_handshake() keeps pages from other sites out; like the TCI server there
 *  is no login: the radio's own network is trusted.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* accept4, pipe2 */

#include <stdio.h>       /* before jpeglib.h, which wants FILE */

#include "remote_server.h"

#include "lv_drivers/display/drm.h"
#include "main.h"
#include "screen_light.h"
#include "tci_ws.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
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

#define MAX_CLIENTS 3
#define IN_SIZE     4096
#define OUT_SIZE    (384 * 1024)    /* two frames at the most */
#define TEXT_MAX    256
#define FRAME_MS    100             /* ten frames a second at the most */
#define STILL_MS    1000            /* an unchanged screen: a frame this often */
#define IDLE_MS     30000           /* a silent client is gone (the page pings every 5 s) */
#define PING_MS     10000           /* a client silent this long is pinged: a page in a background tab has its
                                       timers throttled to a minute, the browser answers a PING by itself */
#define CONF_MS     2000
#define QUALITY     70
#define MAX_HELD    8

/* The input devices: what the GUI reads (gui_main.c inputs_open) */
#define DEV_KEYPAD "/dev/input/event0"
#define DEV_MAIN   "/dev/input/event1"
#define DEV_VOL    "/dev/input/event2"
#define DEV_MFK    "/dev/input/event3"
#define DEV_POWER  "/dev/input/event4"   /* the PMIC's key: the matrix keypad drops KEY_POWER */

typedef struct {
    int             fd;
    bool            up;
    char            ip[48];
    uint8_t         in[IN_SIZE];
    size_t          in_len;
    uint8_t        *out;            /* OUT_SIZE, under lock: the encoder writes here too */
    size_t          out_len;
    pthread_mutex_t lock;
    char            text[TEXT_MAX];
    size_t          text_len;
    uint64_t        last_ms;
    uint64_t        ping_ms;        /* the last PING we sent */
    uint16_t        held[MAX_HELD]; /* keys this client holds down: released when it goes */
    int             nheld;
} client_t;

typedef struct {
    uint16_t w, h;
    uint32_t seq;
} frame_hdr_t;

static client_t    clients[MAX_CLIENTS];
static atomic_bool enabled = true;
static atomic_int  port    = REMOTE_DEFAULT_PORT;
static atomic_int  nclients;
static int         wake_fd[2] = {-1, -1};
static int         dev_keypad = -1, dev_power = -1, dev_main = -1, dev_vol = -1, dev_mfk = -1;

/* The screen: shadow (LVGL thread), frame (handed over), enc and prev (encoder thread) */
static pthread_mutex_t fmut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  fcond = PTHREAD_COND_INITIALIZER;
static uint8_t        *shadow, *frame, *enc, *prev;
static int32_t         sw, sh;          /* the shadow's size: the display's */
static int32_t         fw, fh, ew, eh, pw, ph;
static bool            frame_ready, dirty;
static atomic_uint     seq;

static const struct {
    const char *name;
    uint16_t    code;
} keys[] = {
    {"POWER", KEY_POWER},
    {"GEN", BTN_TRIGGER_HAPPY1},     {"KEY", BTN_TRIGGER_HAPPY2},      {"DFN", BTN_TRIGGER_HAPPY3},
    {"PTT", BTN_TRIGGER_HAPPY4},     {"BAND_DOWN", BTN_TRIGGER_HAPPY5}, {"BAND_UP", BTN_TRIGGER_HAPPY6},
    {"APP", BTN_TRIGGER_HAPPY7},     {"MSG", BTN_TRIGGER_HAPPY8},      {"DFL", BTN_TRIGGER_HAPPY9},
    {"AM", BTN_TRIGGER_HAPPY10},     {"CW", BTN_TRIGGER_HAPPY11},      {"SSB", BTN_TRIGGER_HAPPY12},
    {"F1", BTN_TRIGGER_HAPPY13},     {"F2", BTN_TRIGGER_HAPPY14},      {"F3", BTN_TRIGGER_HAPPY15},
    {"AB", BTN_TRIGGER_HAPPY16},     {"PRE", BTN_TRIGGER_HAPPY17},     {"ATU", BTN_TRIGGER_HAPPY18},
    {"F4", BTN_TRIGGER_HAPPY19},     {"F5", BTN_TRIGGER_HAPPY20},      {"VOL", BTN_TRIGGER_HAPPY21},
    {"VM", BTN_TRIGGER_HAPPY22},     {"AGC", BTN_TRIGGER_HAPPY23},     {"FST", BTN_TRIGGER_HAPPY24},
    {"LOCK", BTN_TRIGGER_HAPPY25},   {"MFK", BTN_TRIGGER_HAPPY27},
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

/* ---- Configuration (as tci_server.c: the web configurator shares the file) ---------- */

static time_t conf_mtime(void) {
    struct stat st;
    return stat(REMOTE_CONF, &st) == 0 ? st.st_mtime : 0;
}

static void conf_read(void) {
    FILE *f = fopen(REMOTE_CONF, "r");
    char  line[64];
    bool  en = true;
    int   p  = REMOTE_DEFAULT_PORT;

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

    snprintf(tmp, sizeof(tmp), "%s.tmp", REMOTE_CONF);
    if (!(f = fopen(tmp, "w")))
        return;
    fprintf(f, "enabled=%d\nport=%d\n", atomic_load(&enabled) ? 1 : 0, atomic_load(&port));
    fclose(f);
    rename(tmp, REMOTE_CONF);
}

static void wake(void) {
    if (wake_fd[1] >= 0 && write(wake_fd[1], "w", 1) < 0) {}
}

/* ---- Input devices ------------------------------------------------------------------ */

static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev[2];

    if (fd < 0)
        return;
    memset(ev, 0, sizeof(ev));
    ev[0].type  = type;
    ev[0].code  = code;
    ev[0].value = value;
    ev[1].type  = EV_SYN;
    ev[1].code  = SYN_REPORT;
    if (write(fd, ev, sizeof(ev)) < 0) {}
}

static void key_emit(uint16_t code, bool down) {
    emit(code == KEY_POWER ? dev_power : dev_keypad, EV_KEY, code, down ? 1 : 0);
}

static int key_code(const char *name) {
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strcasecmp(keys[i].name, name) == 0)
            return keys[i].code;
    return -1;
}

/* A knob turned n detents (signed): one event per detent, as the hardware reports them */
static bool knob_emit(const char *name, int n) {
    int      fd, axis;

    if (!strcasecmp(name, "main"))
        fd = dev_main, axis = REL_X;
    else if (!strcasecmp(name, "vol"))
        fd = dev_vol, axis = REL_Y;
    else if (!strcasecmp(name, "mfk"))
        fd = dev_mfk, axis = REL_Z;
    else
        return false;
    if (n > 50) n = 50;
    if (n < -50) n = -50;
    for (int i = 0; i < abs(n); i++)
        emit(fd, EV_REL, axis, n > 0 ? 1 : -1);
    return true;
}

static void client_release_keys(client_t *c) {
    for (int i = 0; i < c->nheld; i++)
        key_emit(c->held[i], false);
    c->nheld = 0;
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

/* A whole frame or nothing: a slow client loses frames, never gets a cut one. Any thread. */
static bool client_frame(client_t *c, uint8_t opcode, const void *p1, size_t l1, const void *p2, size_t l2) {
    uint8_t head[WS_HEAD_MAX];
    size_t  hl = ws_frame_head(head, opcode, l1 + l2);
    bool    ok = false;

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
        ok = true;
    }
    pthread_mutex_unlock(&c->lock);
    return ok;
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

static void client_close(client_t *c) {
    client_release_keys(c);
    pthread_mutex_lock(&c->lock);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        atomic_fetch_sub(&nclients, 1);
    }
    c->up      = false;
    c->out_len = c->in_len = c->text_len = 0;
    pthread_mutex_unlock(&c->lock);
}

/* "key NAME 1|0", "knob main|vol|mfk N", "ping" */
static void handle_text(client_t *c, char *text) {
    char *a[4] = {0};
    int   n    = 0;

    for (char *t = strtok(text, " \t\r\n"); t && n < 4; t = strtok(NULL, " \t\r\n"))
        a[n++] = t;
    if (n == 0)
        return;
    if (!strcmp(a[0], "ping")) {
        client_text(c, "pong");
    } else if (!strcmp(a[0], "key") && n >= 3) {
        int  code = key_code(a[1]);
        bool down = atoi(a[2]) != 0;
        if (code < 0) {
            client_textf(c, "err unknown key %s", a[1]);
            return;
        }
        int i = 0;
        while (i < c->nheld && c->held[i] != code) i++;
        if (down) {
            if (i == c->nheld) {
                if (c->nheld == MAX_HELD) {
                    client_text(c, "err too many keys held");
                    return;
                }
                c->held[c->nheld++] = (uint16_t)code;
            }
        } else if (i < c->nheld) {
            c->held[i] = c->held[--c->nheld];
        }
        key_emit((uint16_t)code, down);
    } else if (!strcmp(a[0], "knob") && n >= 3) {
        if (!knob_emit(a[1], atoi(a[2])))
            client_textf(c, "err unknown knob %s", a[1]);
    } else {
        client_textf(c, "err unknown command %s", a[0]);
    }
}

static void client_read(client_t *c) {
    ssize_t n = recv(c->fd, c->in + c->in_len, IN_SIZE - c->in_len, MSG_DONTWAIT);

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
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
        client_textf(c, "hello %s", VERSION);
        pthread_mutex_lock(&fmut);
        int32_t w = sw, h = sh;
        pthread_mutex_unlock(&fmut);
        client_textf(c, "size %d %d", w ? w : 800, h ? h : 480);
        LV_LOG_USER("remote: viewer %s", c->ip);
        return;
    }

    size_t pos = 0;
    while (c->fd >= 0) {
        ws_frame_t f;
        int        used = ws_frame_parse(c->in + pos, c->in_len - pos, IN_SIZE - 16, &f);
        if (used == 0)
            break;
        if (used < 0) {
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
                    c->text_len = 0;
                    break;
                }
                if (f.fin) {
                    c->text[c->text_len] = 0;
                    c->text_len = 0;
                    handle_text(c, c->text);
                }
                break;
            case WS_OP_PING:
                client_frame(c, WS_OP_PONG, f.payload, f.len, NULL, 0);
                break;
            case WS_OP_CLOSE:
                client_frame(c, WS_OP_CLOSE, f.payload, f.len > 2 ? 2 : f.len, NULL, 0);
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
        client_close(c);
}

static int listen_open(int p) {
    int                fd  = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a   = {.sin_family = AF_INET, .sin_port = htons((uint16_t)p), .sin_addr.s_addr = htonl(INADDR_ANY)};
    int                one = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
        LV_LOG_ERROR("remote: can't listen on port %d", p);
        close(fd);
        return -1;
    }
    LV_LOG_USER("remote: listening on port %d", p);
    return fd;
}

static void accept_client(int listen_fd) {
    struct sockaddr_in peer;
    socklen_t          pl  = sizeof(peer);
    int                fd  = accept4(listen_fd, (struct sockaddr *)&peer, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int                one = 1;

    if (fd < 0)
        return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        if (c->fd >= 0)
            continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_mutex_lock(&c->lock);
        c->fd      = fd;
        c->up      = false;
        c->in_len = c->out_len = c->text_len = 0;
        c->nheld   = 0;
        c->last_ms = c->ping_ms = now_ms();
        inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof(c->ip));
        pthread_mutex_unlock(&c->lock);
        atomic_fetch_add(&nclients, 1);
        return;
    }
    close(fd);
}

static void *server_thread(void *arg) {
    int      listen_fd = -1, listen_port = 0;
    uint64_t conf_ms = 0, idle_ms = 0;
    time_t   mtime = conf_mtime();
    (void)arg;

    while (true) {
        bool en = atomic_load(&enabled);
        int  p  = atomic_load(&port);

        if ((!en || p != listen_port) && listen_fd >= 0) {
            close(listen_fd);
            listen_fd = -1;
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].fd >= 0)
                    client_close(&clients[i]);
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
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0)
                continue;
            pthread_mutex_lock(&c->lock);
            short ev = POLLIN | (c->out_len ? POLLOUT : 0);
            pthread_mutex_unlock(&c->lock);
            slot[n]  = i;
            pfd[n++] = (struct pollfd){.fd = c->fd, .events = ev};
        }
        if (poll(pfd, n, listen_fd >= 0 ? 500 : 1000) < 0 && errno != EINTR) {
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
                client_read(c);
            else if (c->fd >= 0 && (pfd[k].revents & (POLLERR | POLLHUP | POLLNVAL)))
                client_close(c);
        }

        uint64_t now = now_ms();
        if (now - idle_ms >= 1000) {
            idle_ms = now;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &clients[i];
                if (c->fd < 0)
                    continue;
                if (now - c->last_ms > IDLE_MS) {
                    client_close(c);
                } else if (c->up && now - c->last_ms > PING_MS && now - c->ping_ms > PING_MS) {
                    c->ping_ms = now;
                    client_frame(c, WS_OP_PING, NULL, 0, NULL, 0);
                }
            }
        }
        if (now - conf_ms >= CONF_MS) {
            conf_ms = now;
            time_t m = conf_mtime();
            if (m != mtime) {
                mtime = m;
                conf_read();
            }
        }
    }
    return NULL;
}

/* ---- The screen ---------------------------------------------------------------------- */

static void jpeg_fail(j_common_ptr c) {
    longjmp(*(jmp_buf *)c->client_data, 1);
}

/* The JPEG into a buffer libjpeg allocates, the caller frees. Its own buffer is never handed
 * back in: jpeg_mem_dest() reads *size as the buffer's capacity but writes the JPEG's length
 * into it, so a buffer reused that way "grew" on every frame bigger than the one before, each
 * time leaving the old one behind: 200 kB a second with a viewer watching the waterfall, an
 * out-of-memory kill after an hour. */
static bool encode(const uint8_t *px, int32_t w, int32_t h, uint8_t **out, unsigned long *size) {
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr       jerr;
    jmp_buf                     jb;

    *out  = NULL;
    *size = 0;
    c.err           = jpeg_std_error(&jerr);
    jerr.error_exit = jpeg_fail;
    jpeg_create_compress(&c);
    c.client_data = &jb;
    if (setjmp(jb)) {
        jpeg_destroy_compress(&c);
        *out = NULL;    /* libjpeg's buffer, lost: it may already have freed what *out points to */
        return false;
    }
    jpeg_mem_dest(&c, out, size);
    c.image_width      = w;
    c.image_height     = h;
    c.input_components = 4;
    c.in_color_space   = JCS_EXT_BGRX;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, QUALITY, TRUE);
    c.dct_method = JDCT_IFAST;
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < (JDIMENSION)h) {
        JSAMPROW row = (JSAMPROW)(px + c.next_scanline * w * 4);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return true;
}

/* LVGL thread, from screen_rec_flush(): the area into the shadow while someone watches */
void remote_flush(const lv_area_t *area, const lv_color_t *color_p) {
    if (!atomic_load(&nclients))
        return;
    int32_t w = lv_disp_get_hor_res(NULL), h = lv_disp_get_ver_res(NULL);

    pthread_mutex_lock(&fmut);
    if (!shadow || w != sw || h != sh) {
        free(shadow);
        shadow = calloc((size_t)w * h, 4);
        sw = w, sh = h;
        if (!shadow) {
            pthread_mutex_unlock(&fmut);
            return;
        }
    }
    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t x2 = area->x2 >= w ? w - 1 : area->x2;
    int32_t aw = area->x2 - area->x1 + 1;
    for (int32_t y = area->y1 < 0 ? 0 : area->y1; y <= area->y2 && y < h; y++)
        memcpy(shadow + ((size_t)y * w + x1) * 4, (const uint8_t *)color_p + ((size_t)(y - area->y1) * aw + x1 - area->x1) * 4,
               (x2 - x1 + 1) * 4);
    dirty = true;
    pthread_mutex_unlock(&fmut);
}

/* LVGL thread, every FRAME_MS: the viewers' coming and going, the shadow into a frame */
static void tick_cb(lv_timer_t *t) {
    static bool had_viewers;
    bool        any = atomic_load(&nclients) > 0;
    (void)t;

    if (any != had_viewers) {
        had_viewers = any;
        backlight_viewers(any);     /* drawn while dark for them */
        if (any) {
            /* the shadow starts empty: everything drawn once */
            lv_obj_invalidate(lv_scr_act());
            lv_obj_invalidate(lv_layer_top());
            lv_obj_invalidate(lv_layer_sys());
        }
    }
    if (!any)
        return;
    pthread_mutex_lock(&fmut);
    if (shadow && !frame_ready) {
        size_t bytes = (size_t)sw * sh * 4;
        if (!frame || fw != sw || fh != sh) {
            free(frame);
            frame = malloc(bytes);
            fw = sw, fh = sh;
        }
        if (frame) {
            memcpy(frame, shadow, bytes);
#if USE_DRM
            if (sw == 800 && sh == 480)
                drm_compose_landscape((uint32_t *)frame);   /* + the waterfall plane */
#endif
            frame_ready = true;
            dirty       = false;
            pthread_cond_signal(&fcond);
        }
    }
    pthread_mutex_unlock(&fmut);
}

/* Encoder thread: a frame to JPEG, to every viewer with room; an unchanged screen once a second */
static void *encoder_thread(void *arg) {
    uint64_t      sent_ms = 0;
    uint8_t      *jpg     = NULL;
    unsigned long jpg_len = 0;
    (void)arg;

    while (true) {
        pthread_mutex_lock(&fmut);
        while (!frame_ready)
            pthread_cond_wait(&fcond, &fmut);
        /* the buffers change places, each with its size: LVGL fills the old enc next */
        uint8_t *tmp = enc;
        int32_t  tw = ew, th = eh;
        enc = frame, ew = fw, eh = fh;
        frame = tmp, fw = tw, fh = th;
        int32_t w = ew, h = eh;
        frame_ready = false;
        pthread_mutex_unlock(&fmut);

        if (!atomic_load(&nclients))
            continue;
        uint64_t now  = now_ms();
        size_t   bytes = (size_t)w * h * 4;
        bool     same  = prev && pw == w && ph == h && memcmp(prev, enc, bytes) == 0;
        if (same && now - sent_ms < STILL_MS) {
            usleep(FRAME_MS * 1000 / 2);
            continue;
        }
        if (!encode(enc, w, h, &jpg, &jpg_len)) {
            usleep(FRAME_MS * 1000);
            continue;
        }
        frame_hdr_t hdr = {.w = (uint16_t)w, .h = (uint16_t)h, .seq = atomic_fetch_add(&seq, 1)};
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0 || !c->up)
                continue;
            pthread_mutex_lock(&c->lock);
            bool room = c->out_len == 0;      /* the last frame still on its way: this one is skipped */
            pthread_mutex_unlock(&c->lock);
            if (room)
                client_frame(c, WS_OP_BINARY, &hdr, sizeof(hdr), jpg, jpg_len);
        }
        free(jpg);
        jpg     = NULL;
        jpg_len = 0;
        sent_ms = now;
        /* what went out becomes the frame to compare with */
        if (!prev || pw != w || ph != h) {
            free(prev);
            prev = malloc(bytes);
            pw = w, ph = h;
        }
        if (prev)
            memcpy(prev, enc, bytes);
        /* hold the pace: the LVGL side fills the next frame meanwhile */
        uint64_t spent = now_ms() - now;
        if (spent < FRAME_MS)
            usleep((FRAME_MS - spent) * 1000);
    }
    return NULL;
}

/* ---- Public ------------------------------------------------------------------------- */

void remote_server_init(void) {
    pthread_t th;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd  = -1;
        clients[i].out = malloc(OUT_SIZE);
        pthread_mutex_init(&clients[i].lock, NULL);
    }
    dev_keypad = open(DEV_KEYPAD, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    dev_power  = open(DEV_POWER, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    dev_main   = open(DEV_MAIN, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    dev_vol    = open(DEV_VOL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    dev_mfk    = open(DEV_MFK, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    conf_read();
    if (pipe2(wake_fd, O_NONBLOCK | O_CLOEXEC) < 0) {
        LV_LOG_ERROR("remote: no wake pipe");
        return;
    }
    lv_timer_create(tick_cb, FRAME_MS, NULL);
    pthread_create(&th, NULL, encoder_thread, NULL);
    pthread_detach(th);
    pthread_create(&th, NULL, server_thread, NULL);
    pthread_detach(th);
}

bool remote_server_get_enabled(void) {
    return atomic_load(&enabled);
}

void remote_server_set_enabled(bool on) {
    atomic_store(&enabled, on);
    conf_write();
    wake();
}

int remote_server_port(void) {
    return atomic_load(&port);
}

int remote_server_viewers(void) {
    return atomic_load(&nclients);
}

const char *remote_server_viewer(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].up)
            return clients[i].ip;
    return NULL;
}
