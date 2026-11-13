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
 *  tells it is read from the other. No listener. Every case runs three times: its commands as
 *  written, in capitals (Hamlib's TCI backend) and in mixed case, which TCI says are the same;
 *  the answers are the same, in lower case.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "tci_server.c"

#include <sys/wait.h>

enum { A = 0, B = 1 };

extern bool     stub_defer, stub_tx_refused;
int             stub_run(int n);
extern char     stub_cw_text[4096], stub_said[128];
extern int      stub_cw_stops, stub_said_n;
void            stub_cw_end(bool all);
extern uint32_t stub_ask_id, stub_ask_last;
extern char     stub_ask_who[128];
extern int      stub_ask_shown, stub_ask_why;

static int      peer[MAX_CLIENTS];
static rstate_t last;
static int      failed;
static int      letters;    /* how a client writes its commands: 0 as the case has them, 1 in capitals, 2 mixed */

/* The first n characters of s written as letters says ("trx:0,true,tci;": TRX:0,TRUE,TCI; or TrX:0,TrUe,tCi;) */
static void lettered(char *s, size_t n) {
    for (size_t i = 0; s[i] && i < n; i++)
        if (letters == 1 || (letters == 2 && i % 2 == 0))
            s[i] = (char)toupper((unsigned char)s[i]);
}

/* The server as tci_server_init leaves it, without its threads: the case calls their steps */
static void server_start(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        pthread_mutex_init(&clients[i].lock, NULL);
    }
    txs.owner  = -1;
    stub_defer = true;
    ctl_init();
    unlink(TCI_ALLOWED);        /* nobody allowed always yet */
    allowed_load();
    state_follow(&last, true);
}

/* A client through its handshake from addr, as accept_client and client_read leave it: a program on the radio itself
 * from 127.0.0.1, one on the network from elsewhere; app the name it gave ("" none) */
static void client_from(int ci, const char *addr, const char *app) {
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
    snprintf(c->ip, sizeof(c->ip), "%s", addr);
    snprintf(c->app, sizeof(c->app), "%s", app);
    c->local = !strcmp(addr, "127.0.0.1");
    atomic_fetch_add(&nclients, 1);
    peer[ci] = sv[1];
}

/* The cases of who owns a transmission: programs on the radio, which are never asked */
static void client_open(int ci) {
    client_from(ci, "127.0.0.1", "");
}

/* A connection over Tailscale Funnel as tailscaled hands it over: its upgrade request with tailscaled's header,
 * read by client_read. True when its handshake was taken (the 101 read here) */
static bool funnel_connect(int ci, const char *addr) {
    client_t *c = &clients[ci];
    int       sv[2];
    char      req[512], resp[512];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) < 0) {
        perror("socketpair");
        exit(2);
    }
    c->fd       = sv[0];
    c->fmt_rate = 48000;
    c->fmt_type = TCI_FLOAT32;
    c->last_ms = c->ping_ms = now_ms();
    snprintf(c->ip, sizeof(c->ip), "127.0.0.1");
    atomic_fetch_add(&nclients, 1);
    peer[ci] = sv[1];
    snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: x6200.tail1234.ts.net\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
             "Tailscale-Funnel-Request: ?1\r\nX-Forwarded-For: %s\r\n\r\n", addr);
    if (send(peer[ci], req, strlen(req), MSG_NOSIGNAL) != (ssize_t)strlen(req))
        exit(2);
    client_read(ci);
    ssize_t n = recv(peer[ci], resp, sizeof(resp) - 1, 0);
    return c->fd >= 0 && c->ws && n > 12 && !strncmp(resp, "HTTP/1.1 101", 12);
}

/* A text frame from client ci as a browser sends it (masked), read by client_read; nothing when it is closed. Its
 * word ("auth") as letters says, the token as it is */
static void funnel_say(int ci, const char *text) {
    uint8_t f[256] = {0x81, (uint8_t)(0x80 | strlen(text))};   /* a mask of zeros: the text as it is */

    memcpy(f + 6, text, strlen(text));
    lettered((char *)f + 6, strcspn(text, ": "));
    if (clients[ci].fd >= 0 && send(peer[ci], f, 6 + strlen(text), MSG_NOSIGNAL) == (ssize_t)(6 + strlen(text)))
        client_read(ci);
}

/* The connection lost, as client_read handles it */
static void client_gone(int ci) {
    tx_session_end(ci, false);
    client_close(&clients[ci]);
}

/* Commands from client ci, as letters says */
static void say(int ci, const char *cmd) {
    char line[TEXT_MAX];

    snprintf(line, sizeof(line), "%s", cmd);
    lettered(line, sizeof(line));
    handle_text(ci, line);
}

/* The socket thread's round after a poll (server_thread): the session tick, the question, the state followed */
static void follow(void) {
    tx_session_tick(now_ms());
    ask_tick(now_ms());
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

/* TCI audio in DIGU (WSJT-X on Mode Data/Pkt) and DIGL keys the radio, the modem bit taking the
 * client's audio as the radio's own digital apps do in USB-D; only CW refuses it (#17) */
static void tci_audio_in_dig(void) {
    const int   modes[] = {radio_mode_usb_dig, radio_mode_lsb_dig};
    const char *names[] = {"DIGU", "DIGL"};
    const char *h;
    char        what[96];

    client_open(A);
    for (int i = 0; i < 2; i++) {
        cparam_i_set(cfg_cur_mode, modes[i]);
        gui(-1);
        heard(A);
        say(A, "trx:0,true,tci;");
        gui(-1);
        h = heard(A);
        snprintf(what, sizeof(what), "in %s A's TCI audio start keys, A hears trx:0,true", names[i]);
        check(radio_get_modem() && atomic_load(&txs.keyed) && txs.owner == A && strstr(h, "trx:0,true;")
              && !strstr(h, "trx:0,false;"), what);
        say(A, "trx:0,false;");
        gui(-1);
        h = heard(A);
        snprintf(what, sizeof(what), "in %s A's unkey ends it, A hears trx:0,false", names[i]);
        check(!on_air() && !atomic_load(&txs.keyed) && txs.owner < 0 && strstr(h, "trx:0,false;"), what);
    }
}

static bool ctl_free(void) {
    ctl_info_t o;

    ctl_get(&o);
    return o.kind == CTL_NONE;
}

/* A browser keying its microphone through its TCI link: the start takes control from the browser's session, which
 * the radio cannot tell from that link, and its end gives it back; free again when it was free (a page just opened
 * keys before anything else) or the one it came from left during the over; not when the control changed hands
 * during the over (a knob), or when the client had it before its start (it tuned) */
static void control_given_back(void) {
    client_open(A);
    say(A, "trx:0,true,tci;");          /* control free: nobody took it yet */
    gui(-1);
    check(radio_get_modem() && ctl_holds(CTL_TCI, A), "control free: A's TCI audio start takes it");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air() && ctl_free(), "A's unkey: control free again");

    ctl_claim(CTL_BROWSER, 0, "Macbook", true);
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == A && ctl_holds(CTL_TCI, A), "A's TCI audio start keys, control with A");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air() && ctl_holds(CTL_BROWSER, 0), "A's unkey: control back with the browser");

    say(A, "trx:0,true,tci;");
    gui(-1);
    ctl_local_input();                  /* a knob at the radio during the over */
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air() && ctl_holds(CTL_LOCAL, 0), "a knob during the over: control stays at the radio");

    ctl_claim(CTL_BROWSER, 0, "Macbook", true);
    say(A, "trx:0,true,tci;");
    gui(-1);
    ctl_release(CTL_BROWSER, 0);        /* the browser gone during the over */
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air() && ctl_free(), "the browser gone during the over: control free, as its leaving left it");

    ctl_claim(CTL_BROWSER, 0, "Macbook", true);
    say(A, "vfo:0,0,7100000;");         /* A tunes first: control is A's before its start */
    gui(-1);
    say(A, "trx:0,true,tci;");
    gui(-1);
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air() && ctl_holds(CTL_TCI, A), "A had control before its start: A keeps it");
}

/* Listen only: tuning, the mode, filter, AGC, RIT, drive and a key are answered with the state as it is; nothing goes
 * to the LVGL thread and control stays where it is */
static void listen_only(void) {
    const char *h;

    client_open(A);
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    atomic_store(&trust_operate, false);
    say(A, "vfo:0,0,14074000;modulation:0,usb;rx_filter_band:0,100,3000;agc_mode:0,slow;rit_offset:0,100;");
    say(A, "drive:0,80;trx:0,true,tci;");
    h = heard(A);
    check(strstr(h, "vfo:0,0,7074000;") && strstr(h, "modulation:0,lsb;") && strstr(h, "rx_filter_band:0,-2700,-300;")
          && strstr(h, "agc_mode:0,fast;") && strstr(h, "rit_offset:0,0;") && strstr(h, "drive:0,")
          && strstr(h, "trx:0,false;"), "each answered with the state");
    check(stub_run(-1) == 0 && !on_air() && ctl_holds(CTL_BROWSER, 0), "nothing done, control stays with the browser");
    atomic_store(&trust_operate, true);
    say(A, "vfo:0,0,14074000;");
    gui(-1);
    check(cparam_i_get(cfg_fg_freq) == 14074000 && ctl_holds(CTL_TCI, A), "on Operate A tunes and takes control");
}

/* dds (Hamlib's TCI backend sends one before each vfo) is answered with the frequency, set or asked, and an unknown
 * command (its digl_offset) is left unanswered: neither tunes nor takes control. The vfo after it tunes */
static void dds_and_unknown(void) {
    const char *h;

    client_open(A);
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "dds:0,14074000;dds:0;");
    h = heard(A);
    check(!strcmp(h, "dds:0,7074000;dds:0,7074000;"), "dds answered with the frequency");
    say(A, "digl_offset:1500;rx_nb_enable:0,true;");
    check(!*heard(A), "an unknown command left unanswered");
    check(stub_run(-1) == 0 && ctl_holds(CTL_BROWSER, 0), "nothing done, control stays with the browser");
    say(A, "dds:0,14074000;vfo:0,0,14074000;");
    gui(-1);
    h = heard(A);
    check(strstr(h, "dds:0,7074000;") && strstr(h, "vfo:0,0,14074000;") && cparam_i_get(cfg_fg_freq) == 14074000,
          "dds then vfo, as Hamlib sets a frequency: the vfo tunes");
}

/* A question takes no control: a vfo without its frequency, a trx without true or false, the filter without its two
 * edges, the mode, AGC, RIT and drive without their value. Hamlib's TCI backend asks VFO:0,0; and TRX:0; at every
 * poll: each is answered with the state, nothing is done, and the browser or the program that has control keeps it,
 * also during the asking client's own transmission, after it and during another's. What sets something takes
 * control as before, also a value the radio has already, and a trx true or false; Listen only asks the same */
static void questions_take_no_control(void) {
    struct {
        const char *ask;
        char        answer[48];
    } q[] = {
        {"vfo:0,0;", "vfo:0,0,7074000;"},
        {"vfo:0;", "vfo:0,0,7074000;"},
        {"vfo;", "vfo:0,0,7074000;"},
        {"trx:0;", "trx:0,false;"},
        {"trx;", "trx:0,false;"},
        {"modulation:0;", "modulation:0,lsb;"},
        {"rx_filter_band:0;", "rx_filter_band:0,-2700,-300;"},
        {"rx_filter_band:0,300;", "rx_filter_band:0,-2700,-300;"},  /* one edge: no filter to set */
        {"agc_mode:0;", "agc_mode:0,fast;"},
        {"rit_enable:0;", "rit_enable:0,false;"},
        {"rit_offset:0;", "rit_offset:0,0;"},
        {"drive:0;", ""},                                           /* the stub's power as TCI's drive, below */
    };
    static const char *const sets[] = {     /* the second vfo sets the frequency the radio has by then */
        "vfo:0,0,14074000;", "vfo:0,0,14074000;", "modulation:0,usb;", "rx_filter_band:0,100,3000;",
        "agc_mode:0,slow;", "rit_offset:0,100;", "rit_enable:0,false;", "drive:0,80;", "trx:0,false;",
    };
    int         nq = sizeof(q) / sizeof(q[0]);
    const char *h;
    char        what[128];
    rstate_t    s;
    bool        idle;

    state_read(&s);
    snprintf(q[nq - 1].answer, sizeof(q[nq - 1].answer), "drive:0,%d;", s.drive);
    client_open(A);
    client_open(B);
    for (int i = 0; i < nq; i++) {
        ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
        say(A, q[i].ask);
        h    = heard(A);
        idle = stub_run(-1) == 0;
        snprintf(what, sizeof(what), "%s answered %s, nothing done, control stays with the browser", q[i].ask,
                 q[i].answer);
        check(!strcmp(h, q[i].answer) && idle && ctl_holds(CTL_BROWSER, 0), what);
    }

    say(B, "vfo:0,0,7100000;");             /* B tunes: control is B's */
    gui(-1);
    check(cparam_i_get(cfg_fg_freq) == 7100000 && ctl_holds(CTL_TCI, B), "B tunes and takes control");
    heard(A);
    say(A, "dds:0;vfo:0,0;trx:0;");         /* Hamlib's poll */
    check(!strcmp(heard(A), "dds:0,7100000;vfo:0,0,7100000;trx:0,false;") && ctl_holds(CTL_TCI, B),
          "A's poll answered, control stays with B");

    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "trx:0,true;");                  /* A keys: control is A's for its transmission */
    gui(-1);
    check(on_air() && ptt_owner == A && ctl_holds(CTL_TCI, A), "A's start keys, control with A");
    heard(A);
    heard(B);
    say(A, "trx:0;");
    say(B, "vfo:0,0;trx:0;");
    check(!strcmp(heard(A), "trx:0,true;") && !strcmp(heard(B), "vfo:0,0,7100000;trx:0,true;") &&
          ctl_holds(CTL_TCI, A), "A's and B's polls during A's transmission: control stays with A");
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air() && ctl_holds(CTL_BROWSER, 0), "A's unkey ends it, control back with the browser");
    heard(A);
    say(A, "vfo:0,0;trx:0;");
    check(!strcmp(heard(A), "vfo:0,0,7100000;trx:0,false;") && ctl_holds(CTL_BROWSER, 0),
          "A's poll after its transmission: control stays with the browser");

    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
        say(A, sets[i]);
        gui(-1);
        snprintf(what, sizeof(what), "%s takes control%s", sets[i],
                 i && !strcmp(sets[i], sets[i - 1]) ? ", the frequency the radio has" : "");
        check(ctl_holds(CTL_TCI, A), what);
    }
    check(cparam_i_get(cfg_fg_freq) == 14074000 && cparam_i_get(cfg_cur_mode) == radio_mode_usb &&
          cparam_i_get(cfg_cur_agc) == radio_agc_slow, "the sets are done");

    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    atomic_store(&trust_operate, false);    /* Listen only */
    heard(A);
    say(A, "vfo:0,0;trx:0;");
    check(!strcmp(heard(A), "vfo:0,0,14074000;trx:0,false;") && stub_run(-1) == 0 && ctl_holds(CTL_BROWSER, 0),
          "on Listen only A's poll answered, control stays with the browser");
}

/* Over Tailscale Funnel (#58) only the Panel's own sound and its key with it: a session that may not transmit, or
 * one that keyed nothing, asking for the TX state or unkeying is answered with the state and takes no control (the
 * browser at home keeps it, as when that session's browser asks on the browser remote's socket); the session that
 * keyed unkeys its own transmission */
static void funnel_takes_no_control(void) {
    client_open(A);
    client_open(B);
    clients[A].funnel = clients[B].funnel = true;
    snprintf(clients[A].token, sizeof(clients[A].token), "%s", "11111111111111111111111111111111");  /* no TX */
    snprintf(clients[B].token, sizeof(clients[B].token), "%s", "0123456789abcdef0123456789abcdef");
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "trx:0,false;");
    say(A, "trx;");
    say(B, "trx:0,false;");
    check(ctl_holds(CTL_BROWSER, 0), "an unkey or a question from a client that keyed nothing takes no control");
    check(strstr(heard(A), "trx:0,false;") && strstr(heard(B), "trx:0,false;"), "both answered with the state");
    say(A, "trx:0,true,tci;");
    check(ctl_holds(CTL_BROWSER, 0) && strstr(heard(A), "trx:0,false;"), "the session without TX: its key refused");
    say(B, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == B && ctl_holds(CTL_TCI, B), "B keys with its audio, control with it");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air() && txs.owner < 0, "B's unkey ends it");
}

/* Over Tailscale Funnel one browser at a time has the Panel's sound and microphone (#58). A phone that goes from
 * the WiFi to its mobile data leaves its connection stale, without a word, up until IDLE_MS: the same browser's new
 * connection, its key the same, takes its place once that one has been silent FUNNEL_STALE_MS, and what it had on
 * the air ends with it. While its page answers (the Panel and the Screen page in two tabs) the place stays its, and
 * another browser's key is refused. One connection at the most waits for its key; wrong keys pause Funnel (the
 * stub's rule: three) */
static void funnel_same_browser_back(void) {
    enum { C = 2, D = 3 };
    const char *op = "auth:0123456789abcdef0123456789abcdef;", *listener = "auth:11111111111111111111111111111111;";

    check(funnel_connect(A, "203.0.113.7"), "A over Funnel: its handshake taken");
    funnel_say(A, op);
    check(clients[A].up && strstr(heard(A), "ready;"), "A's key: A greeted");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == A, "A on the air with its audio");
    check(!funnel_connect(B, "198.51.100.2") && clients[B].fd < 0, "A's page answers: another connection refused");

    clients[A].last_ms = now_ms() - FUNNEL_STALE_MS - 1;   /* A's page gone without a word */
    check(funnel_connect(B, "198.51.100.2"), "A silent: another connection's handshake taken");
    funnel_say(B, listener);
    check(clients[B].fd < 0 && clients[A].up && txs.owner == A, "another browser's key: refused, A keeps the air");
    check(funnel_connect(B, "198.51.100.3"), "the same browser from another network: its handshake taken");
    funnel_say(B, op);
    check(clients[A].fd < 0 && clients[B].up && strstr(heard(B), "ready;"), "its key: A closed, B greeted");
    gui(-1);
    check(!on_air() && txs.owner < 0, "A's transmission ended with it");
    say(B, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == B, "B keys with its audio");
    say(B, "trx:0,false;");
    gui(-1);
    check(!on_air(), "B's unkey ends it");

    clients[B].last_ms = now_ms() - FUNNEL_STALE_MS - 1;
    funnel_connect(C, "198.51.100.4");
    funnel_connect(D, "198.51.100.5");
    check(clients[C].fd < 0 && clients[D].fd >= 0 && clients[B].up, "a second waiting for its key: the first goes");
    for (int k = 0; k < 3; k++) {
        if (k)
            funnel_connect(D, "198.51.100.5");
        funnel_say(D, "auth:00000000000000000000000000000007;");
        check(clients[D].fd < 0 && clients[B].up && remote_server_funnel_paused() == (k == 2),
              k < 2 ? "a wrong key: closed, B kept" : "the third wrong key: Funnel paused, B kept");
    }
    check(!funnel_connect(D, "198.51.100.5"), "paused: a connection over Funnel refused");
}

/* ---- A client on the network asks before it transmits ----------------------------- */

#define LAN_A  "192.168.1.20"
#define LAN_B  "192.168.1.30"
#define TOKEN  "0123456789abcdef0123456789abcdef"   /* the stub's paired browser that may transmit */
#define LISTEN "11111111111111111111111111111111"   /* and the one that may not */

/* A connection through its handshake as client_read takes it: from addr (the TCP peer), with more request headers
 * ("Name: value\r\n" lines). True when it was taken (the 101 read here) */
static bool lan_connect(int ci, const char *addr, const char *headers) {
    static char resp[65536];    /* the 101 and the greeting after it, all the server wrote: frames read whole later */
    client_t   *c = &clients[ci];
    int         sv[2];
    char        req[768];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) < 0) {
        perror("socketpair");
        exit(2);
    }
    c->fd       = sv[0];
    c->fmt_rate = 48000;
    c->fmt_type = TCI_FLOAT32;
    c->last_ms = c->ping_ms = now_ms();
    snprintf(c->ip, sizeof(c->ip), "%s", addr);
    atomic_fetch_add(&nclients, 1);
    peer[ci] = sv[1];
    snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n%s\r\n", headers);
    if (send(peer[ci], req, strlen(req), MSG_NOSIGNAL) != (ssize_t)strlen(req))
        exit(2);
    client_read(ci);
    ssize_t n = recv(peer[ci], resp, sizeof(resp) - 1, 0);
    return c->fd >= 0 && c->up && n > 12 && !strncmp(resp, "HTTP/1.1 101", 12);
}

/* The browser Panel's key on its TCI link, the word as letters says, the token as it is */
static void say_auth(int ci, const char *token) {
    char line[64];

    snprintf(line, sizeof(line), "auth:%s", token);
    lettered(line, 4);
    handle_text(ci, line);
}

/* The operator answers the question on the screen; the socket thread takes it, the box hears what became of it */
static void answer(tci_ask_t a) {
    tci_server_answer(stub_ask_id, a);
    gui(-1);
    gui(-1);
}

/* An unkey, and the radio back on receive */
static void unkey(int ci) {
    say(ci, "trx:0,false;");
    gui(-1);
}

/* A start from a client on the network not allowed yet: trx:0,false at once, the question on the screen, nothing on
 * the air and control where it was. A second start from it asks nothing more; one from another client meanwhile is
 * refused without a question of its own. Allow once: its next start keys, while that connection stays open; the
 * same client back on a new connection is asked again, and nothing was kept */
static void ask_allow_once(void) {
    const char *h;

    client_from(A, LAN_A, "WSJT-X");
    client_from(B, LAN_B, "");
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "trx:0,true,tci;");
    h = heard(A);
    gui(-1);
    check(strstr(h, "trx:0,false;") && !strstr(h, "trx:0,true;"), "A's start: trx:0,false at once");
    check(stub_ask_id && !strcmp(stub_ask_who, LAN_A " (WSJT-X)") && stub_ask_shown == 1,
          "the radio asks: \"" LAN_A " (WSJT-X) wants to transmit\"");
    check(!on_air() && txs.owner < 0 && ctl_holds(CTL_BROWSER, 0), "nothing on the air, control stays with the browser");
    say(A, "trx:0,true;");
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && stub_ask_shown == 1 && !on_air(), "A's second start: no second question");
    say(B, "trx:0,true;");
    gui(-1);
    check(strstr(heard(B), "trx:0,false;") && stub_ask_shown == 1 && !on_air() && ask.ci == A,
          "B's start while A's question is open: refused, no question of its own");
    answer(TCI_ASK_ONCE);
    check(!stub_ask_id && ask.id == 0 && !on_air(), "Allow once: the question gone, nothing keyed by itself");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == A && ctl_holds(CTL_TCI, A), "A's next start keys with its audio");
    unkey(A);
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A && stub_ask_shown == 1, "and again, without a question");
    unkey(A);
    check(!on_air() && tci_server_allowed(NULL, 0) == 0, "nothing kept for its address");
    client_gone(A);
    client_from(A, LAN_A, "WSJT-X");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && stub_ask_shown == 2 && !on_air(), "back on a new connection: asked again");
    say(B, "trx:0,true;");
    gui(-1);
    check(strstr(heard(B), "trx:0,false;") && stub_ask_shown == 2, "B still refused while A's question is open");
    answer(TCI_ASK_NO);
    say(B, "trx:0,true;");
    gui(-1);
    check(strstr(heard(B), "trx:0,false;") && stub_ask_shown == 3 && ask.ci == B, "B's next start: its own question");
}

/* Always: the address kept (TCI_ALLOWED, with the name it gave), its start keys, also on a new connection and after a
 * restart, which reads the file again; another address is still asked. Forgotten, its next start asks again, also on
 * the connection that was allowed */
static void ask_always(void) {
    char buf[128], addr[48], name[32];
    uint64_t seen;
    bool     online;

    client_from(A, LAN_A, "WSJT-X");
    say(A, "trx:0,true,tci;");
    gui(-1);
    answer(TCI_ASK_ALWAYS);
    check(stub_ask_why == TCI_ASK_ALWAYS && tci_server_allowed(buf, sizeof(buf)) == 1 &&
          !strcmp(buf, LAN_A " (WSJT-X)"), "Always: kept, the box told so");
    check(tci_server_allowed_get(0, addr, sizeof(addr), name, sizeof(name), &seen, &online) && !strcmp(addr, LAN_A) &&
          !strcmp(name, "WSJT-X") && online && seen, "listed: its address, its name, connected now");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == A, "A's next start keys");
    unkey(A);
    client_gone(A);
    check(tci_server_allowed_get(0, addr, sizeof(addr), name, sizeof(name), &seen, &online) && !online,
          "listed as not connected once it left");
    client_from(A, LAN_A, "WSJT-X");
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A && stub_ask_shown == 1, "back on a new connection: keys, no question");
    unkey(A);
    client_gone(A);
    nallowed = 0;                   /* a restart: what the card has */
    allowed_load();
    client_from(A, LAN_A, "");      /* the same address, no name given this time */
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A && stub_ask_shown == 1, "after a restart: keys, no question");
    unkey(A);
    check(tci_server_allowed(buf, sizeof(buf)) == 1 && !strcmp(buf, LAN_A " (WSJT-X)"), "the name it gave kept");
    client_from(B, LAN_B, "");
    say(B, "trx:0,true;");
    gui(-1);
    check(strstr(heard(B), "trx:0,false;") && stub_ask_shown == 2 && !on_air(), "another address: asked");
    answer(TCI_ASK_NO);
    check(tci_server_forget(LAN_A) && tci_server_allowed(NULL, 0) == 0, "forgotten");
    say(A, "trx:0,true;");
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && stub_ask_shown == 3 && !on_air(),
          "forgotten: A's next start asks again, on the same connection");
    nallowed = 0;
    allowed_load();
    check(tci_server_allowed(NULL, 0) == 0, "and the card has it forgotten too");

    /* A file edited by hand: a name with quotes cleaned, an address that is none dropped (the admin's list is JSON) */
    FILE *f = fopen(TCI_ALLOWED, "w");

    fprintf(f, "%s\tLog\"ger\\x\t1\t2\n\"bad\"\tX\t1\t2\n", LAN_A);
    fclose(f);
    nallowed = 0;
    allowed_load();
    check(tci_server_allowed(buf, sizeof(buf)) == 1 && !strcmp(buf, LAN_A " (Loggerx)"),
          "a file edited by hand: the quotes gone from the name, the line without an address dropped");
    tci_server_forget(NULL);
}

/* No: refused while that connection stays open, without asking again; a new connection is asked again */
static void ask_no(void) {
    client_from(A, LAN_A, "");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(stub_ask_id && !strcmp(stub_ask_who, LAN_A), "the radio asks: \"" LAN_A " wants to transmit\"");
    answer(TCI_ASK_NO);
    check(!stub_ask_id && stub_ask_why == TCI_ASK_NO && !on_air(), "No: the question gone, nothing on the air");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && stub_ask_shown == 1 && !on_air(), "A's next start: refused, not asked");
    check(tci_server_allowed(NULL, 0) == 0, "nothing kept");
    client_gone(A);
    client_from(A, LAN_A, "");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(stub_ask_shown == 2 && !on_air(), "a new connection: asked again");
}

/* No answer within TCI_ASK_S is a No, with a note on the screen; an answer that comes after does nothing. The client
 * leaving takes its question off the screen, and the next client's start asks */
static void ask_timeout_and_gone(void) {
    uint32_t id;

    client_from(A, LAN_A, "");
    client_from(B, LAN_B, "");
    say(A, "trx:0,true;");
    gui(-1);
    id = stub_ask_id;
    ask_tick(now_ms() + ASK_MS - 1000);
    check(ask.id == id, "still open a second before the time");
    ask_tick(now_ms() + ASK_MS + 1);
    gui(-1);
    check(!stub_ask_id && stub_ask_why == TCI_ASK_TIMEOUT && ask.id == 0, "no answer in time: closed, with its note");
    tci_server_answer(id, TCI_ASK_ONCE);    /* pressed too late */
    gui(-1);
    say(A, "trx:0,true;");
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && stub_ask_shown == 1 && !on_air(), "a No: A refused, not asked again");

    say(B, "trx:0,true;");
    gui(-1);
    check(stub_ask_id && ask.ci == B && stub_ask_shown == 2, "B asks");
    client_gone(B);
    gui(-1);
    check(!stub_ask_id && stub_ask_why == TCI_ASK_GONE && ask.id == 0, "B leaves: its question goes with it");
    client_from(B, LAN_B, "");
    say(B, "trx:0,true;");
    gui(-1);
    check(stub_ask_id && ask.ci == B && stub_ask_shown == 3, "B back: asked again");
    answer(TCI_ASK_ONCE);
    say(B, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == B, "Allow once: B keys");
    unkey(B);
}

/* Not asked, as before: a program on the radio itself, a browser Panel that gave the key of a paired browser that may
 * transmit (one whose session may not is refused without a question); a start that cannot be had anyway (the radio
 * on the air) and one under Listen only ask nothing. Who is who comes from the handshake: from 127.0.0.1 a program
 * on the radio, unless it is a browser's page or a proxy on the radio (tailscaled, the web server's https port) names
 * the client in X-Forwarded-For; from elsewhere that header is the client's own word, not believed */
static void ask_not_for(void) {
    enum { C = 2 };

    check(lan_connect(A, "127.0.0.1", "User-Agent: Pat/1.0\r\n") && clients[A].local &&
          !strcmp(clients[A].app, "Pat/1.0"), "from 127.0.0.1: a program on the radio, its name taken");
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A && !stub_ask_shown, "a program on the radio keys, not asked");
    unkey(A);
    client_gone(A);

    check(lan_connect(A, "127.0.0.1", "X-Forwarded-For: 203.0.113.9, 100.101.102.103\r\n") && !clients[A].local &&
          !strcmp(clients[A].ip, "100.101.102.103"), "through a proxy on the radio: the client it names, the last");
    check(lan_connect(B, "127.0.0.1", "Origin: http://127.0.0.1\r\n") && !clients[B].local &&
          !strcmp(clients[B].ip, "127.0.0.1"), "a browser's page from 127.0.0.1: not a program on the radio");
    check(lan_connect(C, LAN_B, "X-Forwarded-For: 127.0.0.1\r\nUser-Agent: Logger (v2; beta)\r\n") &&
          !clients[C].local && !strcmp(clients[C].ip, LAN_B) && !strcmp(clients[C].app, "Logger (v2 beta)"),
          "from the network, X-Forwarded-For not believed; its name without the marks");
    say(A, "trx:0,true;");
    gui(-1);
    check(!on_air() && stub_ask_shown == 1 && !strcmp(stub_ask_who, "100.101.102.103"), "the proxied client: asked");
    answer(TCI_ASK_NO);
    client_gone(A);
    client_gone(B);
    client_gone(C);

    client_from(A, LAN_A, "");          /* the browser Panel on the LAN */
    say_auth(A, TOKEN);
    check(!*heard(A), "its key on the TCI link: not answered");
    say(A, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == A && stub_ask_shown == 1, "a paired browser that may transmit: keys");
    unkey(A);
    client_from(B, LAN_B, "");
    say_auth(B, LISTEN);
    say(B, "trx:0,true,tci;");
    gui(-1);
    check(strstr(heard(B), "trx:0,false;") && !on_air() && stub_ask_shown == 1 && !ask.id,
          "a paired browser that may not: refused, not asked");
    say_auth(B, "00000000000000000000000000000007");
    say(B, "trx:0,true,tci;");
    gui(-1);
    check(strstr(heard(B), "trx:0,false;") && stub_ask_shown == 2 && ask.ci == B, "a wrong key: asked as any client");
    answer(TCI_ASK_NO);

    client_from(C, LAN_B, "");
    radio_set_ptt(true);                /* the operator on the air */
    say(C, "trx:0,true;");
    check(strstr(heard(C), "trx:0,false;") && !ask.id, "the radio on the air: refused, nothing asked");
    radio_set_ptt(false);
    gui(-1);
    atomic_store(&trust_operate, false);
    say(C, "trx:0,true;");
    gui(-1);
    check(strstr(heard(C), "trx:0,false;") && !ask.id && stub_ask_shown == 2, "Listen only: refused, nothing asked");
    atomic_store(&trust_operate, true);
}

/* tx_confirm=0: nobody is asked, a client on the network keys as before; turning it off takes an open question off
 * the screen. Saved in TCI_CONF and read back */
static void ask_off(void) {
    client_from(A, LAN_A, "");
    say(A, "trx:0,true;");
    gui(-1);
    check(stub_ask_id && ask.ci == A, "on: A asked");
    tci_server_set_tx_confirm(false);
    gui(-1);                            /* the socket thread sees it */
    gui(-1);                            /* the box hears */
    check(!stub_ask_id && !ask.id && stub_ask_why == TCI_ASK_GONE, "turned off: the question goes");
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A && stub_ask_shown == 1, "off: A keys without a question");
    unkey(A);
    client_from(B, LAN_B, "");
    say(B, "trx:0,true,tci;");
    gui(-1);
    check(radio_get_modem() && txs.owner == B && stub_ask_shown == 1, "off: B keys with its audio");
    unkey(B);
    atomic_store(&tx_confirm, true);
    conf_read();
    check(!atomic_load(&tx_confirm), "tx_confirm=0 read back from the conf");
    tci_server_set_tx_confirm(true);
    conf_read();
    check(atomic_load(&tx_confirm), "and tx_confirm=1");
}


/* ---- CW from a client (#179) ------------------------------------------------------------ */

/* The radio in mode m, the clients told so before the case's first look */
static void radio_mode(int32_t m) {
    cparam_i_set(cfg_cur_mode, m);
    follow();
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            heard(i);
}

/* In CW a client's text goes on the radio's own keyer as the keyer takes it (TCI's stand-ins for its separators back,
 * a run between bars one sign), every client hears the radio on the air, the client takes control from the browser;
 * another client's key and text meanwhile are refused. All of it sent: cw_macros_empty to the client alone, the radio
 * back on receive, control back with the browser. The same in CW-R */
static void cw_keyed(void) {
    const char *h;

    radio_mode(radio_mode_cw);
    client_open(A);
    client_open(B);
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "cw_macros:0,cq de on6ure^ 5nn~ tu* |sk|;");
    check(!*heard(A) && cw_owner == A && ctl_holds(CTL_TCI, A), "A's text: no answer yet, A has control");
    gui(-1);
    check(!strcmp(stub_cw_text, "CQ DE ON6URE: 5NN, TU; <SK>\n"), "on the keyer: CQ DE ON6URE: 5NN, TU; <SK>");
    check(on_air() && strstr(heard(B), "trx:0,true;") && strstr(heard(A), "trx:0,true;"), "everyone hears trx true");
    say(B, "trx:0,true;");
    check(!strcmp(heard(B), "trx:0,false;"), "B's key meanwhile: trx:0,false");
    say(B, "cw_macros:0,test;");
    gui(-1);
    check(!strcmp(heard(B), "trx:0,false;") && !strstr(stub_cw_text, "TEST"), "B's text meanwhile: trx:0,false, not keyed");
    stub_cw_end(true);
    gui(-1);
    h = heard(A);
    check(!strcmp(h, "cw_macros_empty:0;trx:0,false;"), "all of it sent: A hears cw_macros_empty, then trx false");
    check(!strcmp(heard(B), "trx:0,false;"), "B hears trx false, no cw_macros_empty");
    check(cw_owner < 0 && ctl_holds(CTL_BROWSER, 0), "over, control back with the browser");
    radio_mode(radio_mode_cwr);
    say(B, "cw_macros:0,tu;");
    gui(-1);
    check(on_air() && cw_owner == B && strstr(stub_cw_text, "TU\n"), "in CW-R B's text keys");
    stub_cw_end(true);
    gui(-1);
    check(!on_air() && cw_owner < 0 && strstr(heard(B), "cw_macros_empty:0;"), "and ends");
}

/* Outside CW and CW-R a text is refused at once, trx:0,false, with a note on the screen (one in 5 s however often it
 * comes), nothing asked, nothing keyed, control where it was. A mode changed while the text was on its way to the
 * keyer: refused there, the same */
static void cw_not_cw(void) {
    client_open(A);
    client_from(B, LAN_B, "QLog");
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "cw_macros:0,test;");
    check(!strcmp(heard(A), "trx:0,false;") && stub_run(-1) == 0 && !stub_cw_text[0], "in LSB: trx:0,false, nothing keyed");
    check(stub_said_n == 1 && !strcmp(stub_said, "TCI CW: set CW or CW-R mode"), "the screen: TCI CW: set CW or CW-R mode");
    check(ctl_holds(CTL_BROWSER, 0) && cw_owner < 0, "control stays with the browser");
    say(A, "cw_macros:0,test;");
    say(B, "cw_macros:0,test;");
    gui(-1);
    check(!strcmp(heard(A), "trx:0,false;") && !strcmp(heard(B), "trx:0,false;") && stub_said_n == 1,
          "again, and from another client: refused, no second note within 5 s");
    check(!stub_ask_shown, "a client on the network: not asked about a text the radio would not key");
    radio_mode(radio_mode_cw);
    say(A, "cw_macros:0,test;");
    cparam_i_set(cfg_cur_mode, radio_mode_usb);    /* the mode changed before the LVGL thread took the text */
    atomic_store(&cw_said_ms, 0);
    gui(-1);
    check(!stub_cw_text[0] && !on_air() && strstr(heard(A), "trx:0,false;") && cw_owner < 0,
          "the mode changed on its way: refused at the keyer, trx:0,false");
    check(stub_said_n == 2 && ctl_holds(CTL_BROWSER, 0), "the note again; control back with the browser");
}

/* Ask before TX: a client on the network not allowed yet is asked about at its first text, refused (trx:0,false),
 * nothing keyed, control where it was; Allow once, and its next text keys. Listen only: answered with the radio's
 * state, nothing keyed, nothing asked; its speed answered with KEY Speed, unchanged */
static void cw_ask_and_listen(void) {
    radio_mode(radio_mode_cw);
    client_from(A, LAN_A, "QLog");
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "CW_MACROS:0,TEST;");
    gui(-1);
    check(!strcmp(heard(A), "trx:0,false;") && stub_ask_shown == 1 && !strcmp(stub_ask_who, LAN_A " (QLog)"),
          "its first text: trx:0,false, the radio asks: \"" LAN_A " (QLog) wants to transmit\"");
    check(!stub_cw_text[0] && !on_air() && ctl_holds(CTL_BROWSER, 0), "nothing keyed, control with the browser");
    answer(TCI_ASK_ONCE);
    check(!stub_cw_text[0], "Allow once: nothing keyed by itself");
    say(A, "CW_MACROS:0,TEST;");
    gui(-1);
    check(on_air() && !strcmp(stub_cw_text, "TEST\n") && stub_ask_shown == 1, "its next text keys");
    stub_cw_end(true);
    gui(-1);
    client_gone(A);

    client_open(B);
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    atomic_store(&trust_operate, false);
    say(B, "cw_macros:0,test;cw_macros_speed:30;cw_keyer_speed:0,30;");
    check(!strcmp(heard(B), "trx:0,false;cw_macros_speed:15;cw_keyer_speed:15;"),
          "Listen only: answered with the state and KEY Speed");
    check(stub_run(-1) == 0 && !strcmp(stub_cw_text, "TEST\n") && param_i_get(cfg_key_speed) == 15 &&
          ctl_holds(CTL_BROWSER, 0) && stub_ask_shown == 1, "nothing keyed, nothing set, nothing asked, control stays");
}

/* Where the radio may not transmit the keyer's first key down is refused (the screen says why, radio.c): the text is
 * over at once, its client hears trx:0,false, and the next start of anyone keys */
static void cw_tx_refused(void) {
    radio_mode(radio_mode_cw);
    client_open(A);
    client_open(B);
    stub_tx_refused = true;
    say(A, "cw_macros:0,test;");
    gui(-1);
    check(!on_air() && strstr(heard(A), "trx:0,false;") && cw_owner < 0, "refused at its first key: A hears trx false");
    stub_tx_refused = false;
    say(B, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == B, "B's start then keys");
    say(B, "trx:0,false;");
    gui(-1);
    say(B, "cw_macros:0,test;");
    gui(-1);
    heard(B);
    say(B, "trx:0,true;");
    check(!strcmp(heard(B), "trx:0,false;"), "a key from the client whose text goes: trx:0,false");
    stub_cw_end(true);
    gui(-1);
    say(A, "trx:0,true;");
    gui(-1);
    heard(A);
    say(A, "cw_macros:0,test;");
    check(!strcmp(heard(A), "trx:0,false;") && ptt_owner == A, "a text from the client on the PTT: trx:0,false");
    say(A, "trx:0,false;");
    gui(-1);
}

/* The keyer is the operator's while it sends a text of the radio's (the CW keyboard, a message, a beacon): a client's
 * text then is refused, also when it got past the socket thread before the operator started; the operator's text goes
 * on. A text of the operator's started while a client's goes takes the keyer (the last started stops the other): the
 * client's is over, trx:0,false and cw_macros_empty, what it had waiting dropped, the operator's not stopped */
static void cw_keyer_shared(void) {
    radio_mode(radio_mode_cw);
    client_open(A);
    unsigned op = cw_encoder_send("CQ TEST", false);     /* the operator's CW keyboard */
    follow();
    heard(A);
    say(A, "cw_macros:0,test;");
    check(!strcmp(heard(A), "trx:0,false;") && stub_run(-1) == 0, "the operator's text on the keyer: trx:0,false");
    stub_cw_end(true);
    follow();
    heard(A);
    say(A, "cw_macros:0,test;");
    op = cw_encoder_send("CQ TEST", false);              /* before the LVGL thread took A's */
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && cw_owner < 0 && cw_encoder_sending() == op && !stub_cw_stops,
          "started on its way to the keyer: refused there, the operator's goes on");
    stub_cw_end(true);
    follow();
    heard(A);
    say(A, "cw_macros:0,cq cq ;");
    gui(-1);
    heard(A);
    say(A, "cw_macros:0,de on6ure ;");
    op = cw_encoder_send("QRL?", false);                 /* the operator starts a text of the radio's */
    gui(-1);
    check(!strcmp(heard(A), "trx:0,false;cw_macros_empty:0;") && cw_owner < 0, "A's cut short: trx false, cw_macros_empty");
    check(cw_encoder_sending() == op && !stub_cw_stops, "the operator's goes on, not stopped");
    stub_cw_end(true);
    gui(-1);
    check(!strstr(stub_cw_text, "DE ON6URE"), "what A had waiting is dropped");
}

/* cw_macros_stop stops the client's own text (another client's stop changes nothing), what waits is dropped, and no
 * cw_macros_empty follows: the radio's trx does. Its trx:0,false and its leaving stop it too, also a text on its way */
static void cw_stopped(void) {
    radio_mode(radio_mode_cw);
    client_open(A);
    client_open(B);
    say(A, "cw_macros:0,cq cq ;");
    gui(-1);
    say(A, "cw_macros:0,de on6ure ;");
    say(B, "cw_macros_stop;");
    gui(-1);
    check(on_air() && cw_owner == A && !stub_cw_stops, "B's stop: A's text goes on");
    say(A, "CW_MACROS_STOP;");
    gui(-1);
    check(!on_air() && stub_cw_stops == 1 && cw_owner < 0, "A's stop: stopped");
    check(!strcmp(heard(A), "trx:0,true;trx:0,false;"), "A hears the radio on the air and back, no cw_macros_empty");
    gui(-1);
    check(!strstr(stub_cw_text, "DE ON6URE"), "what waited is dropped");

    say(A, "cw_macros:0,test;");
    gui(-1);
    say(A, "trx:0,false;");
    gui(-1);
    check(!on_air() && stub_cw_stops == 2 && cw_owner < 0, "its trx:0,false stops it");
    say(B, "cw_macros:0,test;");
    gui(-1);
    client_gone(B);
    gui(-1);
    check(!on_air() && stub_cw_stops == 3 && cw_owner < 0, "its client leaving stops it");
    say(A, "cw_macros:0,paris;");
    client_gone(A);
    gui(-1);
    check(!on_air() && !strstr(stub_cw_text, "PARIS") && stub_cw_stops == 3, "a text on its way when it left keys nothing");
}

/* A stop lost to a full LVGL queue goes again until the keyer is seen stopped; meanwhile nobody starts */
static void cw_stop_resent(void) {
    radio_mode(radio_mode_cw);
    client_open(A);
    client_open(B);
    say(A, "cw_macros:0,test;");
    gui(-1);
    say(A, "cw_macros_stop;");
    tx_session_tick(now_ms() + 2 * CW_STOP_MS);
    heard(B);
    say(B, "trx:0,true;");
    check(!strcmp(heard(B), "trx:0,false;"), "B's start while the stop is not seen done: refused");
    check(stub_run(-1) == 2 && !on_air() && stub_cw_stops == 1, "the stop sent again, the keyer stopped once");
    follow();
    say(B, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == B, "then B keys");
    say(B, "trx:0,false;");
    gui(-1);
}

/* More text from the client while its own goes waits and follows it, as one text (TCI's queue); the end of the last
 * says cw_macros_empty once. Out of CW the text on the air stops: trx false, cw_macros_empty, a note */
static void cw_more_text(void) {
    radio_mode(radio_mode_cw);
    client_open(A);
    say(A, "cw_macros:0,cq cq ;");
    gui(-1);
    say(A, "cw_macros:0,de on6ure ;cw_macros:0,k;");
    gui(-1);
    check(!strcmp(stub_cw_text, "CQ CQ \n"), "the rest waits for the keyer");
    stub_cw_end(true);
    gui(-1);                    /* the socket thread sees the keyer done, hands the rest over */
    gui(-1);                    /* the LVGL thread puts it on the keyer */
    check(!strcmp(stub_cw_text, "CQ CQ \nDE ON6URE K\n") && on_air() && cw_owner == A, "then goes, as one text");
    check(!strstr(heard(A), "cw_macros_empty"), "no cw_macros_empty between the two");
    stub_cw_end(true);
    gui(-1);
    check(!strcmp(heard(A), "cw_macros_empty:0;trx:0,false;") && cw_owner < 0, "the last ends: cw_macros_empty once");

    say(A, "cw_macros:0,test;");
    gui(-1);
    cparam_i_set(cfg_cur_mode, radio_mode_usb);
    gui(-1);
    check(strstr(heard(A), "trx:0,false;cw_macros_empty:0;") && !strcmp(stub_said, "TCI CW stopped: not CW"),
          "the mode set to USB: stopped, A told, the screen says why");
    gui(-1);
    check(!on_air() && stub_cw_stops == 1 && cw_owner < 0, "the keyer stopped");
}

/* KEY Speed: asked (TCI's form, and with the transceiver first: 0 is no speed, a question) and set (either form,
 * either name; within 5 to 50 WPM), told to every client by both names as it changes; set to the value it has, the
 * answer to the setter alone. A question takes no control, a set does */
static void cw_speed(void) {
    client_open(A);
    client_open(B);
    ctl_claim(CTL_BROWSER, 0, "Shack PC", true);
    say(A, "cw_macros_speed;cw_keyer_speed;cw_macros_speed:0;cw_keyer_speed:fast;");
    check(!strcmp(heard(A), "cw_macros_speed:15;cw_keyer_speed:15;cw_macros_speed:15;cw_keyer_speed:15;"),
          "asked: KEY Speed, 15");
    check(stub_run(-1) == 0 && ctl_holds(CTL_BROWSER, 0), "a question takes no control");
    say(A, "cw_macros_speed:25;");
    gui(-1);
    check(param_i_get(cfg_key_speed) == 25 && ctl_holds(CTL_TCI, A), "set to 25, A takes control");
    check(!strcmp(heard(A), "cw_macros_speed:25;cw_keyer_speed:25;") &&
          !strcmp(heard(B), "cw_macros_speed:25;cw_keyer_speed:25;"), "both names to every client");
    say(B, "cw_keyer_speed:0,30;");
    gui(-1);
    check(param_i_get(cfg_key_speed) == 30 && strstr(heard(A), "cw_keyer_speed:30;"), "cw_keyer_speed:0,30 sets 30");
    say(A, "cw_macros_speed:60;");
    gui(-1);
    check(param_i_get(cfg_key_speed) == 50 && strstr(heard(A), "cw_macros_speed:50;"), "60: KEY Speed's top, 50");
    say(A, "cw_keyer_speed:2;");
    gui(-1);
    check(param_i_get(cfg_key_speed) == 5 && strstr(heard(B), "cw_keyer_speed:5;"), "2: its bottom, 5");
    heard(A);
    say(A, "cw_macros_speed:5;");
    check(!strcmp(heard(A), "cw_macros_speed:5;") && !*heard(B) && stub_run(-1) == 0, "the speed it has: to A alone");
    param_i_set(cfg_key_speed, 22);                     /* KEY > Speed on the radio */
    follow();
    check(!strcmp(heard(A), "cw_macros_speed:22;cw_keyer_speed:22;"), "a change on the radio told too");
}

/* A text's rules (TCI 2.1, 3.2.1): the stand-ins for the separators, runs between bars, speed steps (and the speed
 * back at the end), letters in capitals, what has no sign left out. And as commands: another transceiver, no text or
 * nothing to key ignored without an answer, nothing asked; a comma left in the text kept */
static void cw_text_rules(void) {
    static const struct {
        const char *in, *out;
    } t[] = {
        {"cq^ 5nn~ tu*", "CQ: 5NN, TU;"},
        {"|SK| |va| |BT| |ar| |KN| |SOS| |s k|", "<SK> <SK> = <AR> <KN> <SOS> <SK>"},
        {"|AA| |e|", "AA E"},                               /* .-.- has no sign of its own: the letters */
        {"a|b", "AB"},                                      /* a bar that does not close */
        {">TU <RA6LH >>599 004 |SK|", "\x01TU \x02RA6LH \x01\x01" "599 004 <SK>\x03"},  /* TCI's own example */
        {"#%&{call}", "CALL"},
        {"tab\there\r\n", "TAB HERE  "},
        {"73 \xc3\xa9 @ ? / = !", "73  @ ? / = !"},
        {"   ", ""},
        {"<>", ""},
        {"###", ""},
        {"", ""},
    };
    char   out[CW_QUEUE], big[3000];
    size_t n;
    bool   ok = true;

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        n = cw_text(t[i].in, out, sizeof(out));
        if (strcmp(out, *t[i].out ? t[i].out : out) || n != strlen(t[i].out)) {
            printf("  %-20s -> %s (%zu)\n", t[i].in, out, n);
            ok = false;
        }
    }
    check(ok, "^ ~ * back, |..| one sign or its letters, < > steps and the speed back, capitals, unknown left out");
    memset(big, 'e', sizeof(big) - 1);
    big[0]               = '>';
    big[sizeof(big) - 1] = 0;
    n = cw_text(big, out, sizeof(out));
    check(n == CW_QUEUE - 1 && out[n - 1] == CW_ENCODER_SPEED && !out[n], "a text longer than the queue: cut, the speed back");

    radio_mode(radio_mode_cw);
    client_from(A, LAN_A, "");
    say(A, "cw_macros:1,test;cw_macros:0;cw_macros;cw_macros:0,###;cw_macros:0,  ;cw_msg:0,tu,ra6lh,599;cw_terminal:0,true;");
    check(!*heard(A) && stub_run(-1) == 0 && !stub_ask_shown && cw_owner < 0, "ignored: no answer, nothing asked or keyed");
    client_gone(A);
    client_open(A);
    say(A, "cw_macros:0,a,b,c,d,e;");
    gui(-1);
    check(!strcmp(stub_cw_text, "A,B,C,D,E\n"), "commas left in the text kept");
    stub_cw_end(true);
    gui(-1);
}

/* ---- Over Bluetooth (the VFO-Knob's Bluetooth firmware) ----------------------------- */

#define KNOB "AA:BB:CC:DD:EE:01"

/* A connection BlueZ hands over for the device at addr, taken by the socket thread's round (en: TCI on), then its
 * upgrade request; *bluez a copy of the socket kept as BlueZ keeps one. The place it took, -1 none */
static int bt_connect(const char *addr, bool en, int *bluez) {
    static char resp[65536];    /* the 101 and the greeting after it, all the server wrote: frames read whole later */
    int         sv[2];
    char        req[512];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        exit(2);
    }
    *bluez = dup(sv[0]);
    tci_server_bt_take(sv[0], addr);
    bt_handed(en);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != sv[0])
            continue;
        fcntl(sv[1], F_SETFL, O_NONBLOCK);
        peer[i] = sv[1];
        snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: x6100.local\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                 "User-Agent: VFO-Knob BT/1.0\r\n\r\n");
        if (send(sv[1], req, strlen(req), MSG_NOSIGNAL) != (ssize_t)strlen(req))
            exit(2);
        client_read(i);
        ssize_t n = recv(sv[1], resp, sizeof(resp) - 1, 0);
        return n > 12 && !strncmp(resp, "HTTP/1.1 101", 12) ? i : -1;
    }
    peer[0] = sv[1];    /* for ended() */
    return -1;
}

/* The other end saw the connection end (a shutdown reaches it while BlueZ's copy is still open) */
static bool ended(int fd) {
    char    b[65536];
    ssize_t n;

    fcntl(fd, F_SETFL, O_NONBLOCK);
    while ((n = recv(fd, b, sizeof(b), 0)) > 0) {}
    return n == 0;
}

/* A knob's connection: a client on the network known by its address, "Bluetooth" on the screen, asked before its first
 * key; Always kept by that address, its next connection keys without a question. Silent BT_TX_SILENT_MS while on the
 * air: its transmission ends, the connection stays; silent as long with nothing on the air, it is only pinged. BlueZ
 * asking to let go closes it, and with a shutdown the other end sees that while BlueZ's copy is open. TCI off: a
 * connection is refused the same way */
static void bluetooth(void) {
    int  bluez, ci = bt_connect(KNOB, true, &bluez);
    char buf[256];

    check(ci == A && clients[A].bt && !clients[A].local && !strcmp(clients[A].ip, KNOB) &&
          !strcmp(clients[A].app, "VFO-Knob BT/1.0"), "handed over: a client known by the device's address");
    say(A, "trx:0,true;");
    gui(-1);
    check(strstr(heard(A), "trx:0,false;") && !on_air() && stub_ask_id &&
          !strcmp(stub_ask_who, "Bluetooth " KNOB " (VFO-Knob BT/1.0)"),
          "its first key: refused, the radio asks \"Bluetooth " KNOB " (VFO-Knob BT/1.0) wants to transmit\"");
    answer(TCI_ASK_ALWAYS);
    check(tci_server_allowed(buf, sizeof(buf)) == 1 && !strcmp(buf, KNOB " (VFO-Knob BT/1.0)"),
          "Always: kept by its address");
    say(A, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == A, "its next key: on the air");
    clients[A].last_ms = now_ms() - BT_TX_SILENT_MS + 1000;
    clients_tick(now_ms());
    gui(-1);
    check(on_air() && ptt_owner == A, "silent 4 s: still on the air");
    clients[A].last_ms = now_ms() - BT_TX_SILENT_MS - 1;
    clients_tick(now_ms());
    gui(-1);
    gui(-1);
    check(!on_air() && ptt_owner < 0 && clients[A].fd >= 0, "silent 5 s while on the air: unkeyed, still connected");
    clients[A].last_ms = now_ms() - BT_TX_SILENT_MS - 1;
    clients_tick(now_ms());
    check(clients[A].fd >= 0, "silent with nothing on the air: kept (pinged)");
    clients[A].last_ms = now_ms();
    tci_server_bt_drop(KNOB);
    bt_handed(true);
    check(clients[A].fd < 0 && ended(peer[A]), "BlueZ asks to let go: closed, the other end told (BlueZ's copy open)");
    close(bluez);

    ci = bt_connect(KNOB, true, &bluez);
    say(ci, "trx:0,true;");
    gui(-1);
    check(on_air() && ptt_owner == ci && stub_ask_shown == 1, "back on a new connection: keys, no question");
    tx_session_end(ci, false);      /* the link dropped (client_read: recv failed) */
    client_close(&clients[ci]);
    gui(-1);
    gui(-1);
    check(!on_air() && ended(peer[ci]), "the link gone: unkeyed at once");
    close(bluez);

    ci = bt_connect("AA:BB:CC:DD:EE:02", false, &bluez);
    check(ci < 0 && ended(peer[0]), "TCI off: refused, the other end told");
    close(bluez);
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
        {"TCI audio in DIGU and DIGL", tci_audio_in_dig},
        {"A browser's TCI audio start gives the browser its control back at the end", control_given_back},
        {"Listen only: a client's tuning and key answered with the state", listen_only},
        {"dds answered with the frequency, an unknown command with nothing", dds_and_unknown},
        {"A question (Hamlib's poll) takes no control, a set does", questions_take_no_control},
        {"Over Funnel, an unkey or a question from a client that keyed nothing", funnel_takes_no_control},
        {"Over Funnel, the same browser back from another network", funnel_same_browser_back},
        {"A client on the network asks: Allow once", ask_allow_once},
        {"A client on the network asks: Always, kept across a reconnect and a restart, then forgotten", ask_always},
        {"A client on the network asks: No", ask_no},
        {"A client on the network asks: no answer in time, the client leaving", ask_timeout_and_gone},
        {"Not asked: a program on the radio, a paired browser, the radio on the air, Listen only", ask_not_for},
        {"Asking turned off", ask_off},
        {"CW: a client's text on the radio's keyer, in CW and CW-R", cw_keyed},
        {"CW: refused outside CW and CW-R, with a note", cw_not_cw},
        {"CW: Ask before TX and Listen only", cw_ask_and_listen},
        {"CW: refused where the radio may not transmit; a key and a text during the other", cw_tx_refused},
        {"CW: the keyer shared with the operator's texts", cw_keyer_shared},
        {"CW: stopped by cw_macros_stop, an unkey, the client leaving", cw_stopped},
        {"CW: a stop lost to a full queue sent again", cw_stop_resent},
        {"CW: more text waits and follows; out of CW it stops", cw_more_text},
        {"CW: cw_macros_speed and cw_keyer_speed", cw_speed},
        {"CW: the text's rules, and commands ignored", cw_text_rules},
        {"Over Bluetooth: asked, kept by address, silent while on the air, let go, gone, TCI off", bluetooth},
    };
    static const char *const written[] = {"", ", in capitals", ", in mixed case"};
    int  n = sizeof(cases) / sizeof(cases[0]), bad = 0;
    char name[160];

    for (letters = 0; letters < 3; letters++)
        for (int i = 0; i < n; i++) {
            snprintf(name, sizeof(name), "%s%s", cases[i].name, written[letters]);
            bad += run(name, cases[i].fn);
        }
    if (bad) {
        printf("OWNER FAILED: %d of %d cases\n", bad, 3 * n);
        return 1;
    }
    printf("OWNER OK: %d cases, each as written, in capitals and in mixed case\n", n);
    return 0;
}
