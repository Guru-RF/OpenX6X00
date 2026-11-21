/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - DMR (APP > DMR): a DMR network's talkgroups heard over the internet, receive only. The window: the
 *  talkgroups of the web page's list, the one heard now, who talks with their name, and the last heard. The MFK on a
 *  row listens to that talkgroup; Connect to the one last chosen. rootfs/usr/sbin/x6100-dmr logs in to the network's
 *  master as a hotspot without a radio and passes the voice frames on; this window's thread decodes them
 *  (dmr_ambe.c, mbelib), and plays them on the speaker at the radio's VOL, or on the Bluetooth headphones while
 *  they stand in for it, a call at a time: the speaker is the receiver's between calls (audio.h).
 *
 *  Only listening: nothing here or in the helper sends voice or data to the network, and nothing keys the radio.
 *  When the radio transmits, whatever keys it, a call's audio stops at once (radio.c cuts it before its PTT and its
 *  modem key, this window on any TX state) and the next one plays once the radio has received for a second.
 *  Nothing connects until a talkgroup is chosen here; closing the window logs out.
 *
 *  The network, the DMR ID, the hotspot password and the talkgroups are on the web page's DMR page (webserver
 *  apps.py); the callsign is the radio's (Settings > General). The network's rules: the manual, DMR.
 *
 *  Map (the second button) shows, in place of the two lists, the stations the radio heard in the last 24 hours on
 *  the world (live_map.c), each in the country of its callsign, and the one talking now in two rings with a window:
 *  its callsign, name, its record's city and the talkgroup. The helper works the places out (its header: cty.dat,
 *  radioid.net, a place search) and writes them to MAP_FILE while this window touches MAP_WANT; while the app is not
 *  connected the helper's "map" gives the stations kept, once, to MAP_KEPT. The MFK zooms, its press goes back.
 *
 *  After the CX8ANM edition's DMR app (condoruy, Cóndor Tecnológico): DMR heard through mbelib, the window's
 *  talkgroups and last heard, the map.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dialog_dmr.h"

#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "cJSON.h"
#include "cfg/cfg_api.h"
#include "dmr_ambe.h"
#include "dmr_proto.h"
#include "gui_events.h"
#include "i18n.h"
#include "keyboard.h"
#include "live_map.h"
#include "map_geo.h"
#include "msg_box.h"
#include "olivia_resamp.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "station.h"
#include "styles.h"
#include "theme.h"
#include "util.h"
#include "voice.h"
#include "voice_focus.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

#define HELPER       X6100_SBIN "/x6100-dmr"
#define DMR_DIR      BOARD_DATA "/dmr"
#define DMR_CONF     DMR_DIR "/dmr.conf"
#define DMR_PASSWORD DMR_DIR "/password"
#define DMR_LOG      BOARD_DATA "/app_logs/dmr.txt"
#define DMR_PARAMS   BOARD_DATA "/params.db"
#define LOG_MAX      (256 * 1024)
#define TGS_MAX      32
#define TAIL_MS      1000   /* silence after a call: its last words out of the stream's 800 ms before the speaker goes */
#define QUIET_MS     1000   /* after a TX: the radio received this long before a call plays again */
#define STALL_MS     3000
#define VOICE_GAP_MS 3000   /* no voice and no end from the helper: the call is over here anyway */
#define OPEN_AGAIN_MS 5000  /* the stream could not open: tried again after this */
#define END_WAIT_MS  4000   /* the helper's logout, then it is killed */
#define MAP_FILE     "/tmp/x6100-dmr-map.json"         /* the helper's, while it listens and a map is looked at */
#define MAP_WANT     "/tmp/x6100-dmr-map.want"         /* touched while the map shows: the helper writes MAP_FILE */
#define MAP_KEPT     "/tmp/x6100-dmr-map-kept.json"    /* the helper's "map": the stations kept, asked for here */
#define MAP_MS       2000   /* the map drawn again */
#define MAP_WANT_MS  30000  /* MAP_WANT touched again (the helper gives up after 90 s without) */
#define MAP_KEPT_MS  60000  /* MAP_KEPT asked for again while MAP_FILE is not written */
#define MAP_FRESH_S  10     /* MAP_FILE older than this: the helper does not write it now */
#define MAP_PTS      300    /* the helper's own most */

extern char **environ;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void conn_cb(button_data_t *b);
static const char *conn_label(void);
static void map_cb(button_data_t *b);
static const char *map_label(void);

static button_data_t btn_conn = {.type = BTN_TEXT_FN, .label_fn = conn_label, .press = conn_cb};
static button_data_t btn_map  = {.type = BTN_TEXT_FN, .label_fn = map_label, .press = map_cb};

static buttons_page_t page_1 = {{&btn_conn, &btn_map}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page_1,
};

dialog_t *dialog_dmr = &dialog;

/* ---------- the settings (the web page's) ---------- */

typedef struct {
    uint32_t tg;
    char     name[40];
} tg_row_t;

static tg_row_t tgs[TGS_MAX];
static int      tgs_n;
static bool     set_up;             /* a DMR ID, a master and a password */
static time_t   conf_mtime = -1;

/* dmr.conf again when the web page changed it: the talkgroups, and whether it is set up */
static bool read_conf(void) {
    struct stat sb;
    char        line[512], id[16] = "", host[128] = "";
    FILE       *f;

    if (stat(DMR_CONF, &sb) != 0) sb.st_mtime = 0;
    if (sb.st_mtime == conf_mtime) return false;
    conf_mtime = sb.st_mtime;
    tgs_n      = 0;
    if ((f = fopen(DMR_CONF, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "DMR_ID=", 7)) snprintf(id, sizeof(id), "%s", line + 7);
            else if (!strncmp(line, "HOST=", 5)) snprintf(host, sizeof(host), "%s", line + 5);
            else if (!strncmp(line, "TALKGROUPS=", 11)) {
                char *save = NULL;

                for (char *it = strtok_r(line + 11, "|", &save); it && tgs_n < TGS_MAX; it = strtok_r(NULL, "|", &save)) {
                    char         *end;
                    unsigned long tg = strtoul(it, &end, 10);

                    if (end == it || !tg || tg > 0xFFFFFF) continue;
                    while (*end == ' ') end++;
                    tgs[tgs_n].tg = (uint32_t)tg;
                    snprintf(tgs[tgs_n].name, sizeof(tgs[tgs_n].name), "%s", end);
                    tgs_n++;
                }
            }
        }
        fclose(f);
    }
    set_up = id[0] && host[0] && access(DMR_PASSWORD, F_OK) == 0;
    if (!tgs_n) {       /* no list: no options asked for, the talkgroups the master has for the hotspot */
        tgs[0].tg      = 0;
        tgs[0].name[0] = 0;
        tgs_n          = 1;
    }
    return true;
}

/* ---------- the session: the helper and the thread that plays its calls ---------- */

static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static dmr_state_t     st;                  /* what the helper said, under st_lock */
static int             helper_fd = -1;      /* under st_lock */
static atomic_bool     running;             /* the thread goes on */
static bool            thread_live;         /* GUI thread: a thread to wait for */
static bool            want_on;             /* GUI thread: the window wants a helper */
static uint32_t        want_tg;
static bool            restart_pending;

typedef struct {
    pid_t pid;
    int   fd;
} session_t;

/* A call plays: the radio receives (a PTT, the modem, a TX of the base's own: the hand mic, VOX, the KEY jack) */
static bool keep_listening(void) {
    return atomic_load(&running) && radio_get_state() == RADIO_RX && !radio_get_modem() && !radio_get_ptt();
}

typedef struct {
    dmr_ambe_t     *dec;
    olivia_resamp_t rs;
    float           in[DMR_BURST_PCM];
    float          *out;
    int16_t        *out16;
    int             out_max;
    bool            open;           /* the stream open and the speaker claimed: a call plays */
    uint64_t        last_voice;
    uint64_t        open_again;     /* the stream failed: not before this */
} player_t;

/* The call's last words played: a second of silence after them, then the speaker the receiver's again */
static void call_over(player_t *p) {
    if (!p->open) return;
    int n = AUDIO_PLAY_RATE * TAIL_MS / 1000;

    memset(p->out16, 0, sizeof(int16_t) * (size_t)p->out_max);
    while (n > 0) {
        int k = n < p->out_max ? n : p->out_max;

        if (audio_listen_play(AUDIO_USER_DMR, p->out16, (size_t)k, keep_listening, STALL_MS) != 0) break;
        n -= k;
    }
    audio_listen_end(AUDIO_USER_DMR);
    p->open = false;
}

static void cut_off(player_t *p) {
    audio_listen_end(AUDIO_USER_DMR);
    p->open = false;
}

static void voice(player_t *p, const uint8_t *burst) {
    int16_t  pcm[DMR_BURST_PCM];
    uint64_t now = get_time();

    dmr_ambe_burst(p->dec, burst, pcm);     /* decoded even when not played: the next frames build on it */
    p->last_voice = now;
    if (!p->open) {
        if (now < p->open_again || !keep_listening() || !audio_listen_resume(AUDIO_USER_DMR, QUIET_MS)) return;
        if (audio_listen_open(AUDIO_USER_DMR) < 0) {
            audio_listen_end(AUDIO_USER_DMR);
            p->open_again = now + OPEN_AGAIN_MS;
            return;
        }
        p->open = true;
        olivia_resamp_reset(&p->rs);
    }
    for (int i = 0; i < DMR_BURST_PCM; i++) p->in[i] = pcm[i] / 32768.0f;
    int n = olivia_resamp_process(&p->rs, p->in, DMR_BURST_PCM, p->out);

    for (int i = 0; i < n; i++) {
        float v = p->out[i] * 32767.0f;

        p->out16[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    int res = audio_listen_play(AUDIO_USER_DMR, p->out16, (size_t)n, keep_listening, STALL_MS);

    if (res == -1) cut_off(p);      /* the radio transmits, or a key cut it: the rest after a second of RX */
    else if (res < 0) {
        cut_off(p);
        p->open_again = now + OPEN_AGAIN_MS;
    }
}

static void line(player_t *p, char *text) {
    dmr_ev_t ev;

    pthread_mutex_lock(&st_lock);
    ev = dmr_proto_line(&st, text, get_time());
    pthread_mutex_unlock(&st_lock);
    switch (ev) {
        case DMR_EV_CALL:
            call_over(p);           /* one before it without its end */
            dmr_ambe_reset(p->dec);
            break;
        case DMR_EV_END:
        case DMR_EV_LOST:
        case DMR_EV_ERROR:
            call_over(p);
            break;
        default:
            break;
    }
}

static void session_ended(void *arg);

/* The helper's end: its logout waited for, then killed */
static void reap(pid_t pid) {
    for (int waited = 0; waited < END_WAIT_MS; waited += 50) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void *session_fn(void *arg) {
    session_t s = *(session_t *)arg;
    player_t  p = {0};
    uint8_t   rec[512];

    free(arg);
    p.dec = dmr_ambe_new();
    if (p.dec && olivia_resamp_init(&p.rs, DMR_AMBE_RATE, AUDIO_PLAY_RATE) == 0) {
        p.out_max = olivia_resamp_max_out(&p.rs, DMR_BURST_PCM);
        p.out     = malloc(sizeof(float) * (size_t)p.out_max);
        p.out16   = malloc(sizeof(int16_t) * (size_t)p.out_max);
    }
    audio_listen_begin(AUDIO_USER_DMR);
    while (p.out && p.out16 && atomic_load(&running)) {
        struct pollfd pf = {.fd = s.fd, .events = POLLIN};
        int           r  = poll(&pf, 1, 100);

        if (p.open && !keep_listening()) cut_off(&p);
        else if (p.open && get_time() - p.last_voice > VOICE_GAP_MS) call_over(&p);
        if (r <= 0) continue;
        ssize_t n = recv(s.fd, rec, sizeof(rec) - 1, 0);

        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;      /* the helper ended */
        if (rec[0] == 0) {
            if (n == 1 + DMR_BURST_BYTES) voice(&p, rec + 1);
        } else {
            rec[n] = 0;
            line(&p, (char *)rec);
        }
    }
    if (p.open) cut_off(&p);
    audio_listen_end(AUDIO_USER_DMR);

    pthread_mutex_lock(&st_lock);
    close(s.fd);
    helper_fd = -1;
    pthread_mutex_unlock(&st_lock);
    reap(s.pid);
    if (p.dec) dmr_ambe_free(p.dec);
    if (p.out) olivia_resamp_free(&p.rs);
    free(p.out);
    free(p.out16);
    atomic_store(&running, false);
    while (!scheduler_put_noargs(session_ended)) usleep(20000);
    return NULL;
}

/* The helper's stderr: its log, started again from empty once it is over LOG_MAX; else nowhere. The descriptor to
 * close after the start, or -1 */
static int open_log(posix_spawn_file_actions_t *fa) {
    struct stat sb;
    int         flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, fd;

    mkdir(BOARD_DATA "/app_logs", 0755);
    if (stat(DMR_LOG, &sb) == 0 && sb.st_size > LOG_MAX) flags |= O_TRUNC;
    fd = open(DMR_LOG, flags, 0644);
    if (fd >= 0) posix_spawn_file_actions_adddup2(fa, fd, 2);
    else posix_spawn_file_actions_addopen(fa, 2, "/dev/null", O_WRONLY, 0);
    return fd;
}

/* The helper's environment: the GUI's, with the folders of this build (a PC build's data folder is not /mnt) */
static char **helper_env(void) {
    static char *ours[] = {"X6100_DMR_DIR=" DMR_DIR, "X6100_DMR_LOG=" DMR_LOG, "X6100_PARAMS_DB=" DMR_PARAMS};
    size_t       n = 0, k = 0;

    while (environ[n]) n++;
    char **env = calloc(n + 4, sizeof(char *));

    if (!env) return NULL;
    for (size_t i = 0; i < n; i++) {
        bool mine = false;

        for (size_t j = 0; j < 3; j++) mine |= !strncmp(environ[i], ours[j], strchr(ours[j], '=') - ours[j] + 1);
        if (!mine) env[k++] = environ[i];
    }
    for (size_t j = 0; j < 3; j++) env[k++] = ours[j];
    return env;
}

/* GUI thread: the helper started for talkgroup tg, with the thread. False: it could not be */
static bool launch(uint32_t tg) {
    int                        sv[2];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    sigset_t                   none, dfl;
    pthread_t                  t;
    char                       tg_s[16];
    char                      *argv[] = {(char *)HELPER, (char *)"run", tg_s, NULL};
    char                     **env    = helper_env();
    session_t                 *s;
    pid_t                      pid;

    snprintf(tg_s, sizeof(tg_s), "%u", (unsigned)tg);
    if (!env) return false;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        free(env);
        return false;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
    int log_fd = open_log(&fa);

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
    int r = posix_spawn(&pid, HELPER, &fa, &attr, argv, env);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    free(env);
    close(sv[1]);
    if (log_fd >= 0) close(log_fd);
    if (r != 0 || !(s = malloc(sizeof(*s)))) {
        if (r == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
        close(sv[0]);
        return false;
    }
    s->pid = pid;
    s->fd  = sv[0];
    pthread_mutex_lock(&st_lock);
    dmr_proto_init(&st);
    st.conn   = DMR_CONN_CONNECTING;
    st.tg     = tg;
    helper_fd = sv[0];
    pthread_mutex_unlock(&st_lock);
    atomic_store(&running, true);
    if (pthread_create(&t, NULL, session_fn, s) != 0) {
        atomic_store(&running, false);
        pthread_mutex_lock(&st_lock);
        helper_fd = -1;
        st.conn   = DMR_CONN_OFF;
        pthread_mutex_unlock(&st_lock);
        close(sv[0]);
        free(s);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }
    pthread_detach(t);
    thread_live = true;
    return true;
}

/* The socket shut: the helper sees its stdin end and logs out, the thread ends */
static void stop_run(void) {
    atomic_store(&running, false);
    pthread_mutex_lock(&st_lock);
    if (helper_fd >= 0) shutdown(helper_fd, SHUT_RDWR);
    pthread_mutex_unlock(&st_lock);
}

/* GUI thread, after the thread */
static void session_ended(void *arg) {
    (void)arg;
    thread_live = false;
    if (restart_pending && want_on) {
        restart_pending = false;
        if (!launch(want_tg)) {
            want_on = false;
            msg_update_text_fmt("DMR: %s", _("the helper could not start"));
        }
        return;
    }
    restart_pending = false;
    want_on         = false;    /* the helper gave up (its error stays on screen) or the window closed */
}

static bool session_start(uint32_t tg) {
    want_on = true;
    want_tg = tg;
    if (thread_live) {      /* another talkgroup: once the one before has logged out */
        restart_pending = true;
        stop_run();
        pthread_mutex_lock(&st_lock);
        st.conn = DMR_CONN_CONNECTING;
        st.tg   = tg;
        pthread_mutex_unlock(&st_lock);
        return true;
    }
    if (!launch(tg)) {
        want_on = false;
        return false;
    }
    return true;
}

static void session_stop(void) {
    want_on         = false;
    restart_pending = false;
    stop_run();
}

/* ---------- the window ---------- */

static lv_obj_t   *head_lbl, *state_lbl, *talk_lbl, *table, *heard_lbl;
static lv_timer_t *status_timer;
static dmr_state_t ss;              /* the session as status_cb last read it */
static bool        ss_on;           /* a helper runs, or is about to */

static const char *conn_label(void) {
    return ss_on ? "Disconnect" : "Connect";
}

static void ago_text(char *buf, size_t n, uint64_t ms) {
    uint64_t s = ms / 1000;

    if (s < 60) snprintf(buf, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(buf, n, "%um", (unsigned)(s / 60));
    else snprintf(buf, n, "%uh", (unsigned)(s / 3600));
}

static void tg_text(char *buf, size_t n, uint32_t tg) {
    if (tg) snprintf(buf, n, "TG %u", (unsigned)tg);
    else snprintf(buf, n, "%s", _("the master's talkgroups"));
}

/* A cell, only when its text changed (the table is shown again four times a second) */
static void cell(uint16_t row, uint16_t col, const char *txt) {
    const char *was = lv_table_get_cell_value(table, row, col);

    if (!was || strcmp(was, txt)) lv_table_set_cell_value(table, row, col, txt);
}

/* The talkgroups: the one heard marked */
/* Speech: a talkgroup's row, "T G 91, Worldwide, listening" (the table's cells have the number and name apart) */
static void tg_row_said(lv_obj_t *t, uint16_t row, char *buf, size_t size) {
    uint32_t now_tg = ss_on ? ss.tg : (uint32_t)param_i_get(cfg_dmr_tg);

    (void)t;
    if (row >= tgs_n) {
        buf[0] = '\0';
        return;
    }
    if (tgs[row].tg)
        snprintf(buf, size, "T G %u%s%s%s", (unsigned)tgs[row].tg, tgs[row].name[0] ? ", " : "", tgs[row].name,
                 tgs[row].tg == now_tg && ss_on ? ", listening" : "");
    else
        snprintf(buf, size, "%s%s", _("the master's talkgroups"), now_tg == 0 && ss_on ? ", listening" : "");
}

static void show_tgs(bool again) {
    char     line[96], tg[48];
    uint32_t now_tg = ss_on ? ss.tg : (uint32_t)param_i_get(cfg_dmr_tg);

    if (!table) return;
    if (again) lv_table_set_row_cnt(table, tgs_n);
    for (int i = 0; i < tgs_n; i++) {
        tg_text(tg, sizeof(tg), tgs[i].tg);
        snprintf(line, sizeof(line), "%s %s", tgs[i].tg == now_tg && (ss_on || now_tg) ? ">" : "  ", tg);
        cell(i, 0, line);
        cell(i, 1, tgs[i].name);
    }
}

/* The last heard, the newest first */
static void show_heard(void) {
    uint64_t now = get_time();
    char     text[1024], t[16], who[DMR_NAME_MAX + 8];
    size_t   n = 0;

    if (!heard_lbl) return;
    n += (size_t)snprintf(text + n, sizeof(text) - n, "#%06x %s#", theme_hex(TC_text_dim), _("Heard"));
    for (int i = 0; i < ss.n_heard && i < 9 && n < sizeof(text) - 120; i++) {
        const dmr_call_t *c   = &ss.heard[i];
        uint64_t          end = c->start_ms + (uint64_t)c->secs * 1000;

        ago_text(t, sizeof(t), now > end ? now - end : 0);
        dmr_proto_who(c, who, sizeof(who));
        n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%s  #%06x %s%u  %s, %us#", who, theme_hex(TC_text_dim),
                              c->private_call ? "" : "TG ", (unsigned)(c->private_call ? c->id : c->dst), t,
                              (unsigned)c->secs);
    }
    if (!ss.n_heard) snprintf(text + n, sizeof(text) - n, "\n#%06x %s#", theme_hex(TC_text_dim), _("nobody yet"));
    lv_label_set_text(heard_lbl, text);
}

static void show_state(void) {
    char tg[48];

    if (!ss_on && ss.conn == DMR_CONN_ERROR) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s: %s#", theme_hex(TC_err), _("Not connected"), _(ss.why));
    } else if (!set_up) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_warn),
                              _("Set up DMR on the radio's web page: the network, your DMR ID, the hotspot password"));
    } else if (!params.callsign.x[0]) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_warn),
                              _("Set your callsign first: Settings > General"));
    } else if (!ss_on) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("Not connected: choose a talkgroup with the MFK, or Connect"));
    } else if (ss.conn == DMR_CONN_ONLINE) {
        tg_text(tg, sizeof(tg), ss.tg);
        lv_label_set_text_fmt(state_lbl, "#%06x %s#  #%06x %s#  #%06x %s %u#", theme_hex(TC_ok), ss.master,
                              theme_hex(TC_accent), tg, theme_hex(TC_text_dim), _("hotspot"), (unsigned)ss.hotspot);
    } else if (ss.conn == DMR_CONN_RETRY) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s: %s#", theme_hex(TC_warn), _("Not connected"), _(ss.why));
    } else {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim), _("Logging in to the network"));
    }
}

static void show_talk(void) {
    char        who[DMR_NAME_MAX + 8], t[16];
    static char said_talker[DMR_NAME_MAX + 40];  /* speech: the call said, "" none under way */

    if (ss_on && ss.talking) {
        const dmr_call_t *c = &ss.talker;
        char              now[sizeof(said_talker)];

        dmr_proto_who(c, who, sizeof(who));
        ago_text(t, sizeof(t), get_time() - c->start_ms);
        snprintf(now, sizeof(now), "%s %u %llu", who, (unsigned)(c->private_call ? 0 : c->dst),
                 (unsigned long long)c->start_ms);
        if (strcmp(now, said_talker)) {         /* a new call: who, and where (the seconds go on, not said) */
            char spelled[DMR_NAME_MAX + 40];

            snprintf(said_talker, sizeof(said_talker), "%s", now);
            voice_spell_calls(who, spelled, sizeof(spelled));
            if (voice_auto_active()) {
                if (c->private_call)
                    voice_say_text_fmt("Talking: %s, a private call to you", spelled);
                else
                    voice_say_text_fmt("Talking: %s, T G %u", spelled, (unsigned)c->dst);
            }
        }
        if (c->private_call)
            lv_label_set_text_fmt(talk_lbl, "#%06x %s# #%06x %s# #%06x %s %s#", theme_hex(TC_text_dim),
                                  _("Talking now:"), theme_hex(TC_accent), who, theme_hex(TC_text_dim),
                                  _("a private call to you"), t);
        else
            lv_label_set_text_fmt(talk_lbl, "#%06x %s# #%06x %s# #%06x TG %u  %s#", theme_hex(TC_text_dim),
                                  _("Talking now:"), theme_hex(TC_accent), who, theme_hex(TC_text_dim),
                                  (unsigned)c->dst, t);
    } else if (ss_on && ss.conn == DMR_CONN_ONLINE) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("Listening only: the radio never transmits for DMR"));
    } else {
        lv_label_set_text(talk_lbl, "");
    }
    if (!(ss_on && ss.talking)) said_talker[0] = '\0';
}

static void status_cb(lv_timer_t *t) {
    static const char *conn_shown;
    bool               again = read_conf();

    (void)t;
    if (!state_lbl) return;
    pthread_mutex_lock(&st_lock);
    ss = st;
    pthread_mutex_unlock(&st_lock);
    ss_on = want_on;
    show_state();
    show_talk();
    show_tgs(again);
    show_heard();
    {
        static char said_state[200];

        voice_say_label_change(state_lbl, said_state, sizeof(said_state), "DMR");
    }
    if (conn_label() != conn_shown) {
        conn_shown = conn_label();
        buttons_refresh(&btn_conn);
    }
}

/* ---------- the map ---------- */

static lv_obj_t     *map_obj;       /* the canvas; NULL while the lists show */
static lv_timer_t   *map_timer;
static int           map_zoom = 1;
static uint32_t      map_want_ms, map_kept_ms;      /* lv_tick of the last touch and the last "map" asked; 0 never */
static live_map_pt_t map_pts[MAP_PTS + 1];

static const char *map_label(void) {
    return map_obj ? "List" : "Map";
}

static time_t file_mtime(const char *path) {
    struct stat sb;

    return stat(path, &sb) == 0 ? sb.st_mtime : 0;
}

/* The helper's "map": the stations kept, to MAP_KEPT; its end waited for here */
static void *map_kept_fn(void *arg) {
    char                     **env    = arg;
    char                      *argv[] = {(char *)HELPER, (char *)"map", (char *)MAP_KEPT, NULL};
    posix_spawn_file_actions_t fa;
    pid_t                      pid;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (posix_spawn(&pid, HELPER, &fa, NULL, argv, env) == 0) waitpid(pid, NULL, 0);
    posix_spawn_file_actions_destroy(&fa);
    free(env);
    return NULL;
}

/* While the map shows: MAP_WANT touched, and the stations kept asked for while no helper writes MAP_FILE */
static void map_ask(bool live) {
    if (!map_want_ms || lv_tick_elaps(map_want_ms) > MAP_WANT_MS) {
        int fd = open(MAP_WANT, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

        if (fd >= 0) {
            futimens(fd, NULL);
            close(fd);
        }
        map_want_ms = lv_tick_get() | 1;
    }
    if (!live && (!map_kept_ms || lv_tick_elaps(map_kept_ms) > MAP_KEPT_MS)) {
        char    **env = helper_env();
        pthread_t t;

        map_kept_ms = lv_tick_get() | 1;
        if (env && pthread_create(&t, NULL, map_kept_fn, env) == 0) pthread_detach(t);
        else free(env);
    }
}

static cJSON *map_read(const char *path) {
    FILE  *f = fopen(path, "r");
    cJSON *j = NULL;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);

    fseek(f, 0, SEEK_SET);
    char *text = len > 0 && len < 512 * 1024 ? malloc((size_t)len + 1) : NULL;

    if (text) {
        text[fread(text, 1, (size_t)len, f)] = 0;
        j = cJSON_Parse(text);
        free(text);
    }
    fclose(f);
    return j;
}

static bool map_num(const cJSON *o, const char *k, double *v) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);

    if (!cJSON_IsNumber(j)) return false;
    *v = j->valuedouble;
    return true;
}

static const char *map_str(const cJSON *o, const char *k) {
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));

    return t ? t : "";
}

static void map_show(void) {
    bool        live = want_on && time(NULL) - file_mtime(MAP_FILE) < MAP_FRESH_S;
    char        box[200] = "", footer[160], place[96];
    double      lat, lon, clat = 0, clon = 0;
    const char *src;
    int         n = 0, heard = 0;

    if (!map_obj) return;
    map_ask(live);
    cJSON *j = map_read(live ? MAP_FILE : MAP_KEPT);
    cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "talker"), *st;

    /* The station talking: only what a helper that listens says of it */
    if (live && cJSON_IsObject(t)) {
        const char *call = map_str(t, "call"), *city = map_str(t, "city"), *country = map_str(t, "country");
        double      id = 0, tg = 0;

        map_num(t, "id", &id);
        map_num(t, "tg", &tg);
        snprintf(place, sizeof(place), "%s%s%s", city, city[0] && country[0] ? ", " : "", country);
        if (call[0]) snprintf(box, sizeof(box), "%.15s\n%.40s\n%s\nTG %u", call, map_str(t, "name"), place, (unsigned)tg);
        else snprintf(box, sizeof(box), "%u\n%s\nTG %u", (unsigned)id, _("its callsign is not known yet"), (unsigned)tg);
        if (map_num(t, "lat", &lat) && map_num(t, "lon", &lon)) {
            map_pts[n].lat  = (float)lat;
            map_pts[n].lon  = (float)lon;
            map_pts[n].kind = LIVE_MAP_HIGHLIGHT;
            snprintf(map_pts[n].label, sizeof(map_pts[n].label), "%s", call);
            n++;
        }
    }
    cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(j, "stations")) {
        if (n >= MAP_PTS || !map_num(st, "lat", &lat) || !map_num(st, "lon", &lon)) continue;
        map_pts[n].lat      = (float)lat;
        map_pts[n].lon      = (float)lon;
        map_pts[n].kind     = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "recent")) ? LIVE_MAP_ACTIVE : LIVE_MAP_IDLE;
        map_pts[n].label[0] = 0;
        n++;
        heard++;
    }
    cJSON_Delete(j);

    if (heard) {
        snprintf(footer, sizeof(footer), _("%d stations heard in the last 24 hours, each in its callsign's country"), heard);
        size_t l = strlen(footer);

        if (map_zoom > 1) snprintf(footer + l, sizeof(footer) - l, "   x%d", map_zoom);
    } else {
        snprintf(footer, sizeof(footer), "%s", _("No station heard in the last 24 hours: the ones the app hears come here"));
    }
    if (station_position(&lat, &lon, &src)) {     /* without a station talking: the radio's own part of the world */
        clat = lat;
        clon = lon;
    }
    live_map_draw(map_pts, n, map_zoom, clat, clon, box[0] ? box : NULL, footer);
}

static void map_timer_cb(lv_timer_t *t) {
    (void)t;
    map_show();
}

static void map_close(void);

/* The map's keys: the MFK's turn zooms, its press goes back to the lists */
static void map_key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            if (map_zoom < MAP_ZOOM_MAX) map_zoom *= 2;
            map_show();
            break;
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            if (map_zoom > 1) map_zoom /= 2;
            map_show();
            break;
        case LV_KEY_ENTER:
            map_close();
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

static void map_pressed_cb(lv_event_t *e) {
    (void)e;
    map_close();
}

/* Back to the lists */
static void map_close(void) {
    if (!map_obj) return;
    if (map_timer) lv_timer_del(map_timer);
    map_timer = NULL;
    lv_group_remove_obj(map_obj);
    lv_obj_del_async(map_obj);  /* a key of its own may have asked: not deleted under its event */
    map_obj = NULL;
    lv_obj_clear_flag(table, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(heard_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_group_add_obj(keyboard_group, table);
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);
    buttons_refresh(&btn_map);
}

/* Map / List: the map in the place of the talkgroups and the last heard */
static void map_cb(button_data_t *b) {
    (void)b;
    if (!dialog.obj || !table) return;
    if (map_obj) {
        map_close();
        return;
    }
    map_obj = live_map_create(dialog.obj, 780, DIALOG_H - 108);
    if (!map_obj) return;
    lv_obj_set_pos(map_obj, 10, 100);
    lv_obj_add_flag(table, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(heard_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_group_remove_obj(table);
    lv_obj_add_event_cb(map_obj, map_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(map_obj, map_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_group_add_obj(keyboard_group, map_obj);
    lv_group_focus_obj(map_obj);
    lv_group_set_editing(keyboard_group, true);
    map_want_ms = map_kept_ms = 0;
    unlink(MAP_KEPT);       /* one of an earlier look: not shown as now's */
    map_show();
    map_timer = lv_timer_create(map_timer_cb, MAP_MS, NULL);
    buttons_refresh(&btn_map);
}

static bool may_start(void) {
    read_conf();
    if (!set_up) {
        msg_update_text_fmt("DMR: %s", _("set it up on the radio's web page first (DMR)"));
        return false;
    }
    if (!params.callsign.x[0]) {
        msg_update_text_fmt("DMR: %s", _("set your callsign first (Settings > General)"));
        return false;
    }
    return true;
}

static void listen_to(uint32_t tg) {
    char text[48];

    if (!may_start()) return;
    param_i_set(cfg_dmr_tg, (int32_t)tg);
    if (!session_start(tg)) {
        msg_update_text_fmt("DMR: %s", _("the helper could not start"));
        return;
    }
    tg_text(text, sizeof(text), tg);
    msg_update_text_fmt("DMR: %s", text);
    status_cb(NULL);
}

static int selected(void) {
    uint16_t row = LV_TABLE_CELL_NONE, col = 0;

    if (!table) return -1;
    lv_table_get_selected_cell(table, &row, &col);
    return row == LV_TABLE_CELL_NONE || row >= tgs_n ? -1 : row;
}

/* The MFK on row i. This LVGL has no lv_table_set_selected_cell(): the keys the MFK sends move it, as app_sstv_tx.c */
static void select_row(int i) {
    uint32_t key = LV_KEY_DOWN;

    if (selected() < 0) lv_event_send(table, LV_EVENT_KEY, &key);
    for (int r = selected(); r >= 0 && r < i; r = selected()) {
        int was = r;

        lv_event_send(table, LV_EVENT_KEY, &key);
        if (selected() == was) break;
    }
}

/* Connect: the talkgroup last chosen when it is still in the list, else the row the MFK is on */
static void conn_cb(button_data_t *b) {
    (void)b;
    if (want_on) {
        session_stop();
        status_cb(NULL);
        return;
    }
    read_conf();
    uint32_t last = (uint32_t)param_i_get(cfg_dmr_tg);
    int      i    = selected();

    for (int k = 0; k < tgs_n; k++)
        if (tgs[k].tg == last) i = k;
    listen_to(tgs[i < 0 ? 0 : i].tg);
}

/* The MFK on a row: that talkgroup heard (the helper logs in again with it) */
static void pressed_cb(lv_event_t *e) {
    (void)e;
    int i = selected();

    if (i < 0) return;
    if (want_on && ss.tg == tgs[i].tg && !restart_pending) return;
    listen_to(tgs[i].tg);
}

/* Before the table's own: the MFK's turn (left, right) moves a row, not a cell */
static void rows_only_cb(lv_event_t *e) {
    uint32_t *key = lv_event_get_param(e);

    if (*key == LV_KEY_RIGHT) *key = LV_KEY_DOWN;
    else if (*key == LV_KEY_LEFT) *key = LV_KEY_UP;
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

/* Any TX, whatever keyed it (the hand mic, VOX and the KEY jack too, which radio.c does not see coming): the call's
 * audio queued dropped at once */
static void radio_tx_cb(lv_event_t *e) {
    (void)e;
    if (want_on) audio_listen_cut();
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
    buttons_load_page(&page_1);
    conf_mtime = -1;
    read_conf();
    if (!thread_live) {     /* the last session's error or heard list: not shown again */
        pthread_mutex_lock(&st_lock);
        dmr_proto_init(&st);
        pthread_mutex_unlock(&st_lock);
    }

    head_lbl  = text(6, &sony_22);
    state_lbl = text(36, &sony_24);
    talk_lbl  = text(68, &sony_22);
    lv_label_set_text_fmt(head_lbl, "#%06x DMR#  #%06x %s#", theme_hex(TC_text_head), theme_hex(TC_text_dim),
                          _("talkgroups on the internet, listening only, never on the air"));

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_pos(table, 10, 100);
    lv_obj_set_size(table, 470, DIALOG_H - 108);
    lv_table_set_col_cnt(table, 2);
    lv_table_set_col_width(table, 0, 220);
    lv_table_set_col_width(table, 1, 240);
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
    lv_obj_add_event_cb(table, rows_only_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_group_add_obj(keyboard_group, table);
    voice_focus_set_row_cb(table, tg_row_said);
    voice_focus_hint("DMR, talkgroups on the internet, listening only. MFK: choose a talkgroup");
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);
    show_tgs(true);
    for (int i = 0; i < tgs_n; i++) {      /* the MFK on the talkgroup last chosen */
        if (tgs[i].tg == (uint32_t)param_i_get(cfg_dmr_tg)) {
            select_row(i);
            break;
        }
    }

    heard_lbl = lv_label_create(dialog.obj);
    lv_label_set_recolor(heard_lbl, true);
    lv_label_set_long_mode(heard_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(heard_lbl, 300, DIALOG_H - 112);
    lv_obj_set_style_text_font(heard_lbl, &sony_20, 0);
    lv_obj_set_style_text_color(heard_lbl, TC(text), 0);
    lv_obj_set_pos(heard_lbl, 490, 104);

    lv_obj_add_event_cb(dialog.obj, radio_tx_cb, EVENT_RADIO_TX, NULL);
    status_timer = lv_timer_create(status_cb, 250, NULL);
    status_cb(NULL);
}

/* Closing the window logs out */
static void destruct_cb(void) {
    session_stop();
    if (map_timer) lv_timer_del(map_timer);
    map_timer = NULL;
    if (map_obj) lv_group_remove_obj(map_obj);
    map_obj = NULL;         /* deleted with the window */
    if (status_timer) lv_timer_del(status_timer);
    status_timer = NULL;
    lv_group_remove_obj(table);
    head_lbl = state_lbl = talk_lbl = table = heard_lbl = NULL;
}
