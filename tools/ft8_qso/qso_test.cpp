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

/* me_std: our call as ft8_window.c finds it (msg_encode.c ftx_call_is_standard); qth: the QTH setting, as typed */
static void begin(const char *name, bool auto_on = true, int max_repeats = 6, const char *me = ME,
                  bool me_std = true, const char *qth = MY_QTH "CV") {
    if (qso) ftx_qso_processor_delete(qso);
    qso = ftx_qso_processor_init(me, qth, on_saved, max_repeats);
    ftx_qso_processor_set_is_pota(qso, is_pota);
    ftx_qso_processor_set_auto(qso, auto_on);
    ftx_qso_processor_set_local_standard(qso, me_std);
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

/* ---- The report logged: the one of ours that went out -------------------------------------------------------- */

static void report_out(void) {
    begin("Our report to A out, A's grid again: the new report goes, A's R+report logs that one");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80", -10);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA IN80", -14);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -14, false), "logged with sent -14");

    begin("Our report to A out, A's grid again, TX CQ before the new one went: A's R+report logs the one out");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80", -10);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA IN80", -14);
    tx_cq();
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false) && !times_sent("A1AAA Q1QQQ -14"), "logged with sent -10");

    begin("Our R-report to A out, A's report again, B pressed before the new one went: A's RR73 logs the one out");
    answered_to_r_report();
    rx("Q1QQQ A1AAA -05", -14);
    ftx_msg_meta_t b = rx("CQ B1BBB JN11");
    press(b);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true) && !times_sent("A1AAA Q1QQQ R-14"), "logged with sent -10");
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

    begin("A put aside 4 slots, its late 73 logs it: its CQ pressed 3 slots after the 73, no second entry");
    answered_to_r_report();
    b = rx("CQ B1BBB JN11");
    press(b);
    slot_end();
    our_slot();
    slot_end();     /* its RR73 not decoded */
    our_slot();
    rx("Q1QQQ A1AAA 73");
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged at its 73");
    slot_end();
    our_slot();
    ftx_msg_meta_t acq = rx("CQ A1AAA IN80");
    slot_end();
    press(acq);
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logs_of("A1AAA") == 1 && next_is("A1AAA Q1QQQ 73"), "one entry (%d), 73 to A (%s)", logs_of("A1AAA"),
          tx.msg);

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

    begin("Hound on after our report to A (we called CQ), A calls again in Hound mode: its R+report logs it once");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    slot_end();
    our_slot();
    hound();
    fox = "F1FOX";
    rx_hound("Q1QQQ A1AAA IN80", -14);
    slot_end();
    slot_end();
    rx_hound("Q1QQQ A1AAA R-12");
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, true), "A logged once, sent -10 (the report that went out)");
    hound();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logs_of("A1AAA") == 1 && next_is("A1AAA Q1QQQ RR73"), "Hound off, its R+report again: RR73 to A (%s)",
          tx.msg);

    begin("Hound on after our R-report to A, A's report again in Hound mode, its RR73: logged once");
    answered_to_r_report();
    hound();
    fox = "F1FOX";
    rx_hound("Q1QQQ A1AAA -05", -14);
    slot_end();
    slot_end();
    rx_hound("Q1QQQ A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged once, sent -10");

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

/* We call CQ, A and B answer, our report to A goes out; the operator presses B's row (A put aside), B's R+report
 * logs B and our RR73 to B goes: B's QSO is over */
static void b_over_a_aside(void) {
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80");
    ftx_msg_meta_t b = rx("Q1QQQ B1BBB JN11", -15);
    slot_end();
    our_slot();
    press(b);
    slot_end();
    our_slot();
    rx("Q1QQQ B1BBB R-03");
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

    begin("B's QSO over, A's late R+report: A logged, our RR73 to A at once");
    b_over_a_aside();
    rx("Q1QQQ A1AAA R-12");
    check(logged_once("A1AAA", "IN80", -12, -10, false) && next_is("A1AAA Q1QQQ RR73"), "A logged, RR73 to A (%s)",
          tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(times_sent("A1AAA Q1QQQ RR73") == 1 && logs_of("A1AAA") == 1 && logs_of("B1BBB") == 1,
          "RR73 to A once, A and B once each");

    begin("B's QSO over, A's late R+report then C's call in one slot: RR73 to A, C answered at its next call");
    b_over_a_aside();
    rx("Q1QQQ A1AAA R-12");
    rx("Q1QQQ C1CCC JN22", -9);
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1, "RR73 to A, A logged (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ C1CCC JN22", -9);
    check(next_is("C1CCC Q1QQQ -09"), "C answered (%s)", tx.msg);

    begin("B's QSO over, C's call then A's late R+report in one slot: RR73 to A, C answered at its next call");
    b_over_a_aside();
    rx("Q1QQQ C1CCC JN22", -9);
    rx("Q1QQQ A1AAA R-12");
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1, "RR73 to A, A logged (%s)", tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ C1CCC JN22", -9);
    check(next_is("C1CCC Q1QQQ -09"), "C answered (%s)", tx.msg);

    begin("B's QSO over, B's 73 then A's late R+report in one slot: RR73 to A");
    b_over_a_aside();
    rx("Q1QQQ B1BBB 73");
    rx("Q1QQQ A1AAA R-12");
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1 && logs_of("B1BBB") == 1, "RR73 to A (%s)", tx.msg);

    begin("B's QSO over, A's late R+report then B's 73 in one slot: RR73 to A");
    b_over_a_aside();
    rx("Q1QQQ A1AAA R-12");
    rx("Q1QQQ B1BBB 73");
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1 && logs_of("B1BBB") == 1, "RR73 to A (%s)", tx.msg);

    begin("B's QSO over, A's late R+report then B's R+report again in one slot: B's RR73 again first, then A's");
    b_over_a_aside();
    rx("Q1QQQ A1AAA R-12");
    rx("Q1QQQ B1BBB R-03");
    check(next_is("B1BBB Q1QQQ RR73") && logs_of("A1AAA") == 1 && logs_of("B1BBB") == 1, "RR73 to B again (%s)",
          tx.msg);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    check(next_is("A1AAA Q1QQQ RR73") && logs_of("A1AAA") == 1, "A's R+report again: RR73 to A (%s)", tx.msg);

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

    /* Logged before our R-report went: kept aside all the same, its station's report again goes on with it */
    begin("Force QSO save before our R-report went, TX CQ, A's report again: our R-report, its RR73 no second entry");
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    rx("Q1QQQ A1AAA -05");
    check(force_save() && logged_once("A1AAA", "IN80", -5, -10, false), "logged: rcvd -05, sent -10");
    tx_cq();
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    check(next_is("A1AAA Q1QQQ R-10"), "our R-report to A (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logs_of("A1AAA") == 1 && next_is("A1AAA Q1QQQ 73"), "one entry (%d), 73 to A (%s)", logs_of("A1AAA"),
          tx.msg);

    begin("Force QSO save before our R-report went, Hound on and off, A's report again: its RR73 no second entry");
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    rx("Q1QQQ A1AAA -05");
    force_save();
    hound();
    slot_end();
    hound();
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logs_of("A1AAA") == 1 && next_is("A1AAA Q1QQQ 73"), "one entry (%d), 73 to A (%s)", logs_of("A1AAA"),
          tx.msg);

    begin("Force QSO save, B's CQ pressed before our R-report went, A repeats while B's QSO runs: one entry for A");
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    rx("Q1QQQ A1AAA -05");
    ftx_msg_meta_t b = rx("CQ B1BBB JN11");
    force_save();
    press(b);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA -05");
    rx("Q1QQQ B1BBB -03");
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA -05");
    rx("Q1QQQ B1BBB RR73");
    slot_end();
    our_slot();     /* our 73 to B */
    rx("Q1QQQ A1AAA -05");
    slot_end();
    check(next_is("A1AAA Q1QQQ R-10"), "B's QSO over: our R-report to A (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logs_of("A1AAA") == 1 && logged_once("B1BBB", "JN11", -3, -10, false), "A once (%d), B logged",
          logs_of("A1AAA"));

    begin("Force QSO save, B's CQ pressed before our R-report went, then A's report row: one entry for A");
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    ftx_msg_meta_t report = rx("Q1QQQ A1AAA -05");
    force_save();
    b = rx("CQ B1BBB JN11");
    press(b);
    slot_end();
    our_slot();
    rx("Q1QQQ A1AAA -05");
    slot_end();
    press(report);
    check(next_is("A1AAA Q1QQQ R-10"), "our R-report to A (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA RR73");
    slot_end();
    check(logs_of("A1AAA") == 1 && next_is("A1AAA Q1QQQ 73"), "one entry (%d), 73 to A (%s)", logs_of("A1AAA"),
          tx.msg);

    begin("A's 73 crosses our R-report (logged before it went), A calls again: one entry for A");
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    rx("Q1QQQ A1AAA -05");
    rx("Q1QQQ A1AAA 73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false) && !ftx_qso_processor_has_current(qso), "A logged at its 73");
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80", -14);
    slot_end();
    check(next_is("A1AAA Q1QQQ -14"), "A answered (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA R-12");
    slot_end();
    check(logs_of("A1AAA") == 1 && next_is("A1AAA Q1QQQ RR73"), "one entry (%d), RR73 to A (%s)", logs_of("A1AAA"),
          tx.msg);
}

/* ---- A non-standard call of ours (SP2KMO's upstream d10ae10), or of the other station ------------------------- */

/* KF0QGT/6Y, PA/ON6URE: as WSJT-X, the answers without a report are type 4 with our call in plain text ("<A1AAA>
 * KF0QGT/6Y"), the reports type 1 with our call hashed (the text "A1AAA KF0QGT/6Y -10", on the air "A1AAA
 * <KF0QGT/6Y> -10"). The other station's messages come with our call in brackets, from the hash table worker.c keeps
 * it in */
#define LONG "KF0QGT/6Y"

static void own_call(void) {
    begin("A non-standard call of ours answers A's CQ: our call in plain text, the reports as usual, A's RR73 logs it",
          true, 6, LONG, false);
    ftx_msg_meta_t cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    check(next_is("<A1AAA> " LONG), "type 4 answer, no grid (%s)", tx.msg);
    our_slot();
    ftx_msg_meta_t m = rx("<" LONG "> A1AAA -05", -10);
    check(m.to_me, "\"<" LONG "> A1AAA -05\" is to us");
    slot_end();
    check(next_is("A1AAA " LONG " R-10"), "our R-report, type 1 (%s)", tx.msg);
    our_slot();
    rx("<" LONG "> A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false), "logged at its RR73: rcvd -05, sent -10");
    check(next_is("<A1AAA> " LONG " 73"), "type 4 73 (%s)", tx.msg);
    our_slot();
    slot_end();
    our_slot();
    check(times_sent("<A1AAA> " LONG " 73") == 1 && !tx.msg[0], "73 sent once, nothing after it");

    begin("A non-standard call of ours calls CQ: A answers, our report, its R+report logs it, our RR73 type 4", true, 6,
          LONG, false);
    snprintf(tx.msg, sizeof(tx.msg), "CQ %s", LONG);    /* cq_make_message: no grid, no modifier with this call */
    tx.repeats = retries;
    ftx_qso_processor_reset(qso);
    our_slot();
    rx("<" LONG "> A1AAA IN80", -10);
    slot_end();
    check(next_is("A1AAA " LONG " -10"), "our report to A, type 1 (%s)", tx.msg);
    our_slot();
    rx("<" LONG "> A1AAA R-12", -11);
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false), "logged at its R+report: rcvd -12, sent -10");
    check(next_is("<A1AAA> " LONG " RR73"), "type 4 RR73 (%s)", tx.msg);

    begin("A non-standard call of ours: our R-report out, B's CQ pressed, A's late RR73 logs A aside", true, 6, LONG,
          false);
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    rx("<" LONG "> A1AAA -05", -10);
    slot_end();
    our_slot();
    ftx_msg_meta_t cq_b = rx("CQ B1BBB JN11");
    slot_end();
    press(cq_b);
    check(next_is("<B1BBB> " LONG), "B answered, type 4 (%s)", tx.msg);
    our_slot();
    rx("<" LONG "> A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, true), "A logged aside at its late RR73");

    begin("A non-standard call of ours not in the table: \"<...> A1AAA -05\" is not to us, nothing queued", true, 6,
          LONG, false);
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    m = rx("<...> A1AAA -05");
    slot_end();
    check(!m.to_me && next_is("<A1AAA> " LONG), "not to us, our call again (%s)", tx.msg);

    begin("Our call with /P is standard: the answer keeps the grid", true, 6, "Q1QQQ/P", true);
    cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    check(next_is("A1AAA Q1QQQ/P JO21"), "type 1 with the grid (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ/P A1AAA R-05");
    slot_end();
    check(next_is("A1AAA Q1QQQ/P RR73"), "type 1 RR73 (%s)", tx.msg);

    /* A compound call answers our CQ as WSJT-X (and our own firmware) sends it: type 4, its call in plain text, ours
     * from its hash, no grid. Our report goes type 1 with its call hashed ("<PA/A1AAA> Q1QQQ -10" on the air) */
    begin("A compound call answers our CQ with \"<Q1QQQ> PA/A1AAA\": our report, its R+report logs it, our RR73");
    tx_cq();
    our_slot();
    m = rx("<" ME "> PA/A1AAA", -10);
    check(m.to_me && m.type == FTX_MSG_TYPE_GRID && !strcmp(m.call_de, "PA/A1AAA") && !m.grid[0],
          "to us, a call without a grid from PA/A1AAA (type %d, '%s')", m.type, m.call_de);
    slot_end();
    check(next_is("PA/A1AAA " ME " -10"), "our report (%s)", tx.msg);
    our_slot();
    rx(ME " <PA/A1AAA> R-12", -11);
    slot_end();
    check(logged_once("PA/A1AAA", "", -12, -10, false), "logged at its R+report: no grid, rcvd -12, sent -10");
    check(next_is("PA/A1AAA " ME " RR73"), "RR73 (%s)", tx.msg);

    begin("Auto off: the same call pressed in the list gets our report", false);
    tx_cq();
    our_slot();
    m = rx("<" ME "> PA/A1AAA", -8);
    slot_end();
    check(next_is("CQ " ME " " MY_QTH), "nothing answered by itself (%s)", tx.msg);
    press(m);
    check(next_is("PA/A1AAA " ME " -08"), "our report (%s)", tx.msg);

    begin("Two words that are not a call to us: free text, another station's, ours unknown");
    m = rx(ME " TNX");
    check(!m.to_me && m.type == FXT_MSG_TYPE_OTHER, "free text \"" ME " TNX\" is no call (type %d)", m.type);
    m = rx("<B1BBB> PA/A1AAA");
    check(!m.to_me && !tx.msg[0], "\"<B1BBB> PA/A1AAA\" is not to us, nothing queued");
    m = rx("<...> PA/A1AAA");
    check(!m.to_me && !tx.msg[0], "\"<...> PA/A1AAA\" is not to us, nothing queued");
}

/* ---- The QTH in our answers ------------------------------------------------------------------------------------ */

/* The settings take a locator in either case and of 2 characters; the answer carries its square in upper case, or
 * none: "A1AAA Q1QQQ jo21" went out as the report "A1AAA Q1QQQ +00" */
static void own_qth(void) {
    const char *typed[] = {"jo21cv", "Jo21", "JO21CV", "JO", ""};
    const char *answer[] = {"A1AAA " ME " JO21", "A1AAA " ME " JO21", "A1AAA " ME " JO21", "A1AAA " ME,
                            "A1AAA " ME};

    for (size_t i = 0; i < sizeof(typed) / sizeof(typed[0]); i++) {
        char name[80];

        snprintf(name, sizeof(name), "QTH \"%s\": the answer to A's CQ", typed[i]);
        begin(name, true, 6, ME, true, typed[i]);
        ftx_msg_meta_t cq = rx("CQ A1AAA IN80");
        slot_end();
        press(cq);
        check(next_is(answer[i]), "%s (%s)", answer[i], tx.msg);
    }
}

/* Free text with a bare sign where the report goes: "A B +" is a valid free-text frame that anyone can send. The
 * dispatch looked at the first character only and std::stoi("+") threw std::invalid_argument; on the radio nothing
 * catches it on the FT8 decoder thread, so the GUI ended (std::terminate). Not a report: nothing thrown, queued or
 * logged, and a real report after it still works */
static void bare_sign(void) {
    begin("Free text with a bare sign where the report goes: not a report");
    for (const char *t : {"A B +", "A B -", "A B R+", "A B R-", "A B +X", "A B R-1X", "Q1QQQ A1AAA +", "Q1QQQ A1AAA R-"}) {
        bool threw = false;
        try {
            rx(t);
        } catch (...) {
            threw = true;
        }
        check(!threw && logged.empty() && !tx.msg[0], "\"%s\": nothing thrown, queued or logged", t);
    }
    tx_cq();
    our_slot();
    rx("Q1QQQ A1AAA IN80", -10);
    slot_end();
    check(next_is("A1AAA Q1QQQ -10"), "a call after them answered (%s)", tx.msg);
    our_slot();
    rx("Q1QQQ A1AAA R+", -11);
    slot_end();
    check(logged.empty(), "its bare R+ logs nothing");
    our_slot();
    rx("Q1QQQ A1AAA R-12", -11);
    slot_end();
    check(logged_once("A1AAA", "IN80", -12, -10, false), "its real R-report logs it: rcvd -12, sent -10");
}

/* ---- No station to answer (#228) ------------------------------------------------------------------------------- */

/* "<...>": a call sent as a hash the decoder has not heard in full (a compound or long call). It was taken as a
 * station called "...": Auto answered "... Q1QQQ R-10" in place of our CQ, and that does not encode (ft8_lib refuses
 * it), so its retries went by with TX rows and nothing on the air. Still a message to us, no QSO made of it */
static void unknown_call(void) {
    begin("We call CQ: a report from a call we know only as <...> gets no answer, the CQ goes on");
    tx_cq();
    our_slot();
    ftx_msg_meta_t m = rx(ME " <...> -05", -10);
    check(m.to_me && !m.call_de[0], "a message to us from no call we know (to_me %d, '%s')", m.to_me, m.call_de);
    slot_end();
    check(next_is("CQ " ME " " MY_QTH) && !ftx_qso_processor_has_current(qso), "the CQ stays, no QSO (%s)", tx.msg);
    for (const char *t : {ME " <...> JO22", ME " <...> R-05", ME " <...> RR73", ME " <...> 73"}) {
        m = rx(t);
        check(m.to_me && !m.call_de[0] && next_is("CQ " ME " " MY_QTH), "\"%s\": the CQ stays (%s)", t, tx.msg);
    }
    slot_end();
    check(logged.empty(), "nothing logged");
    rx(ME " A1AAA IN80", -12);
    slot_end();
    check(next_is("A1AAA " ME " -12"), "a call after them answered (%s)", tx.msg);

    begin("We answer A's CQ: a report from <...> in between leaves A's QSO as it is");
    ftx_msg_meta_t cq = rx("CQ A1AAA IN80");
    slot_end();
    press(cq);
    our_slot();
    rx(ME " <...> -03");
    rx(ME " A1AAA -05");
    slot_end();
    check(next_is("A1AAA " ME " R-10"), "our R-report to A (%s)", tx.msg);
    our_slot();
    rx(ME " A1AAA RR73");
    slot_end();
    check(logged_once("A1AAA", "IN80", -5, -10, false) && logged.size() == 1, "A logged, nothing else");

    begin("A CQ from <...>: no call in its row");
    m = rx("CQ <...>");
    check(m.type == FTX_MSG_TYPE_CQ && !m.call_de[0], "a CQ with no call (type %d, '%s')", m.type, m.call_de);
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    good_paths();
    incomplete();
    moved_on();
    report_out();
    aside();
    new_caller();
    force();
    own_call();
    own_qth();
    bare_sign();
    unknown_call();
    ftx_qso_processor_delete(qso);
    printf("FT8 QSO %s: %d of %d cases\n", good == cases ? "OK" : "FAILED", good, cases);
    return good == cases ? 0 : 1;
}
