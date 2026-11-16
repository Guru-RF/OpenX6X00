/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the SVXConnect app's session: /usr/bin/svxconnect (SVXConnect-CLI, tools/svxconnect) run as
 *  `svxconnect --stdio -c /mnt/svxconnect/svxconnect.conf`, started from the app (dialog_svx.c) in the internet voice
 *  apps' session (voice_net.c), which plays the talkgroup, sends the user's overs and runs the Simplex repeater under
 *  main's TX rules. Here: the helper's command line, its lines (svx_proto.c) and its commands, and the Bluetooth mic.
 *
 *  The helper is SVXConnect's own client, built for the radio with the audio through the GUI (AUDIO=pipe): it logs in
 *  with the certificate the reflector's sysop signed (the web page asks for it), follows the talkgroups by their
 *  priorities, and sends and takes Opus over the encrypted UDP. Its audio and the GUI's cross the session's socket at
 *  16 kHz, as Zello's do. Its own time limit (tx_timeout_sec on the web page) ends an over it sends.
 *
 *  The Bluetooth mic: the helper has no sound card, so the GUI records a headset's mic (a bluez_source.* or
 *  bluez_input.* PulseAudio has on the headset's own profile) with parec at 16 kHz while an over from it is sent, and
 *  sends it as the hand mic's would go; pactl tells whether one is there while Talk via says Bluetooth. The radio's
 *  images keep headsets on A2DP (rootfs/usr/sbin/x6100-zello says why), so there it says "No BT mic".
 *
 *  Its log, without the key or anything secret in it: /mnt/app_logs/svxconnect.txt.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "svx.h"

#include "cfg/cfg_api.h"
#include "util.h"

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
#include <sys/wait.h>
#include <unistd.h>

#ifndef X6100_SVX_BIN
#define X6100_SVX_BIN "/usr/bin/svxconnect"
#endif

#define BT_LOOK_MS  3000            /* while Talk via says Bluetooth: pactl asked this often */
#define BT_FRAME    320             /* samples a read of parec, 20 ms at 16 kHz */
#define BT_FIRST_MS 3000            /* parec's first audio: the over waits this long for it */

extern char **environ;

/* What the helper said, from the play thread to the screen */
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static svx_state_t     st;

/* The Bluetooth mic */
static pthread_mutex_t bt_lock = PTHREAD_MUTEX_INITIALIZER;
static char            bt_source[128];      /* the headset's mic pactl found, "" none */
static pid_t           bt_pid;              /* parec, while an over from it is sent */
static pthread_t       bt_thread;
static bool            bt_reading;
static atomic_bool     bt_looking;          /* a pactl run in flight */
static atomic_bool     bt_ptt;              /* the over's ptt on waits for parec's first audio */
static uint64_t        bt_looked_ms;

/* The helper's program: X6100_SVX_BIN in the environment (the PC's tests and screenshots), else the radio's */
static const char *bin(void) {
    const char *b = getenv("X6100_SVX_BIN");

    return b && *b ? b : X6100_SVX_BIN;
}

bool svx_set_up(void) {
    return access(SVX_CONF, R_OK) == 0 && access(bin(), X_OK) == 0;
}

static char *const *s_argv(void) {
    static char  path[256];
    static char *argv[] = {path, "--stdio", "-c", SVX_CONF, NULL};

    if (!svx_set_up()) return NULL;
    snprintf(path, sizeof(path), "%s", bin());
    return argv;
}

static void s_reset(void) {
    pthread_mutex_lock(&st_lock);
    svx_proto_init(&st);
    st.conn = SVX_CONN_CONNECTING;
    pthread_mutex_unlock(&st_lock);
}

/* A line of the helper's (play thread) */
static void s_line(char *line) {
    char     why[96];
    svx_ev_t ev;

    pthread_mutex_lock(&st_lock);
    ev = svx_proto_line(&st, line, get_time(), why, sizeof(why));
    vnet_set_connected(st.conn == SVX_CONN_CONNECTED);
    vnet_set_talker(svx_proto_talker(&st) != NULL);
    pthread_mutex_unlock(&st_lock);

    switch (ev) {
        case SVX_EV_TX_ON:   vnet_talk_event(VNET_TALK_ON, NULL); break;
        case SVX_EV_TX_OFF:  vnet_talk_event(VNET_TALK_OFF, why); break;
        case SVX_EV_REFUSED: vnet_talk_event(VNET_TALK_REFUSED, why); break;
        case SVX_EV_ERROR:   vnet_gave_up(); break;
        default:             break;
    }
}

/* ---------- the Bluetooth mic ---------- */

#define ENV_MAX 128

/* The environment with PulseAudio's system socket, as the Zello helper has it: into env, server its text */
static void pulse_env(char **env, char *server, size_t n) {
    const char *ps = getenv("PULSE_SERVER");
    int         k  = 0;

    snprintf(server, n, "PULSE_SERVER=%s", ps && *ps ? ps : "unix:/var/run/pulse/native");
    for (char **e = environ; *e && k < ENV_MAX - 2; e++) {
        if (strncmp(*e, "PULSE_SERVER=", 13)) env[k++] = *e;
    }
    env[k++] = server;
    env[k]   = NULL;
}

/* A program with its stdout into a pipe: its pid, *out the read end; -1 it could not start */
static pid_t spawn_out(char *const argv[], int *out) {
    posix_spawn_file_actions_t fa;
    int                        pp[2];
    pid_t                      pid;
    char                      *env[ENV_MAX], server[192];

    if (pipe2(pp, O_CLOEXEC) != 0) return -1;
    pulse_env(env, server, sizeof(server));
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pp[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    close(pp[1]);
    if (r != 0) {
        close(pp[0]);
        return -1;
    }
    *out = pp[0];
    return pid;
}

/* pactl's sources: the first headset mic (not a monitor), into bt_source; the session told */
static void *look_fn(void *arg) {
    (void)arg;
    char *const argv[] = {"pactl", "list", "short", "sources", NULL};
    char        buf[8192], found[128] = "";
    size_t      n = 0;
    int         fd;
    pid_t       pid = spawn_out(argv, &fd);

    if (pid > 0) {
        ssize_t r;

        while (n < sizeof(buf) - 1 && (r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0) n += (size_t)r;
        close(fd);
        waitpid(pid, NULL, 0);
    }
    buf[n] = 0;
    for (char *save = NULL, *l = strtok_r(buf, "\n", &save); l && !found[0]; l = strtok_r(NULL, "\n", &save)) {
        char *name = strchr(l, '\t'), *end;

        if (!name) continue;
        name++;
        end = strchr(name, '\t');
        if (end) *end = 0;
        if ((!strncmp(name, "bluez_source.", 13) || !strncmp(name, "bluez_input.", 12)) &&
            !strstr(name, ".monitor") && strlen(name) < sizeof(found)) {
            snprintf(found, sizeof(found), "%s", name);
        }
    }
    pthread_mutex_lock(&bt_lock);
    snprintf(bt_source, sizeof(bt_source), "%s", found);
    pthread_mutex_unlock(&bt_lock);
    vnet_set_btmic(found[0] != 0);
    atomic_store(&bt_looking, false);
    return NULL;
}

static void bt_look(void) {
    pthread_t t;

    if (atomic_exchange(&bt_looking, true)) return;
    if (pthread_create(&t, NULL, look_fn, NULL) != 0) {
        atomic_store(&bt_looking, false);
        return;
    }
    pthread_detach(t);
}

/* parec's audio, a frame at a time, to the helper as the over's. The over starts (ptt on) with its first frame:
 * svxconnect ends an over its mic gives nothing for half a second, and a headset's mic can take longer to start */
static void *read_fn(void *arg) {
    int           fd = (int)(intptr_t)arg;
    int16_t       pcm[BT_FRAME];
    size_t        have = 0;
    struct pollfd pf   = {.fd = fd, .events = POLLIN};

    if (poll(&pf, 1, BT_FIRST_MS) <= 0 && atomic_exchange(&bt_ptt, false)) {
        vnet_talk_event(VNET_TALK_REFUSED, "the Bluetooth mic gives no audio");
    }
    for (;;) {
        ssize_t r = read(fd, (uint8_t *)pcm + have, sizeof(pcm) - have);

        if (r <= 0) break;
        have += (size_t)r;
        if (have == sizeof(pcm)) {
            if (atomic_exchange(&bt_ptt, false)) vnet_send("ptt on\n");
            vnet_send_audio(pcm, BT_FRAME);
            have = 0;
        }
    }
    if (atomic_exchange(&bt_ptt, false)) vnet_talk_event(VNET_TALK_REFUSED, "the Bluetooth mic gives no audio");
    close(fd);
    return NULL;
}

/* parec on the headset's mic, and the thread that reads it. False: it did not start */
static bool bt_start(void) {
    char  dev[160];
    int   fd;
    pid_t pid;
    bool  ok = false;

    pthread_mutex_lock(&bt_lock);
    if (bt_pid > 0 || !bt_source[0]) {
        pthread_mutex_unlock(&bt_lock);
        return bt_pid > 0;
    }
    snprintf(dev, sizeof(dev), "--device=%s", bt_source);

    char *const argv[] = {"parec", dev, "--rate=16000", "--channels=1", "--format=s16le", "--latency-msec=60",
                          "--raw", NULL};

    atomic_store(&bt_ptt, true);
    pid = spawn_out(argv, &fd);
    if (pid > 0) {
        if (pthread_create(&bt_thread, NULL, read_fn, (void *)(intptr_t)fd) == 0) {
            bt_reading = true;
            ok         = true;
        } else {
            close(fd);
        }
        bt_pid = pid;
    }
    if (!ok) atomic_store(&bt_ptt, false);
    pthread_mutex_unlock(&bt_lock);
    return ok;
}

static void bt_stop(void) {
    pid_t     pid;
    pthread_t t;
    bool      reading;

    atomic_store(&bt_ptt, false);
    pthread_mutex_lock(&bt_lock);
    pid        = bt_pid;
    t          = bt_thread;
    reading    = bt_reading;
    bt_pid     = 0;
    bt_reading = false;
    pthread_mutex_unlock(&bt_lock);
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    if (reading) pthread_join(t, NULL);     /* parec gone: its pipe ends */
    waitpid(pid, NULL, 0);
}

/* ---------- the helper's commands ---------- */

/* An over's start or end, the user's or the link's from the receiver: the PTT of svxconnect's control commands. From a
 * Bluetooth headset the ptt on waits for its mic's first audio (read_fn) */
static void s_talk(bool start, bool bt, bool link) {
    if (start) {
        if (!bt || link) vnet_send("ptt on\n");
        else if (!bt_start()) vnet_talk_event(VNET_TALK_REFUSED, "the Bluetooth mic did not start");
    } else {
        vnet_send("ptt off\n");
        if (!link) bt_stop();
    }
}

/* While Talk via says Bluetooth: is a headset's mic there */
static void s_tick(void) {
    uint64_t now = get_time();

    if (param_i_get(cfg_svx_talk_bt) && now - bt_looked_ms >= BT_LOOK_MS) {
        bt_looked_ms = now;
        bt_look();
    }
}

const vnet_app_t svx_app = {
    .name    = "SVXConnect",
    .net     = "SVX",
    .badge   = "SVX LINK",
    .tag     = "S",
    .not_yet = "not on the reflector yet",
    .user    = AUDIO_USER_SVX,
    .talk_bt = &cfg_svx_talk_bt,
    .to_rf   = &cfg_svx_to_rf,
    .to_net  = &cfg_svx_to_svx,
    .limit   = &cfg_svx_limit,
    .pause   = &cfg_svx_pause,
    .id      = &cfg_svx_id,
    .id_min  = &cfg_svx_id_min,
    .argv    = s_argv,
    .log     = BOARD_DATA "/app_logs/svxconnect.txt",
    .reset   = s_reset,
    .line    = s_line,
    .talk    = s_talk,
    .tick    = s_tick,
};

/* ---------- the API ---------- */

void svx_status(svx_status_t *out) {
    memset(out, 0, sizeof(*out));
    vnet_status(&svx_app, &out->v);
    pthread_mutex_lock(&st_lock);
    out->h = st;
    pthread_mutex_unlock(&st_lock);
    if (!out->v.on) out->h.conn = SVX_CONN_NONE;
    out->set_up = svx_set_up();
}

bool svx_connect(void) {
    bt_looked_ms = 0;
    return vnet_connect(&svx_app);
}

void svx_disconnect(void) {
    vnet_disconnect(&svx_app);
}

void svx_window_opened(void) {
    vnet_window_opened(&svx_app);
}

void svx_window_closed(void) {
    vnet_window_closed(&svx_app);
}

void svx_talk_start(bool by_ptt) {
    vnet_talk_start(&svx_app, by_ptt);
}

void svx_talk_stop(void) {
    vnet_talk_stop(&svx_app);
}

void svx_select(uint32_t tg) {
    char line[32];

    if (vnet_current() != &svx_app) return;
    snprintf(line, sizeof(line), "tg %u\n", (unsigned)tg);
    vnet_send(line);
}

void svx_lock(bool on) {
    if (vnet_current() != &svx_app) return;
    vnet_send(on ? "lock on\n" : "lock off\n");
}

void svx_link_rearm_rf(void) {
    vnet_link_rearm_rf(&svx_app);
}

bool svx_mic_quiet(void) {
    return vnet_mic_quiet(&svx_app);
}
