/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the FT8/FT4/FT2 QSO sequence (src/ft8/qso.cpp, #150); see the Makefile.
 *
 *  The slots take turns as on the air: the other stations' (their decodes, then the slot's end, as audio_worker.c
 *  gives them) and ours (the queued message goes out, then the slot ends). The processor is made and fed as
 *  ft8_window.c does it, the TX message is the one ft8_window.c keeps, and the operator's presses do what its
 *  handlers do. The save callback notes what would be logged.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "qso.h"

#define ME     "Q1QQQ"
#define MY_QTH "JO21"

static bool verbose;
static int  cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- The sequence as ft8_window.c runs it ------------------------------------------------------------------- */

struct logged_t {
    std::string call, grid;
    int         rcvd, sent;
    bool        aside;
};

static std::vector<logged_t>    logged;
static std::vector<std::string> sent;       /* our transmissions */
static FTxQsoProcessor         *qso;
static ftx_tx_msg_t             tx;
static int                      retries;
static const char              *pota_call;
static bool                     hound_on;
static const char              *fox;        /* the Fox Hound calls */
static char                     said[200];  /* -v: what was logged, said after the step that logged it */

static void on_saved(const char *call, const char *grid, const int rcvd, const int sent_snr, bool aside) {
    logged.push_back({call, grid, rcvd, sent_snr, aside});
    snprintf(said, sizeof(said), "      logged %s %s rcvd %d sent %d%s\n", call, grid, rcvd, sent_snr,
             aside ? " (aside)" : "");
}

static void say(const char *fmt, ...) {
    va_list ap;

    if (verbose) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        fputs(said, stdout);
    }
    said[0] = '\0';
}

static bool is_pota(const char *call) {
    return pota_call && !strcmp(call, pota_call);
}

static void begin(const char *name, bool auto_on = true, int max_repeats = 6) {
    if (qso) ftx_qso_processor_delete(qso);
    qso = ftx_qso_processor_init(ME, MY_QTH "CV", on_saved, max_repeats);
    ftx_qso_processor_set_is_pota(qso, is_pota);
    ftx_qso_processor_set_auto(qso, auto_on);
    memset(&tx, 0, sizeof(tx));
    logged.clear();
    sent.clear();
    retries   = max_repeats;
    pota_call = NULL;
    hound_on  = false;
    fox       = NULL;
    printf("%s\n", name);
}

/* A decode in the other stations' slot (add_rx_text): the sequence's answer goes to the TX message */
static ftx_msg_meta_t rx(const char *text, int snr = -10) {
    ftx_msg_meta_t meta = {};
    char           was[sizeof(tx.msg)];

    snprintf(was, sizeof(was), "%s", tx.msg);
    ftx_qso_processor_add_rx_text(qso, text, snr, &meta, &tx);
    say("    rx %-36s%s%s\n", text, strcmp(was, tx.msg) ? "next: " : "", strcmp(was, tx.msg) ? tx.msg : "");
    return meta;
}

/* In Hound mode the sequence's answers are not sent, and of its QSOs only one put aside is logged, not the Fox's
 * (add_rx_text drops the others: Hound logs its own) */
static void rx_hound(const char *text, int snr = -10) {
    ftx_msg_meta_t meta = {};
    ftx_tx_msg_t   ignored = {};
    size_t         n = logged.size(), kept = n;

    ftx_qso_processor_add_rx_text(qso, text, snr, &meta, &ignored);
    for (size_t i = n; i < logged.size(); i++)
        if (logged[i].aside && (!fox || logged[i].call != fox)) logged[kept++] = logged[i];
    logged.resize(kept);
    if (kept == n) said[0] = '\0';
    say("    rx %-36s(Hound)\n", text);
}

/* The slot's end: its last decode came before it */
static void slot_end() {
    ftx_qso_processor_start_new_slot(qso);
}

/* The queued message goes out (transmit(): the processor told, one repeat less, none left and it is cleared) */
static void went() {
    if (tx.msg[0]) {
        ftx_qso_processor_sent(qso, tx.msg);
        sent.push_back(tx.msg);
        say("    tx %s\n", tx.msg);
        if (tx.repeats > 0) tx.repeats--;
        if (tx.repeats == 0) tx.msg[0] = '\0';
    }
}

/* Our slot: the queued message goes out, the slot ends */
static void our_slot() {
    went();
    slot_end();
}

/* A row pressed: the list, the For you strip, the browser's reply (list_pressed) */
static void press(const ftx_msg_meta_t &row) {
    ftx_msg_meta_t meta = row;

    ftx_qso_processor_start_qso(qso, &meta, &tx);
    say("    press %s, next: %s\n", row.call_de, tx.msg);
}

/* TX CQ (tx_cq_cb) */
static void tx_cq() {
    snprintf(tx.msg, sizeof(tx.msg), "CQ %s %s", ME, MY_QTH);
    tx.repeats = retries;
    ftx_qso_processor_reset(qso);
    say("    TX CQ\n");
}

/* Force QSO save (force_save_cb) */
static bool force_save() {
    bool saved = ftx_qso_processor_can_save_qso(qso) && ftx_qso_processor_force_save_qso(qso);

    say("    Force QSO save: %s\n", saved ? "saved" : "Can't save incomplete QSO");
    return saved;
}

/* Hound on or off (hound_cb) */
static void hound() {
    hound_on  = !hound_on;
    tx.msg[0] = '\0';
    ftx_qso_processor_set_hound(qso, hound_on);
    say("    Hound %s\n", hound_on ? "on" : "off");
}

static int logs_of(const char *call) {
    int n = 0;
    for (auto &l : logged) n += l.call == call;
    return n;
}

static const logged_t *log_of(const char *call) {
    for (auto &l : logged)
        if (l.call == call) return &l;
    return NULL;
}

/* Logged once, with these reports, aside or not */
static bool logged_once(const char *call, const char *grid, int rcvd, int sent_snr, bool aside) {
    const logged_t *l = log_of(call);
    return logs_of(call) == 1 && l->grid == grid && l->rcvd == rcvd && l->sent == sent_snr && l->aside == aside;
}

static int times_sent(const char *msg) {
    int n = 0;
    for (auto &s : sent) n += s == msg;
    return n;
}

static bool next_is(const char *msg) {
    return !strcmp(tx.msg, msg);
}

/* We answer A's CQ, its report comes: our R-report (sent -10) went out once */
static void answered_to_r_report(int a_snr = -10) {
    ftx_msg_meta_t cq = rx("CQ A1AAA IN80", a_snr);
    slot_end();
    press(cq);
    our_slot();
    rx("Q1QQQ A1AAA -05", a_snr);
    slot_end();
    our_slot();
}

/* ---- What logs today, and must go on logging ---------------------------------------------------------------- */

static void good_paths(void) {
    begin("We call CQ: A answers, its R+report logs it, our RR73, its 73");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80", -10);
    slot_end();
    check(next_is("A1AAA Q1QQQ -10"), "our report to A queued (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA R-12", -11);
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false), "logged at its R+report: rcvd -12, sent -10, not aside");
    check(next_is("A1AAA Q1QQQ RR73"), "RR73 queued (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    our_slot();
    check(logged.size() == 1, "its 73 makes no second entry (%zu)", logged.size());
    check(times_sent("A1AAA Q1QQQ RR73") == 1 && !tx.msg[0], "RR73 sent once, nothing after it");

    begin("We answer A's CQ: its report, our R-report, its RR73 logs it, our 73 once");
    ftx_msg_meta_t cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    check(next_is("A1AAA Q1QQQ JO21"), "our call with the grid cut to 4 queued (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA -05", -10);
    slot_end();
    check(next_is("A1AAA Q1QQQ R-10"), "our R-report queued (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false), "logged at its RR73: rcvd -05, sent -10");
    check(next_is("A1AAA Q1QQQ 73"), "73 queued (%s)", tx.msg);
    our_slot();
    slot_end();
    our_slot();
    check(times_sent("A1AAA Q1QQQ 73") == 1 && !tx.msg[0], "73 sent once, nothing after it");

    begin("We answer A's CQ: its RRR logs it, its 73 no second entry");
    answered_to_r_report();
    rx("Q1QQQ A1AAA RRR");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false), "logged at its RRR");
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged.size() == 1, "its 73 makes no second entry (%zu)", logged.size());

    begin("We answer A's CQ: its RR73 missed, its 73 logs it");
    answered_to_r_report();
    slot_end();     /* its RR73 not decoded */
    our_slot();     /* our R-report again */
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false), "logged at its 73");

    begin("We answer A's CQ: its RR73 to us in a multi-stream message logs it");
    answered_to_r_report();
    rx("Q1QQQ RR73; B1BBB <A1AAA> -07");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false), "logged at the RR73 in \"Q1QQQ RR73; B1BBB <A1AAA> -07\"");
    check(next_is("A1AAA Q1QQQ 73"), "73 queued (%s)", tx.msg);

    begin("We call CQ: A first, B calls in A's slots: A logged, B not");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    rx("Q1QQQ B1BBB JN11");
    slot_end();
    check(next_is("A1AAA Q1QQQ -10"), "A, the first, answered (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ B1BBB JN11");
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false) && logged.size() == 1, "A logged, B not");
    check(next_is("A1AAA Q1QQQ RR73"), "RR73 to A queued (%s)", tx.msg);

    begin("We answer A's CQ: B calls us in between, A's RR73 logs A");
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    rx("Q1QQQ B1BBB JN11");
    rx("Q1QQQ A1AAA -05");
    slot_end();
    check(next_is("A1AAA Q1QQQ R-10"), "our R-report to A queued, not B's answer (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ B1BBB JN11");
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false) && logged.size() == 1, "A logged, B not");

    begin("We call CQ: a stray 73 from B logs nothing, A's R+report logs A");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB 73");
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false) && logged.size() == 1, "A logged, B not");

    begin("Auto off: the operator presses each row, A's RR73 logs it", false);
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    ftx_msg_meta_t report = rx("Q1QQQ A1AAA -05");
    slot_end();
    check(next_is("A1AAA Q1QQQ JO21"), "Auto off: the message stays (%s)", tx.msg);
    press(report);
    check(next_is("A1AAA Q1QQQ R-10"), "the report pressed: our R-report (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false), "logged at its RR73");

    begin("We call CQ: A and a POTA activator call in one slot, the activator first");
    pota_call = "P1POT";
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    rx("Q1QQQ P1POT JN11", -14);
    slot_end();
    check(next_is("P1POT Q1QQQ -14"), "the activator answered (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ P1POT R-08");
    slot_end();
    check(logged_once("P1POT", "JN11", -8, -14, false) && logged.size() == 1, "the activator logged, A not");
}

/* ---- Never a QSO that was not complete ---------------------------------------------------------------------- */

static void incomplete(void) {
    begin("We answer A's CQ, move on to B before A's report: A's RR73 logs nothing");
    ftx_msg_meta_t cq = rx("CQ A1AAA IN80");
    ftx_msg_meta_t b  = rx("CQ B1BBB JN11");
    slot_end();
    press(cq);
    our_slot();
    press(b);
    rx("Q1QQQ A1AAA RR73");
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged.empty(), "nothing logged (%zu)", logged.size());

    begin("We answer A's CQ, its report comes, B pressed before our R-report went: A's 73 logs nothing");
    cq = rx("CQ A1AAA IN80");
    b  = rx("CQ B1BBB JN11");
    slot_end();
    press(cq);
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    check(next_is("A1AAA Q1QQQ R-10"), "our R-report to A made (%s)", tx.msg);
    press(b);
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged.empty() && !times_sent("A1AAA Q1QQQ R-10"), "nothing logged (%zu)", logged.size());

    begin("We call CQ, A answers: its RR73 or 73 without its R+report logs nothing");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged.empty(), "nothing logged (%zu)", logged.size());

    begin("We call CQ, A answers, we move on to B: A's 73 logs nothing");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    b = rx("Q1QQQ B1BBB JN11");
    slot_end();
    our_slot();
    press(b);
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged.empty(), "nothing logged (%zu)", logged.size());

    begin("We call CQ, B called first and is answered: A's R+report with no report from us logs nothing");
    tx_cq();
    our_slot();
    rx("Q1QQQ B1BBB JN11");
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    check(next_is("B1BBB Q1QQQ -10"), "B answered (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged.empty(), "nothing logged (%zu)", logged.size());
}

/* ---- #150: the operator moves on before the station's last message ------------------------------------------ */

static void moved_on(void) {
    begin("#150: reports exchanged with A, the operator answers B, then A's RR73 comes");
    ftx_msg_meta_t cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    ftx_msg_meta_t b = rx("Q1QQQ B1BBB JN11", -15);
    rx("Q1QQQ A1AAA -05");
    slot_end();
    our_slot();
    press(b);
    check(next_is("B1BBB Q1QQQ -15"), "B answered (%s)", tx.msg);
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged once at its late RR73, rcvd -05 sent -10, aside");
    check(next_is("B1BBB Q1QQQ -15"), "B's QSO goes on: no 73 to A (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    rx("Q1QQQ B1BBB R-09");
    slot_end();
    check(logs_of("A1AAA") == 1, "A's RR73 again: no second entry (%d)", logs_of("A1AAA"));
    check(logged_once("B1BBB", "JN11", -9, -15, false), "B logged at its R+report");

    begin("#150: the same, A's RRR then its 73");
    answered_to_r_report();
    b = rx("CQ B1BBB JN11");
    press(b);
    rx("Q1QQQ A1AAA RRR");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged once, at its RRR");

    begin("#150: the same, A's RR73 missed, its 73 comes");
    answered_to_r_report();
    b = rx("CQ B1BBB JN11");
    press(b);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged once, at its 73");

    begin("#150: we answer A's CQ, our R-report runs out of retries, then A's RR73 comes");
    answered_to_r_report();
    for (int i = 0; i < 7; i++) {
        slot_end();
        our_slot();
    }
    check(!ftx_qso_processor_has_current(qso) && !tx.msg[0] && times_sent("A1AAA Q1QQQ R-10") == 6,
          "given up after 6 R-reports, no QSO current");
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged once at its late RR73");

    begin("#150: reports exchanged with A, the operator presses TX CQ, then A's RR73 comes");
    answered_to_r_report();
    tx_cq();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged once at its late RR73");
    check(next_is("CQ Q1QQQ JO21"), "the CQ goes on (%s)", tx.msg);

    begin("#150: we call CQ, report to A, the operator answers B, then A's R+report comes");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    b = rx("Q1QQQ B1BBB JN11", -15);
    slot_end();
    our_slot();
    press(b);
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, true), "A logged once at its R+report, rcvd -12 sent -10");
    check(next_is("B1BBB Q1QQQ -15"), "no RR73 to A, B's QSO goes on (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logs_of("A1AAA") == 1, "its R+report again: no second entry (%d)", logs_of("A1AAA"));

    begin("#150: we call CQ, report to A, the operator presses TX CQ, then A's R+report comes: our RR73");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    tx_cq();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false), "A logged once at its R+report, rcvd -12 sent -10");
    check(next_is("A1AAA Q1QQQ RR73") && ftx_qso_processor_has_current(qso), "RR73 to A, the CQ ends (%s)",
          tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(times_sent("A1AAA Q1QQQ RR73") == 1 && logs_of("A1AAA") == 1, "RR73 sent once, its 73 no second entry");

    begin("#150: we call CQ, our report to A runs out of retries, then A's R+report comes: our RR73");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    for (int i = 0; i < 7; i++) {
        slot_end();
        our_slot();
    }
    check(!ftx_qso_processor_has_current(qso) && times_sent("A1AAA Q1QQQ -10") == 6, "given up after 6 reports");
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false) && next_is("A1AAA Q1QQQ RR73"), "A logged, RR73 to A (%s)",
          tx.msg);

    begin("#150: TX CQ after our report to A, B's call then A's R+report in one slot: our RR73 to A first");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    tx_cq();
    rx("Q1QQQ B1BBB JN11", -15);
    rx("Q1QQQ A1AAA R-12");
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1, "RR73 to A, A logged (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered at its next call (%s)", tx.msg);

    begin("#150: TX CQ after our report to A, A's R+report then B's call in one slot: our RR73 to A first");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    tx_cq();
    rx("Q1QQQ A1AAA R-12");
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1, "RR73 to A, A logged (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered at its next call (%s)", tx.msg);
}

/* ---- The QSOs put aside: how long, how many, once each ------------------------------------------------------ */

static void aside(void) {
    begin("Put aside 4 slots: A's RR73 then still logs it");
    answered_to_r_report();
    tx_cq();
    slot_end();
    our_slot();
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "logged");

    begin("Put aside 5 slots: A's RR73 then logs nothing");
    answered_to_r_report();
    tx_cq();
    slot_end();
    our_slot();
    slot_end();
    our_slot();
    slot_end();
    rx("Q1QQQ A1AAA RR73");
    check(logged.empty(), "nothing logged (%zu)", logged.size());

    begin("A logged and put aside: a new call from A is answered, never logged again");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    check(next_is("A1AAA Q1QQQ -10"), "A answered again (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(next_is("A1AAA Q1QQQ RR73") && logged.size() == 1, "RR73 to A, still one entry (%zu)", logged.size());

    begin("A logged aside: the operator presses its rows, never logged again");
    answered_to_r_report();
    ftx_msg_meta_t b = rx("CQ B1BBB JN11");
    press(b);
    ftx_msg_meta_t rr73 = rx("Q1QQQ A1AAA RR73");
    slot_end();
    press(rr73);
    check(next_is("A1AAA Q1QQQ 73"), "its RR73 pressed: our 73 (%s)", tx.msg);
    our_slot();
    ftx_msg_meta_t report = rx("Q1QQQ A1AAA -05");
    press(report);
    slot_end();
    check(next_is("A1AAA Q1QQQ R-10"), "its report pressed: our R-report (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logs_of("A1AAA") == 1, "one entry (%d)", logs_of("A1AAA"));

    begin("Five QSOs put aside: the oldest goes, the others' RR73 still logs them");
    static const char *calls[] = {"A1AAA", "B1BBB", "C1CCC", "D1DDD", "E1EEE"};
    char text[40];
    for (auto call : calls) {      /* each to our R-report, the slots' ends left out (none expires) */
        snprintf(text, sizeof(text), "CQ %s IN80", call);
        ftx_msg_meta_t m = rx(text);
        press(m);
        went();
        snprintf(text, sizeof(text), "Q1QQQ %s -05", call);
        rx(text);
        went();
    }
    tx_cq();     /* E1EEE aside too: A1AAA, the oldest of five, goes */
    rx("Q1QQQ A1AAA RR73");
    rx("Q1QQQ B1BBB RR73");
    rx("Q1QQQ E1EEE RR73");
    check(logged.size() == 2 && logs_of("B1BBB") == 1 && logs_of("E1EEE") == 1, "B and E logged, A not (%zu)",
          logged.size());

    /* B logged, then C, D and E each worked to our R-report, as if in a slot of their own (the slots' ends left
     * out, so that none put aside expires): B is the oldest of four put aside */
    begin("B logged, the oldest of four put aside: a press on its CQ takes the same QSO up, no second entry");
    static const char *cde[] = {"C1CCC", "D1DDD", "E1EEE"};
    b = rx("CQ B1BBB JN11");
    press(b);
    went();
    rx("Q1QQQ B1BBB -05");
    went();
    rx("Q1QQQ B1BBB RR73");
    for (auto call : cde) {
        snprintf(text, sizeof(text), "CQ %s IN80", call);
        ftx_msg_meta_t m = rx(text);
        press(m);
        went();
        snprintf(text, sizeof(text), "Q1QQQ %s -05", call);
        rx(text);
        went();
    }
    ftx_msg_meta_t a = rx("CQ A1AAA IN80");
    press(a);
    went();
    rx("Q1QQQ A1AAA -05");
    went();
    press(b);
    check(next_is("B1BBB Q1QQQ JO21"), "our call to B (%s)", tx.msg);
    went();
    rx("Q1QQQ B1BBB -05");
    went();
    rx("Q1QQQ B1BBB RR73");
    rx("Q1QQQ A1AAA RR73");
    rx("Q1QQQ C1CCC RR73");
    check(logs_of("B1BBB") == 1 && logs_of("A1AAA") == 1 && logs_of("C1CCC") == 1,
          "B once, A and C at their RR73 (B %d, A %d, C %d)", logs_of("B1BBB"), logs_of("A1AAA"), logs_of("C1CCC"));

    begin("B logged, the oldest of four put aside: B calls once our RR73 to A went, no second entry");
    b = rx("CQ B1BBB JN11");
    press(b);
    went();
    rx("Q1QQQ B1BBB -05");
    went();
    rx("Q1QQQ B1BBB RR73");
    for (auto call : cde) {
        snprintf(text, sizeof(text), "CQ %s IN80", call);
        ftx_msg_meta_t m = rx(text);
        press(m);
        went();
        snprintf(text, sizeof(text), "Q1QQQ %s -05", call);
        rx(text);
        went();
    }
    tx_cq();
    went();
    rx("Q1QQQ A1AAA IN80");
    went();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB R-03");
    slot_end();
    check(logs_of("B1BBB") == 1 && logged_once("A1AAA", "IN80", -12, -10, false),
          "B once (%d), A once", logs_of("B1BBB"));
    check(next_is("B1BBB Q1QQQ RR73"), "RR73 to B (%s)", tx.msg);

    begin("A station waiting as the next is pressed: one QSO for it");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    b = rx("Q1QQQ B1BBB JN11", -15);
    press(b);
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB R-09");
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB 73");
    slot_end();
    check(logged_once("B1BBB", "JN11", -9, -15, false), "B logged once");
    check(!tx.msg[0], "its 73 queues nothing more for B (%s)", tx.msg);

    begin("Hound on after our R-report to A: A's late RR73 logs it once, the Fox's QSO is Hound's alone");
    answered_to_r_report();
    hound();
    fox = "F1FOX";
    rx_hound("Q1QQQ F1FOX -08");
    rx_hound("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged once at its late RR73");
    rx_hound("Q1QQQ RR73; B1BBB <F1FOX> -07");
    slot_end();
    hound();
    rx("Q1QQQ A1AAA RR73");
    rx("Q1QQQ RR73; B1BBB <F1FOX> -07");
    slot_end();
    check(logged.size() == 1 && !logs_of("F1FOX"), "nothing more, after Hound off too (%zu)", logged.size());

    begin("Hound on after our report to A (we called CQ): A's late R+report logs it, no RR73 (Hound sends)");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    hound();
    rx_hound("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, true) && !ftx_qso_processor_has_current(qso),
          "A logged once, not taken up");

    begin("Hound mode: a station that sends us a report and an RR73 logs nothing, then or after Hound off");
    hound();
    rx_hound("Q1QQQ C1CCC -05");
    slot_end();
    slot_end();
    rx_hound("Q1QQQ C1CCC RR73");
    slot_end();
    hound();
    rx("Q1QQQ C1CCC RR73");
    check(logged.empty(), "nothing logged (%zu)", logged.size());
}

/* ---- A new caller after our last message: answered at once ------------------------------------------------- */

/* We call CQ, A answers, its R+report logs it, our RR73 goes out */
static void cq_to_rr73(void) {
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    our_slot();
}

static void new_caller(void) {
    begin("We call CQ, our RR73 to A went, no 73: B's first call is answered");
    cq_to_rr73();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered at its first call (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA 73");
    rx("Q1QQQ B1BBB R-03");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false) && logged_once("B1BBB", "JN11", -3, -15, false),
          "A logged once, B at its R+report, A's late 73 no second entry");
    check(next_is("B1BBB Q1QQQ RR73"), "RR73 to B (%s)", tx.msg);

    begin("We answer A's CQ, our 73 to its RR73 went: B's first call is answered");
    answered_to_r_report();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered at its first call (%s)", tx.msg);
    check(logged_once("A1AAA", "IN80", -5, -10, false), "A logged once");

    begin("B calls in the slot of A's R+report: our RR73 to A first, B answered after it");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(next_is("A1AAA Q1QQQ RR73"), "RR73 to A (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered at its next call (%s)", tx.msg);

    begin("After our RR73, B's call then A's R+report again in one slot: A gets its RR73 again");
    cq_to_rr73();
    rx("Q1QQQ B1BBB JN11", -15);
    rx("Q1QQQ A1AAA R-12");
    check(next_is("A1AAA Q1QQQ RR73"), "RR73 to A again (%s)", tx.msg);
    slot_end();
    our_slot();
    check(times_sent("A1AAA Q1QQQ RR73") == 2 && logs_of("A1AAA") == 1, "RR73 sent twice, A logged once");
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered at its next call (%s)", tx.msg);

    begin("After our RR73, A's R+report again then B's call in one slot: A gets its RR73 again");
    cq_to_rr73();
    rx("Q1QQQ A1AAA R-12");
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("A1AAA Q1QQQ RR73"), "RR73 to A again (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15") && logs_of("A1AAA") == 1, "B answered at its next call, A logged once");

    begin("After our RR73 B's call is taken, the operator presses C: A's R+report again stays aside");
    cq_to_rr73();
    rx("Q1QQQ B1BBB JN11", -15);
    ftx_msg_meta_t c = rx("CQ C1CCC JN22");
    press(c);
    rx("Q1QQQ A1AAA R-12");
    check(next_is("C1CCC Q1QQQ JO21") && logs_of("A1AAA") == 1, "the operator's C goes on, A logged once (%s)",
          tx.msg);

    begin("A logged, the operator calls A again from its CQ: its answer missed once, B's call waits");
    cq_to_rr73();
    tx_cq();
    our_slot();
    ftx_msg_meta_t acq = rx("CQ A1AAA IN80");
    slot_end();
    press(acq);
    check(next_is("A1AAA Q1QQQ JO21"), "our call to A (%s)", tx.msg);
    our_slot();
    slot_end();     /* its answer not decoded */
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("A1AAA Q1QQQ JO21"), "the operator's call to A goes on (%s)", tx.msg);

    begin("A logged aside, the operator presses its report: its RR73 missed once, B's call waits");
    answered_to_r_report();
    c = rx("CQ C1CCC JN22");
    press(c);
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    our_slot();
    ftx_msg_meta_t report = rx("Q1QQQ A1AAA -05");
    slot_end();
    press(report);
    check(next_is("A1AAA Q1QQQ R-10") && logs_of("A1AAA") == 1, "our R-report to A (%s)", tx.msg);
    our_slot();
    slot_end();     /* its RR73 not decoded */
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("A1AAA Q1QQQ R-10"), "our R-report to A goes on (%s)", tx.msg);

    begin("A logged, TX CQ, A calls again: its R+report missed once, B's call waits, then A's RR73");
    cq_to_rr73();
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    check(next_is("A1AAA Q1QQQ -10"), "our report to A (%s)", tx.msg);
    our_slot();
    slot_end();     /* its R+report not decoded */
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("A1AAA Q1QQQ -10"), "our report to A goes on (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1, "RR73 to A, one entry (%s)", tx.msg);

    begin("Auto off, after our RR73 B calls: nothing sent by itself, B's row answers it", false);
    tx_cq();
    our_slot();
    ftx_msg_meta_t a = rx("Q1QQQ A1AAA IN80");
    slot_end();
    press(a);
    our_slot();
    ftx_msg_meta_t r = rx("Q1QQQ A1AAA R-12");
    slot_end();
    press(r);
    our_slot();
    ftx_msg_meta_t b = rx("Q1QQQ B1BBB JN11", -15);
    check(!tx.msg[0], "nothing queued (%s)", tx.msg);
    press(b);
    check(next_is("B1BBB Q1QQQ -15") && logged_once("A1AAA", "IN80", -12, -10, false), "B's row: our report to B");
}

/* ---- Force QSO save ------------------------------------------------------------------------------------------ */

static void force(void) {
    begin("Force QSO save after our R-report: logged, the TX message stays, A's RR73 gets our 73");
    answered_to_r_report();
    slot_end();     /* its RR73 missed */
    check(force_save() && logged_once("A1AAA", "IN80", -5, -10, false), "logged: rcvd -05, sent -10");
    check(next_is("A1AAA Q1QQQ R-10") && ftx_qso_processor_has_current(qso), "the R-report stays queued (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(next_is("A1AAA Q1QQQ 73") && logs_of("A1AAA") == 1, "73 to A, one entry (%s)", tx.msg);
    our_slot();
    check(!tx.msg[0], "nothing more queued (%s)", tx.msg);

    begin("Force QSO save, then A's report again: our R-report again, never a second entry");
    answered_to_r_report();
    slot_end();
    force_save();
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    check(next_is("A1AAA Q1QQQ R-10"), "our R-report to A (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logs_of("A1AAA") == 1 && next_is("A1AAA Q1QQQ 73"), "one entry, 73 to A (%s)", tx.msg);

    begin("Force QSO save, A's report again, its RR73: our 73, then B's first call is answered");
    answered_to_r_report();
    slot_end();
    force_save();
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    our_slot();
    check(times_sent("A1AAA Q1QQQ 73") == 1 && logs_of("A1AAA") == 1, "73 to A went, one entry");
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered at its first call (%s)", tx.msg);

    begin("Force QSO save, A quiet, then B calls: B answered");
    answered_to_r_report();
    slot_end();
    force_save();
    our_slot();
    rx("Q1QQQ B1BBB JN11", -15);
    check(next_is("B1BBB Q1QQQ -15"), "B answered (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    rx("Q1QQQ B1BBB R-03");
    slot_end();
    check(logs_of("A1AAA") == 1 && logged_once("B1BBB", "JN11", -3, -15, false), "A once, B logged");

    begin("Force QSO save with no report from A: refused, nothing logged");
    ftx_msg_meta_t cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    check(!force_save() && logged.empty(), "refused");

    begin("Force QSO save twice: refused the second time, one entry");
    answered_to_r_report();
    force_save();
    check(!force_save() && logs_of("A1AAA") == 1, "one entry (%d)", logs_of("A1AAA"));
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    good_paths();
    incomplete();
    moved_on();
    aside();
    new_caller();
    force();
    ftx_qso_processor_delete(qso);
    printf("FT8 QSO %s: %d of %d cases\n", good == cases ? "OK" : "FAILED", good, cases);
    return good == cases ? 0 : 1;
}
