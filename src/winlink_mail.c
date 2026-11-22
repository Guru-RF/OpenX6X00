/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - APP > Mail: Winlink mail on the radio (#294). The inbox, the outbox and the sent messages, a message
 *  read, a new one or a reply written, and a gateway called, all on the radio, with the touch screen, the MFK or a
 *  USB or Bluetooth keyboard: no phone or WiFi, where the web mailbox needs one of them. Plain text only;
 *  attachments and forms stay in the web mailbox.
 *
 *  Pat stays the mail program: x6100-winlink-mail (rootfs/usr/sbin) asks its API, and runs in a thread of its own
 *  here, its JSON answer coming back through the scheduler. The web mailbox and the radio show the same mailbox. A
 *  connection is Pat's, through the modem and src/winlink.c, with every check before a key that a connection from
 *  the web mailbox has; Pat's questions during it (a busy channel, the password, a new account) are answered
 *  here, on the buttons. x6100-winlink turns Winlink on and off, keeps the password and changes the modem, as the web
 *  page does. Connect's first press puts the radio on the gateway's frequency and sends nothing (winlink_preset(),
 *  src/winlink.c), for the antenna tuner; the second calls.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE     /* pipe2 */
#endif

#include "winlink_mail.h"
#include "voice.h"
#include "voice_focus.h"

#include "app_frame.h"
#include "buttons.h"
#include "cJSON.h"
#include "events.h"
#include "keyboard.h"
#include "msg.h"
#include "radio.h"
#include "scheduler.h"
#include "styles.h"
#include "textarea_window.h"
#include "theme.h"
#include "winlink.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#ifndef X6100_SBIN
#define X6100_SBIN    "/usr/sbin"
#endif
#define HELPER        X6100_SBIN "/x6100-winlink-mail"
#define SUPERVISOR    X6100_SBIN "/x6100-winlink"
#define OUT_MAX       (2 * 1024 * 1024)  /* bytes of a helper's answer read */
#define ROW_H         30
#define LOG_LINES     6
#define POLL_S        15                 /* s between looks at Pat's state in the lists */
#define POLL_FAST_S   2                  /* while Winlink starts or stops */
#define CONFIRM_S     5                  /* s a second press has, for Connect and Delete */
/* s for the modem chosen to be ready: Pat restarts (15 s at most), the other one stops (Mercury with a session still
 * up ends it on the air first, x6100-winlink's MERCURY_TERM_WAIT: 55 s at most), the chosen one starts */
#define MODEM_WAIT_S  100
#define TO_MAX        200
#define SUBJECT_MAX   128                /* Pat refuses longer */
#define BODY_MAX      8000               /* characters typed on the radio: Winlink over HF is slow */
#define LINE_MAX      120                /* a line typed on the on-screen keys */

#define COLOR_HEAD    theme_hex(TC_text_head)
#define COLOR_DIM     theme_hex(TC_text_dim)

typedef enum { V_LIST, V_READ, V_COMPOSE, V_GATEWAYS, V_SESSION } view_t;

static const char *const boxes[]      = {"in", "out", "sent"};
static const char *const box_names[]  = {N_("Inbox"), N_("Outbox"), N_("Sent")};
#define BOXES ((int)(sizeof(boxes) / sizeof(boxes[0])))

/* The bands of Pat's gateway list (app/freq.go), All first */
static const char *const bands[] = {"", "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"};
#define BANDS ((int)(sizeof(bands) / sizeof(bands[0])))

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void click_cb(lv_event_t *e);

static buttons_page_t page_list_1, page_list_2;

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page_list_1,
    .audio_cb     = NULL,
    .key_cb       = NULL,
};

dialog_t *dialog_winlink_mail = &dialog;

/* ---- State, the GUI thread's ------------------------------------------------------------------------------------ */

static unsigned    gen;              /* a new one at each open: answers of jobs from before are dropped */
static app_frame_t frame;
static lv_obj_t   *body;
static lv_timer_t *timer;
static view_t      view;
static int         box_i;
static int         band_i;

static cJSON *status;                /* x6100-winlink-mail status, the last one */
static cJSON *list;                  /* the messages of the box shown */
static cJSON *message;               /* the one read */
static cJSON *gateways;
static int    sel, top;              /* selected row, first row shown: the message list and the gateways */
static int    rows_n;                /* rows that fit */
static lv_obj_t *rows[12];
static lv_obj_t *read_box;           /* V_READ: the scrolled text */

static bool   job_busy;
static bool   refresh_wanted;        /* a refresh asked while a job ran */
static time_t next_poll;
static char   note[200];             /* the status line: what was done, or what is wrong */
static time_t note_until;

static time_t confirm_until;         /* Connect or Delete pressed once */
static int    confirm_what;          /* 1 Connect, 2 Delete */
static char   confirm_url[220];      /* the gateway Connect was pressed for */

/* Connect's first press for a gateway (#322): the radio is put on its frequency and nothing is sent, so the antenna
 * tuner can be run there; the second press calls. The gateway the first press was for; the radio's side is
 * winlink_preset() (src/winlink.c). Kept across the window: sess_running counts the helpers of connections begun here
 * that have not ended, also once the window is closed, and the last one's end takes the radio back */
static char   tuned_url[220];
static char   ask_url[220];          /* the gateway of a first press, while Pat is asked that no connection runs */
static bool   tuned_shown;           /* what the Connect button says now: the second press */
static int    sess_running;

/* Modem pressed (#322, #323): the one asked for, followed in the status until it is ready or did not come up */
static bool   modem_moving;
static char   modem_to[16], modem_to_name[16];
static time_t modem_until;

/* A view's number: a job's answer for a view that is gone (left, or built again) changes no view */
static unsigned view_seq, res_seq;
static int      res_tag;             /* the job's box (a list) or band (the gateways) */
static bool     gateways_wanted;     /* the gateway list asked while a job ran */
static int      saved_sel, saved_top;   /* the message list's, while sel and top are the gateways' */
static bool     sel_is_gw;

/* A message being written, kept while Pat's question takes the screen; New brings it back */
static char *draft[4];
static int   draft_field;

/* Compose */
#define FIELDS 4
static lv_obj_t   *fields[FIELDS];   /* To, Cc, Subject, the text */
static int         field_i;
static bool        osk_open_now;
static bool        discard_armed;

/* The connection */
static pid_t    sess_pid;
static int      sess_in = -1;        /* the helper's stdin: answers, abort */
static bool     sess_on;
static char     sess_call[24], sess_freq[24];
static char     sess_log[LOG_LINES][160];
static int      sess_log_n;
static bool     sess_connected, sess_dialing;
static int      prog_done, prog_total;
static bool     prog_rx;
static char     prog_subject[84];
static char     sess_result[240];
static time_t   sess_started;
static time_t   sess_result_status_at;   /* the supervisor's word on the outcome, asked a little after the end */
static char     prompt_id[48], prompt_kind[32], prompt_msg[220];
static char     prompt_all[1024], prompt_checked[1024];  /* multi-select: every message, the small ones */
static bool     prompt_on;
static bool     prompt_password_open;

static void show(void);
static void refresh(void);
static void set_view(view_t v);
static void load_page(void);

/* ---- Helpers run in a thread ------------------------------------------------------------------------------------- */

typedef void (*job_cb_t)(int rc, cJSON *out);

typedef struct {
    job_cb_t cb;
    unsigned gen, seq;
    int      tag;
    int      rc;
    char    *out;
} job_res_t;

typedef struct {
    job_cb_t cb;
    unsigned gen, seq;
    int      tag;
    char    *argv[6];
    char    *in;
} job_t;

static void load_gateways(void);
static void modem_followed(void);

/* A helper with its stdin on a socket (the GUI never dies of SIGPIPE writing to one that has gone: MSG_NOSIGNAL) and
 * its stdout on a pipe; its stderr to nowhere. -1: it did not start */
static pid_t spawn_helper(char *const argv[], int *out_fd, int *in_fd) {
    int                        out[2], in[2];
    pid_t                      pid;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    sigset_t                   none, def;

    if (pipe2(out, O_CLOEXEC)) {
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in)) {
        close(out[0]);
        close(out[1]);
        return -1;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[1], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGTERM);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (posix_spawnp(&pid, argv[0], &fa, &attr, argv, environ) != 0) {
        pid = -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(out[1]);
    close(in[1]);
    if (pid < 0) {
        close(out[0]);
        close(in[0]);
        return -1;
    }
    *out_fd = out[0];
    *in_fd  = in[0];
    return pid;
}

static void send_all(int fd, const char *s) {
    size_t n = strlen(s);

    while (n) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return;
        }
        s += w;
        n -= w;
    }
}

static void job_done_cb(void *arg) {
    job_res_t *r   = arg;
    cJSON     *out = NULL;

    if (r->gen != gen || !body) {
        free(r->out);
        return;
    }
    job_busy = false;
    if (r->out && r->out[0]) {
        out = cJSON_Parse(r->out);
    }
    free(r->out);
    res_seq = r->seq;
    res_tag = r->tag;
    r->cb(r->rc, out);
    cJSON_Delete(out);
    if (gateways_wanted && !job_busy) {
        gateways_wanted = false;
        if (view == V_GATEWAYS) {
            load_gateways();
        }
    }
    if (refresh_wanted && !job_busy) {
        refresh_wanted = false;
        refresh();
    }
}

static void *job_thread(void *arg) {
    job_t    *j = arg;
    job_res_t r = {.cb = j->cb, .gen = j->gen, .seq = j->seq, .tag = j->tag, .rc = -1, .out = NULL};
    int       out_fd, in_fd;
    pid_t     pid = spawn_helper(j->argv, &out_fd, &in_fd);

    if (pid > 0) {
        size_t len = 0, cap = 4096;
        char  *buf = malloc(cap);
        int    st;

        if (j->in) {
            send_all(in_fd, j->in);
        }
        close(in_fd);
        while (buf) {
            ssize_t n;

            if (len + 1 >= cap) {
                char *b = cap < OUT_MAX ? realloc(buf, cap * 2) : NULL;

                if (!b) {
                    break;
                }
                buf = b;
                cap *= 2;
            }
            n = read(out_fd, buf + len, cap - len - 1);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            len += n;
        }
        close(out_fd);
        if (buf) {
            buf[len] = 0;
        }
        r.out = buf;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        r.rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    for (int i = 0; j->argv[i]; i++) {
        free(j->argv[i]);
    }
    if (j->in) {
        memset(j->in, 0, strlen(j->in));     /* a password, for one */
        free(j->in);
    }
    free(j);
    for (int tries = 0; !scheduler_put(job_done_cb, &r, sizeof(r)); tries++) {
        if (tries > 50) {     /* the queue full for 5 s: the answer is lost, the next refresh asks again */
            free(r.out);
            return NULL;
        }
        usleep(100000);
    }
    return NULL;
}

/* One job at a time; false (and nothing run) while one runs. tag: what its answer is for (res_tag then) */
static bool job_start_tag(job_cb_t cb, int tag, const char *in, const char *a0, const char *a1, const char *a2,
                          const char *a3) {
    pthread_t      t;
    pthread_attr_t attr;
    job_t         *j;
    const char    *a[] = {a0, a1, a2, a3};
    int            n   = 0;

    if (job_busy) {
        return false;
    }
    j = calloc(1, sizeof(*j));
    if (!j) {
        return false;
    }
    j->cb  = cb;
    j->gen = gen;
    j->seq = view_seq;
    j->tag = tag;
    for (int i = 0; i < 4 && a[i]; i++) {
        j->argv[n++] = strdup(a[i]);
    }
    j->in = in ? strdup(in) : NULL;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, job_thread, j) != 0) {
        for (int i = 0; i < n; i++) {
            free(j->argv[i]);
        }
        free(j->in);
        free(j);
        pthread_attr_destroy(&attr);
        return false;
    }
    pthread_attr_destroy(&attr);
    job_busy = true;
    return true;
}

static bool job_start(job_cb_t cb, const char *in, const char *a0, const char *a1, const char *a2, const char *a3) {
    return job_start_tag(cb, 0, in, a0, a1, a2, a3);
}

/* ---- Small things ------------------------------------------------------------------------------------------------ */

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static bool jbool(const cJSON *o, const char *key) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

static int jint(const cJSON *o, const char *key, int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsNumber(v) ? v->valueint : def;
}

static double jnum(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

/* A message for the status line, for a while; the error of a helper's answer when there is one. The format in the
 * language chosen, as msg_box.c's (a translation whose conversions differ is not taken, i18n.h) */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(note, sizeof(note), i18n_tr(fmt), ap);
    va_end(ap);
    note_until = time(NULL) + 8;
    show();
}

static void plain(char *dst, size_t size, const char *src);

static bool failed(int rc, const cJSON *out, const char *what) {
    if (rc == 0 && out) {
        return false;
    }
    if (out && jstr(out, "error")[0]) {
        char e[200];

        plain(e, sizeof(e), jstr(out, "error"));
        say("#%06x %s:# %s", theme_hex(TC_err), _(what), e);
    } else if (rc < 0 || rc == 127) {
        say("#%06x %s:# %s did not run", theme_hex(TC_err), _(what), HELPER);
    } else {
        say("#%06x %s:# no answer", theme_hex(TC_err), _(what));
    }
    return true;
}

/* "#" would start a colour in a recoloured label: shown as a space there */
static void plain(char *dst, size_t size, const char *src) {
    size_t i = 0;

    for (; *src && i + 1 < size; src++) {
        dst[i++] = *src == '#' ? ' ' : *src;
    }
    dst[i] = 0;
}

static bool winlink_on(void) {
    return status && jbool(status, "enabled");
}

static bool pat_up(void) {
    return status && jbool(status, "pat");
}

static bool pat_busy(void) {
    return sess_on || (status && (jbool(status, "connected") || jbool(status, "dialing")));
}

static int list_n(void) {
    return view == V_GATEWAYS ? cJSON_GetArraySize(gateways) : cJSON_GetArraySize(list);
}

/* A gateway's dial frequency in Hz, 0 when Pat's list has none for it */
static int32_t gw_hz(const cJSON *g) {
    double khz = g ? jnum(g, "dial") : 0;

    return khz > 0 && khz < 2000000.0 ? (int32_t)llround(khz * 1000.0) : 0;
}

/* Connect's first press was for this gateway and the radio is still on its frequency: the next press calls it */
static bool gw_tuned(const cJSON *g) {
    return g && tuned_url[0] && !strcmp(tuned_url, jstr(g, "url")) && winlink_preset_at(gw_hz(g));
}

/* No call after the first press (the list left, the app closed): the radio back where it was, unless a connection
 * begun here still runs, whose end does that */
static void tuned_drop(void) {
    tuned_url[0] = 0;
    if (!sess_running) {
        winlink_preset_back(false);
    }
}

static const char *modem_name(void) {
    return status && jstr(status, "modem_name")[0] ? jstr(status, "modem_name") : "ARDOP";
}

/* ---- Refresh: the state, the box shown --------------------------------------------------------------------------- */

/* The modem asked for with Modem, in the status just read: ready (Pat answers again, that modem runs and is on the
 * radio's audio), not started (the supervisor's reason), or not there in time. Until then the status line says how
 * far it is (show) */
static void modem_followed(void) {
    char t[160];

    if (!modem_moving || !status) {
        return;
    }
    plain(t, sizeof(t), jstr(status, "text"));
    if (strcmp(jstr(status, "modem_choice"), modem_to)) {
        modem_moving = false;       /* another one chosen meanwhile, on the Winlink page or by Pat's Connect */
        say("#%06x Modem:# now %s, chosen elsewhere meanwhile", theme_hex(TC_warn), modem_name());
    } else if (!winlink_on()) {
        modem_moving = false;
        say("Modem: %s, it starts with Winlink On", modem_to_name);
    } else if (pat_up() && !strcmp(jstr(status, "modem_state"), "running") &&
               (!strcmp(jstr(status, "modem"), "ready") || !strcmp(jstr(status, "modem"), "behind"))) {
        modem_moving = false;
        say("#%06x Modem: %s is ready#", theme_hex(TC_ok), modem_to_name);
    } else if (!strcmp(jstr(status, "state"), "error") && !strcmp(jstr(status, "modem_state"), "stopped")) {
        modem_moving = false;
        say("#%06x Modem: %s did not start:# %s", theme_hex(TC_err), modem_to_name, t);
    } else if (time(NULL) >= modem_until) {
        modem_moving = false;
        say("#%06x Modem: %s is not ready yet:# %s", theme_hex(TC_warn), modem_to_name, t);
    }
}

static void view_done(int rc, cJSON *out) {
    cJSON *st, *ls;

    if (failed(rc, out, "Mail")) {
        next_poll = time(NULL) + POLL_S;
        return;
    }
    st = cJSON_DetachItemFromObjectCaseSensitive(out, "status");
    ls = cJSON_DetachItemFromObjectCaseSensitive(out, "list");
    if (st) {
        cJSON_Delete(status);
        status = st;
    }
    if (ls && cJSON_IsArray(ls) && res_tag != box_i) {
        cJSON_Delete(ls);           /* the box before Box was pressed: the one shown now is asked again */
        refresh_wanted = true;
    } else if (ls && cJSON_IsArray(ls)) {
        char keep[16] = "";
        int *s        = sel_is_gw ? &saved_sel : &sel;     /* the list's selection, also under the gateways */

        if (list && *s < cJSON_GetArraySize(list)) {
            snprintf(keep, sizeof(keep), "%s", jstr(cJSON_GetArrayItem(list, *s), "mid"));
        }
        cJSON_Delete(list);
        list = ls;
        /* the selection stays on its message when one came in above it */
        *s = 0;
        for (int i = 0; keep[0] && i < cJSON_GetArraySize(list); i++) {
            if (!strcmp(jstr(cJSON_GetArrayItem(list, i), "mid"), keep)) {
                *s = i;
                break;
            }
        }
    } else {
        cJSON_Delete(ls);
    }
    modem_followed();
    {
        const char *state = jstr(status, "state");
        bool        moving = winlink_on() ? !pat_up() : !strcmp(state, "stopping") || !strcmp(state, "starting");

        next_poll = time(NULL) + (moving || modem_moving ? POLL_FAST_S : POLL_S);
    }
    if (sess_result_status_at == -1 && status && jstr(status, "last_text")[0] && sess_call[0] &&
        jint(status, "last_time", 0) >= sess_started - 5 && !strncmp(jstr(status, "last_gateway"), sess_call,
                                                                   strlen(sess_call)) &&
        (!strcmp(jstr(status, "last_result"), "delivered") || !strcmp(jstr(status, "last_result"), "done") ||
         !strcmp(jstr(status, "last_result"), "failed"))) {
        /* after a connection: the supervisor's word on it (from the logs), with what to do next; only its word on
         * this connection, not on an earlier one, nor one that is not finished in the logs yet */
        snprintf(sess_result, sizeof(sess_result), "%s%s%s", jstr(status, "last_text"),
                 jstr(status, "last_next")[0] ? ". " : "", jstr(status, "last_next"));
        sess_result_status_at = 0;
    }
    load_page();
    show();
}

static void refresh(void) {
    if (!job_start_tag(view_done, box_i, NULL, HELPER, "view", boxes[box_i], NULL)) {
        refresh_wanted = true;
    }
}

/* ---- Drawing ----------------------------------------------------------------------------------------------------- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void row_click_cb(lv_event_t *e);

/* A row of the lists: four labels, the selection its background; touched, it is chosen (row_click_cb) */
static lv_obj_t *make_row(int i) {
    static const int x[] = {6, 30, 190, 360}, w[] = {22, 156, 166, 395};
    lv_obj_t        *r   = lv_obj_create(body);

    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, APP_FRAME_W - 8, ROW_H);
    lv_obj_set_pos(r, 3, 4 + i * ROW_H);
    lv_obj_set_style_bg_color(r, TC(sel_bg), 0);
    lv_obj_set_style_radius(r, 4, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    for (int c = 0; c < 4; c++) {
        lv_obj_t *l = label(r, &sony_20, theme_hex(TC_text));

        lv_obj_set_pos(l, x[c], 3);
        lv_obj_set_width(l, w[c]);
        lv_obj_set_height(l, ROW_H - 4);           /* one line: what is longer ends in "..." */
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    return r;
}

static void row_set(int i, bool on, bool strong, const char *a, const char *b, const char *c, const char *d) {
    lv_obj_t   *r = rows[i];
    const char *t[4] = {a, b, c, d};

    lv_obj_clear_flag(r, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(r, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    for (int k = 0; k < 4; k++) {
        lv_obj_t *l = lv_obj_get_child(r, k);
        uint32_t  color = on ? theme_hex(TC_sel_text) : strong ? theme_hex(TC_accent_soft)
                                                     : k == 1 ? COLOR_DIM : theme_hex(TC_text);

        (lv_label_set_text)(l, t[k]);      /* mail and callsigns: past the language hook (i18n_hook.h) */
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    }
}

static lv_obj_t *empty_lbl;

static void build_rows(void) {
    lv_obj_clean(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    rows_n = (APP_FRAME_BODY_H - 8) / ROW_H;
    if (rows_n > (int)(sizeof(rows) / sizeof(rows[0]))) {
        rows_n = sizeof(rows) / sizeof(rows[0]);
    }
    for (int i = 0; i < rows_n; i++) {
        rows[i] = make_row(i);
    }
    empty_lbl = label(body, &sony_22, COLOR_DIM);
    lv_obj_set_width(empty_lbl, APP_FRAME_W - 40);
    lv_label_set_long_mode(empty_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(empty_lbl, true);
    lv_obj_set_pos(empty_lbl, 20, 16);
}

static void keep_sel_shown(void) {
    int n = list_n();

    if (sel >= n) {
        sel = n - 1;
    }
    if (sel < 0) {
        sel = 0;
    }
    if (sel < top) {
        top = sel;
    }
    if (sel >= top + rows_n) {
        top = sel - rows_n + 1;
    }
    if (top > n - rows_n) {
        top = n - rows_n;
    }
    if (top < 0) {
        top = 0;
    }
}

static void show_list(void) {
    int  n = cJSON_GetArraySize(list);
    char who[80], subj[200], date[24];

    keep_sel_shown();
    for (int i = 0; i < rows_n; i++) {
        int          k = top + i;
        const cJSON *m = k < n ? cJSON_GetArrayItem(list, k) : NULL;

        if (!m) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        /* "2026-11-16 21:05" as "11-16 21:05": the year only when it is not this one */
        snprintf(date, sizeof(date), "%s", jstr(m, "date"));
        {
            time_t    now = time(NULL);
            struct tm tm;
            char      year[16];

            gmtime_r(&now, &tm);
            snprintf(year, sizeof(year), "%04d", tm.tm_year + 1900);
            if (!strncmp(date, year, 4) && strlen(date) > 5) {
                memmove(date, date + 5, strlen(date + 5) + 1);
            }
        }
        snprintf(who, sizeof(who), "%s", box_i == 0 ? jstr(m, "from") : jstr(m, "to"));
        for (char *c = who; (c = strstr(c, ", ")); ) {   /* one line, cut with "...": no space to wrap at */
            memmove(c + 1, c + 2, strlen(c + 2) + 1);
        }
        snprintf(subj, sizeof(subj), "%s%s", jstr(m, "subject"), jint(m, "files", 0) ? "  [+]" : "");
        row_set(i, k == sel, jbool(m, "unread"), jbool(m, "unread") ? "*" : "", date, who, subj);
    }
    if (n) {
        lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    if (!status) {
        lv_label_set_text(empty_lbl, "Reading the mailbox...");
    } else if (!winlink_on()) {
        lv_label_set_text_fmt(empty_lbl,
                              "#%06x Winlink is off.#\n\nPat and the modem do not run. Winlink On (page 2) starts "
                              "them, and they start with the radio from then on. Your callsign is the radio's "
                              "(Settings > General) unless the web configurator's Operating > Winlink sets "
                              "another.",
                              theme_hex(TC_text));
    } else if (!pat_up()) {
        char t[120];

        plain(t, sizeof(t), jstr(status, "text"));
        lv_label_set_text_fmt(empty_lbl, "%s", t[0] ? t : "Starting Pat...");
    } else {
        lv_label_set_text(empty_lbl, box_i == 0   ? "No messages. Connect calls a gateway for new mail."
                                     : box_i == 1 ? "Nothing to send. New writes a message."
                                                  : "Nothing sent yet.");
    }
}

static void show_gateways(void) {
    int  n = cJSON_GetArraySize(gateways);
    char freq[24], dist[40], call[24];

    keep_sel_shown();
    for (int i = 0; i < rows_n; i++) {
        int          k = top + i;
        const cJSON *g = k < n ? cJSON_GetArrayItem(gateways, k) : NULL;
        double       khz;

        if (!g) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        khz = jnum(g, "dial");
        snprintf(freq, sizeof(freq), "%.1f kHz", khz);
        if (jint(g, "dist", -1) >= 0) {
            snprintf(dist, sizeof(dist), "%d km  %s", jint(g, "dist", -1), jstr(g, "modes"));
        } else {
            snprintf(dist, sizeof(dist), "%s", jstr(g, "modes"));
        }
        snprintf(call, sizeof(call), "%s", jstr(g, "call"));
        row_set(i, k == sel, jbool(g, "recent"), jbool(g, "recent") ? "+" : "", call, freq, dist);
    }
    if (n) {
        lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(empty_lbl, job_busy || gateways_wanted ? "Reading Pat's gateway list..."
                                                                  : "No gateway on this band.");
    }
}

static void build_read(void) {
    lv_obj_t *head, *text;
    char      buf[1400], line[260];

    lv_obj_clean(body);
    read_box = lv_obj_create(body);
    lv_obj_remove_style_all(read_box);
    lv_obj_set_size(read_box, APP_FRAME_W - 4, APP_FRAME_BODY_H - 4);
    lv_obj_set_pos(read_box, 2, 2);
    lv_obj_set_style_pad_all(read_box, 8, 0);
    lv_obj_set_flex_flow(read_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(read_box, LV_SCROLLBAR_MODE_AUTO);   /* clickable: a finger scrolls it too */

    head = label(read_box, &sony_20, COLOR_DIM);
    lv_label_set_recolor(head, true);
    lv_obj_set_width(head, APP_FRAME_W - 30);
    lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
    {
        char from[80], to[200], cc[200], subj[200];
        int  na = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(message, "attachments"));

        plain(from, sizeof(from), jstr(message, "from"));
        plain(to, sizeof(to), jstr(message, "to"));
        plain(cc, sizeof(cc), jstr(message, "cc"));
        plain(subj, sizeof(subj), jstr(message, "subject"));
        /* the words translated here, the mail's own text past the language hook (i18n_hook.h): a line of it that
         * reads like one of the GUI's would be translated too */
        snprintf(buf, sizeof(buf), "%s #%06x %s#   %s UTC\n", _("From"), theme_hex(TC_text), from,
                 jstr(message, "date"));
        snprintf(line, sizeof(line), "%s #%06x %s#", _("To"), theme_hex(TC_text), to);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        if (cc[0]) {
            snprintf(line, sizeof(line), "\n%s #%06x %s#", _("Cc"), theme_hex(TC_text), cc);
            strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        }
        snprintf(line, sizeof(line), "\n#%06x %s#", COLOR_HEAD, subj);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        if (na) {
            const cJSON *a;

            snprintf(line, sizeof(line), "\n%s:", _("Attachments, in the web mailbox"));
            strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
            cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(message, "attachments")) {
                char name[90];

                plain(name, sizeof(name), jstr(a, "name"));
                snprintf(line, sizeof(line), " #%06x %s# (%d kB)", theme_hex(TC_text), name,
                         (jint(a, "size", 0) + 1023) / 1024);
                strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
            }
        }
        (lv_label_set_text)(head, buf);
    }

    text = label(read_box, &sony_22, theme_hex(TC_text));
    lv_obj_set_width(text, APP_FRAME_W - 30);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(text, 8, 0);
    {
        const char *b    = jstr(message, "body");
        const char *rest = jbool(message, "cut") ? _("\n\n[the rest is in the web mailbox]") : "";
        size_t      n    = strlen(b);
        char       *t    = malloc(n + strlen(rest) + 1);

        if (t) {
            /* tabs as spaces: the fonts have no tab */
            for (size_t i = 0; i <= n; i++) {
                t[i] = b[i] == '\t' ? ' ' : b[i];
            }
            strcat(t, rest);
            (lv_label_set_text)(text, t);     /* the mail itself: past the language hook */
            free(t);
        }
    }
}

static const char *field_names[FIELDS] = {"To", "Cc", "Subject", ""};

static void show_fields(void) {
    for (int i = 0; i < FIELDS; i++) {
        lv_obj_t *box = lv_obj_get_parent(fields[i]);

        lv_obj_set_style_border_color(box, i == field_i ? TC(sel_border) : TC(panel_border), 0);
        if (i == field_i) {
            lv_obj_add_state(fields[i], LV_STATE_FOCUSED);   /* the cursor */
        } else {
            lv_obj_clear_state(fields[i], LV_STATE_FOCUSED);
        }
    }
}

/* A field in a box of x, y, w, h: its name on the left (none for the text), the text after it */
static lv_obj_t *make_field(int i, int x, int y, int w, int h) {
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_t *name, *ta;
    int       tx  = i == 2 ? 86 : 46;     /* where the text starts, after the name */

    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, TC(panel_border), 0);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_bg_color(box, TC(side), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    name = label(box, &sony_20, COLOR_DIM);
    lv_label_set_text(name, field_names[i]);
    lv_obj_set_pos(name, 8, (h - 24) / 2);

    ta = lv_textarea_create(box);
    lv_obj_remove_style(ta, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_style_text_font(ta, &sony_22, 0);
    lv_obj_set_style_text_color(ta, TC(text), 0);
    /* the cursor in the field being typed only */
    lv_obj_set_style_bg_color(ta, TC(text), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_80, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    if (i < FIELDS - 1) {
        lv_textarea_set_one_line(ta, true);
        lv_obj_set_pos(ta, tx, 2);
        lv_obj_set_size(ta, w - tx - 6, h - 4);
        lv_textarea_set_max_length(ta, i == 2 ? SUBJECT_MAX : TO_MAX);
    } else {
        lv_obj_set_pos(ta, 8, 4);
        lv_obj_set_size(ta, w - 16, h - 8);
        lv_textarea_set_max_length(ta, BODY_MAX);
        lv_textarea_set_placeholder_text(ta, "the message: plain text, short is best over HF");
    }
    if (i == 0) {
        lv_textarea_set_placeholder_text(ta, "calls or e-mail addresses, commas between");
    }
    fields[i] = ta;
    return box;
}

/* To and Cc side by side, the subject under them, the text in what is left: all of it on the screen at once */
static void build_compose(const char *to, const char *subject) {
    const int h = 32, w = APP_FRAME_W - 10, w_to = w * 3 / 5;

    lv_obj_clean(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    make_field(0, 4, 4, w_to - 4, h);
    make_field(1, 4 + w_to, 4, w - w_to, h);
    make_field(2, 4, 4 + h + 4, w, h);
    make_field(FIELDS - 1, 4, 4 + 2 * (h + 4), w, APP_FRAME_BODY_H - 2 * (h + 4) - 10);
    lv_textarea_set_text(fields[0], to ? to : "");
    lv_textarea_set_text(fields[2], subject ? subject : "");
    field_i       = to && to[0] ? FIELDS - 1 : 0;
    discard_armed = false;
    show_fields();
}

/* The Connection page: what goes on, what it means, Pat's last log lines. Each wraps: the outcome of a connection
 * with what to do next ("No answer from ...: check the power, the antenna ...") was one line cut with "..." at the
 * right edge, where the Winlink page shows it whole (#321) */
static const int session_y[3] = {8, 40, 70};

static void build_session(void) {
    lv_obj_t *l;

    lv_obj_clean(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; i++) {
        l = label(body, i == 0 ? &sony_24 : &sony_20, i == 0 ? theme_hex(TC_text) : COLOR_DIM);
        lv_label_set_recolor(l, true);
        lv_obj_set_width(l, APP_FRAME_W - 24);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(l, 12, session_y[i]);
    }
}

/* Each line under the one above, where that one takes more than its one line; the log's last lines give way */
static void session_layout(void) {
    int y = 0;

    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = lv_obj_get_child(body, i);

        y = LV_MAX(y, session_y[i]);
        lv_obj_set_y(l, y);
        lv_obj_update_layout(l);
        y += lv_obj_get_height(l) + (i == 0 ? 4 : 6);
    }
}

static void show_session_lines(void);

static void show_session(void) {
    show_session_lines();
    session_layout();
}

static void show_session_lines(void) {
    lv_obj_t *l0 = lv_obj_get_child(body, 0), *l1 = lv_obj_get_child(body, 1), *l2 = lv_obj_get_child(body, 2);
    char      buf[LOG_LINES * 170 + 400];

    if (prompt_on) {
        char m[240];

        plain(m, sizeof(m), prompt_msg);
        lv_label_set_text_fmt(l0, "#%06x Pat asks:# %s", theme_hex(TC_warn), m);
        if (!strcmp(prompt_kind, "busy-channel")) {
            lv_label_set_text(l1, "Someone is on the gateway's frequency. Pat waits for it to clear.");
        } else if (!strcmp(prompt_kind, "password")) {
            lv_label_set_text(l1, "The gateway asks for your Winlink password.");
        } else if (!strcmp(prompt_kind, "pre-account-activation")) {
            lv_label_set_text(l1, "Pat could not confirm that your Winlink account is active.");
        } else if (!strcmp(prompt_kind, "account-activation")) {
            lv_label_set_text(l1, "A new account: its password comes in a message now.");
        } else if (!strcmp(prompt_kind, "multi-select")) {
            lv_label_set_text(l1, "Messages are waiting at the gateway.");
        } else {
            lv_label_set_text(l1, "");
        }
        if (!strcmp(prompt_kind, "pre-account-activation")) {
            snprintf(buf, sizeof(buf), "%s",
                     _("Continue: the account is activated over the air, and a message brings a new password. It is "
                       "the only key to the account: write it down. Better: create the account first (winlink.org), "
                       "or set your password (page 2)."));
        } else if (!strcmp(prompt_kind, "account-activation")) {
            snprintf(buf, sizeof(buf), "%s",
                     _("Download now: the message with the password comes in during this connection. Write the "
                       "password down and set it on the radio (Password, page 2): it cannot be recovered."));
        } else if (!strcmp(prompt_kind, "multi-select")) {
            char p[sizeof(prompt_all)];

            plain(p, sizeof(p), prompt_all);
            snprintf(buf, sizeof(buf),
                     _("Get all: every one now. Get small: those under Pat's size limit. Later: none now. Waiting: %s"), p);
        } else {
            buf[0] = 0;
        }
        lv_label_set_text(l2, buf);
        return;
    }

    if (sess_on) {
        if (sess_connected) {
            lv_label_set_text_fmt(l0, "#%06x Connected# to #%06x %s# on %s kHz", theme_hex(TC_ok), COLOR_HEAD,
                                  sess_call, sess_freq);
        } else {
            lv_label_set_text_fmt(l0, "#%06x Calling# #%06x %s# on %s kHz, %s", theme_hex(TC_warn), COLOR_HEAD,
                                  sess_call, sess_freq, modem_name());
        }
        if (prog_total > 0) {
            char s[90];

            plain(s, sizeof(s), prog_subject);
            lv_label_set_text_fmt(l1, prog_rx ? "Receiving %s: %d of %d bytes" : "Sending %s: %d of %d bytes", s,
                                  prog_done, prog_total);
        } else {
            lv_label_set_text(l1, sess_connected ? "Exchanging mail..." : "The radio transmits when Pat calls.");
        }
    } else if (pat_busy()) {
        lv_label_set_text_fmt(l0, "#%06x A connection is under way#", theme_hex(TC_warn));
        lv_label_set_text(l1, "Started from the web mailbox, or before the app was closed: Abort ends it.");
    } else {
        char r[sizeof(sess_result)];

        plain(r, sizeof(r), sess_result);
        lv_label_set_text_fmt(l0, "#%06x %s#", COLOR_HEAD, sess_call[0] ? sess_call : _("No connection"));
        lv_label_set_text(l1, r);
    }
    buf[0] = 0;
    for (int i = 0; i < sess_log_n; i++) {
        char line[170];

        plain(line, sizeof(line), sess_log[i]);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 2);
        strcat(buf, "\n");
    }
    (lv_label_set_text)(l2, buf);   /* Pat's and the gateway's words: past the language hook */
}


/* ---- Speech: the window draws its lists and fields itself, so it says them itself ----------------------------------
 * A view as it opens: its name and its help line, then what is chosen in it. The lists: the row chosen, "Unread, from
 * K1ABC, Subject, 11-16 21:05, 2 of 9". Compose: the field moved to and what is in it, the characters typed echoed. A
 * message: read a line at a time (the line reader), Up / Down or the MFK. The connection: its state and what goes over
 * it as they change, Pat's question with its answers on the F-keys. The status line's notes as they come */

static char     prompt_labels[3][24];   /* Pat's answers on the F-keys (defined with the buttons) */
static unsigned said_view_seq = ~0u;
static int      said_sel = -1, said_box = -1;
static char     said_empty[300], said_l0[300], said_l1[200], said_note[200], said_prompt[300];

static void clean_of(lv_obj_t *l, char *buf, size_t size) {
    buf[0] = '\0';
    if (l) voice_clean_text(lv_label_get_text(l), buf, size);
}

/* The row chosen, as said */
static void sel_said(char *buf, size_t size) {
    int n = list_n();

    buf[0] = '\0';
    if (sel < 0 || sel >= n) return;
    if (view == V_LIST) {
        const cJSON *m = cJSON_GetArrayItem(list, sel);
        char         who[120], subj[200];

        voice_spell_calls(box_i == 0 ? jstr(m, "from") : jstr(m, "to"), who, sizeof(who));
        voice_clean_text(jstr(m, "subject"), subj, sizeof(subj));
        snprintf(buf, size, "%s%s %s, %s%s, %s, %d of %d", jbool(m, "unread") ? "Unread, " : "",
                 box_i == 0 ? "from" : "to", who, subj[0] ? subj : "no subject",
                 jint(m, "files", 0) ? ", with attachments" : "", jstr(m, "date"), sel + 1, n);
    } else {
        const cJSON *g = cJSON_GetArrayItem(gateways, sel);
        char         call[60];

        voice_spell_calls(jstr(g, "call"), call, sizeof(call));
        if (jint(g, "dist", -1) >= 0)
            snprintf(buf, size, "%s, %.1f kilohertz, %d kilometres, %s%s, %d of %d", call, jnum(g, "dial"),
                     jint(g, "dist", -1), jstr(g, "modes"), jbool(g, "recent") ? ", called lately" : "", sel + 1, n);
        else
            snprintf(buf, size, "%s, %.1f kilohertz, %s%s, %d of %d", call, jnum(g, "dial"), jstr(g, "modes"),
                     jbool(g, "recent") ? ", called lately" : "", sel + 1, n);
    }
}

/* A field of the message being written, as said: "Subject, Weekly net" */
static void field_said(char *buf, size_t size) {
    static const char *names[FIELDS] = {"To", "Cc", "Subject", "Message"};
    const char        *t             = fields[field_i] ? lv_textarea_get_text(fields[field_i]) : "";
    char               v[300];

    voice_clean_text(t, v, sizeof(v));
    snprintf(buf, size, "%s, %s", names[field_i], v[0] ? v : "empty");
}

/* The message's lines for the line reader: the head's, then the text's, blank lines left out */
static void read_lines(void) {
    voice_lines_begin();
    if (read_box) {
        for (uint32_t c = 0; c < lv_obj_get_child_cnt(read_box); c++) {
            char *t = strdup(lv_label_get_text(lv_obj_get_child(read_box, c)));

            if (!t) continue;
            for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
                char clean[400];

                voice_clean_text(line, clean, sizeof(clean));
                if (clean[0]) voice_lines_add(clean);
            }
            free(t);
        }
    }
    voice_lines_done("Message. Up, Down or M F K: line by line. R: reply. Escape: back");
}

static void mail_speech(void) {
    char head[400] = "", item[1200] = "";

    if (!body || !voice_auto_active()) return;

    if (view_seq != said_view_seq) {        /* a view opened: its name and help */
        char name[160], help[200];

        said_view_seq = view_seq;
        said_sel = said_box = -1;
        said_empty[0] = said_l0[0] = said_l1[0] = '\0';
        voice_lines_clear();
        clean_of(frame.head_left, name, sizeof(name));
        clean_of(frame.status, help, sizeof(help));
        snprintf(said_note, sizeof(said_note), "%s", help);     /* said here, not again as a note */
        snprintf(head, sizeof(head), "%s%s%s", name, help[0] ? ". " : "", help);
        if (view == V_READ) {
            read_lines();
            return;
        }
    } else {
        char now[200];

        clean_of(frame.status, now, sizeof(now));
        if (strcmp(now, said_note)) {       /* a note: what was done, what is wrong, a second press asked */
            snprintf(said_note, sizeof(said_note), "%s", now);
            if (note[0] && time(NULL) < note_until && now[0]) snprintf(head, sizeof(head), "%s", now);
        }
    }

    switch (view) {
        case V_LIST:
        case V_GATEWAYS:
            if (list_n() > 0) {
                if (sel != said_sel || box_i != said_box) {
                    said_sel = sel;
                    said_box = box_i;
                    sel_said(item, sizeof(item));
                }
            } else {
                char e[300];

                clean_of(empty_lbl, e, sizeof(e));
                if (strcmp(e, said_empty)) {
                    snprintf(said_empty, sizeof(said_empty), "%s", e);
                    snprintf(item, sizeof(item), "%s", e);
                }
                said_sel = -1;
            }
            break;

        case V_COMPOSE:
            if (head[0] && fields[field_i]) field_said(item, sizeof(item));
            break;

        case V_SESSION: {
            lv_obj_t *l0 = lv_obj_get_child(body, 0), *l1 = lv_obj_get_child(body, 1), *l2 = lv_obj_get_child(body, 2);
            char      a[300], b[300];

            if (prompt_on) {
                char q[300], answers[160] = "";

                snprintf(q, sizeof(q), "%s %s", prompt_id, prompt_msg);
                if (strcmp(q, said_prompt)) {
                    char t0[300], t1[200], t2[700];

                    snprintf(said_prompt, sizeof(said_prompt), "%s", q);
                    clean_of(l0, t0, sizeof(t0));
                    clean_of(l1, t1, sizeof(t1));
                    clean_of(l2, t2, sizeof(t2));
                    for (int i = 0; i < 3; i++) {
                        size_t n = strlen(answers);

                        if (prompt_labels[i][0])
                            snprintf(answers + n, sizeof(answers) - n, "%sF%d %s", n ? ", " : "", i + 2,
                                     prompt_labels[i]);
                    }
                    snprintf(item, sizeof(item), "%s. %s%s%s%s%s", t0, t1, t1[0] ? ". " : "", t2, t2[0] ? ". " : "",
                             answers);
                }
                break;
            }
            said_prompt[0] = '\0';
            clean_of(l0, a, sizeof(a));
            if (prog_total > 0) {           /* the bytes go on: said once per message, not each count */
                snprintf(b, sizeof(b), "%s %s", prog_rx ? "Receiving" : "Sending", prog_subject);
            } else {
                clean_of(l1, b, sizeof(b));
            }
            if (strcmp(a, said_l0)) {
                snprintf(said_l0, sizeof(said_l0), "%s", a);
                snprintf(item, sizeof(item), "%s", a);
            }
            if (strcmp(b, said_l1)) {
                size_t n = strlen(item);

                snprintf(said_l1, sizeof(said_l1), "%s", b);
                if (b[0]) snprintf(item + n, sizeof(item) - n, "%s%s", n ? ". " : "", b);
            }
            break;
        }

        default:
            break;
    }

    if (head[0] && item[0])
        voice_say_text_fmt("%s. %s", head, item);
    else if (head[0])
        voice_say_text_fmt("%s", head);
    else if (item[0] && (view == V_LIST || view == V_GATEWAYS))
        voice_delay_say_text_fmt("%s", item);   /* the knob turned on: the row it stops at */
    else if (item[0])
        voice_say_text_fmt("%s", item);
}

/* The header, the status line, and the view's body */
static void show(void) {
    char right[120] = "", left[120];

    if (!body) {
        return;
    }
    switch (view) {
        case V_LIST:
        case V_READ:
            snprintf(left, sizeof(left), "%s - #%06x %s#", _("WINLINK MAIL"), theme_hex(TC_text),
                     _(box_names[box_i]));
            break;
        case V_COMPOSE:
            snprintf(left, sizeof(left), "%s - #%06x %s#", _("WINLINK MAIL"), theme_hex(TC_text), _("New message"));
            break;
        case V_GATEWAYS:
            snprintf(left, sizeof(left), "%s - #%06x %s, %s#", _("CONNECT"), theme_hex(TC_text), modem_name(),
                     bands[band_i][0] ? bands[band_i] : _("all bands"));
            break;
        case V_SESSION:
            snprintf(left, sizeof(left), "%s", _("CONNECTION"));
            break;
    }
    lv_label_set_text(frame.head_left, left);

    if (sess_on) {
        snprintf(right, sizeof(right), "#%06x %s %s#", theme_hex(sess_connected ? TC_ok : TC_warn),
                 sess_connected ? _("Connected") : _("Calling"), sess_call);
    } else if (status && pat_up()) {
        int unread = jint(status, "unread", 0), out = jint(status, "out", 0);

        if (unread) {
            snprintf(right, sizeof(right), _("#%06x %d unread#"), theme_hex(TC_accent_soft), unread);
        }
        if (out) {
            size_t n = strlen(right);

            snprintf(right + n, sizeof(right) - n, "%s", n ? "  " : "");
            n = strlen(right);
            snprintf(right + n, sizeof(right) - n, _("%d to send"), out);
        }
        if (!right[0]) {
            snprintf(right, sizeof(right), "%s", jstr(status, "mycall")[0] ? jstr(status, "mycall") : _("Ready"));
        }
    } else if (status) {
        snprintf(right, sizeof(right), "#%06x %s#", COLOR_DIM, winlink_on() ? _("Starting") : _("Winlink off"));
    }
    lv_label_set_text(frame.head_right, right);

    if (note[0] && time(NULL) < note_until) {
        lv_label_set_text(frame.status, note);
    } else if (confirm_until && time(NULL) < confirm_until) {
        /* the note says it */
    } else if (modem_moving && view == V_LIST) {
        char t[160];

        plain(t, sizeof(t), status ? jstr(status, "text") : "");
        lv_label_set_text_fmt(frame.status, "#%06x Modem: switching to %s...# %s", theme_hex(TC_warn), modem_to_name,
                              t);
    } else if (view == V_LIST && status && winlink_on() && pat_up() && !strcmp(jstr(status, "password"), "none")) {
        lv_label_set_text_fmt(frame.status, "#%06x No Winlink password set:# Password (page 2) sets it",
                              theme_hex(TC_warn));
    } else if (view == V_LIST && status && pat_up() && jstr(status, "last_text")[0]) {
        char t[240];     /* whole: the line scrolls */

        plain(t, sizeof(t), jstr(status, "last_text"));
        lv_label_set_text_fmt(frame.status, "Last connection: %s", t);
    } else if (view == V_COMPOSE) {
        lv_label_set_text(frame.status, keyboard_ready() ? "Enter or Down: next field - in the text Enter is a new line - Esc: back"
                                                         : "MFK: next field - MFK press: type - Send when done");
    } else if (view == V_READ) {
        lv_label_set_text(frame.status, "Up / Down or MFK: scroll - R: reply - Esc: back");
    } else if (view == V_GATEWAYS && gw_tuned(cJSON_GetArrayItem(gateways, sel))) {
        const cJSON *g = cJSON_GetArrayItem(gateways, sel);
        char         call[24];

        plain(call, sizeof(call), jstr(g, "call"));
        lv_label_set_text_fmt(frame.status,
                              "#%06x On %.1f kHz for %s, nothing sent: tune the antenna now if needed - Connect calls, "
                              "the radio transmits#",
                              theme_hex(TC_warn), jnum(g, "dial"), call);
    } else if (view == V_GATEWAYS) {
        lv_label_set_text(frame.status,
                          "+ called lately - Set freq: the radio goes there, nothing sent - then Connect calls");
    } else if (view == V_SESSION && prompt_on) {
        /* the question's page has no Back, and its Abort is an answer to Pat */
        lv_label_set_text(frame.status, "Pat waits for the answer: a button here, or the web mailbox");
    } else if (view == V_SESSION && sess_on) {
        lv_label_set_text(frame.status, "Abort ends the connection; Back leaves it running");
    } else if (view == V_LIST) {
        lv_label_set_text(frame.status, keyboard_ready() ? "Enter: read - N: new - C: connect - B: next box"
                                                         : "MFK: choose - MFK press: read");
    } else {
        lv_label_set_text(frame.status, "");
    }

    if (sess_on && prog_total > 0) {
        app_frame_bar(&frame, (int)(1000LL * prog_done / prog_total), theme_hex(TC_ok_bar));
    } else {
        app_frame_bar(&frame, -1, 0);
    }

    switch (view) {
        case V_LIST:     show_list(); break;
        case V_GATEWAYS: show_gateways(); break;
        case V_SESSION:  show_session(); break;
        default:         break;
    }
    mail_speech();
}

/* ---- Buttons ----------------------------------------------------------------------------------------------------- */

static void box_cb(button_data_t *b);
static void new_cb(button_data_t *b);
static void connect_view_cb(button_data_t *b);
static void close_cb(button_data_t *b);
static void onoff_cb(button_data_t *b);
static void password_cb(button_data_t *b);
static void modem_cb(button_data_t *b);
static void refresh_cb(button_data_t *b);
static void session_view_cb(button_data_t *b);
static void reply_cb(button_data_t *b);
static void delete_cb(button_data_t *b);
static void back_cb(button_data_t *b);
static void next_field_cb(button_data_t *b);
static void type_cb(button_data_t *b);
static void send_cb(button_data_t *b);
static void band_cb(button_data_t *b);
static void connect_cb(button_data_t *b);
static void abort_cb(button_data_t *b);
static void prompt_cb(button_data_t *b);

static const char *box_label(void) {
    static char s[64];

    snprintf(s, sizeof(s), "%s:\n%s", _("Box"), _(box_names[box_i]));
    return s;
}

static const char *onoff_label(void) {
    return winlink_on() ? "Winlink:\nOn" : "Winlink:\nOff";
}

static const char *modem_label(void) {
    static char s[64];

    snprintf(s, sizeof(s), "%s:\n%s", _("Modem"), modem_moving ? modem_to_name : modem_name());
    return s;
}

static const char *band_label(void) {
    static char s[64];

    snprintf(s, sizeof(s), "%s:\n%s", _("Band"), bands[band_i][0] ? bands[band_i] : _("All"));
    return s;
}

/* The gateway list's Connect: what the next press does for the gateway chosen */
static const char *go_label(void) {
    const cJSON *g = view == V_GATEWAYS ? cJSON_GetArrayItem(gateways, sel) : NULL;

    tuned_shown = gw_tuned(g);
    return tuned_shown || (g && !gw_hz(g)) ? "Connect" : "Set\nfreq";
}

static const char *conn_label(void) {
    return sess_on || pat_busy() ? "Connection" : "Connect";
}

static button_data_t btn_l1      = {.type = BTN_TEXT, .label = "(MAIL 1:2)", .press = button_next_page_cb,
                                    .hold = button_prev_page_cb, .next = &page_list_2, .prev = &page_list_2};
static button_data_t btn_box     = {.type = BTN_TEXT_FN, .label_fn = box_label, .press = box_cb};
static button_data_t btn_new     = {.type = BTN_TEXT, .label = "New", .press = new_cb};
static button_data_t btn_conn    = {.type = BTN_TEXT_FN, .label_fn = conn_label, .press = connect_view_cb};
static button_data_t btn_close   = {.type = BTN_TEXT, .label = "Close", .press = close_cb};
static button_data_t btn_l2      = {.type = BTN_TEXT, .label = "(MAIL 2:2)", .press = button_next_page_cb,
                                    .hold = button_prev_page_cb, .next = &page_list_1, .prev = &page_list_1};
static button_data_t btn_onoff   = {.type = BTN_TEXT_FN, .label_fn = onoff_label, .press = onoff_cb};
static button_data_t btn_modem   = {.type = BTN_TEXT_FN, .label_fn = modem_label, .press = modem_cb};
static button_data_t btn_pass    = {.type = BTN_TEXT, .label = "Password", .press = password_cb};
static button_data_t btn_refresh = {.type = BTN_TEXT, .label = "Refresh", .press = refresh_cb};

static buttons_page_t page_list_1 = {{&btn_l1, &btn_box, &btn_new, &btn_conn, &btn_close}};
/* Close is on page 1 (and Esc): the five buttons of page 2 are taken */
static buttons_page_t page_list_2 = {{&btn_l2, &btn_onoff, &btn_modem, &btn_pass, &btn_refresh}};

static button_data_t  btn_rd      = {.type = BTN_TEXT, .label = "(READ 1:1)"};
static button_data_t  btn_reply   = {.type = BTN_TEXT, .label = "Reply", .press = reply_cb};
static button_data_t  btn_delete  = {.type = BTN_TEXT, .label = "Delete", .press = delete_cb};
static button_data_t  btn_back    = {.type = BTN_TEXT, .label = "Back", .press = back_cb};
static buttons_page_t page_read   = {{&btn_rd, &btn_reply, &btn_delete, &btn_back}};

static button_data_t  btn_cp      = {.type = BTN_TEXT, .label = "(NEW 1:1)"};
static button_data_t  btn_next    = {.type = BTN_TEXT, .label = "Next\nfield", .press = next_field_cb};
static button_data_t  btn_type    = {.type = BTN_TEXT, .label = "Edit", .press = type_cb};
static button_data_t  btn_send    = {.type = BTN_TEXT, .label = "Send", .press = send_cb};
static button_data_t  btn_cancel  = {.type = BTN_TEXT, .label = "Cancel", .press = back_cb};
static buttons_page_t page_compose = {{&btn_cp, &btn_next, &btn_type, &btn_send, &btn_cancel}};

static button_data_t  btn_gp      = {.type = BTN_TEXT, .label = "(CONNECT 1:1)"};
static button_data_t  btn_band    = {.type = BTN_TEXT_FN, .label_fn = band_label, .press = band_cb};
static button_data_t  btn_go      = {.type = BTN_TEXT_FN, .label_fn = go_label, .press = connect_cb};
static button_data_t  btn_sess    = {.type = BTN_TEXT, .label = "Connection", .press = session_view_cb};
static button_data_t  btn_back2   = {.type = BTN_TEXT, .label = "Back", .press = back_cb};
static buttons_page_t page_gateways = {{&btn_gp, &btn_band, &btn_go, &btn_sess, &btn_back2}};

static button_data_t  btn_sp      = {.type = BTN_TEXT, .label = "(LINK 1:1)"};
static button_data_t  btn_abort   = {.type = BTN_TEXT, .label = "Abort", .press = abort_cb};
static button_data_t  btn_back3   = {.type = BTN_TEXT, .label = "Back", .press = back_cb};
static buttons_page_t page_session = {{&btn_sp, NULL, NULL, &btn_abort, &btn_back3}};

/* Pat's question: up to three answers; .ctrl is the answer's number */
static char           prompt_labels[3][24];
static button_data_t  btn_pq      = {.type = BTN_TEXT, .label = "(PAT ASKS)"};
static button_data_t  btn_pa[3]   = {
    {.type = BTN_TEXT, .label = prompt_labels[0], .press = prompt_cb, .ctrl = 0},
    {.type = BTN_TEXT, .label = prompt_labels[1], .press = prompt_cb, .ctrl = 1},
    {.type = BTN_TEXT, .label = prompt_labels[2], .press = prompt_cb, .ctrl = 2},
};
static buttons_page_t page_prompt = {{&btn_pq, &btn_pa[0], &btn_pa[1], &btn_pa[2]}};
static const char    *prompt_values[3];

static void load_page(void) {
    buttons_page_t *cur = buttons_get_cur_page();

    if (!body || osk_open_now) {
        return;
    }
    if (prompt_on) {
        if (cur != &page_prompt) {
            buttons_load_page(&page_prompt);
        }
        return;
    }
    switch (view) {
        case V_LIST:
            if (cur != &page_list_1 && cur != &page_list_2) {
                buttons_load_page(&page_list_1);
            } else {
                buttons_refresh(&btn_box);
                buttons_refresh(&btn_conn);
                buttons_refresh(&btn_onoff);
                buttons_refresh(&btn_modem);
            }
            break;
        case V_READ:
            if (cur != &page_read) {
                buttons_load_page(&page_read);
            }
            buttons_disabled(&btn_reply, box_i != 0);
            break;
        case V_COMPOSE:
            if (cur != &page_compose) {
                buttons_load_page(&page_compose);
            }
            break;
        case V_GATEWAYS:
            if (cur != &page_gateways) {
                buttons_load_page(&page_gateways);
            }
            buttons_refresh(&btn_band);
            buttons_refresh(&btn_go);
            buttons_disabled(&btn_sess, !sess_on && !pat_busy());
            break;
        case V_SESSION:
            if (cur != &page_session) {
                buttons_load_page(&page_session);
            }
            buttons_disabled(&btn_abort, !sess_on && !pat_busy());
            break;
    }
}

static void set_view(view_t v) {
    if (v == V_LIST && sel_is_gw) {     /* back from the gateways: the message chosen before */
        sel       = saved_sel;
        top       = saved_top;
        sel_is_gw = false;
    }
    if (view == V_GATEWAYS && v != V_GATEWAYS) {
        tuned_drop();      /* the list left with no call (a call begun counts in sess_running) */
    }
    view          = v;
    view_seq++;
    confirm_until = 0;
    note[0]       = 0;     /* what was said is about the view left */
    if (v != V_COMPOSE) {
        memset(fields, 0, sizeof(fields));    /* deleted with the body's children below */
    }
    switch (v) {
        case V_LIST:
        case V_GATEWAYS:
            build_rows();
            break;
        case V_READ:
            build_read();
            break;
        case V_SESSION:
            build_session();
            break;
        case V_COMPOSE:
            break;    /* build_compose() with what it starts with */
    }
    load_page();
    show();
}

/* ---- The list and reading ---------------------------------------------------------------------------------------- */

static void osk_end(void);

/* A button pressed while the text popup is open: the popup closes (as its Cancel), nothing else happens. Pat's
 * password question stays open: it is answered in it */
static bool popup_first(void) {
    if (!osk_open_now) {
        return false;
    }
    if (!prompt_password_open) {
        textarea_window_close();
        osk_end();
    }
    return true;
}

static void box_cb(button_data_t *b) {
    if (popup_first()) {
        return;
    }
    box_i = (box_i + 1) % BOXES;
    sel = top = 0;
    cJSON_Delete(list);
    list = NULL;
    buttons_refresh(b);
    show();
    refresh();
}

static void read_done(int rc, cJSON *out) {
    if (res_seq != view_seq) {
        return;     /* asked from a view that is gone: a message being written is not thrown away for it */
    }
    if (failed(rc, out, "Read")) {
        return;
    }
    cJSON_Delete(message);
    message = cJSON_Duplicate(out, true);
    set_view(V_READ);
    refresh();      /* the unread count */
}

static void open_selected(void) {
    const cJSON *m = cJSON_GetArrayItem(list, sel);

    if (!m) {
        return;
    }
    if (!job_start(read_done, NULL, HELPER, "read", boxes[box_i], jstr(m, "mid"))) {
        say("Wait, still busy");
    }
}

static void delete_done(int rc, cJSON *out) {
    if (res_seq != view_seq) {
        refresh();
        return;
    }
    if (failed(rc, out, "Delete")) {
        return;
    }
    set_view(V_LIST);
    say("Deleted");
    refresh();
}

static void delete_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    if (confirm_what != 2 || time(NULL) >= confirm_until) {
        confirm_what  = 2;
        confirm_until = time(NULL) + CONFIRM_S;
        say(box_i == 1 ? "Delete: press again, the message is not sent" : "Delete: press again");
        return;
    }
    confirm_until = 0;
    if (!job_start(delete_done, NULL, HELPER, "delete", boxes[box_i], jstr(message, "mid"))) {
        say("Wait, still busy");
    }
}

static void back_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    if (view == V_COMPOSE) {
        bool typed = false;

        for (int i = 0; i < FIELDS; i++) {
            typed |= *lv_textarea_get_text(fields[i]) != 0;
        }
        if (typed && !discard_armed) {
            discard_armed = true;
            say("The message is not sent: Cancel or Esc again leaves it");
            return;
        }
    }
    set_view(V_LIST);
    refresh();
}

static void close_cb(button_data_t *b) {
    (void)b;
    dialog_destruct();
}

static void refresh_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    say("Reading the mailbox...");
    refresh();
}

/* ---- Writing ----------------------------------------------------------------------------------------------------- */

static void draft_free(void) {
    for (int i = 0; i < FIELDS; i++) {
        free(draft[i]);
        draft[i] = NULL;
    }
}

/* The message being written, kept */
static void draft_keep(void) {
    draft_free();
    for (int i = 0; i < FIELDS; i++) {
        draft[i] = strdup(fields[i] ? lv_textarea_get_text(fields[i]) : "");
    }
    draft_field = field_i;
}

/* ... and back on the screen: false when none is kept */
static bool draft_restore(void) {
    if (!draft[0]) {
        return false;
    }
    view = V_COMPOSE;
    build_compose(NULL, NULL);
    for (int i = 0; i < FIELDS; i++) {
        lv_textarea_set_text(fields[i], draft[i] ? draft[i] : "");
    }
    field_i = draft_field;
    draft_free();
    set_view(V_COMPOSE);
    show_fields();
    say("The message you were writing");
    return true;
}

static void new_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    if (!pat_up()) {
        say("Winlink is off: Winlink On (page 2) first");
        return;
    }
    if (draft_restore()) {
        return;
    }
    view = V_COMPOSE;
    build_compose(NULL, NULL);
    set_view(V_COMPOSE);
}

static void reply_cb(button_data_t *b) {
    char subj[SUBJECT_MAX + 8];

    (void)b;
    if (popup_first()) {
        return;
    }
    if (!message || box_i != 0) {
        return;
    }
    if (strncasecmp(jstr(message, "subject"), "re:", 3)) {
        snprintf(subj, sizeof(subj), "Re: %s", jstr(message, "subject"));
    } else {
        snprintf(subj, sizeof(subj), "%s", jstr(message, "subject"));
    }
    /* at most SUBJECT_MAX bytes, cut where a letter starts: an accented one is two bytes, and half of it would
     * make the text that Send hands the helper no UTF-8 */
    if (strlen(subj) > SUBJECT_MAX) {
        size_t n = SUBJECT_MAX;

        while (n && ((unsigned char)subj[n] & 0xC0) == 0x80) {
            n--;
        }
        subj[n] = 0;
    }
    view = V_COMPOSE;
    build_compose(jstr(message, "from"), subj);
    set_view(V_COMPOSE);
}

static void field_move(int d) {
    field_i = (field_i + d + FIELDS) % FIELDS;
    show_fields();
    if (voice_auto_active() && fields[field_i]) {
        char t[400];

        field_said(t, sizeof(t));
        voice_say_text_fmt("%s", t);
    }
}

static void next_field_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    field_move(1);
}

static void send_done(int rc, cJSON *out) {
    if (res_seq != view_seq) {       /* the app went on meanwhile: said, and the view left as it is */
        msg_schedule_text_fmt(rc == 0 ? "Winlink: the message is in the outbox" : "Winlink: the message was not sent");
        refresh();
        return;
    }
    if (failed(rc, out, "Not sent")) {
        return;
    }
    box_i = 1;
    sel = top = 0;
    set_view(V_LIST);
    say("In the outbox: Connect sends it");
    refresh();
}

static void send_cb(button_data_t *b) {
    cJSON *m = cJSON_CreateObject();
    char  *s;

    (void)b;
    if (popup_first()) {
        return;
    }
    cJSON_AddStringToObject(m, "to", lv_textarea_get_text(fields[0]));
    cJSON_AddStringToObject(m, "cc", lv_textarea_get_text(fields[1]));
    cJSON_AddStringToObject(m, "subject", lv_textarea_get_text(fields[2]));
    cJSON_AddStringToObject(m, "body", lv_textarea_get_text(fields[3]));
    s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!s) {
        return;
    }
    if (!job_start(send_done, s, HELPER, "send", NULL, NULL)) {
        say("Wait, still busy");
    }
    free(s);
}

/* The on-screen keys, or a USB keyboard's line: a field's text replaced, a line added to the message */

static void osk_end(void) {
    textarea_window_close();
    osk_open_now = false;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
    load_page();
}

static bool osk_ok(void) {
    const char *val = textarea_window_get();

    if (view != V_COMPOSE || !fields[field_i]) {
        osk_end();
        return true;
    }
    if (field_i == FIELDS - 1) {
        if (val && *val) {
            lv_textarea_set_cursor_pos(fields[field_i], LV_TEXTAREA_CURSOR_LAST);
            lv_textarea_add_text(fields[field_i], val);
            lv_textarea_add_char(fields[field_i], '\n');
        }
    } else {
        lv_textarea_set_text(fields[field_i], val ? val : "");
        field_move(1);
    }
    osk_end();
    return true;
}

static bool osk_cancel(void) {
    osk_end();
    return true;
}

static void type_cb(button_data_t *b) {
    lv_obj_t *ta;

    (void)b;
    if (osk_open_now || view != V_COMPOSE) {
        return;
    }
    osk_open_now = true;
    lv_group_remove_obj(dialog.obj);
    if (field_i == FIELDS - 1) {
        textarea_window_open_w_label(osk_ok, osk_cancel, "Line: ");
    } else {
        textarea_window_open_w_label(osk_ok, osk_cancel, field_i == 0 ? "To: " : field_i == 1 ? "Cc: " : "Subj: ");
    }
    ta = textarea_window_text();
    lv_textarea_set_max_length(ta, field_i == FIELDS - 1 ? LINE_MAX : field_i == 2 ? SUBJECT_MAX : TO_MAX);
    if (field_i < FIELDS - 1) {
        textarea_window_set(lv_textarea_get_text(fields[field_i]));
    }
}

/* ---- Winlink on and off, the password ---------------------------------------------------------------------------- */

static void onoff_done(int rc, cJSON *out) {
    (void)out;
    if (rc != 0) {
        say("#%06x Winlink:# %s failed", theme_hex(TC_err), SUPERVISOR);
    }
    next_poll = 0;
    refresh();
}

static void onoff_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    if (!status) {
        say("Wait: reading Winlink's state");    /* On or Off is not known yet */
        return;
    }
    if (pat_busy()) {
        say("A connection is under way: Abort it first");
        return;
    }
    if (!job_start(onoff_done, NULL, SUPERVISOR, winlink_on() ? "off" : "on", NULL, NULL)) {
        say("Wait, still busy");
        return;
    }
    say(winlink_on() ? "Stopping Pat and the modem..." : "Starting Pat and the modem...");
}

/* Modem (#322, #323): the next of the modems on this radio, as the Winlink page's Modem and Save. x6100-winlink
 * writes the choice and has the supervisor restart Pat, stop the modem that runs and start that one; the status
 * read every 2 s from then on says when it is ready (modem_followed) */
static void modem_done(int rc, cJSON *out) {
    if (rc != 0 || !out) {
        char e[160];

        plain(e, sizeof(e), out ? jstr(out, "error") : "");
        modem_moving = false;
        say("#%06x Modem:# %s", theme_hex(TC_err), e[0] ? e : _("not changed"));
        load_page();
        return;
    }
    next_poll = 0;
    refresh();
}

static void modem_cb(button_data_t *b) {
    const cJSON *all, *m, *next = NULL;
    bool         after = false;

    (void)b;
    if (popup_first()) {
        return;
    }
    if (!status) {
        say("Wait: reading Winlink's state");
        return;
    }
    if (pat_busy()) {
        say("A connection is under way: the modem after it");     /* Pat restarts for another modem */
        return;
    }
    if (modem_moving) {
        say("Wait: switching to %s", modem_to_name);
        return;
    }
    all = cJSON_GetObjectItemCaseSensitive(status, "modems");
    cJSON_ArrayForEach(m, all) {     /* the one after the choice, the first after the last */
        if (after && !next) {
            next = m;
        }
        after |= !strcmp(jstr(m, "id"), jstr(status, "modem_choice"));
    }
    if (!next) {
        next = cJSON_GetArrayItem(all, 0);
    }
    if (!next || !strcmp(jstr(next, "id"), jstr(status, "modem_choice"))) {
        say("No other modem on this radio");
        return;
    }
    snprintf(modem_to, sizeof(modem_to), "%s", jstr(next, "id"));
    snprintf(modem_to_name, sizeof(modem_to_name), "%s", jstr(next, "name"));
    if (!job_start(modem_done, NULL, SUPERVISOR, "modem", modem_to, NULL)) {
        say("Wait, still busy");
        return;
    }
    modem_moving = true;
    modem_until  = time(NULL) + MODEM_WAIT_S;
    load_page();
    show();
}

static void apply_done(int rc, cJSON *out) {
    (void)out;
    say(rc == 0 ? "Password set: Pat starts again with it" : "#%06x Password set; Pat did not restart#",
        theme_hex(TC_warn));
    next_poll = 0;
    refresh();
}

static void password_done(int rc, cJSON *out) {
    (void)out;
    if (rc != 0) {
        say("#%06x Password not set:# 1-64 characters", theme_hex(TC_err));
        return;
    }
    if (winlink_on()) {
        job_start(apply_done, NULL, SUPERVISOR, "apply", NULL, NULL);
    } else {
        say("Password set: kept on the radio, never shown");
    }
}

static bool pw_ok(void) {
    const char *val = textarea_window_get();
    size_t      n;
    char       *in;

    /* spaces around it dropped, as the web page drops them */
    while (val && *val == ' ') {
        val++;
    }
    n = val ? strlen(val) : 0;
    while (n && val[n - 1] == ' ') {
        n--;
    }
    if (!n) {
        return false;
    }
    in = malloc(n + 2);
    if (in) {
        memcpy(in, val, n);
        in[n]     = '\n';
        in[n + 1] = 0;
        if (!job_start(password_done, in, SUPERVISOR, "set-password", NULL, NULL)) {
            say("Wait, still busy");
        }
        memset(in, 0, strlen(in));
        free(in);
    }
    textarea_window_set("");
    osk_end();
    return true;
}

static void password_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    if (pat_busy()) {
        say("A connection is under way: the password after it");
        return;
    }
    osk_open_now = true;
    lv_group_remove_obj(dialog.obj);
    textarea_window_open_w_label(pw_ok, osk_cancel, "Password: ");
    lv_textarea_set_password_mode(textarea_window_text(), true);
}

/* ---- Gateways and the connection --------------------------------------------------------------------------------- */

static void gateways_done(int rc, cJSON *out) {
    if (view != V_GATEWAYS || res_tag != band_i) {
        return;     /* left, or another band chosen meanwhile (its list is asked for already) */
    }
    if (failed(rc, out, "Gateways")) {
        show();
        return;
    }
    cJSON_Delete(gateways);
    gateways = cJSON_IsArray(out) ? cJSON_Duplicate(out, true) : cJSON_CreateArray();
    sel = top = 0;
    load_page();            /* Connect's label is for the gateway chosen */
    show();
}

static void load_gateways(void) {
    cJSON_Delete(gateways);
    gateways = cJSON_CreateArray();
    sel = top = 0;
    confirm_until = 0;
    tuned_drop();           /* another band's list: the gateway the radio was put on is not in it */
    if (!job_start_tag(gateways_done, band_i, NULL, HELPER, "gateways", bands[band_i][0] ? bands[band_i] : NULL,
                       NULL)) {
        gateways_wanted = true;     /* when the job running now is done */
    }
    show();
}

static void connect_view_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    if (sess_on || pat_busy()) {
        set_view(V_SESSION);
        return;
    }
    if (!pat_up()) {
        say("Winlink is off: Winlink On (page 2) first");
        return;
    }
    if (!sel_is_gw) {
        saved_sel = sel;
        saved_top = top;
        sel_is_gw = true;
    }
    set_view(V_GATEWAYS);
    load_gateways();
}

static void session_view_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    set_view(V_SESSION);
}

static void band_cb(button_data_t *b) {
    if (popup_first()) {
        return;
    }
    band_i = (band_i + 1) % BANDS;
    buttons_refresh(b);
    load_gateways();
}

static void session_line_cb(void *arg);
static void session_end_cb(void *arg);

typedef struct {
    unsigned gen;
    char    *line;
} sess_msg_t;

typedef struct {
    unsigned gen;
    int      fd;
    pid_t    pid;
} sess_thread_t;

static void *session_thread(void *arg) {
    sess_thread_t s = *(sess_thread_t *)arg;
    FILE         *f = fdopen(s.fd, "r");
    char         *line = NULL;     /* a whole line, however long: half of one is no JSON, and Pat's question in it */
    size_t        cap  = 0;        /* would be lost (a multi-select of many messages runs past 4 KiB) */
    int           st;

    free(arg);
    while (f && getline(&line, &cap, f) > 0) {
        sess_msg_t m = {.gen = s.gen, .line = strdup(line)};

        if (!m.line) {
            continue;
        }
        for (int tries = 0; !scheduler_put(session_line_cb, &m, sizeof(m)); tries++) {
            if (tries > 50) {
                free(m.line);
                break;
            }
            usleep(100000);
        }
    }
    free(line);
    if (f) {
        fclose(f);
    } else {
        close(s.fd);
    }
    while (waitpid(s.pid, &st, 0) < 0 && errno == EINTR) {
    }
    {
        sess_msg_t m = {.gen = s.gen, .line = NULL};

        while (!scheduler_put(session_end_cb, &m, sizeof(m))) {
            usleep(100000);
        }
    }
    return NULL;
}

static void prompt_clear(void) {
    if (prompt_password_open) {
        textarea_window_close();
        prompt_password_open = false;
        osk_open_now         = false;
        lv_group_add_obj(keyboard_group, dialog.obj);
        lv_group_focus_obj(dialog.obj);
        lv_group_set_editing(keyboard_group, true);
    }
    prompt_on   = false;
    prompt_id[0] = 0;
}

static void prompt_answer(const char *value) {
    cJSON *o = cJSON_CreateObject(), *p = cJSON_AddObjectToObject(o, "prompt");
    char  *s;

    cJSON_AddStringToObject(p, "id", prompt_id);
    cJSON_AddStringToObject(p, "value", value);
    s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s && sess_in >= 0) {
        send_all(sess_in, s);
        send_all(sess_in, "\n");
    }
    if (s) {
        memset(s, 0, strlen(s));    /* a password in it */
        free(s);
    }
    prompt_clear();
    if (view == V_SESSION && draft_restore()) {    /* back to the message Pat's question interrupted */
        return;
    }
    load_page();
    show();
}

static bool prompt_pw_ok(void) {
    const char *val = textarea_window_get();
    char        pw[80];

    snprintf(pw, sizeof(pw), "%s", val ? val : "");
    textarea_window_set("");
    prompt_password_open = false;
    osk_end();
    prompt_answer(pw);
    memset(pw, 0, sizeof(pw));
    return true;
}

static bool prompt_pw_cancel(void) {
    prompt_password_open = false;
    osk_end();
    prompt_answer("");
    return true;
}

static void prompt_cb(button_data_t *b) {
    int i = (int)b->ctrl;

    if (osk_open_now) {
        return;     /* the password's field is open already */
    }
    if (!prompt_on || i < 0 || i > 2 || !prompt_values[i]) {
        return;
    }
    if (!strcmp(prompt_kind, "password") && i == 0) {
        osk_open_now         = true;
        prompt_password_open = true;
        lv_group_remove_obj(dialog.obj);
        textarea_window_open_w_label(prompt_pw_ok, prompt_pw_cancel, "Password: ");
        lv_textarea_set_password_mode(textarea_window_text(), true);
        return;
    }
    prompt_answer(prompt_values[i]);
}

/* Pat asks: the answers on the buttons, as its web page offers them */
static void prompt_show(const cJSON *p) {
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(p, "options");
    const cJSON *o;

    snprintf(prompt_id, sizeof(prompt_id), "%s", jstr(p, "id"));
    snprintf(prompt_kind, sizeof(prompt_kind), "%s", jstr(p, "kind"));
    snprintf(prompt_msg, sizeof(prompt_msg), "%s", jstr(p, "message"));
    prompt_all[0] = prompt_checked[0] = 0;
    cJSON_ArrayForEach(o, opts) {
        const char *v = jstr(o, "value");

        if (strlen(prompt_all) + strlen(v) + 2 < sizeof(prompt_all)) {
            if (prompt_all[0]) {
                strcat(prompt_all, ",");
            }
            strcat(prompt_all, v);
        }
        if (jbool(o, "checked") && strlen(prompt_checked) + strlen(v) + 2 < sizeof(prompt_checked)) {
            if (prompt_checked[0]) {
                strcat(prompt_checked, ",");
            }
            strcat(prompt_checked, v);
        }
    }
    memset(prompt_values, 0, sizeof(prompt_values));
    for (int i = 0; i < 3; i++) {
        prompt_labels[i][0] = 0;
    }
    if (!strcmp(prompt_kind, "busy-channel")) {
        snprintf(prompt_labels[1], sizeof(prompt_labels[1]), "Continue\nanyway");
        snprintf(prompt_labels[2], sizeof(prompt_labels[2]), "Abort");
        prompt_values[1] = "continue";
        prompt_values[2] = "abort";
    } else if (!strcmp(prompt_kind, "password")) {
        snprintf(prompt_labels[0], sizeof(prompt_labels[0]), "Type it");
        snprintf(prompt_labels[2], sizeof(prompt_labels[2]), "Abort");
        prompt_values[0] = "";
        prompt_values[2] = "";
    } else if (!strcmp(prompt_kind, "pre-account-activation")) {
        snprintf(prompt_labels[0], sizeof(prompt_labels[0]), "Continue\nanyway");
        snprintf(prompt_labels[2], sizeof(prompt_labels[2]), "Abort");
        prompt_values[0] = "confirmed";
        prompt_values[2] = "abort";
    } else if (!strcmp(prompt_kind, "account-activation")) {
        snprintf(prompt_labels[0], sizeof(prompt_labels[0]), "Download\nnow");
        snprintf(prompt_labels[2], sizeof(prompt_labels[2]), "Later");
        prompt_values[0] = "accept";
        prompt_values[2] = "defer";
    } else if (!strcmp(prompt_kind, "multi-select")) {
        snprintf(prompt_labels[0], sizeof(prompt_labels[0]), "Get\nall");
        snprintf(prompt_labels[1], sizeof(prompt_labels[1]), "Get\nsmall");
        snprintf(prompt_labels[2], sizeof(prompt_labels[2]), "Later");
        prompt_values[0] = prompt_all;
        prompt_values[1] = prompt_checked;
        prompt_values[2] = "";
    } else {
        snprintf(prompt_labels[0], sizeof(prompt_labels[0]), "OK");
        prompt_values[0] = "";
    }
    for (int i = 0; i < 3; i++) {
        page_prompt.items[1 + i] = prompt_labels[i][0] ? &btn_pa[i] : NULL;
    }
    prompt_on = true;
    if (osk_open_now) {      /* a field being typed: Pat's question goes first */
        textarea_window_close();
        osk_open_now = false;
        lv_group_add_obj(keyboard_group, dialog.obj);
        lv_group_focus_obj(dialog.obj);
        lv_group_set_editing(keyboard_group, true);
    }
    msg_schedule_text_fmt("Winlink: Pat asks, see APP > Mail");
    if (view == V_COMPOSE) {
        draft_keep();       /* back after the answer, or with New */
    }
    if (view != V_SESSION) {
        set_view(V_SESSION);
    } else {
        buttons_load_page(&page_prompt);
        show();
    }
}

static void log_add(const char *line) {
    if (!line[0]) {
        return;
    }
    if (sess_log_n == LOG_LINES) {
        memmove(sess_log[0], sess_log[1], sizeof(sess_log[0]) * (LOG_LINES - 1));
        sess_log_n--;
    }
    snprintf(sess_log[sess_log_n++], sizeof(sess_log[0]), "%s", line);
}

static void session_line_cb(void *arg) {
    sess_msg_t  *m  = arg;
    cJSON       *ev = m->gen == gen && body ? cJSON_Parse(m->line) : NULL;
    const cJSON *o;

    free(m->line);
    if (!ev) {
        return;
    }
    if ((o = cJSON_GetObjectItemCaseSensitive(ev, "log"))) {
        log_add(cJSON_IsString(o) ? o->valuestring : "");
    } else if ((o = cJSON_GetObjectItemCaseSensitive(ev, "status"))) {
        sess_connected = jbool(o, "connected");
        sess_dialing   = jbool(o, "dialing");
    } else if ((o = cJSON_GetObjectItemCaseSensitive(ev, "progress"))) {
        prog_done  = jint(o, "done", 0);
        prog_total = jint(o, "total", 0);
        prog_rx    = jbool(o, "receiving");
        snprintf(prog_subject, sizeof(prog_subject), "%s", jstr(o, "subject"));
    } else if ((o = cJSON_GetObjectItemCaseSensitive(ev, "prompt"))) {
        prompt_show(o);
    } else if ((o = cJSON_GetObjectItemCaseSensitive(ev, "prompt_abort"))) {
        if (cJSON_IsString(o) && !strcmp(o->valuestring, prompt_id)) {
            prompt_clear();   /* answered in the web mailbox, or Pat gave up asking */
            if (!(view == V_SESSION && draft_restore())) {
                load_page();
            }
        }
    } else if ((o = cJSON_GetObjectItemCaseSensitive(ev, "done"))) {
        int got = jint(o, "received", 0);

        if (jbool(o, "ok") && got == 1) {
            snprintf(sess_result, sizeof(sess_result), "%s", _("Done: 1 message received"));
        } else if (jbool(o, "ok")) {
            snprintf(sess_result, sizeof(sess_result), _("Done: %d messages received"), got);
        } else {
            snprintf(sess_result, sizeof(sess_result), _("Failed: %s"), jstr(o, "error"));
        }
        sess_result_status_at = time(NULL) + 3;   /* then the supervisor's reading of the logs */
    } else if ((o = cJSON_GetObjectItemCaseSensitive(ev, "error"))) {
        snprintf(sess_result, sizeof(sess_result), _("Failed: %s"), cJSON_IsString(o) ? o->valuestring : "");
    }
    cJSON_Delete(ev);
    show();
}

static void session_end_cb(void *arg) {
    sess_msg_t *m = arg;

    /* Also for a window closed meanwhile: Pat found the radio on the gateway's frequency (Connect's first press) and
     * tuned "back" to it a second after the session. That is over by now: Pat v1.0.1 defers it in Connect
     * (app/connect.go:154, the QSX at :243-246), /api/connect answers only once Connect has returned
     * (api/api.go:381), and the helper ends a second after that answer. Now to where it was before that press */
    if (sess_running > 0) {
        sess_running--;
    }
    if (!sess_running) {
        winlink_preset_back(true);
    }
    if (m->gen != gen) {
        return;
    }
    sess_on        = false;
    sess_connected = sess_dialing = false;
    prog_total     = 0;
    if (sess_in >= 0) {
        close(sess_in);
        sess_in = -1;
    }
    sess_pid = 0;
    prompt_clear();
    if (!sess_result[0]) {
        snprintf(sess_result, sizeof(sess_result), "%s", _("Ended"));
    }
    {
        char r[sizeof(sess_result)];

        plain(r, sizeof(r), sess_result);
        msg_schedule_text_fmt("Winlink: %s", r);
    }
    load_page();
    show();
    refresh();
}

/* Connect's first press, once x6100-winlink-mail busy has answered: the radio to the gateway's frequency when no
 * connection is under way, from here or from the web mailbox; else why not. Nothing for a gateway no longer the one
 * chosen, or a list that was left */
static void preset_asked(int rc, cJSON *out) {
    const cJSON *g = view == V_GATEWAYS ? cJSON_GetArrayItem(gateways, sel) : NULL;
    char         why[160];

    if (res_seq != view_seq || !g || !ask_url[0] || strcmp(ask_url, jstr(g, "url"))) {
        ask_url[0] = 0;
        return;
    }
    ask_url[0] = 0;
    if (failed(rc, out, "Connect")) {
        return;
    }
    /* only Pat's plain "no" moves the radio: an answer without it is taken as a connection */
    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "busy")) || sess_running) {
        plain(why, sizeof(why), jstr(out, "why"));
        say("#%06x Not on %.1f kHz:# %s", theme_hex(TC_err), jnum(g, "dial"),
            why[0] ? why : "a connection is under way");
        return;
    }
    if (!winlink_preset(gw_hz(g), why, sizeof(why))) {
        tuned_url[0] = 0;
        say("#%06x Not on %.1f kHz:# %s", theme_hex(TC_err), jnum(g, "dial"), why);
    } else {
        snprintf(tuned_url, sizeof(tuned_url), "%s", jstr(g, "url"));
        note[0] = 0;        /* the status line says what the next press does */
    }
    load_page();
    show();
}

static void connect_cb(button_data_t *b) {
    const cJSON   *g = cJSON_GetArrayItem(gateways, sel);
    char          *argv[] = {HELPER, "session", NULL, NULL};
    char           call[24];
    int            out_fd, in_fd;
    pid_t          pid;
    pthread_t      t;
    pthread_attr_t attr;
    sess_thread_t *st;

    (void)b;
    if (popup_first()) {
        return;
    }
    if (!g) {
        return;
    }
    if (sess_on || sess_running || pat_busy()) {
        say("A connection is under way");
        return;
    }
    plain(call, sizeof(call), jstr(g, "call"));
    if (gw_hz(g)) {
        /* The first press (#322): the radio on the gateway's frequency, nothing sent, for as long as the antenna
         * tuner takes. The second press, for the same gateway with the radio still there, calls; another gateway
         * chosen or the dial turned meanwhile, and the press is a first one again */
        if (!gw_tuned(g)) {
            /* Pat is asked first: the radio is never moved under a connection, also one begun from the web mailbox
             * while this list is open, which the status read in the other views does not show here */
            snprintf(ask_url, sizeof(ask_url), "%s", jstr(g, "url"));
            if (!job_start(preset_asked, NULL, HELPER, "busy", NULL, NULL)) {
                ask_url[0] = 0;
                say("Wait, still busy");
                return;
            }
            say("Asking Pat that no connection is under way...");
            return;
        }
    } else if (confirm_what != 1 || time(NULL) >= confirm_until || strcmp(confirm_url, jstr(g, "url"))) {
        /* a gateway with no frequency in Pat's list: nothing to tune to, the second press within 5 s calls where the
         * radio is */
        confirm_what  = 1;
        confirm_until = time(NULL) + CONFIRM_S;
        snprintf(confirm_url, sizeof(confirm_url), "%s", jstr(g, "url"));
        say("#%06x Connect to %s: press again, the radio transmits where it is#", theme_hex(TC_warn), call);
        return;
    }
    confirm_until = 0;
    argv[2]       = (char *)jstr(g, "url");
    pid           = spawn_helper(argv, &out_fd, &in_fd);
    if (pid < 0) {
        say("#%06x Connect:# %s did not run", theme_hex(TC_err), HELPER);
        return;
    }
    st = malloc(sizeof(*st));
    if (!st) {
        close(out_fd);
        close(in_fd);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return;
    }
    *st = (sess_thread_t){.gen = gen, .fd = out_fd, .pid = pid};
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, session_thread, st) != 0) {
        pthread_attr_destroy(&attr);
        free(st);
        close(out_fd);
        close(in_fd);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return;
    }
    pthread_attr_destroy(&attr);
    sess_pid = pid;
    sess_in  = in_fd;
    sess_on  = true;
    sess_running++;
    tuned_url[0] = 0;       /* the call is made: the radio goes back when it ends (session_end_cb) */
    sess_connected = sess_dialing = false;
    sess_log_n     = 0;
    prog_total     = 0;
    sess_result[0] = 0;
    sess_result_status_at = 0;
    sess_started          = time(NULL);
    snprintf(sess_call, sizeof(sess_call), "%s", jstr(g, "call"));
    snprintf(sess_freq, sizeof(sess_freq), "%.1f", jnum(g, "dial"));
    set_view(V_SESSION);
}

static void abort_done(int rc, cJSON *out) {
    if (!failed(rc, out, "Abort")) {
        say("Ending the connection...");
    }
    refresh();
}

static void abort_cb(button_data_t *b) {
    (void)b;
    if (popup_first()) {
        return;
    }
    if (sess_on && sess_in >= 0) {
        /* a second press: at once, as Pat's own second Abort */
        static time_t first;
        bool          again = time(NULL) - first < 10;

        send_all(sess_in, again ? "abort!\n" : "abort\n");
        first = time(NULL);
        say(again ? "Ending the connection now" : "Ending the connection (Abort again: at once)");
    } else if (pat_busy() && !job_start(abort_done, NULL, HELPER, "abort", NULL, NULL)) {
        say("Wait, still busy: Abort again");
    }
}

/* ---- Keys -------------------------------------------------------------------------------------------------------- */

static bool from_encoder(void) {
    lv_indev_t *indev = lv_indev_get_act();

    return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;
}

static void move_sel(int d) {
    sel += d;
    if (confirm_until && confirm_what == 1) {   /* Connect's first press was for the gateway left */
        confirm_until = 0;
        note[0]       = 0;
    }
    if (view == V_GATEWAYS) {
        keep_sel_shown();
        buttons_refresh(&btn_go);               /* Set freq or Connect, for the gateway chosen now */
    }
    show();
}

/* A row touched: a message is read at once, as an MFK press reads the one chosen; a gateway is only chosen, and
 * Connect, pressed twice, calls it */
static void row_click_cb(lv_event_t *e) {
    int k = top + (int)(intptr_t)lv_event_get_user_data(e);

    if (popup_first() || (view != V_LIST && view != V_GATEWAYS) || k >= list_n()) {
        return;
    }
    if (k != sel) {
        move_sel(k - sel);
    }
    if (view == V_LIST) {
        open_selected();
    }
}

static void compose_key(uint32_t key) {
    lv_obj_t *ta = fields[field_i];

    switch (key) {
        case LV_KEY_ESC:
            back_cb(NULL);
            return;
        case LV_KEY_NEXT:
            field_move(1);
            return;
        case LV_KEY_PREV:
            field_move(-1);
            return;
        case LV_KEY_ENTER:      /* '\n' */
        case '\r':
            if (field_i == FIELDS - 1) {
                lv_textarea_add_char(ta, '\n');
            } else {
                field_move(1);
            }
            break;
        case LV_KEY_BACKSPACE:
            lv_textarea_del_char(ta);
            if (voice_auto_active()) voice_say_text_fmt("deleted");
            break;
        case LV_KEY_DEL:
            lv_textarea_del_char_forward(ta);
            if (voice_auto_active()) voice_say_text_fmt("deleted");
            break;
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
            if (from_encoder()) {
                field_move(key == LV_KEY_RIGHT ? 1 : -1);
            } else if (key == LV_KEY_LEFT) {
                lv_textarea_cursor_left(ta);
            } else {
                lv_textarea_cursor_right(ta);
            }
            break;
        case LV_KEY_UP:
            if (field_i == FIELDS - 1 && lv_textarea_get_cursor_pos(ta) > 0) {
                lv_textarea_cursor_up(ta);
            } else {
                field_move(-1);
            }
            break;
        case LV_KEY_DOWN:
            if (field_i == FIELDS - 1) {
                lv_textarea_cursor_down(ta);
            } else {
                field_move(1);
            }
            break;
        case LV_KEY_HOME:
            lv_textarea_set_cursor_pos(ta, 0);
            break;
        case LV_KEY_END:
            lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
            break;
        default:
            /* a character: ASCII, or a letter beyond it in UTF-8 as the keyboard's layout gives it (kbd_layout.c:
             * ä, é, ß), which lv_textarea_add_char takes as it is. The radio's own keys are not here (key_cb) */
            if (key >= ' ') {
                lv_textarea_add_char(ta, key);
                discard_armed = false;
                if (voice_auto_active() && key < 0x7f) {      /* the character echoed, as CW Keyboard does */
                    char c[2] = {(char)key, 0};

                    voice_say_text_fmt("%s", key == ' ' ? "space" : c);
                }
            }
            break;
    }
}

/* Tab on a keyboard is LVGL's own: it leaves the group's edit mode, where the MFK turns into key events for this
 * window. Back in it */
static void keep_editing(void) {
    if (!osk_open_now && dialog.obj && lv_group_get_focused(keyboard_group) == dialog.obj &&
        !lv_group_get_editing(keyboard_group)) {
        lv_group_set_editing(keyboard_group, true);
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    keep_editing();
    if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) {
        radio_change_vol(-1);
        return;
    }
    if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) {
        radio_change_vol(1);
        return;
    }
    if (view == V_COMPOSE) {
        if (!textarea_key_writes_nothing(key) || key == LV_KEY_ESC) {
            compose_key(key);
        }
        return;
    }
    if (prompt_on && key == LV_KEY_ESC) {
        return;     /* answered on the buttons only */
    }
    switch (view) {
        case V_LIST:
        case V_GATEWAYS:
            switch (key) {
                case LV_KEY_UP:
                case LV_KEY_LEFT:
                    move_sel(-1);
                    break;
                case LV_KEY_DOWN:
                case LV_KEY_RIGHT:
                    move_sel(1);
                    break;
                case LV_KEY_HOME:
                    move_sel(-sel);
                    break;
                case LV_KEY_END:
                    move_sel(list_n() - 1 - sel);
                    break;
                case LV_KEY_ENTER:
                case '\r':
                    if (view == V_LIST) {
                        open_selected();
                    } else {
                        connect_cb(NULL);
                    }
                    break;
                case 'b':
                case 'B':
                    if (view == V_LIST) {
                        box_cb(&btn_box);
                    } else {
                        band_cb(&btn_band);
                    }
                    break;
                case LV_KEY_ESC:
                    if (view == V_LIST) {
                        dialog_destruct();
                    } else {
                        back_cb(NULL);
                    }
                    break;
                case 'n':
                case 'N':
                    if (view == V_LIST) {
                        new_cb(NULL);
                    }
                    break;
                case 'c':
                case 'C':
                    if (view == V_LIST) {
                        connect_view_cb(NULL);
                    }
                    break;
                default:
                    break;
            }
            break;
        case V_READ:
            switch (key) {
                case LV_KEY_UP:
                case LV_KEY_LEFT:
                    lv_obj_scroll_by_bounded(read_box, 0, 60, LV_ANIM_OFF);
                    if (voice_auto_active()) voice_lines_step(-1);
                    break;
                case LV_KEY_DOWN:
                case LV_KEY_RIGHT:
                    lv_obj_scroll_by_bounded(read_box, 0, -60, LV_ANIM_OFF);
                    if (voice_auto_active()) voice_lines_step(1);
                    break;
                case ' ':           /* a page on */
                    lv_obj_scroll_by_bounded(read_box, 0, -(APP_FRAME_BODY_H - 40), LV_ANIM_OFF);
                    break;
                case 'r':
                case 'R':
                    reply_cb(NULL);
                    break;
                case LV_KEY_ESC:
                    back_cb(NULL);
                    break;
                default:
                    break;
            }
            break;
        case V_SESSION:
            if (key == LV_KEY_ESC) {
                back_cb(NULL);
            }
            break;
        default:
            break;
    }
}

/* MFK press: read the message chosen, call the gateway chosen, type into the field */
static void click_cb(lv_event_t *e) {
    (void)e;
    if (!from_encoder()) {
        return;
    }
    switch (view) {
        case V_LIST:     open_selected(); break;
        case V_GATEWAYS: connect_cb(NULL); break;
        case V_COMPOSE:  type_cb(NULL); break;
        default:         break;
    }
}

/* ---- Open, close, the timer -------------------------------------------------------------------------------------- */

static void timer_cb(lv_timer_t *t) {
    time_t now = time(NULL);

    (void)t;
    keep_editing();
    if (sess_result_status_at > 0 && now >= sess_result_status_at) {
        sess_result_status_at = -1;      /* view_done takes the supervisor's last_text */
        refresh();
    } else if (!job_busy && !osk_open_now && now >= next_poll && (view == V_LIST || view == V_SESSION)) {
        next_poll = now + POLL_S;
        refresh();
    }
    if (note[0] && now >= note_until) {
        note[0] = 0;
        show();
    }
    if (confirm_until && now >= confirm_until) {
        confirm_until = 0;
        show();
    }
    if (view == V_GATEWAYS && !osk_open_now && tuned_shown != gw_tuned(cJSON_GetArrayItem(gateways, sel))) {
        buttons_refresh(&btn_go);    /* the dial turned after the first press: the next one is a first one again */
        show();
    }
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    gen++;
    job_busy       = false;
    refresh_wanted = false;
    osk_open_now   = false;
    note[0]        = 0;
    confirm_until  = 0;
    next_poll      = 0;
    modem_moving   = false;
    if (!sess_on) {
        sess_call[0] = sess_result[0] = 0;
        sess_log_n = 0;
    }

    app_frame_create(&frame, dialog.obj);
    body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    set_view(V_LIST);
    timer = lv_timer_create(timer_cb, 1000, NULL);
    refresh();
}

/* Closed: a connection under way goes on (Pat's, as one from the web mailbox), followed no more; its answers and
 * the jobs' come back to a newer gen and are dropped */
static void destruct_cb(void) {
    voice_lines_clear();
    said_view_seq = ~0u;
    said_prompt[0] = '\0';
    if (osk_open_now || textarea_window_is_open()) {
        textarea_window_close();
        osk_open_now = false;
    }
    if (prompt_on) {
        prompt_on            = false;
        prompt_password_open = false;
    }
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    lv_group_remove_obj(dialog.obj);
    tuned_drop();           /* closed after Connect's first press, with no call: the radio back where it was */
    if (sess_in >= 0) {
        close(sess_in);     /* the helper sees its stdin end and goes on to the end of the connection */
        sess_in = -1;
    }
    sess_on = false;
    gen++;
    cJSON_Delete(list);
    cJSON_Delete(message);
    cJSON_Delete(gateways);
    cJSON_Delete(status);
    list = message = gateways = status = NULL;
    rows_n = 0;
    body   = NULL;
}
