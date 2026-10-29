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
 *  "ping [N]". Answers: "hello <version> <boot>", "size W H", "pong [N]", "err <text>". <boot> is a
 *  number chosen when the GUI started: a page that sees it change knows the radio restarted and
 *  nothing it had asked for survived (R6). The pong repeats the ping's number: a pong is dropped
 *  with the rest when the client's queue is full, so the page tells by the number which ping it
 *  answers rather than by counting (audit C02).
 *
 *  Sessions and control (R3): a new connection may only watch and listen ("role view 0 -").
 *  "pair?" puts a six-digit code on the radio's screen for two minutes; "pair CODE NAME" with
 *  the right code makes a session (the first one ever is admin, the others operate, with TX),
 *  answered "paired TOKEN ROLE TX"; the page keeps the token and sends "auth TOKEN" at every
 *  connection, answered "role ROLE TX NAME" (view, operate, admin). Keys, knobs, "set", "ft8"
 *  need operate and control: one holder at a time for what touches the RF side, taken with
 *  "control take" (refused while the radio is on the air), given back with "control release";
 *  "ctl WHO MINE" tells every client who has it (none, local, tci, browser NAME). The log needs
 *  operate. An admin lists the sessions ("sessions" -> "sessions [...]") and ends one ("revoke
 *  N": its sockets close, its token is gone); "unpair" forgets the caller's own. A held PTT
 *  key is a lease: "key PTT 1 HOLD" presses it, HOLD the page's number for the press, and the
 *  page repeats it every second; the radio unkeys without it for three seconds, and after three
 *  minutes whatever happens (R4). A hold that ended stays ended: only a new number keys again
 *  (audit C02). "tx 0|1" goes to every client when the radio's TX state changes.
 *
 *  The Panel page works the radio natively instead of through a picture of its screen: a client
 *  that sends "panel 1" gets the radio's state as JSON ("state {...}": frequency, mode, filter,
 *  the switches, the levels), the S-meter ("m <dB>", ten a second), the TX meters while on the
 *  air ("t <W> <SWR> <ALC>"), the band list and the band plan once ("bands [...]", "plan [...]"),
 *  the cluster spots as they change ("spots [...]") and the spectrum as binary frames: 16 bytes
 *  header {u16 0xFFFF, u16 bins, u32 centre Hz, u32 width Hz, u32 sequence}, then one int8 dB
 *  per bin (the waterfall's own FFT, up to 15 a second, a kilobyte each where a screen frame is
 *  thirty). "screen 0" stops the screen frames for a client that does not show them. "set NAME
 *  VALUE" changes the radio on the LVGL thread: freq, mode, agc, pre, att, nb, nr, sql, rfg,
 *  vol, pwr (tenths of a watt), step, zoom, band (+1/-1), vfo (toggle), filter LO HI, atu, lock;
 *  one refused ("err ...") is followed by the whole state, changed or not (audit C13).
 *  A panel thread reads the radio (the settings' getters are safe from any thread, as the TCI
 *  server uses them) and tells the panel clients what changed every 50 ms. The apps: the FT8
 *  list's rows go out as they come ("ft8r {...}", the last 48 to a page that opens later), the
 *  app's state when it changes ("ft8 {...}"), the CW decoder's text ("cw \"...\""); "ft8
 *  open|close|cq|stop|auto|hold|mode", "ft8 txfreq HZ", "ft8 reply ID" work the app, "log
 *  CALL|RSTS|RSTR|NAME" writes the day's log as the CW keyboard does ("logged ok|no"). "logq ID
 *  CALL|..." is the same with a number the page chose: the answer is "logged ID ok|no", kept for
 *  the last 64, and "logq? ID CALL|..." asks for it again after a dropped connection or a reload,
 *  so a QSO sent into a gap is found ("ok", "no", "pending" while the GUI still has it) or declared
 *  "unknown", never logged twice by a retry (R6). A receipt is bound to its QSO: the same number
 *  with another QSO is answered "conflict", never with the other QSO's answer (audit C03).
 *
 *  The Origin check of ws_handshake() keeps pages from other sites out; like the TCI server there
 *  is no login: the radio's own network is trusted.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* accept4, pipe2 */

#include <stdio.h>       /* before jpeglib.h, which wants FILE */

#include "remote_server.h"
#include "control_owner.h"
#include "front_keys.h"
#include "msg_box.h"
#include "board.h"

#include "band_plan.h"
#include "cfg/cfg_api.h"
#include "cluster/client.h"
#include "cluster/spots.h"
#include "dialog_field.h"
#include "dsp.h"
#include "ft8_window.h"
#include "lv_drivers/display/drm.h"
#include "main.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "screen_main.h"
#include "smeter.h"
#include "tci_ws.h"
#include "tx_meters.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <linux/input.h>
#include <math.h>
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
/* The picture a viewer asks for ("picture sharp|balanced|fast"): JPEG quality, and half size. A whole 800x480
 * frame is 40-80 kB with a busy waterfall, so a 1-2 Mbit/s WiFi link carries about 3 a second; half size at q60
 * is a quarter of that (issue #6) */
enum { PIC_SHARP, PIC_BALANCED, PIC_FAST, PIC_N };
static const struct { const char *name; int quality; bool half; } pics[PIC_N] = {
    {"sharp", 70, false}, {"balanced", 50, false}, {"fast", 60, true},
};
#define MAX_HELD    8
#define PANEL_MS    50              /* the state is read this often for the panel clients */
#define METER_MS    100
#define SPOTS_MS    3000            /* a busy cluster changes every second: the list goes out this often at most */
#define SPOT_MAX    80              /* the newest spots the page gets */
#define PANEL_ROOM  (16 * 1024)     /* a spectrum frame waits while this much is still on its way */
#define FT8_MS      250             /* the FT8 app's state is read this often */
#define FT8_RING    48              /* rows of the FT8 list kept for a page that opens later */
#define CW_HIST     1024            /* the CW decoder's last characters, for the same */

/* The input devices: what the GUI reads (gui_main.c inputs_open), board.h */
#define DEV_KEYPAD BOARD_DEV_KEYPAD
#define DEV_MAIN   BOARD_DEV_MAIN
#define DEV_VOL    BOARD_DEV_VOL
#define DEV_MFK    BOARD_DEV_MFK
#define DEV_POWER  BOARD_DEV_POWER

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
    bool            screen;         /* wants the screen frames (the Remote page) */
    uint8_t         pic;            /* PIC_*: the picture it asked for */
    bool            screen_stale;   /* a screen change it has not been sent yet */
    uint64_t        screen_ms;      /* its last frame */
    bool            panel;          /* wants the state, the spectrum and the spots (the Panel page) */
    bool            panel_fresh;    /* just subscribed: the whole state goes out next */
    atomic_bool     state_due;      /* a "set" refused: the state goes out to it next, changed or not */
    int             sess;           /* the session it authenticated as, -1 none (a viewer) */
    uint8_t         role;           /* ROLE_VIEW, ROLE_OPERATE, ROLE_ADMIN */
    bool            tx_ok;          /* the session may transmit */
    uint64_t        ptt_ms;         /* the PTT key held: its last renewal, and when it began */
    uint64_t        ptt_start_ms;
    uint32_t        ptt_hold;       /* the page's number for its last PTT press: renewed while held, then over */
    uint32_t        ctl_told;       /* the control version last told ("ctl"), 0 never */
} client_t;

/* Sessions: a paired browser, remembered in REMOTE_SESSIONS until revoked */
enum { ROLE_VIEW = 0, ROLE_OPERATE = 1, ROLE_ADMIN = 2 };
#define SESS_MAX     16
#define TOKEN_LEN    32
#define PAIR_MS      120000   /* the code stays good this long */
#define PAIR_SHOW_MS 15000    /* and shows on the screen this long */
#define PAIR_TRIES   3
#define PTT_LEASE_MS 3000     /* a held PTT without renewal */
#define PTT_LIMIT_MS (180 * 1000)

typedef struct {
    char     token[TOKEN_LEN + 1];
    char     name[32];
    uint8_t  role;
    bool     tx;
    uint64_t created, last;   /* wall clock, seconds */
    char     ip[48];
} session_t;

static session_t       sessions[SESS_MAX];
static int             nsessions;
static pthread_mutex_t sess_mux = PTHREAD_MUTEX_INITIALIZER;
static char            pair_code[8];
static uint64_t        pair_until_ms;
static atomic_uint_fast64_t pair_hide_ms;   /* until then the mirror is withheld from the unpaired (audit B02) */
static int             pair_tries;
static atomic_bool     tx_last;        /* the TX state last told to the clients */
static atomic_bool     disconnect_all_due;   /* Settings: every socket closed, the sessions kept */
static atomic_bool     forget_all_due;       /* Settings: every session revoked */

/* What the panel clients are told; read from the radio every PANEL_MS */
typedef struct {
    int32_t freq, mode, flo, fhi, kt, agc, pre, att, nb, nr, sql, rfg, vol, pwr10, step, zoom, vfo, atu, bat;
    bool    tx, lock;
} pstate_t;

/* A change asked by a panel client, for the LVGL thread */
typedef enum { P_FREQ, P_MODE, P_AGC, P_PRE, P_ATT, P_NB, P_NR, P_SQL, P_RFG, P_VOL, P_PWR, P_STEP, P_ZOOM, P_BAND,
               P_VFO, P_FILTER, P_ATU, P_LOCK } pcmd_id_t;

typedef struct {
    pcmd_id_t id;
    int32_t   a, b;
} pcmd_t;

typedef struct {
    uint16_t w, h;
    uint32_t seq;
} frame_hdr_t;

static client_t    clients[MAX_CLIENTS];
static atomic_bool enabled = true;
static atomic_int  port    = REMOTE_DEFAULT_PORT;
static atomic_int  nclients;
static atomic_int  npanel;          /* clients on the panel feed */
static atomic_bool spots_resend;    /* a new panel client: the spots go out again */
static atomic_bool state_resend;    /* a "set" dropped on the LVGL thread: the state goes out to all again */
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

/* The keys a page may press, with the codes this board's keypad reports for them (front_keys.c
 * decodes the same table the other way): the X6100's codes on the X6200 pressed MSG for APP,
 * ATU for PRE and so on (audit B14). "keys" after "size" tells a page which it has. */
static const struct {
    const char *name;
    uint16_t    code;
} keys[] = {
#ifdef BOARD_X6200
    {"POWER", KEY_POWER},
    {"GEN", BTN_TRIGGER_HAPPY1},     {"APP", BTN_TRIGGER_HAPPY2},      {"KEY", BTN_TRIGGER_HAPPY3},
    {"PTT", BTN_TRIGGER_HAPPY4},     {"BAND_DOWN", BTN_TRIGGER_HAPPY5}, {"BAND_UP", BTN_TRIGGER_HAPPY6},
    {"MSG", BTN_TRIGGER_HAPPY7},     {"DFN", BTN_TRIGGER_HAPPY8},      {"DFL", BTN_TRIGGER_HAPPY9},
    {"AM", BTN_TRIGGER_HAPPY10},     {"CW", BTN_TRIGGER_HAPPY11},      {"SSB", BTN_TRIGGER_HAPPY12},
    {"F1", BTN_TRIGGER_HAPPY13},     {"F2", BTN_TRIGGER_HAPPY14},      {"F3", BTN_TRIGGER_HAPPY15},
    {"AB", BTN_TRIGGER_HAPPY16},     {"ATU", BTN_TRIGGER_HAPPY17},     {"FST", BTN_TRIGGER_HAPPY18},
    {"F4", BTN_TRIGGER_HAPPY19},     {"F5", BTN_TRIGGER_HAPPY20},      {"VOL", BTN_TRIGGER_HAPPY21},
    {"MFK", BTN_TRIGGER_HAPPY27},
#else
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
#endif
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

/* ---- Sessions (R3) -------------------------------------------------------------------- */

static const char *role_name(uint8_t r) {
    return r == ROLE_ADMIN ? "admin" : r == ROLE_OPERATE ? "operate" : "view";
}

static uint64_t wall_s(void) {
    return (uint64_t)time(NULL);
}

/* sess_mux held */
static void sess_save_locked(void) {
    char  tmp[80];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", REMOTE_SESSIONS);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || !(f = fdopen(fd, "w"))) {
        if (fd >= 0)
            close(fd);
        return;
    }
    for (int i = 0; i < nsessions; i++)
        fprintf(f, "%s\t%s\t%d\t%d\t%llu\t%llu\t%s\n", sessions[i].token, sessions[i].name, sessions[i].role,
                sessions[i].tx ? 1 : 0, (unsigned long long)sessions[i].created, (unsigned long long)sessions[i].last,
                sessions[i].ip);
    fclose(f);
    rename(tmp, REMOTE_SESSIONS);
}

static void sess_load(void) {
    FILE *f = fopen(REMOTE_SESSIONS, "r");
    char  line[256];

    if (!f)
        return;
    pthread_mutex_lock(&sess_mux);
    nsessions = 0;
    while (nsessions < SESS_MAX && fgets(line, sizeof(line), f)) {
        session_t *x = &sessions[nsessions];
        char      *save, *tok;
        int        k = 0;
        memset(x, 0, sizeof(*x));
        for (tok = strtok_r(line, "\t\r\n", &save); tok; tok = strtok_r(NULL, "\t\r\n", &save), k++) {
            switch (k) {
                case 0: snprintf(x->token, sizeof(x->token), "%s", tok); break;
                case 1: snprintf(x->name, sizeof(x->name), "%s", tok); break;
                case 2: x->role = (uint8_t)LV_CLAMP(0, atoi(tok), 2); break;
                case 3: x->tx = atoi(tok) != 0; break;
                case 4: x->created = strtoull(tok, NULL, 10); break;
                case 5: x->last = strtoull(tok, NULL, 10); break;
                case 6: snprintf(x->ip, sizeof(x->ip), "%s", tok); break;
            }
        }
        if (strlen(x->token) == TOKEN_LEN && x->name[0])
            nsessions++;
    }
    pthread_mutex_unlock(&sess_mux);
    fclose(f);
}

static bool random_hex(char *out, size_t n) {
    uint8_t raw[32];
    int     fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd < 0 || n / 2 > sizeof(raw) || read(fd, raw, n / 2) != (ssize_t)(n / 2)) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    close(fd);
    for (size_t i = 0; i < n / 2; i++)
        snprintf(out + i * 2, 3, "%02x", raw[i]);
    return true;
}

/* A new session for a browser that typed the code; -1 when the table is full */
static int sess_new(const char *name, const char *ip) {
    pthread_mutex_lock(&sess_mux);
    if (nsessions == SESS_MAX || !random_hex(sessions[nsessions].token, TOKEN_LEN)) {
        pthread_mutex_unlock(&sess_mux);
        return -1;
    }
    session_t *x = &sessions[nsessions];
    snprintf(x->name, sizeof(x->name), "%s", name);
    x->role    = nsessions == 0 ? ROLE_ADMIN : ROLE_OPERATE;   /* the first browser paired is the admin */
    x->tx      = true;
    x->created = x->last = wall_s();
    snprintf(x->ip, sizeof(x->ip), "%s", ip);
    int i = nsessions++;
    sess_save_locked();
    pthread_mutex_unlock(&sess_mux);
    return i;
}

static int sess_find(const char *token) {
    int r = -1;

    pthread_mutex_lock(&sess_mux);
    for (int i = 0; i < nsessions; i++)
        if (!strcmp(sessions[i].token, token))
            r = i;
    pthread_mutex_unlock(&sess_mux);
    return r;
}

static void sess_touch(int i, const char *ip) {
    pthread_mutex_lock(&sess_mux);
    if (i >= 0 && i < nsessions) {
        sessions[i].last = wall_s();
        snprintf(sessions[i].ip, sizeof(sessions[i].ip), "%s", ip);
        sess_save_locked();
    }
    pthread_mutex_unlock(&sess_mux);
}

static void client_close(client_t *c);
static void client_text(client_t *c, const char *text);
static void client_textf(client_t *c, const char *fmt, ...);

/* The session is gone: its token, and every socket that holds it (socket thread) */
static void sess_revoke(int i) {
    for (int k = 0; k < MAX_CLIENTS; k++) {
        client_t *c = &clients[k];
        if (c->fd >= 0 && c->sess == i) {
            client_text(c, "err session revoked");
            client_close(c);
        }
    }
    pthread_mutex_lock(&sess_mux);
    if (i >= 0 && i < nsessions) {
        memmove(&sessions[i], &sessions[i + 1], (nsessions - i - 1) * sizeof(sessions[0]));
        nsessions--;
        sess_save_locked();
    }
    pthread_mutex_unlock(&sess_mux);
    for (int k = 0; k < MAX_CLIENTS; k++)      /* the ones after it moved down */
        if (clients[k].fd >= 0 && clients[k].sess > i)
            clients[k].sess--;
}

/* LVGL thread: the code on the radio's screen */
static void pair_msg_apply(void *arg) {
    (void)arg;
    pthread_mutex_lock(&sess_mux);
    char code[8];
    snprintf(code, sizeof(code), "%s", pair_code);
    pthread_mutex_unlock(&sess_mux);
    msg_schedule_text_ms(PAIR_SHOW_MS, "Pair code: #ffb020 %s#  (2 min)", code);
}

/* A code, new unless one is still good; shown on the radio's screen. Any thread. */
static void pair_show(char *out, size_t n) {
    pthread_mutex_lock(&sess_mux);
    if (!pair_code[0] || now_ms() > pair_until_ms) {
        uint8_t raw[4] = {0};
        int     fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (read(fd, raw, sizeof(raw)) < 0) {}
            close(fd);
        }
        uint32_t v = (raw[0] | raw[1] << 8 | raw[2] << 16 | (uint32_t)raw[3] << 24) % 1000000;
        snprintf(pair_code, sizeof(pair_code), "%06u", v);
        pair_tries = 0;
    }
    pair_until_ms = now_ms() + PAIR_MS;
    if (out)
        snprintf(out, n, "%s", pair_code);
    pthread_mutex_unlock(&sess_mux);
    /* The code is for the eyes at the radio: the screen mirror, which anyone on the network may
     * watch before pairing, holds its frames from the unpaired while the code shows (audit B02) */
    atomic_store(&pair_hide_ms, now_ms() + PAIR_SHOW_MS + 2000);
    scheduler_put_noargs(pair_msg_apply);
}

/* The code typed: a session, or -1 (wrong, or no code; three wrong tries burn the code) */
static int pair_try(const char *code, const char *name, const char *ip) {
    bool ok;

    pthread_mutex_lock(&sess_mux);
    ok = pair_code[0] && now_ms() <= pair_until_ms && !strcmp(code, pair_code);
    if (ok || ++pair_tries >= PAIR_TRIES) {
        pair_code[0]  = 0;
        pair_until_ms = 0;
    }
    pthread_mutex_unlock(&sess_mux);
    return ok ? sess_new(name, ip) : -1;
}

static void wake(void) {
    if (wake_fd[1] >= 0 && write(wake_fd[1], "w", 1) < 0) {}
}

/* ---- Input devices ------------------------------------------------------------------ */

/* -1 for a device the board does not have (NULL in board.h) */
static int dev_open(const char *path) {
    return path ? open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC) : -1;
}

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

static void power_cb(void *arg) {
    keypad_power_set(*(bool *)arg);
}

/* The POWER key where there is no device for it (the X6200), as radio.c posts the base's: told until its job is
 * in. A release dropped by a full queue left the press alone, and its hold timer (front_keys.c, KEYPAD_LONG)
 * powered the radio off: a click became a power-off. power_told: what the GUI was told; the server loop tries
 * again. A press that cannot go in and is let go before there is room is not told at all. Socket thread */
static bool power_down, power_told;

static void power_post(void) {
    if (power_down != power_told) {
        bool down = power_down;

        if (scheduler_put_urgent(power_cb, &down, sizeof(down)))
            power_told = down;
    }
}

static void key_emit(uint16_t code, bool down) {
    ctl_injected();
    if (code == KEY_POWER && dev_power < 0) {
        power_down = down;
        power_post();
        return;
    }
    emit(code == KEY_POWER ? dev_power : dev_keypad, EV_KEY, code, down ? 1 : 0);
}

static int key_code(const char *name) {
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strcasecmp(keys[i].name, name) == 0)
            return keys[i].code;
    return -1;
}

/* A knob turned n detents (signed, positive clockwise): one event per detent, as the hardware
 * reports them. The main knob counts right turns as positive, VOL and MFK count left turns as
 * positive (knob_device.c, mfk_device.c): their events go out the other way round */
static bool knob_emit(const char *name, int n) {
    int      fd, axis, dir;

    if (!strcasecmp(name, "main"))
        fd = dev_main, axis = REL_X, dir = 1;
    else if (!strcasecmp(name, "vol"))
        fd = dev_vol, axis = REL_Y, dir = -1;
    else if (!strcasecmp(name, "mfk"))
        fd = dev_mfk, axis = REL_Z, dir = -1;
    else
        return false;
    if (n > 50) n = 50;
    if (n < -50) n = -50;
    ctl_injected();
    for (int i = 0; i < abs(n); i++)
        emit(fd, EV_REL, axis, n > 0 ? dir : -dir);
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
    bool left = c->out_len > 0;
    pthread_mutex_unlock(&c->lock);
    /* What the socket did not take now goes as soon as it can: the server thread asks for POLLOUT only when it
     * builds its poll set, and slept up to 100 ms with the end of a frame waiting */
    if (left)
        wake();
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
    ctl_release(CTL_BROWSER, (int)(c - clients));
    pthread_mutex_lock(&c->lock);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        atomic_fetch_sub(&nclients, 1);
    }
    if (c->panel)
        atomic_fetch_sub(&npanel, 1);
    c->up      = false;
    c->panel   = false;
    c->out_len = c->in_len = c->text_len = 0;
    pthread_mutex_unlock(&c->lock);
}

/* ---- The panel: the radio's state, the spectrum, the spots ---------------------------- */

/* A mode of this radio: SAM and WFM only where the base has them (audit B15) */
static bool mode_supported(int32_t m) {
    return m >= 0 && m <= radio_mode_last && (m < radio_mode_sam || (radio_caps() & HW_CAP_SAM_WFM));
}

/* LVGL thread */
static void panel_apply(void *arg) {
    pcmd_t *p = (pcmd_t *)arg;

    switch (p->id) {
        case P_FREQ:
            backlight_tick();
            if (!main_screen_freq_locked() && cfg_is_valid_hw_freq(p->a))
                main_screen_set_freq((uint64_t)p->a);
            else
                atomic_store(&state_resend, true);  /* locked since panel_set looked: no page keeps what it asked (C13) */
            break;
        case P_MODE:
            if (!main_screen_mode_locked() && mode_supported(p->a))
                cparam_i_set(cfg_cur_mode, p->a);
            else
                atomic_store(&state_resend, true);
            break;
        case P_AGC:    cparam_i_set(cfg_cur_agc, LV_CLAMP(0, p->a, 3)); break;
        case P_PRE:    cparam_i_set(cfg_cur_pre, p->a != 0); break;
        case P_ATT:    cparam_i_set(cfg_cur_att, p->a != 0); break;
        case P_NB:     param_i_set(cfg_nb, p->a != 0); break;
        case P_NR:     param_i_set(cfg_nr, p->a != 0); break;
        case P_SQL:    param_i_set(cfg_squelch, LV_CLAMP(0, p->a, 100)); break;
        case P_RFG:    param_i_set(cfg_rfgain, LV_CLAMP(0, p->a, 100)); break;
        case P_VOL:    param_i_set(cfg_volume, LV_CLAMP(0, p->a, BOARD_VOL_MAX)); break;
        case P_PWR:    cfg_pwr_set(LV_CLAMP(1, p->a, (int)(BOARD_PWR_MAX * 10)) / 10.0f); break;
        case P_STEP:   param_i_set(cfg_mode_freq_step, LV_CLAMP(1, p->a, 10000)); break;
        case P_ZOOM:   param_i_set(cfg_mode_zoom, LV_CLAMP(1, p->a, 8)); break;
        case P_BAND:
            if (!main_screen_band_locked())
                cfg_band_load_next(p->a > 0);
            else
                atomic_store(&state_resend, true);
            break;
        case P_VFO:    cfg_vfo_toggle(); break;
        case P_FILTER:
            if (p->b > cparam_i_get(cfg_cur_filter_high)) {
                cparam_i_set(cfg_cur_filter_high, p->b);
                cparam_i_set(cfg_cur_filter_low, p->a);
            } else {
                cparam_i_set(cfg_cur_filter_low, p->a);
                cparam_i_set(cfg_cur_filter_high, p->b);
            }
            break;
        case P_ATU:    param_i_set(cfg_atu_enabled, p->a != 0); break;
        case P_LOCK:   main_screen_lock_freq(p->a != 0); break;
    }
}

static bool panel_put(pcmd_id_t id, int32_t a, int32_t b) {
    pcmd_t p = {.id = id, .a = a, .b = b};
    return scheduler_put(panel_apply, &p, sizeof(p));
}

/* A "set" refused, in words; and the state goes out to the client again, changed or not, so a page that showed
 * what it asked for shows the radio's again (audit C13) */
static void set_refused(client_t *c, const char *why) {
    client_textf(c, "err %s", why);
    atomic_store(&c->state_due, true);
}

/* "set NAME VALUE [VALUE2]", for the LVGL thread. What panel_apply would drop without a word (the frequency
 * locked, by LOCK or an app, or one this radio does not tune) is refused here first; one locked in between is
 * still dropped there, with the state to every page again (state_resend) */
static void panel_set(client_t *c, const char *name, const char *v1, const char *v2) {
    static const struct {
        const char *name;
        pcmd_id_t   id;
    } names[] = {
        {"freq", P_FREQ}, {"mode", P_MODE}, {"agc", P_AGC},   {"pre", P_PRE},   {"att", P_ATT},
        {"nb", P_NB},     {"nr", P_NR},     {"sql", P_SQL},   {"rfg", P_RFG},   {"vol", P_VOL},
        {"pwr", P_PWR},   {"step", P_STEP}, {"zoom", P_ZOOM}, {"band", P_BAND}, {"vfo", P_VFO},
        {"filter", P_FILTER}, {"atu", P_ATU}, {"lock", P_LOCK},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(names[i].name, name))
            continue;
        int32_t a = v1 ? (int32_t)strtol(v1, NULL, 10) : 0;
        int32_t b = v2 ? (int32_t)strtol(v2, NULL, 10) : 0;
        if (names[i].id == P_FILTER) {
            a = abs(a), b = abs(b);
            if (a > b) { int32_t t = a; a = b; b = t; }
        }
        if (names[i].id == P_MODE && !mode_supported(a))
            set_refused(c, "mode: not on this radio");
        else if (names[i].id == P_FREQ && main_screen_freq_locked())
            set_refused(c, "freq: locked (LOCK, or an app holds the frequency)");
        else if (names[i].id == P_FREQ && !cfg_is_valid_hw_freq(a))
            set_refused(c, "freq: not a frequency this radio tunes");
        else if (!panel_put(names[i].id, a, b)) {
            char why[48];                       /* named as the others: a page takes "busy..." for its PTT's refusal */
            snprintf(why, sizeof(why), "%s: the radio is busy, try again", names[i].name);
            set_refused(c, why);
        }
        return;
    }
    client_textf(c, "err unknown setting %s", name);
    atomic_store(&c->state_due, true);
}

/* Any thread */
static void pstate_read(pstate_t *s) {
    s->freq  = cparam_i_get(cfg_fg_freq);
    s->mode  = cparam_i_get(cfg_cur_mode);
    s->flo   = cparam_i_get(cfg_cur_filter_low);
    s->fhi   = cparam_i_get(cfg_cur_filter_high);
    s->kt    = param_i_get(cfg_key_tone);       /* CW: the filter edges are tones, the band is around the dial */
    s->agc   = cparam_i_get(cfg_cur_agc);
    s->pre   = cparam_i_get(cfg_cur_pre);
    s->att   = cparam_i_get(cfg_cur_att);
    s->nb    = param_i_get(cfg_nb);
    s->nr    = param_i_get(cfg_nr);
    s->sql   = param_i_get(cfg_squelch);
    s->rfg   = param_i_get(cfg_rfgain);
    s->vol   = param_i_get(cfg_volume);
    s->pwr10 = (int32_t)lroundf(param_f_get(cfg_pwr) * 10.0f);
    s->step  = param_i_get(cfg_mode_freq_step);
    s->zoom  = param_i_get(cfg_mode_zoom);
    s->vfo   = param_i_get(cfg_band_current_vfo);
    s->atu   = param_i_get(cfg_atu_enabled);
    s->bat   = radio_get_battery();
    s->tx    = radio_get_state() != RADIO_RX || radio_get_modem();
    s->lock  = main_screen_freq_locked();
}

static void pstate_line(char *buf, size_t n, const pstate_t *s) {
    snprintf(buf, n,
             "state {\"freq\":%d,\"mode\":%d,\"flo\":%d,\"fhi\":%d,\"kt\":%d,\"agc\":%d,\"pre\":%d,\"att\":%d,"
             "\"nb\":%d,\"nr\":%d,\"sql\":%d,\"rfg\":%d,\"vol\":%d,\"pwr\":%d.%d,\"step\":%d,\"zoom\":%d,"
             "\"vfo\":%d,\"atu\":%d,\"bat\":%d,\"tx\":%d,\"lock\":%d}",
             s->freq, s->mode, s->flo, s->fhi, s->kt, s->agc, s->pre, s->att, s->nb, s->nr, s->sql, s->rfg, s->vol,
             s->pwr10 / 10, s->pwr10 % 10, s->step, s->zoom, s->vfo, s->atu, s->bat, s->tx, s->lock);
}

/* A string into JSON: quotes and backslashes escaped, control characters dropped */
/* The length of the well-formed UTF-8 sequence at s, 0 for none (an overlong form, a surrogate,
 * a stray continuation byte, a Latin-1 byte: a cluster comment arrives in any encoding) */
static int utf8_seq(const unsigned char *s) {
    unsigned char c = s[0], lo = 0x80, hi = 0xBF;
    int           len;

    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        len = 2;
    else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0)
            lo = 0xA0;
        else if (c == 0xED)
            hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0)
            lo = 0x90;
        else if (c == 0xF4)
            hi = 0x8F;
    } else
        return 0;
    if (s[1] < lo || s[1] > hi)
        return 0;
    for (int i = 2; i < len; i++)
        if ((s[i] & 0xC0) != 0x80)
            return 0;
    return len;
}

/* A JSON string, and valid UTF-8 whatever the source: a browser drops the WebSocket on a text frame
 * it can't decode. A byte outside a well-formed sequence becomes '?', a control character is left out */
static size_t json_str(char *out, size_t n, const char *s) {
    size_t k = 0;
    if (n < 3)
        return 0;
    out[k++] = '"';
    while (*s && k + 3 < n) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out[k++] = '\\';
            out[k++] = (char)ch;
            s++;
        } else if (ch < 0x20) {
            s++;
        } else if (ch < 0x80) {
            out[k++] = (char)ch;
            s++;
        } else {
            int len = utf8_seq((const unsigned char *)s);
            if (!len) {
                out[k++] = '?';
                s++;
            } else {
                if (k + len + 2 > n)    /* the sequence, the closing quote and the end: all or nothing */
                    break;
                memcpy(out + k, s, len);
                k += len;
                s += len;
            }
        }
    }
    out[k++] = '"';
    out[k]   = 0;
    return k;
}

static uint16_t utc_now_min(void) {
    time_t    now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
}

static void panel_broadcast(const char *s) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].up && clients[i].panel)
            client_text(&clients[i], s);
}

/* The bands and the band plan: to one client, when it subscribes */
static void panel_hello(client_t *c) {
    static int32_t start[32], stop[32];
    static char    names[32][16];
    static char    buf[4096];       /* the plan with bands of the band list added: more than 2 KB */
    size_t         k = 0;

    client_textf(c, "modes %d", radio_caps() & HW_CAP_SAM_WFM ? radio_mode_last + 1 : radio_mode_sam);
    client_textf(c, "volmax %d", BOARD_VOL_MAX);
    client_textf(c, "pwrmax %d", (int)(BOARD_PWR_MAX * 10));   /* in tenths of a watt, as set pwr */
    int n = cfg_bands_active(start, stop, names, 32);
    k += snprintf(buf + k, sizeof(buf) - k, "bands [");
    for (int i = 0; i < n && k < sizeof(buf) - 64; i++) {
        k += snprintf(buf + k, sizeof(buf) - k, "%s{\"n\":", i ? "," : "");
        k += json_str(buf + k, sizeof(buf) - k, names[i]);
        k += snprintf(buf + k, sizeof(buf) - k, ",\"a\":%d,\"b\":%d}", start[i], stop[i]);
    }
    snprintf(buf + k, sizeof(buf) - k, "]");
    client_text(c, buf);

    k = snprintf(buf, sizeof(buf), "plan [");
    int32_t a, b;
    int     type;
    for (int i = 0; band_plan_segment(i, &a, &b, &type) && k < sizeof(buf) - 48; i++)
        k += snprintf(buf + k, sizeof(buf) - k, "%s[%d,%d,%d]", i ? "," : "", a, b, type);
    snprintf(buf + k, sizeof(buf) - k, "]");
    client_text(c, buf);
}

/* The spots the radio shows (its own filters), newest last, as the page needs them */
static void panel_spots(uint32_t *gen, bool force) {
    static cluster_spot_t   spots[CLUSTER_MAX_SPOTS];
    static cluster_filter_t flt;
    static time_t           flt_mtime = -1;
    static char             buf[SPOT_MAX * 140 + 32];
    static const char *const mode_cls[] = {"", "CW", "SSB", "", "DIG", "", "", "", "AM"};

    uint32_t g = *gen;
    int      n = cluster_client_snapshot(spots, CLUSTER_MAX_SPOTS, &g);
    if (n < 0) {
        if (!force)
            return;
        g = *gen - 1;       /* not the store's generation: the copy is made */
        n = cluster_client_snapshot(spots, CLUSTER_MAX_SPOTS, &g);
        if (n < 0)
            n = 0;
    }
    *gen = g;
    time_t m = cluster_config_mtime();
    if (m != flt_mtime) {
        flt_mtime = m;
        cluster_filter_load(&flt);
    }
    uint16_t now = utc_now_min();
    size_t   k   = snprintf(buf, sizeof(buf), "spots [");
    int      sent = 0;
    for (int i = n - 1; i >= 0 && sent < SPOT_MAX; i--) {     /* newest first */
        const cluster_spot_t *s = &spots[i];
        if (!cluster_filter_pass(&flt, s, now))
            continue;
        if (k + 160 > sizeof(buf))
            break;
        uint8_t cls = cluster_spot_mode_class(s);
        k += snprintf(buf + k, sizeof(buf) - k, "%s{\"f\":%d,\"c\":", sent ? "," : "", s->freq_hz);
        k += json_str(buf + k, sizeof(buf) - k, s->dx);
        k += snprintf(buf + k, sizeof(buf) - k, ",\"m\":\"%s\",\"a\":%u,\"s\":", cls < 9 ? mode_cls[cls] : "",
                      cluster_age_min(s->utc_min, now));
        k += json_str(buf + k, sizeof(buf) - k, s->spotter);
        k += snprintf(buf + k, sizeof(buf) - k, ",\"k\":");
        k += json_str(buf + k, sizeof(buf) - k, s->comment);
        k += snprintf(buf + k, sizeof(buf) - k, ",\"al\":%d}", cluster_alert_match(&flt, s) >= 0);
        sent++;
    }
    snprintf(buf + k, sizeof(buf) - k, "]");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        if (c->fd >= 0 && c->up && c->panel)
            client_frame(c, WS_OP_TEXT, buf, strlen(buf), NULL, 0);
    }
}

/* ---- The apps: the FT8 list, the CW decoder, the log ----------------------------------- */

static pthread_mutex_t app_mux = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t    id;
    cell_data_t cell;
} ft8_ring[FT8_RING];
static uint32_t    ft8_ids;         /* rows seen; a row's slot is id % FT8_RING */
static char        cw_new[512];     /* decoded since the last flush */
static size_t      cw_new_len;
static char        cw_hist[CW_HIST + 1];
static size_t      cw_hist_len;
static uint32_t    boot_id;         /* this GUI run, told in "hello" */
#define LOG_ANSWERS 64
static struct {                     /* the last log commands' answers: id 0 is the old "log" without one */
    uint64_t who;                   /* the session that sent it: older pages count from 1 (audit B07) */
    uint64_t id;                    /* the page's number: random, 53 bits, from any tab of the browser (C03) */
    uint64_t sum;                   /* its QSO's text, hashed: the receipt is this QSO's only (audit C03) */
    uint8_t  result;                /* 0 still with the GUI thread, 1 logged, 2 refused */
    bool     told;                  /* gone out to the session's panel clients */
} log_ans[LOG_ANSWERS];
static int         log_ans_next;
static atomic_bool log_pending;     /* an answer waits to go out */

/* A row as JSON: "ft8r {...}" */
static void ft8_row_line(char *buf, size_t n, uint32_t id, const cell_data_t *cd) {
    size_t k = snprintf(buf, n, "ft8r {\"id\":%u,\"t\":%d,\"x\":", id, (int)cd->cell_type);
    k += json_str(buf + k, n - k, cd->text);
    k += snprintf(buf + k, n - k, ",\"c\":");
    k += json_str(buf + k, n - k, cd->meta.call_de);
    k += snprintf(buf + k, n - k, ",\"g\":");
    k += json_str(buf + k, n - k, cd->meta.grid);
    k += snprintf(buf + k, n - k, ",\"f\":%d,\"snr\":%d,\"me\":%d,\"w\":%d,\"d\":%d,\"p\":%d,\"odd\":%d,\"k\":",
                  (int)(cd->meta.freq_hz + 0.5f), cd->meta.local_snr, cd->meta.to_me, (int)cd->worked_type, cd->dist,
                  cd->pota, cd->odd);
    k += json_str(buf + k, n - k, cd->country);
    snprintf(buf + k, n - k, "}");
}

/* LVGL thread (decode_list.c): kept, and to the panel clients at once */
void remote_server_ft8_row(const void *cell) {
    const cell_data_t *cd = cell;
    char               line[512];
    uint32_t           id;

    pthread_mutex_lock(&app_mux);
    id = ++ft8_ids;
    ft8_ring[id % FT8_RING].id   = id;
    ft8_ring[id % FT8_RING].cell = *cd;
    pthread_mutex_unlock(&app_mux);
    if (!atomic_load(&npanel))
        return;
    ft8_row_line(line, sizeof(line), id, cd);
    panel_broadcast(line);
}

/* Any thread (the CW decoder's) */
void remote_server_cw_text(const char *text) {
    size_t n = strlen(text);

    pthread_mutex_lock(&app_mux);
    if (cw_new_len + n < sizeof(cw_new)) {
        memcpy(cw_new + cw_new_len, text, n);
        cw_new_len += n;
    }
    if (n < CW_HIST) {
        if (cw_hist_len + n > CW_HIST) {        /* the oldest make room */
            size_t drop = cw_hist_len + n - CW_HIST;
            memmove(cw_hist, cw_hist + drop, cw_hist_len - drop);
            cw_hist_len -= drop;
        }
        memcpy(cw_hist + cw_hist_len, text, n);
        cw_hist_len += n;
    }
    pthread_mutex_unlock(&app_mux);
}

/* The rows kept and the CW text so far: to a client that just subscribed */
static void panel_app_history(client_t *c) {
    char line[CW_HIST * 2 + 16];

    pthread_mutex_lock(&app_mux);
    uint32_t first = ft8_ids > FT8_RING ? ft8_ids - FT8_RING + 1 : 1;
    for (uint32_t id = first; id <= ft8_ids; id++) {
        if (ft8_ring[id % FT8_RING].id != id)
            continue;
        ft8_row_line(line, 512, id, &ft8_ring[id % FT8_RING].cell);
        client_text(c, line);
    }
    if (cw_hist_len) {
        char text[CW_HIST + 1];
        memcpy(text, cw_hist, cw_hist_len);
        text[cw_hist_len] = 0;
        size_t k = snprintf(line, sizeof(line), "cw ");
        json_str(line + k, sizeof(line) - k, text);
        client_text(c, line);
    }
    pthread_mutex_unlock(&app_mux);
}

/* New CW text to everyone, from the panel thread */
static void panel_cw_flush(void) {
    char text[512], line[1100];

    pthread_mutex_lock(&app_mux);
    size_t n = cw_new_len;
    memcpy(text, cw_new, n);
    text[n]    = 0;
    cw_new_len = 0;
    pthread_mutex_unlock(&app_mux);
    if (!n)
        return;
    size_t k = snprintf(line, sizeof(line), "cw ");
    json_str(line + k, sizeof(line) - k, text);
    panel_broadcast(line);
}

static void ft8_status_line(char *buf, size_t n, const ft8_status_t *s) {
    size_t k = snprintf(buf, n, "ft8 {\"open\":%d,\"proto\":%d,\"cq\":%d,\"tx\":%d,\"auto\":%d,\"hold\":%d,\"sending\":%d,\"txf\":%d,\"next\":",
                        s->open, s->proto, s->cq, s->tx, s->auto_on, s->hold, s->sending, s->tx_freq);
    k += json_str(buf + k, n - k, s->next);
    k += snprintf(buf + k, n - k, ",\"in\":%d,\"slot\":%d,\"rep\":%d,\"qso\":%d,\"hound\":%d,\"nocall\":%d,\"logged\":",
                  s->in_ms, s->slot_ms, s->repeats, s->qso, s->hound, s->nocall);
    k += json_str(buf + k, n - k, s->logged);
    snprintf(buf + k, n - k, ",\"logged_s\":%d}", s->logged_s);
}

/* What the panel asks of the apps, on the LVGL thread */
typedef enum { A_FT8_OPEN, A_FT8_CLOSE, A_FT8, A_LOG } acmd_id_t;

typedef struct {
    acmd_id_t   id;
    int         what;       /* ft8_rc_t */
    int32_t     arg;
    bool        has_cell;
    cell_data_t cell;
    char        text[200];
    uint64_t    log_id;     /* A_LOG: the page's number for its answer, 0 for none */
    uint64_t    who;        /* A_LOG: the session it came from */
    uint64_t    sum;        /* A_LOG: its text as sent, hashed (log_sum) */
} acmd_t;

/* The session a client logs under: its token hashed (FNV-1a); the one value for the unpaired */
static uint64_t sess_key(const client_t *c) {
    uint64_t h = 1469598103934665603ULL;

    pthread_mutex_lock(&sess_mux);
    if (c->sess >= 0 && c->sess < nsessions)
        for (const char *p = sessions[c->sess].token; *p; p++)
            h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    pthread_mutex_unlock(&sess_mux);
    return h;
}

/* A QSO's text as the page sent it, hashed (FNV-1a): what a receipt is for */
static uint64_t log_sum(const char *text) {
    uint64_t h = 1469598103934665603ULL;

    for (const char *p = text; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    return h;
}

/* A log command's place in the ring: its own if it has one (queued earlier), else the next */
static int log_slot(uint64_t who, uint64_t id) {
    if (id)
        for (int i = 0; i < LOG_ANSWERS; i++)
            if (log_ans[i].id == id && log_ans[i].who == who)
                return i;
    int i = log_ans_next;
    log_ans_next = (log_ans_next + 1) % LOG_ANSWERS;
    log_ans[i].who = who;
    log_ans[i].id  = id;
    return i;
}

/* A log command queued for the GUI thread: "pending" to a query until its answer is in */
static void log_queued(uint64_t who, uint64_t id, uint64_t sum) {
    pthread_mutex_lock(&app_mux);
    int i = log_slot(who, id);
    log_ans[i].sum    = sum;
    log_ans[i].result = 0;
    log_ans[i].told   = true;
    pthread_mutex_unlock(&app_mux);
}

/* The answer to a log command, kept; the panel thread tells the session's clients */
static void log_answer(uint64_t who, uint64_t id, uint64_t sum, bool ok) {
    pthread_mutex_lock(&app_mux);
    int i = log_slot(who, id);
    log_ans[i].sum    = sum;
    log_ans[i].result = ok ? 1 : 2;
    log_ans[i].told   = false;
    pthread_mutex_unlock(&app_mux);
    atomic_store(&log_pending, true);
}

/* "logq? ID CALL|...": the answer kept for this session's ID, pending while the GUI thread has it, or
 * unknown (the radio restarted, the command never arrived, or the answer is too old to be kept:
 * the page leaves it to the operator). Kept for another QSO under that number: "conflict", that
 * QSO's answer is not this one's (audit C03). A page from before C03 asks without the QSO */
static void log_query(client_t *c, uint64_t id, const char *text) {
    const char *r   = "unknown";
    uint64_t    who = sess_key(c);

    pthread_mutex_lock(&app_mux);
    for (int i = 0; i < LOG_ANSWERS; i++)
        if (log_ans[i].id == id && id && log_ans[i].who == who)
            r = text && log_ans[i].sum != log_sum(text) ? "conflict"
              : log_ans[i].result == 1 ? "ok" : log_ans[i].result == 2 ? "no" : "pending";
    pthread_mutex_unlock(&app_mux);
    client_textf(c, "logged %llu %s", (unsigned long long)id, r);
}

/* The answers not yet told, from the panel thread: each to the panel clients of its session.
 * Older pages count their QSOs from 1, so a receipt broadcast to all confirmed another page's
 * QSO 1 (audit B07) */
static void log_flush(void) {
    struct { uint64_t who; uint64_t id; uint8_t result; } out[LOG_ANSWERS];
    int  n = 0;
    char line[64];

    if (!atomic_exchange(&log_pending, false))
        return;
    pthread_mutex_lock(&app_mux);
    for (int i = 0; i < LOG_ANSWERS; i++) {
        if (!log_ans[i].result || log_ans[i].told)
            continue;
        log_ans[i].told = true;
        out[n].who = log_ans[i].who, out[n].id = log_ans[i].id, out[n].result = log_ans[i].result;
        n++;
    }
    pthread_mutex_unlock(&app_mux);
    for (int k = 0; k < n; k++) {
        if (out[k].id)
            snprintf(line, sizeof(line), "logged %llu %s", (unsigned long long)out[k].id, out[k].result == 1 ? "ok" : "no");
        else
            snprintf(line, sizeof(line), "logged %s", out[k].result == 1 ? "ok" : "no");
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && clients[i].up && clients[i].panel && sess_key(&clients[i]) == out[k].who)
                client_text(&clients[i], line);
    }
}

static void app_apply(void *arg) {
    acmd_t *a = (acmd_t *)arg;

    switch (a->id) {
        case A_FT8_OPEN:
            if (!dialog_type_is_run(dialog_ft8)) {
                backlight_tick();
                main_screen_start_app(ACTION_APP_FT8);
            }
            break;
        case A_FT8_CLOSE:
            if (dialog_type_is_run(dialog_ft8))
                main_screen_close_app();
            break;
        case A_FT8:
            ft8_window_remote((ft8_rc_t)a->what, a->arg, a->has_cell ? &a->cell : NULL);
            break;
        case A_LOG:
            log_answer(a->who, a->log_id, a->sum, dialog_field_qso_log(a->text));
            break;
    }
}

/* "ft8 open|close|cq|stop|auto|hold|mode|txcall", "ft8 txfreq HZ", "ft8 reply ID" */
static bool app_ft8_cmd(const char *what, const char *arg) {
    static const struct {
        const char *name;
        ft8_rc_t    rc;
    } rcs[] = {{"cq", FT8_RC_CQ},     {"stop", FT8_RC_STOP},     {"auto", FT8_RC_AUTO},   {"hold", FT8_RC_HOLD},
               {"mode", FT8_RC_MODE}, {"txfreq", FT8_RC_TXFREQ}, {"reply", FT8_RC_REPLY}, {"txcall", FT8_RC_TXCALL}};
    acmd_t a = {.id = A_FT8};

    if (!strcasecmp(what, "open")) {
        a.id = A_FT8_OPEN;
    } else if (!strcasecmp(what, "close")) {
        a.id = A_FT8_CLOSE;
    } else {
        size_t i = 0;
        while (i < sizeof(rcs) / sizeof(rcs[0]) && strcasecmp(rcs[i].name, what)) i++;
        if (i == sizeof(rcs) / sizeof(rcs[0]))
            return false;
        a.what = rcs[i].rc;
        a.arg  = arg ? (int32_t)strtol(arg, NULL, 10) : 0;
        if (a.what == FT8_RC_REPLY) {
            uint32_t id = (uint32_t)a.arg;
            pthread_mutex_lock(&app_mux);
            if (id && ft8_ring[id % FT8_RING].id == id) {
                a.cell     = ft8_ring[id % FT8_RING].cell;
                a.has_cell = true;
            }
            pthread_mutex_unlock(&app_mux);
            if (!a.has_cell)
                return false;
        }
    }
    scheduler_put(app_apply, &a, sizeof(a));
    return true;
}

/* "log CALL|RSTS|RSTR|NAME|QTH|GRID|COUNTRY" (dialog_field_qso_log's text), the day's log;
 * "logq ID CALL|..." the same, answered with the ID. A queue with no room answers "no" at once:
 * the page must not believe a QSO sent into nothing was logged (audit A21). An ID this session
 * sent before with this QSO is not written again: its answer goes out once more (audit B07). With
 * another QSO it is a "conflict", not written and not answered with the other's result: two tabs
 * of one browser share the session, and pages before C03 each counted from 1 */
static void app_log_cmd(client_t *c, uint64_t id, const char *text) {
    uint64_t who = sess_key(c);
    acmd_t   a   = {.id = A_LOG, .log_id = id, .who = who, .sum = log_sum(text)};

    snprintf(a.text, sizeof(a.text), "%s", text);
    if (id) {
        pthread_mutex_lock(&app_mux);
        for (int i = 0; i < LOG_ANSWERS; i++) {
            if (log_ans[i].id != id || log_ans[i].who != who)
                continue;
            bool same = log_ans[i].sum == a.sum;
            if (same && log_ans[i].result) {
                log_ans[i].told = false;
                atomic_store(&log_pending, true);
            }
            pthread_mutex_unlock(&app_mux);
            if (!same)
                client_textf(c, "logged %llu conflict", (unsigned long long)id);
            return;
        }
        pthread_mutex_unlock(&app_mux);
        log_queued(who, id, a.sum);
    }
    if (!scheduler_put(app_apply, &a, sizeof(a))) {
        if (id)
            log_answer(who, id, a.sum, false); /* told by the panel thread, kept for a query */
        else
            client_text(c, "logged no");
    }
}

/* Panel thread: the spectrum as it comes, the state every PANEL_MS, the meters, the spots */
static void *panel_thread(void *arg) {
    static float   db[WATERFALL_NFFT];
    static int8_t  bins[WATERFALL_NFFT];
    pstate_t       last = {0};
    ft8_status_t   ft8_last = {0};
    bool           have_last = false;
    uint32_t       sp_seq = 0, spots_gen = 0;
    uint64_t       state_ms = 0, meter_ms = 0, spots_ms = 0, ft8_ms = 0, ft8_sent_ms = 0;
    char           line[512];
    (void)arg;

    while (true) {
        if (!atomic_load(&npanel)) {
            have_last = false;
            usleep(200000);
            continue;
        }
        uint64_t now = now_ms();

        /* The spectrum: a frame when the DSP has a new one */
        uint32_t center, width, seq;
        if (dsp_spectrum_snapshot(db, WATERFALL_NFFT, &center, &width, &seq) && seq != sp_seq) {
            sp_seq = seq;
            if (center == 0)    /* an old base sends no frequency with its data: the one set, as the waterfall does */
                center = cparam_i_get(cfg_fg_freq) - param_i_get(cfg_band_if_shift) + cparam_i_get(cfg_mode_lo_offset);
            for (int i = 0; i < WATERFALL_NFFT; i++) {
                float v = db[i];
                bins[i] = (int8_t)(v < -128.0f ? -128 : v > 127.0f ? 127 : lroundf(v));
            }
            struct {
                uint16_t mark, n;
                uint32_t center, width, seq;
            } hdr = {0xFFFF, WATERFALL_NFFT, center, width, seq};
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &clients[i];
                if (c->fd < 0 || !c->up || !c->panel)
                    continue;
                pthread_mutex_lock(&c->lock);
                bool room = c->out_len < PANEL_ROOM;
                pthread_mutex_unlock(&c->lock);
                if (room)
                    client_frame(c, WS_OP_BINARY, &hdr, sizeof(hdr), bins, sizeof(bins));
            }
        }

        /* The state: what changed, to everyone; all of it to a new client */
        if (now - state_ms >= PANEL_MS) {
            state_ms = now;
            pstate_t s;
            pstate_read(&s);
            bool changed = !have_last || memcmp(&s, &last, sizeof(s)) != 0;
            if (changed) {
                pstate_line(line, sizeof(line), &s);
                last      = s;
                have_last = true;
            }
            bool all_due = atomic_exchange(&state_resend, false);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &clients[i];
                if (c->fd < 0 || !c->up || !c->panel)
                    continue;
                bool due = atomic_exchange(&c->state_due, false) || all_due;
                if (c->panel_fresh) {
                    c->panel_fresh = false;
                    panel_hello(c);
                    if (!changed)
                        pstate_line(line, sizeof(line), &s);
                    client_text(c, line);
                    ft8_window_status(&ft8_last);
                    ft8_status_line(line, sizeof(line), &ft8_last);
                    client_text(c, line);
                    panel_app_history(c);
                    if (changed)
                        pstate_line(line, sizeof(line), &s);     /* the line the others get */
                } else if (changed || due) {
                    if (!changed)
                        pstate_line(line, sizeof(line), &s);     /* a "set" refused: the state as it is */
                    client_text(c, line);
                }
            }
        }

        /* The meters */
        if (now - meter_ms >= METER_MS) {
            meter_ms = now;
            if (last.tx) {
                float pwr = 0, swr = 0, alc = 0;
                uint8_t seen = 0;
                tx_info_get(&pwr, &swr);
                tx_info_refresh(&seen, &alc, NULL, NULL);
                snprintf(line, sizeof(line), "t %.1f %.2f %.1f", (double)pwr, (double)swr, (double)alc);
            } else {
                snprintf(line, sizeof(line), "m %.1f", (double)meter_get_db());
            }
            panel_broadcast(line);
        }

        /* The spots */
        bool resend = atomic_exchange(&spots_resend, false);
        if (resend || now - spots_ms >= SPOTS_MS) {
            spots_ms = now;
            panel_spots(&spots_gen, resend);
        }

        /* The apps: the FT8 state when it changed, the CW text, a log's answer */
        if (now - ft8_ms >= FT8_MS) {
            ft8_ms = now;
            ft8_status_t fs, a, b;
            ft8_window_status(&fs);
            /* The clocks (the slot countdown, the age of the log) tick on their own: those go
             * once a second, the rest the moment it changes */
            a = fs; b = ft8_last;
            a.in_ms = b.in_ms = 0; a.logged_s = b.logged_s = 0;
            bool ticking = (fs.in_ms >= 0 || fs.logged_s >= 0) && now - ft8_sent_ms >= 1000;
            if (memcmp(&a, &b, sizeof(a)) != 0 || ticking) {
                ft8_last    = fs;
                ft8_sent_ms = now;
                ft8_status_line(line, sizeof(line), &fs);
                panel_broadcast(line);
            }
            panel_cw_flush();
            log_flush();
        }
        usleep(20000);
    }
    return NULL;
}

/* "key NAME 1|0", "knob main|vol|mfk N", "ping", "screen 0|1", "panel 0|1", "set NAME VALUE",
 * "ft8 WHAT [ARG]", "log TEXT" */
/* "ctl WHO MINE": who controls the radio, and whether it is this client */
static void ctl_tell(client_t *c) {
    ctl_info_t o;
    char       who[64];

    ctl_get(&o);
    if (o.kind == CTL_LOCAL)
        snprintf(who, sizeof(who), "local");
    else if (o.kind == CTL_TCI)
        snprintf(who, sizeof(who), "tci");
    else if (o.kind == CTL_BROWSER)
        snprintf(who, sizeof(who), "browser %s", o.name);
    else
        snprintf(who, sizeof(who), "none");
    client_textf(c, "ctl %s %d", who, o.kind == CTL_BROWSER && o.id == (int)(c - clients) ? 1 : 0);
    c->ctl_told = ctl_version() + 1;
}

static void role_tell(client_t *c) {
    client_textf(c, "role %s %d %s", role_name(c->role), c->tx_ok ? 1 : 0, c->sess >= 0 ? sessions[c->sess].name : "-");
}

/* The client may do this: the role, and control of the radio for what touches the RF side
 * (taken when free, else the holder is named and the page offers Take control) */
static bool allowed(client_t *c, uint8_t role, bool control) {
    if (c->role < role) {
        client_text(c, role == ROLE_ADMIN ? "err admin only" : "err view only: pair this browser to operate");
        return false;
    }
    if (control && !ctl_claim(CTL_BROWSER, (int)(c - clients), c->sess >= 0 ? sessions[c->sess].name : c->ip, false)) {
        ctl_info_t o;
        ctl_get(&o);
        client_textf(c, "err controlled by %s: take control first",
                     o.kind == CTL_LOCAL ? "the radio's operator" : o.kind == CTL_TCI ? "a TCI client" : o.name);
        ctl_tell(c);
        return false;
    }
    return true;
}

/* "key PTT 1 HOLD": the radio's own microphone keyed, HOLD the page's number for this press; the page sends the
 * same every second to renew the lease (ptt_lease). Only the hold that keyed is renewed: one that ended (the lease
 * ran out, three minutes, a release) or was refused stays ended, so the renewals a stalled link delivers late key
 * nothing, and a press on purpose comes with a new number (audit C02). A page without the numbers, open since
 * before an update, renewed with the press itself: it is told to reload. Socket thread */
static void ptt_down(client_t *c, const char *hold_text) {
    uint32_t hold = hold_text ? (uint32_t)strtoul(hold_text, NULL, 10) : 0;
    int      k    = 0;

    while (k < c->nheld && c->held[k] != BTN_TRIGGER_HAPPY4) k++;
    if (!hold) {
        client_text(c, "err this session may not transmit from a page older than the radio: reload the page");
        return;
    }
    if (hold < c->ptt_hold || (hold == c->ptt_hold && k == c->nheld))
        return;                                 /* an ended hold's renewal: the page was told when it ended */
    c->ptt_hold = hold;                         /* a press counts once, taken or refused */
    if (!allowed(c, ROLE_OPERATE, true))
        return;
    if (!c->tx_ok) {
        client_text(c, "err this session may not transmit");
        return;
    }
    c->ptt_ms = now_ms();
    if (k < c->nheld)
        return;                                 /* down already: the lease renewed, the three minutes count on */
    if (c->nheld == MAX_HELD) {
        client_text(c, "err too many keys held");
        return;
    }
    c->ptt_start_ms = c->ptt_ms;
    c->held[c->nheld++] = BTN_TRIGGER_HAPPY4;
    key_emit(BTN_TRIGGER_HAPPY4, true);
}

/* A held PTT is a lease (R4): renewed every second by the page, or let go here; three minutes at most. Its hold
 * stays ended (ptt_down). Server thread */
static void ptt_lease(client_t *c, uint64_t now) {
    int k = 0;

    while (k < c->nheld && c->held[k] != BTN_TRIGGER_HAPPY4) k++;
    if (k < c->nheld && (now - c->ptt_ms > PTT_LEASE_MS || now - c->ptt_start_ms > PTT_LIMIT_MS)) {
        c->held[k] = c->held[--c->nheld];
        key_emit(BTN_TRIGGER_HAPPY4, false);
        client_text(c, now - c->ptt_start_ms > PTT_LIMIT_MS ? "err ptt released: three minutes on the air"
                                                            : "err ptt released: no renewal from the page");
    }
}

static void sessions_tell(client_t *c) {
    char line[1024];
    int  n = snprintf(line, sizeof(line), "sessions [");

    pthread_mutex_lock(&sess_mux);
    for (int i = 0; i < nsessions && n < (int)sizeof(line) - 160; i++) {
        bool online = false;
        for (int k = 0; k < MAX_CLIENTS; k++)
            if (clients[k].fd >= 0 && clients[k].up && clients[k].sess == i)
                online = true;
        n += snprintf(line + n, sizeof(line) - n, "%s{\"n\":%d,\"name\":\"%s\",\"role\":\"%s\",\"tx\":%d,\"last\":%llu,\"ip\":\"%s\",\"online\":%d,\"me\":%d}",
                      i ? "," : "", i, sessions[i].name, role_name(sessions[i].role), sessions[i].tx ? 1 : 0,
                      (unsigned long long)sessions[i].last, sessions[i].ip, online ? 1 : 0, c->sess == i ? 1 : 0);
    }
    pthread_mutex_unlock(&sess_mux);
    snprintf(line + n, sizeof(line) - n, "]");
    client_text(c, line);
}

/* A name as typed: letters, digits, space, a few marks, up to 24 */
static void name_clean(char *dst, size_t n, const char *src) {
    size_t k = 0;
    for (; *src && k + 1 < n; src++)
        if (isalnum((unsigned char)*src) || strchr(" -_./'", *src))
            dst[k++] = *src;
    while (k && dst[k - 1] == ' ') k--;
    dst[k] = 0;
    if (!k)
        snprintf(dst, n, "browser");
}

static void handle_text(client_t *c, char *text) {
    char *a[4] = {0};
    int   n    = 0;

    if (!strncmp(text, "log ", 4)) {        /* the text as it is: names have spaces */
        if (allowed(c, ROLE_OPERATE, false))
            app_log_cmd(c, 0, text + 4);
        return;
    }
    /* The numbers are 53 bits: strtoull, as strtoul stops at 32 bits on the radio */
    if (!strncmp(text, "logq? ", 6)) {      /* "logq? ID TEXT", "logq? ID" from a page before C03 */
        char    *end;
        uint64_t id = strtoull(text + 6, &end, 10);
        log_query(c, id, *end == ' ' ? end + 1 : NULL);
        return;
    }
    if (!strncmp(text, "logq ", 5)) {       /* "logq ID TEXT" */
        char    *end;
        uint64_t id = strtoull(text + 5, &end, 10);
        if (!allowed(c, ROLE_OPERATE, false))
            return;
        if (id && *end == ' ')
            app_log_cmd(c, id, end + 1);
        else
            client_text(c, "err logq ID TEXT");
        return;
    }
    if (!strncmp(text, "pair ", 5)) {       /* "pair CODE NAME..." */
        char *sp   = strchr(text + 5, ' ');
        char  name[32];
        if (!sp) {
            client_text(c, "err pair CODE NAME");
            return;
        }
        *sp = 0;
        name_clean(name, sizeof(name), sp + 1);
        int i = pair_try(text + 5, name, c->ip);
        if (i < 0) {
            client_text(c, "err pairing refused: wrong or expired code (press Pair for a new one)");
            return;
        }
        c->sess  = i;
        c->role  = sessions[i].role;
        c->tx_ok = sessions[i].tx;
        client_textf(c, "paired %s %s %d", sessions[i].token, role_name(c->role), c->tx_ok ? 1 : 0);
        role_tell(c);
        ctl_tell(c);
        LV_LOG_USER("remote: paired %s (%s) as %s", name, c->ip, role_name(c->role));
        return;
    }
    for (char *t = strtok(text, " \t\r\n"); t && n < 4; t = strtok(NULL, " \t\r\n"))
        a[n++] = t;
    if (n == 0)
        return;
    if (!strcmp(a[0], "ping")) {
        if (n >= 2)
            client_textf(c, "pong %llu", strtoull(a[1], NULL, 10));
        else
            client_text(c, "pong");
    } else if (!strcmp(a[0], "auth") && n >= 2) {
        int i = strlen(a[1]) == TOKEN_LEN ? sess_find(a[1]) : -1;
        if (i < 0) {
            c->sess = -1; c->role = ROLE_VIEW; c->tx_ok = false;
            client_text(c, "err unknown session: pair this browser again");
        } else {
            c->sess  = i;
            c->role  = sessions[i].role;
            c->tx_ok = sessions[i].tx;
            sess_touch(i, c->ip);
        }
        role_tell(c);
        ctl_tell(c);
    } else if (!strcmp(a[0], "pair?")) {
        pair_show(NULL, 0);
        client_text(c, "pairing code shown on the radio for 2 min");
    } else if (!strcmp(a[0], "unpair")) {
        if (c->sess >= 0)
            sess_revoke(c->sess);     /* closes this socket too */
        else
            client_text(c, "err not paired");
    } else if (!strcmp(a[0], "control") && n >= 2) {
        if (!strcmp(a[1], "take")) {
            if (!allowed(c, ROLE_OPERATE, false))
                return;
            if (!ctl_claim(CTL_BROWSER, (int)(c - clients), c->sess >= 0 ? sessions[c->sess].name : c->ip, true))
                client_text(c, "err busy: the radio is transmitting, control stays where it is");
        } else if (!strcmp(a[1], "release")) {
            ctl_release(CTL_BROWSER, (int)(c - clients));
        }
        ctl_tell(c);
    } else if (!strcmp(a[0], "sessions")) {
        if (allowed(c, ROLE_ADMIN, false))
            sessions_tell(c);
    } else if (!strcmp(a[0], "revoke") && n >= 2) {
        if (!allowed(c, ROLE_ADMIN, false))
            return;
        int i = atoi(a[1]);
        if (i < 0 || i >= nsessions)
            client_text(c, "err no such session");
        else {
            sess_revoke(i);
            if (c->fd >= 0)
                sessions_tell(c);
        }
    } else if (!strcmp(a[0], "screen") && n >= 2) {
        c->screen = atoi(a[1]) != 0;
        c->screen_stale = true;
    } else if (!strcmp(a[0], "picture") && n >= 2) {
        for (int k = 0; k < PIC_N; k++)
            if (!strcmp(a[1], pics[k].name)) {
                c->pic          = (uint8_t)k;
                c->screen_stale = true;
            }
    } else if (!strcmp(a[0], "panel") && n >= 2) {
        bool on = atoi(a[1]) != 0;
        if (on != c->panel) {
            c->panel = on;
            atomic_fetch_add(&npanel, on ? 1 : -1);
            if (on) {
                c->panel_fresh = true;
                atomic_store(&spots_resend, true);
            }
        }
    } else if (!strcmp(a[0], "set") && n >= 2) {
        if (!allowed(c, ROLE_OPERATE, true)) {
            atomic_store(&c->state_due, true);     /* as set_refused: the state again (audit C13) */
            return;
        }
        panel_set(c, a[1], n >= 3 ? a[2] : NULL, n >= 4 ? a[3] : NULL);
    } else if (!strcmp(a[0], "ft8") && n >= 2) {
        if (!allowed(c, ROLE_OPERATE, true))
            return;
        if (!app_ft8_cmd(a[1], n >= 3 ? a[2] : NULL))
            client_textf(c, "err ft8 %s", a[1]);
    } else if (!strcmp(a[0], "key") && n >= 3) {
        int  code = key_code(a[1]);
        bool down = atoi(a[2]) != 0;
        if (code < 0) {
            client_textf(c, "err unknown key %s", a[1]);
            return;
        }
        if (code == BTN_TRIGGER_HAPPY4 && down) {  /* PTT: the radio's own microphone, a lease */
            ptt_down(c, n >= 4 ? a[3] : NULL);
            return;
        }
        int i = 0;
        while (i < c->nheld && c->held[i] != code) i++;
        if (down) {
            if (!allowed(c, ROLE_OPERATE, true))
                return;
            if (i < c->nheld)
                return;                             /* already down: the hardware saw it */
            if (c->nheld == MAX_HELD) {
                client_text(c, "err too many keys held");
                return;
            }
            c->held[c->nheld++] = (uint16_t)code;
        } else if (i < c->nheld) {
            c->held[i] = c->held[--c->nheld];
        } else {
            return;                                 /* not held by this client: nothing to release */
        }
        key_emit((uint16_t)code, down);
    } else if (!strcmp(a[0], "knob") && n >= 3) {
        if (!allowed(c, ROLE_OPERATE, true))
            return;
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
        client_textf(c, "hello %s %u", VERSION, boot_id);
        pthread_mutex_lock(&fmut);
        int32_t w = sw, h = sh;
        pthread_mutex_unlock(&fmut);
        client_textf(c, "size %d %d", w ? w : 800, h ? h : 480);
        {
            char   line[320] = "keys";
            size_t k = 4;
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && k < sizeof(line) - 1; i++)
                k += snprintf(line + k, sizeof(line) - k, " %s", keys[i].name);
            client_text(c, line);
        }
        role_tell(c);
        ctl_tell(c);
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
    if (c->fd < 0)      /* what it asked closed it: "unpair", or its own session revoked */
        return;
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
#ifdef TCP_NOTSENT_LOWAT
        /* Little left unsent in the kernel: a link slower than the frames drops frames here instead of queueing
         * seconds of them, and the picture stays current */
        int lowat = 32 * 1024;
        setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
        pthread_mutex_lock(&c->lock);
        c->fd      = fd;
        c->up      = false;
        c->in_len = c->out_len = c->text_len = 0;
        c->nheld   = 0;
        c->screen  = true;
        c->pic     = PIC_SHARP;
        c->screen_stale = true;
        c->screen_ms    = 0;
        c->panel   = c->panel_fresh = false;
        atomic_store(&c->state_due, false);
        c->sess    = -1;
        c->role    = ROLE_VIEW;
        c->tx_ok   = false;
        c->ptt_ms  = c->ptt_start_ms = 0;
        c->ptt_hold = 0;
        c->ctl_told = 0;
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
        /* A POWER key not told yet (power_post): tried again about as often as the base's packets */
        if (poll(pfd, n, power_down != power_told ? 30 : n > 2 ? 100 : listen_fd >= 0 ? 500 : 1000) < 0 && errno != EINTR) {
            usleep(100000);
            continue;
        }
        power_post();
        if (pfd[0].revents & POLLIN) {
            char buf[64];
            while (read(wake_fd[0], buf, sizeof(buf)) > 0) {}
        }
        if (atomic_exchange(&forget_all_due, false))
            while (nsessions > 0)
                sess_revoke(nsessions - 1);
        if (atomic_exchange(&disconnect_all_due, false))
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].fd >= 0) {
                    client_text(&clients[i], "err disconnected from the radio");
                    client_close(&clients[i]);
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
        /* The PTT leases (ptt_lease). Who controls the radio, and its TX state, to everyone. */
        {
            bool tx = ctl_transmitting();
            bool tx_changed = tx != atomic_exchange(&tx_last, tx);
            uint32_t ver = ctl_version() + 1;      /* +1: 0 means never told */
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &clients[i];
                if (c->fd < 0 || !c->up)
                    continue;
                ptt_lease(c, now);
                if (tx_changed)
                    client_textf(c, "tx %d", tx ? 1 : 0);
                if (c->ctl_told != ver)
                    ctl_tell(c);
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
static bool encode(const uint8_t *px, int32_t w, int32_t h, int quality, uint8_t **out, unsigned long *size) {
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
    jpeg_set_quality(&c, quality, TRUE);
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

/* Half size: each 2x2 block averaged (BGRX), into a buffer that is kept */
static const uint8_t *half_size(const uint8_t *px, int32_t w, int32_t h) {
    static uint8_t *out;
    static size_t   cap;
    int32_t         hw = w / 2, hh = h / 2;
    size_t          need = (size_t)hw * hh * 4;

    if (need > cap) {
        free(out);
        out = malloc(need);
        cap = out ? need : 0;
        if (!out)
            return NULL;
    }
    for (int32_t y = 0; y < hh; y++) {
        const uint8_t *r0 = px + (size_t)(2 * y) * w * 4, *r1 = r0 + (size_t)w * 4;
        uint8_t       *o  = out + (size_t)y * hw * 4;
        for (int32_t x = 0; x < hw; x++, r0 += 8, r1 += 8, o += 4)
            for (int k = 0; k < 4; k++)
                o[k] = (uint8_t)((r0[k] + r0[k + 4] + r1[k] + r1[k + 4] + 2) >> 2);
    }
    return out;
}

/* Encoder thread: a frame to JPEG for the viewers that can take one now (nothing still on its way to them), once
 * per picture kind they asked for. A viewer that could not take a changed frame gets the next one even if the
 * screen stopped changing (it waited for the once-a-second repeat), and nothing is encoded for nobody (at 3 frames
 * a second, seven encodes in ten went nowhere) */
static void *encoder_thread(void *arg) {
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
        uint64_t now   = now_ms();
        size_t   bytes = (size_t)w * h * 4;
        bool     same  = prev && pw == w && ph == h && memcmp(prev, enc, bytes) == 0;
        bool     hide  = now < atomic_load(&pair_hide_ms);   /* a pairing code is on the screen */
        bool     want[MAX_CLIENTS] = {false};
        bool     any = false;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0 || !c->up || !c->screen || (hide && c->sess < 0))
                continue;
            pthread_mutex_lock(&c->lock);
            bool room = c->out_len == 0;      /* the last frame still on its way: none now */
            pthread_mutex_unlock(&c->lock);
            if (!same)
                c->screen_stale = true;
            want[i] = room && (c->screen_stale || now - c->screen_ms >= STILL_MS);
            any |= want[i];
        }
        if (!same) {
            /* the frame to compare the next one with */
            if (!prev || pw != w || ph != h) {
                free(prev);
                prev = malloc(bytes);
                pw = w, ph = h;
            }
            if (prev)
                memcpy(prev, enc, bytes);
        }
        if (!any) {
            usleep(FRAME_MS * 1000 / 2);
            continue;
        }
        frame_hdr_t hdr = {.w = (uint16_t)w, .h = (uint16_t)h, .seq = atomic_fetch_add(&seq, 1)};
        for (int k = 0; k < PIC_N; k++) {
            bool needed = false;
            for (int i = 0; i < MAX_CLIENTS; i++)
                needed |= want[i] && clients[i].pic == k;
            if (!needed)
                continue;
            /* The header keeps the screen's size: the page draws a half-size picture over the whole canvas */
            const uint8_t *src = pics[k].half ? half_size(enc, w, h) : enc;
            if (!src || !encode(src, pics[k].half ? w / 2 : w, pics[k].half ? h / 2 : h, pics[k].quality, &jpg, &jpg_len))
                continue;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &clients[i];
                if (want[i] && c->pic == k && client_frame(c, WS_OP_BINARY, &hdr, sizeof(hdr), jpg, jpg_len)) {
                    c->screen_stale = false;
                    c->screen_ms    = now;
                }
            }
            free(jpg);
            jpg     = NULL;
            jpg_len = 0;
        }
        /* hold the pace: the LVGL side fills the next frame meanwhile */
        uint64_t spent = now_ms() - now;
        if (spent < FRAME_MS)
            usleep((FRAME_MS - spent) * 1000);
    }
    return NULL;
}

/* ---- Public ------------------------------------------------------------------------- */

void remote_server_init(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    boot_id = (uint32_t)ts.tv_sec ^ (uint32_t)ts.tv_nsec ^ (uint32_t)getpid();
    if (!boot_id)
        boot_id = 1;
    pthread_t th;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd  = -1;
        clients[i].out = malloc(OUT_SIZE);
        pthread_mutex_init(&clients[i].lock, NULL);
    }
    dev_keypad = dev_open(DEV_KEYPAD);
    dev_power  = dev_open(DEV_POWER);
    dev_main   = dev_open(DEV_MAIN);
    dev_vol    = dev_open(DEV_VOL);
    dev_mfk    = dev_open(DEV_MFK);
    conf_read();
    sess_load();
    if (pipe2(wake_fd, O_NONBLOCK | O_CLOEXEC) < 0) {
        LV_LOG_ERROR("remote: no wake pipe");
        return;
    }
    lv_timer_create(tick_cb, FRAME_MS, NULL);
    pthread_create(&th, NULL, encoder_thread, NULL);
    pthread_detach(th);
    pthread_create(&th, NULL, panel_thread, NULL);
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

void remote_server_pair_code(char *buf, size_t n) {
    pair_show(buf, n);
}

int remote_server_sessions(char *buf, size_t n) {
    int k = 0;

    pthread_mutex_lock(&sess_mux);
    int count = nsessions;
    if (buf && n) {
        buf[0] = 0;
        for (int i = 0; i < nsessions && k < (int)n - 1; i++) {
            bool online = false;
            for (int j = 0; j < MAX_CLIENTS; j++)
                if (clients[j].fd >= 0 && clients[j].up && clients[j].sess == i)
                    online = true;
            k += snprintf(buf + k, n - k, "%s%s%s%s", i ? ", " : "", sessions[i].name,
                          sessions[i].role == ROLE_ADMIN ? " (admin)" : "", online ? " *" : "");
        }
    }
    pthread_mutex_unlock(&sess_mux);
    return count;
}

/* From the GUI thread: the sockets are the server thread's, so the forgetting is handed to it */
void remote_server_forget_all(void) {
    atomic_store(&forget_all_due, true);
    wake();
}

void remote_server_disconnect_all(void) {
    atomic_store(&disconnect_all_due, true);
    wake();
}
