/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of how the CW Trainer scores (src/cw_trainer_score.c; issue #290).
 *  - The keys that type a place of Copy's answer: the trainer's 40 characters as typed (lower case as capitals, ? .
 *    , / and the digits), and nothing for Space, = # * (the trainer's "not a character"), a letter of two bytes
 *    (Å), LVGL's Enter, Backspace, Esc, Del and arrows, and the radio's own key codes. A set narrower than the 40
 *    takes only its own.
 *  - The answer never longer than the group; Backspace rubs out the last place, and nothing when it is empty.
 *  - The check, place by place: right; one wrong; one missed (the rest still right); one typed twice; two swapped
 *    (both wrong); the first missed and one typed at the end; all wrong with a character of the group typed in
 *    another place (not right); nothing typed; a character twice in the group and once in the answer (the earlier
 *    place takes it); an answer longer than the places checked.
 *  - The on-screen keys: round from the last to the first and back, a level that shrank under the key chosen.
 *  - Stats: a character's last 20 tries (the 21st pushes the oldest out), its per cent, untried -1; a group's result
 *    onto today's totals and onto each of its characters, one not in the set left out; Learn's and Copy's apart.
 *  - The level: 9 of the last 10 right takes it up, 8 not, fewer than 10 groups not; a new level starts its 10
 *    again, the session's totals stay.
 *  - A Copy session end to end: groups typed, each checked and scored as the trainer does.
 */

#include "cw_trainer_score.h"

#include <stdio.h>
#include <string.h>

/* The Koch order, as dialog_cw_trainer.c has it */
#define KOCH "KMRSUAPTLOWI.NJEF0Y,VG5/Q9ZH38B?427C1D6X"

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

/* The marks as a text: '+' right, '-' not */
static const char *marks(const bool *ok, size_t n) {
    static char buf[2][64];
    static int  k;
    char       *b = buf[k++ % 2];

    for (size_t i = 0; i < n; i++) {
        b[i] = ok[i] ? '+' : '-';
    }
    b[n] = 0;
    return b;
}

/* sent typed: the places copied, the group's marks and the answer's */
static void check_copy(const char *sent, const char *typed, int copied, const char *sent_m, const char *typed_m) {
    bool s[64], t[64];
    char what[160];
    int  n = cwt_copy_check(sent, typed, s, t);

    snprintf(what, sizeof(what), "%s typed %s: %d copied %s %s, wanted %d %s %s", sent, typed, n,
             marks(s, strlen(sent)), marks(t, strlen(typed)), copied, sent_m, typed_m);
    check(n == copied && !strcmp(marks(s, strlen(sent)), sent_m) && !strcmp(marks(t, strlen(typed)), typed_m), what);
}

static void test_keys(void) {
    const char *ok_in = "kmrsuaptlowi.njef0y,vg5/q9zh38b?427c1d6xKMX";
    char        what[64];

    for (const char *p = ok_in; *p; p++) {
        char want = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;

        snprintf(what, sizeof(what), "key '%c' types %c", *p, want);
        check(cwt_copy_char((unsigned char)*p, KOCH) == want, what);
    }
    const uint32_t none[] = {' ', '=', '#', '*', '-', '@', '!', '"', '[', ']', 0, 8 /* Backspace */, 10 /* Enter */,
                             13, 27 /* Esc */, 127 /* Del */, 17, 18, 19, 20 /* arrows */, 0x85c3 /* Å */,
                             0x290 /* the VOL knob */, 0xffffffff};

    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        snprintf(what, sizeof(what), "key 0x%x types nothing", (unsigned)none[i]);
        check(cwt_copy_char(none[i], KOCH) == 0, what);
    }
    check(cwt_copy_char('k', "KM") == 'K', "a set of two takes k");
    check(cwt_copy_char('x', "KM") == 0, "a set of two leaves x");
}

static void test_answer(void) {
    char a[8] = "";

    check(!cwt_copy_del(a) && !a[0], "Backspace on nothing");
    check(cwt_copy_add(a, 3, 'K') && cwt_copy_add(a, 3, 'M') && cwt_copy_add(a, 3, 'R') && !strcmp(a, "KMR"),
          "three places of three");
    check(!cwt_copy_add(a, 3, 'S') && !strcmp(a, "KMR"), "a fourth into a group of three");
    check(cwt_copy_del(a) && !strcmp(a, "KM"), "Backspace rubs out R");
    check(cwt_copy_add(a, 3, 'S') && !strcmp(a, "KMS"), "S in R's place");
    a[0] = 0;
    check(cwt_copy_add(a, 1, '?') && !cwt_copy_add(a, 1, 'K') && !strcmp(a, "?"), "a group of one");
}

static void test_check(void) {
    check_copy("K", "K", 1, "+", "+");
    check_copy("K", "M", 0, "-", "-");
    check_copy("KMRSU", "KMRSU", 5, "+++++", "+++++");
    check_copy("KMRSU", "KMRSA", 4, "++++-", "++++-");
    check_copy("KMRSU", "AMRSU", 4, "-++++", "-++++");
    check_copy("KMRSU", "KRSU", 4, "+-+++", "++++");        /* M missed: R S U still right */
    check_copy("KMRSU", "KMMRS", 4, "++++-", "++-++");      /* M typed twice, U never */
    check_copy("KMRSU", "MKRSU", 3, "--+++", "--+++");      /* K and M swapped: both wrong, place by place */
    check_copy("KMRSU", "MRSUK", 4, "-++++", "++++-");      /* K missed at the front, one typed at the end */
    check_copy("IRK", "KSS", 0, "---", "---");              /* not the K typed first for the group's last */
    check_copy("KMRSU", "", 0, "-----", "");
    check_copy("KMK", "K", 1, "+--", "+");                  /* the earlier K takes it */
    check_copy("K", "KK", 1, "+", "+-");
    check_copy("?./,0", "?./,0", 5, "+++++", "+++++");
    check_copy("?./,0", "?.,/0", 3, "++--+", "++--+");     /* / and , swapped */
    check_copy("ABCDE", "VWXYZ", 0, "-----", "-----");

    /* NULL marks, and an answer longer than the places checked (32): the rest is never right */
    char long_t[48];

    memset(long_t, 'K', 40);
    long_t[40] = 0;
    check(cwt_copy_check("KM", "KM", NULL, NULL) == 2, "the check without marks");
    bool t[48];

    check(cwt_copy_check("K", long_t, NULL, t) == 1 && t[0] && !t[1] && !t[39], "an answer of 40 places");
}

static void test_sel(void) {
    check(cwt_copy_sel(0, 4, 1) == 1, "the next key");
    check(cwt_copy_sel(3, 4, 1) == 0, "past the last key: the first");
    check(cwt_copy_sel(0, 4, -1) == 3, "before the first key: the last");
    check(cwt_copy_sel(2, 4, -7) == 3, "seven back from the third of four");
    check(cwt_copy_sel(41, 6, 1) == 0, "the key chosen past a level that shrank");
    check(cwt_copy_sel(5, 0, 1) == 0, "no keys");
}

static void test_stats(void) {
    cwt_stats_t s;
    char        what[64];

    memset(&s, 0, sizeof(s));
    check(cwt_stats_pct(&s, 0) == -1, "an untried character");
    cwt_stats_add(&s, 0, true);
    cwt_stats_add(&s, 0, false);
    check(cwt_stats_pct(&s, 0) == 50 && s.n[0] == 2, "one of two right: 50 %");
    for (int i = 0; i < CWT_HIST; i++) {
        cwt_stats_add(&s, 0, true);
    }
    check(cwt_stats_pct(&s, 0) == 100 && s.n[0] == CWT_HIST, "20 right after the wrong one: 100 %");
    for (int i = 0; i < 5; i++) {
        cwt_stats_add(&s, 0, false);
    }
    snprintf(what, sizeof(what), "5 wrong of the last 20: %d %%", cwt_stats_pct(&s, 0));
    check(cwt_stats_pct(&s, 0) == 75, what);

    /* a group: KMRSU typed KRSU */
    cwt_stats_t g;
    bool        ok[8];

    memset(&g, 0, sizeof(g));
    cwt_copy_check("KMRSU", "KRSU", ok, NULL);
    cwt_stats_group(&g, KOCH, "KMRSU", ok, false);
    check(g.tries == 1 && g.right == 0, "a wrong group today");
    check(g.n[0] == 1 && g.mask[0] == 1 && g.n[1] == 1 && g.mask[1] == 0 && g.n[2] == 1 && g.mask[2] == 1 &&
              g.n[3] == 1 && g.mask[3] == 1 && g.n[4] == 1 && g.mask[4] == 1 && g.n[5] == 0,
          "K R S U right, M wrong, A untried");
    cwt_copy_check("XX", "XX", ok, NULL);
    cwt_stats_group(&g, KOCH, "XX", ok, true);
    check(g.tries == 2 && g.right == 1 && g.n[39] == 2 && cwt_stats_pct(&g, 39) == 100, "XX right: X twice");
    ok[0] = ok[1] = true;
    cwt_stats_group(&g, "KM", "KX", ok, true);
    check(g.n[0] == 2 && g.n[39] == 2, "X not in a set of two: left out");

    /* Learn's and Copy's apart */
    cwt_stats_t learn, copy;

    memset(&learn, 0, sizeof(learn));
    memset(&copy, 0, sizeof(copy));
    ok[0] = true;
    cwt_stats_group(&copy, KOCH, "K", ok, true);
    check(learn.tries == 0 && learn.n[0] == 0 && copy.tries == 1 && copy.n[0] == 1, "Copy's try is not Learn's");
}

static void test_level(void) {
    cwt_session_t s;

    memset(&s, 0, sizeof(s));
    for (int i = 0; i < 9; i++) {
        cwt_session_add(&s, true);
    }
    check(!cwt_session_pass(&s), "9 groups, all right: not yet");
    cwt_session_add(&s, false);
    check(cwt_session_pass(&s) && cwt_session_good(&s) == 9, "9 of 10 right: the next level");
    cwt_session_add(&s, false);
    check(!cwt_session_pass(&s) && cwt_session_good(&s) == 8, "8 of the last 10: not");
    for (int i = 0; i < 8; i++) {
        cwt_session_add(&s, true);
    }
    check(!cwt_session_pass(&s) && cwt_session_good(&s) == 8, "8 right since: the two wrong ones still in the 10");
    cwt_session_add(&s, true);
    check(cwt_session_pass(&s) && cwt_session_good(&s) == 9, "9 right since: the first wrong one out");
    cwt_session_level(&s);
    check(!cwt_session_pass(&s) && s.n == 0 && s.tried == 20 && s.right == 18,
          "a new level: its 10 start again, the totals stay");
}

/* Groups typed as a user would, each checked and scored as the trainer does (evaluate) */
static void test_session(void) {
    static const struct {
        const char *sent, *typed;
        bool        right;
    } groups[] = {
        {"KK", "KM", false}, {"KM", "K", false}, {"KM", "km", true}, {"MK", "MK", true}, {"MM", "MM", true},
        {"KK", "KK", true},  {"KM", "KM", true}, {"MK", "MK", true}, {"MM", "MM", true}, {"KM", "KM", true},
        {"MK", "MK", true},
    };
    cwt_stats_t   st;
    cwt_session_t ses;
    char          what[96];

    memset(&st, 0, sizeof(st));
    memset(&ses, 0, sizeof(ses));
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        char answer[8] = "";
        bool ok[8];

        for (const char *p = groups[g].typed; *p; p++) {
            char c = cwt_copy_char((unsigned char)*p, KOCH);

            if (c) {
                cwt_copy_add(answer, (int)strlen(groups[g].sent), c);
            }
        }
        int  copied = cwt_copy_check(groups[g].sent, answer, ok, NULL);
        bool right  = copied == (int)strlen(groups[g].sent) && strlen(answer) == strlen(groups[g].sent);

        snprintf(what, sizeof(what), "group %zu %s typed %s: %s", g + 1, groups[g].sent, groups[g].typed,
                 right ? "right" : "wrong");
        check(right == groups[g].right && right == !strcmp(answer, groups[g].sent), what);
        cwt_session_add(&ses, right);
        cwt_stats_group(&st, KOCH, groups[g].sent, ok, right);
        if (g == 9) {
            check(!cwt_session_pass(&ses), "8 of the first 10: no new level");
        }
    }
    /* K 11 tries, the second of KK typed KM wrong; M 11, the one of KM typed K wrong: 10 of 11 each, 90 % */
    snprintf(what, sizeof(what), "K %d tries %d %%, M %d tries %d %%", st.n[0], cwt_stats_pct(&st, 0), st.n[1],
             cwt_stats_pct(&st, 1));
    check(st.n[0] == 11 && cwt_stats_pct(&st, 0) == 90 && st.n[1] == 11 && cwt_stats_pct(&st, 1) == 90, what);
    check(st.tries == 11 && st.right == 9 && ses.tried == 11 && ses.right == 9, "11 groups, 9 right");
    check(cwt_session_pass(&ses), "9 of the last 10 (the first wrong one gone): the next level");
}

int main(void) {
    test_keys();
    test_answer();
    test_check();
    test_sel();
    test_stats();
    test_level();
    test_session();
    printf("%d of %d cases right\n", cases - bad, cases);
    return bad ? 1 : 0;
}
