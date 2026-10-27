/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of who owns a TCI transmission (audit C01). src/tci_server.c is included whole and
 *  its socket thread's steps are called one at a time, the LVGL thread's work held back in the
 *  stub scheduler and run when the case says: a second start, a cancel, a refusal, a takeover
 *  at the radio, the operator keying or a client leaving comes between a key and the LVGL thread
 *  applying it. Only one start may key the radio, and whatever is on the air for a client ends
 *  when that client says so or leaves; a start ended before its key ran keys nothing, an unkey
 *  never drops a PTT the operator keyed, a refused start takes no control. Each case runs in a
 *  child process: a server as it starts. A client is one end of a socket pair, what the server
 *  tells it is read from the other. No listener.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "tci_server.c"

#include <sys/wait.h>

enum { A = 0, B = 1 };

extern bool stub_defer, stub_tx_refused;
int         stub_run(int n);

static int      peer[MAX_CLIENTS];
static rstate_t last;
static int      failed;

/* The server as tci_server_init leaves it, without its threads: the case calls their steps */
static void server_start(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        pthread_mutex_init(&clients[i].lock, NULL);
    }
    txs.owner  = -1;
    stub_defer = true;
    ctl_init();
    state_follow(&last, true);
}

/* A client through its handshake, as accept_client and client_read leave it */
static void client_open(int ci) {
    client_t *c = &clients[ci];
    int       sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) < 0) {
        perror("socketpair");
        exit(2);
    }
    c->fd       = sv[0];
    c->up       = true;
    c->fmt_rate = 48000;
    c->fmt_type = TCI_FLOAT32;
    c->last_ms = c->ping_ms = now_ms();
    atomic_fetch_add(&nclients, 1);
    peer[ci] = sv[1];
}

/* The connection lost, as client_read handles it */
static void client_gone(int ci) {
    tx_session_end(ci, false);
    client_close(&clients[ci]);
}

static void say(int ci, const char *cmd) {
    char line[256];

    snprintf(line, sizeof(line), "%s", cmd);
    handle_text(ci, line);
}

/* The socket thread's round after a poll (server_thread): the session tick, the state followed */
static void follow(void) {
    tx_session_tick(now_ms());
    state_follow(&last, false);
}

/* The LVGL thread runs n jobs (all: n < 0), then the socket thread has its round */
static void gui(int n) {
    stub_run(n);
    follow();
}

/* What the server told client ci since the last look: its text frames, one after the other */
static const char *heard(int ci) {
    static char text[8192];
    uint8_t     buf[65536];
    ssize_t     n   = recv(peer[ci], buf, sizeof(buf), 0);
    size_t      at  = 0, len = 0;

    text[0] = 0;
    while (n > 0 && at + 2 <= (size_t)n) {
        size_t pl = buf[at + 1] & 0x7f, h = 2;
        if (pl == 126) {
            pl = (size_t)buf[at + 2] << 8 | buf[at + 3];
            h  = 4;
        }
        if (pl == 127 || at + h + pl > (size_t)n)
            break;
        if ((buf[at] & 0x0f) == WS_OP_TEXT && len + pl < sizeof(text)) {
            memcpy(text + len, buf + at + h, pl);
            len += pl;
            text[len] = 0;
        }
        at += h + pl;
    }
    return text;
}

static bool on_air(void) {
    return radio_get_state() != RADIO_RX || radio_get_modem();
}

static void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failed++;
}

/* ---- The cases: A and B are two clients, both trusted to operate --------------------- */

static void two_starts_a_ends(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    say(B, "trx:0,true;");
    check(strstr(heard(B), "trx:0,false;"), "B's start, with A's on its way, is refused at once");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

static void two_starts_b_unkeys(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    say(B, "trx:0,true;");
    gui(-1);
    say(B, "trx:0,false;");
    gui(-1);
    check(on_air(), "B's unkey leaves A's transmission on the air");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

static void two_starts_a_leaves(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    say(B, "trx:0,true;");
    gui(-1);
    client_gone(A);
    gui(-1);
    check(!on_air(), "A leaving ends A's transmission");
}

static void ptt_then_tci_start(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    say(B, "trx:0,true,tci;");
    check(strstr(heard(B), "trx:0,false;"), "B's TCI audio start, with A's on its way, is refused at once");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    check(!strstr(heard(A), "trx:0,false;"), "A is not told trx false while it transmits");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

/* B's start, refused while A's key is on its way, takes nothing: control stays with A, which
 * tunes during its own transmission (a refused start changes no other session, audit C01) */
static void refused_takes_no_control(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    say(B, "trx:0,true;");
    check(ctl_holds(CTL_TCI, A) && !ctl_holds(CTL_TCI, B), "control stays with A");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    say(A, "vfo:0,0,7010000;");
    gui(-1);
    check(cparam_i_get(cfg_fg_freq) == 7010000, "A tunes during its transmission");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

/* Another client's key during A's transmission is answered trx:0,false (the state, trx:0,true,
 * is what a TCI audio client could take for its own key) and leaves control with A; a client
 * trusted to view only is answered with the state, as for its tuning */
static void second_key_during_over(void) {
    const char *h;

    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    heard(B);
    say(B, "trx:0,true;");
    h = heard(B);
    check(strstr(h, "trx:0,false;") && !strstr(h, "trx:0,true;"), "B's PTT key is answered trx:0,false");
    say(B, "trx:0,true,tci;");
    h = heard(B);
    check(strstr(h, "trx:0,false;") && !strstr(h, "trx:0,true;"), "B's TCI audio key is answered trx:0,false");
    check(ctl_holds(CTL_TCI, A), "control stays with A");
    atomic_store(&trust_operate, false);    /* Listen only */
    say(B, "trx:0,true,tci;");
    check(!strcmp(heard(B), "trx:0,true;"), "B's key under Listen only is answered with the state");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

static void local_takeover(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    ctl_local_input();                  /* a knob at the radio, before the LVGL thread keyed for A */
    say(B, "trx:0,true;");              /* B takes control from it: nothing is on the air yet */
    check(strstr(heard(B), "trx:0,false;"), "B's start, with A's on its way, is refused at once");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    ctl_local_input();                  /* the knob again, during A's transmission */
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it, whoever has control");
}

/* A's start cancelled before its key ran, B's start while A's key and unkey are on their way */
static void cancelled_then_other(const char *a_start) {
    client_open(A);
    client_open(B);
    say(A, a_start);
    say(A, "trx:0,false;");
    say(B, "trx:0,true;");
    gui(1);                             /* A's key */
    gui(1);                             /* A's unkey */
    gui(-1);                            /* B's key, if there is one */
    check(!on_air() || ptt_owner == B, "on the air only for B");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air(), "B's unkey ends it");
    say(B, "trx:0,true;");              /* and B's start once A's is undone */
    gui(-1);
    check(on_air() && ptt_owner == B, "B's start keys for B");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air(), "B's unkey ends it");
}

static void cancel_tci_then_ptt(void) {
    cancelled_then_other("trx:0,true,tci;");
}

static void cancel_ptt_then_ptt(void) {
    cancelled_then_other("trx:0,true;");
}

static void refused_operator_first(void) {
    client_open(A);
    say(A, "trx:0,true;");
    radio_set_ptt(true);                /* the operator keys at the radio before the LVGL thread keyed for A */
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && ptt_owner < 0, "A's start is refused, A holds nothing");
    client_gone(A);
    gui(-1);
    check(on_air(), "A leaving leaves the operator's transmission on the air");
}

static void refused_cancelled(void) {
    client_open(A);
    client_open(B);
    cparam_i_set(cfg_cur_mode, radio_mode_cw);  /* CW: TCI audio has no way in, its key is refused */
    say(A, "trx:0,true,tci;");
    say(A, "trx:0,false;");             /* cancelled before the LVGL thread refused it */
    stub_run(1);                        /* A's key, refused; the socket thread has not looked since */
    say(B, "trx:0,true;");              /* the radio's PTT, which CW takes */
    gui(1);                             /* A's unkey, and now it looks */
    check(!strstr(heard(B), "trx:0,false;"), "B is not told of A's refusal");
    gui(-1);
    check(on_air() && ptt_owner == B, "on the air for B");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air(), "B's unkey ends it");
}

/* The operator keys at the radio (the PTT key; the browser's PTT sets the same bit) before A's key
 * ran, and A leaves before the LVGL thread refused it: A's unkey is not the operator's */
static void refused_then_gone(void) {
    client_open(A);
    say(A, "trx:0,true;");
    radio_set_ptt(true);
    client_gone(A);
    gui(-1);
    check(on_air(), "A leaving leaves the operator's transmission on the air");
}

/* The same, A's key refused and A unkeying before the socket thread saw the refusal */
static void refused_then_unkeyed(void) {
    client_open(A);
    say(A, "trx:0,true;");
    radio_set_ptt(true);
    stub_run(1);                        /* A's key, refused; the socket thread has not looked since */
    say(A, "trx:0,false;");
    gui(-1);
    check(on_air(), "A's unkey leaves the operator's transmission on the air");
}

/* A's unkey waits behind a busy LVGL thread and goes again; the operator keys between the two:
 * the second one is A's unkey, done already, and not the operator's */
static void unkey_resent(void) {
    client_open(A);
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    say(A, "trx:0,false;");
    tx_session_tick(now_ms() + 400);    /* not run 300 ms on: the unkey goes again */
    stub_run(1);                        /* A's unkey */
    check(!on_air(), "A's unkey ends it");
    radio_set_ptt(true);                /* the operator keys */
    gui(-1);                            /* the unkey's second copy */
    check(on_air(), "the unkey sent again leaves the operator's transmission on the air");
}

/* The same, B's start admitted once A's unkey ran and ended before its key ran, both behind A's
 * unkey's second copy: that copy is A's still, not B's unkey, and not the operator's */
static void unkey_resent_then_start(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    gui(-1);
    say(A, "trx:0,false;");
    tx_session_tick(now_ms() + 400);    /* the unkey goes again */
    stub_run(1);                        /* A's unkey */
    follow();
    say(B, "trx:0,true;");
    say(B, "trx:0,false;");
    radio_set_ptt(true);                /* the operator keys */
    gui(-1);                            /* A's unkey's second copy, B's key, B's unkey */
    check(on_air(), "the operator's transmission stays on the air");
    say(B, "trx:0,true;");
    check(strstr(heard(B), "trx:0,false;") && ptt_owner < 0, "B's start during it is refused");
}

static void key_lost(void) {
    char vfo[32];

    client_open(A);
    for (int i = 0; i < 256; i++) {     /* the LVGL thread's queue full of tuning */
        snprintf(vfo, sizeof(vfo), "vfo:0,0,%d;", 7000000 + i);
        say(A, vfo);
    }
    say(A, "trx:0,true;");              /* its key has no room */
    follow();
    check(strstr(heard(A), "trx:0,false;") && ptt_owner < 0, "A's start is refused, A holds nothing");
    gui(-1);
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A, "A's next start keys for A");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

static void end_of_another_seen_late(void) {
    client_open(A);
    radio_set_ptt(true);                /* the operator on the air, and the server has seen it */
    follow();
    radio_set_ptt(false);               /* off the air again, the server not yet */
    say(A, "trx:0,true;");
    follow();                           /* now it sees that transmission end */
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

static void other_leaves(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    gui(-1);
    client_gone(B);
    gui(-1);
    check(on_air(), "B leaving leaves A on the air");
    client_gone(A);
    gui(-1);
    check(!on_air(), "A leaving ends A's transmission");
}

static void back_on_receive(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A");
    radio_set_ptt(false);               /* the SWR guard unkeys */
    gui(-1);
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && ptt_owner < 0, "A's transmission is over, A hears trx false");
    say(B, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == B, "B's start keys for B");
    say(A, "trx:0,false;");
    gui(-1);
    check(on_air(), "A's unkey leaves B's transmission on the air");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air(), "B's unkey ends it");
}

static void never_takes(void) {
    client_open(A);
    client_open(B);
    stub_tx_refused = true;             /* a frequency the radio does not transmit on */
    say(A, "trx:0,true;");
    gui(-1);
    check(!on_air(), "nothing on the air");
    tx_session_tick(now_ms() + 1500);   /* more than a second on */
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && ptt_owner < 0, "A's start is over, A hears trx false");
    stub_tx_refused = false;
    say(B, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == B, "B's start keys for B");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air(), "B's unkey ends it");
}

/* A's key reaches the LVGL thread 1.5 s after the start (a busy LVGL thread): it is on its way,
 * not refused. The second for the radio to go on the air counts from when it ran */
static void late_key(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    tx_session_tick(now_ms() + 1500);
    check(!strstr(heard(A), "trx:0,false;") && ptt_owner == A, "A is not told trx false while its key waits");
    say(B, "trx:0,true;");
    check(strstr(heard(B), "trx:0,false;"), "B's start meanwhile is refused");
    gui(-1);
    check(on_air() && ptt_owner == A, "on the air for A once its key ran");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

/* A start that ended before its key ran (its client's unkey, its client leaving, a TCI audio
 * start's second without its key) keys nothing: no one would hold that transmission */
static void cancelled_keys_nothing(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true;");
    say(A, "trx:0,false;");
    stub_run(1);                        /* A's key */
    check(!on_air(), "A's PTT start, unkeyed before its key ran, keys nothing");
    gui(-1);
    say(A, "trx:0,true,tci;");
    client_gone(A);
    stub_run(1);
    check(!on_air(), "A's TCI audio start, A gone before its key ran, keys nothing");
    gui(-1);
    say(B, "trx:0,true,tci;");
    tx_session_tick(now_ms() + 1500);   /* its key more than a second late: the session is over */
    stub_run(1);
    check(!on_air(), "B's TCI audio start, its key more than a second late, keys nothing");
    gui(-1);
    say(B, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == B, "B's next start keys for B");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air(), "B's unkey ends it");
}

static void two_tci_starts(void) {
    client_open(A);
    client_open(B);
    say(A, "trx:0,true,tci;");
    say(B, "trx:0,true,tci;");
    check(strstr(heard(B), "trx:0,false;"), "B's start, with A's on its way, is refused at once");
    gui(-1);
    check(radio_get_modem() && txs.owner == A, "on the air with A's audio");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air(), "A's unkey ends it");
}

static int run(const char *name, void (*fn)(void)) {
    int   st;
    pid_t pid;

    printf("%s\n", name);
    fflush(stdout);
    if ((pid = fork()) == 0) {
        server_start();
        fn();
        fflush(stdout);
        _exit(failed ? 1 : 0);
    }
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

int main(void) {
    static const struct {
        const char *name;
        void (*fn)(void);
    } cases[] = {
        {"Two starts before the LVGL thread keys, A ends it", two_starts_a_ends},
        {"Two starts before the LVGL thread keys, B unkeys", two_starts_b_unkeys},
        {"Two starts before the LVGL thread keys, A leaves", two_starts_a_leaves},
        {"A PTT start, then B's TCI audio start", ptt_then_tci_start},
        {"B's start refused while A's key is on its way takes no control", refused_takes_no_control},
        {"B keys during A's transmission", second_key_during_over},
        {"Control taken at the radio between A's start and its key", local_takeover},
        {"A's TCI audio start cancelled before its key, B's PTT start behind it", cancel_tci_then_ptt},
        {"A's PTT start cancelled before its key, B's PTT start behind it", cancel_ptt_then_ptt},
        {"A's start refused: the operator keyed first", refused_operator_first},
        {"A's cancelled start refused, B's start before the server looked", refused_cancelled},
        {"The operator keys before A's key ran, A leaves before it is refused", refused_then_gone},
        {"The operator keys before A's key ran, A unkeys after it was refused", refused_then_unkeyed},
        {"A's unkey sent again, the operator keys between the two", unkey_resent},
        {"A's unkey sent again, B's start and unkey before the copy ran, the operator keys", unkey_resent_then_start},
        {"A's key lost to a full LVGL queue", key_lost},
        {"A starts as another transmission ends, the server sees that end after", end_of_another_seen_late},
        {"B leaves during A's transmission", other_leaves},
        {"The radio back on receive by itself during A's transmission", back_on_receive},
        {"A's key never takes (no TX on this frequency)", never_takes},
        {"A's key reaches the LVGL thread more than a second late", late_key},
        {"Starts ended before their key ran key nothing", cancelled_keys_nothing},
        {"Two TCI audio starts", two_tci_starts},
    };
    int n = sizeof(cases) / sizeof(cases[0]), bad = 0;

    for (int i = 0; i < n; i++)
        bad += run(cases[i].name, cases[i].fn);
    if (bad) {
        printf("OWNER FAILED: %d of %d cases\n", bad, n);
        return 1;
    }
    printf("OWNER OK: %d cases\n", n);
    return 0;
}
