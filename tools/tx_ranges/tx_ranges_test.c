/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/tx_ranges.c (#247): South Korea's table row by row against the ranges written out here (the
 *  issue's, docs/MANUAL.md "Amateur bands only"), its edges in and 1 Hz past them out, each inside an amateur band;
 *  the check with TX outside ham bands on and off, on a transverter's range (its IF counts), receive only; the own
 *  list's lines one by one (good, bad, comments), a list from text and from a file (missing, CRLF, a line too long,
 *  more ranges than it keeps), joined where they overlap or touch, empty; the SWR scan's narrowing; Amateur bands as
 *  ham_tx_check had it, frequency by frequency; and a read of the own list while other threads check against it.
 */

#include "cfg/tx_ranges.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* South Korea's ranges, Hz, both in: what the table must hold, written out, not read back from it */
static const struct {
    int32_t lo, hi;
} want_kr[] = {
    {1800000, 1825000},     /* 160 m */
    {3500000, 3550000},     /* 80 m */
    {3790000, 3800000},     /* 80 m */
    {7000000, 7200000},     /* 40 m */
    {10100000, 10150000},   /* 30 m */
    {14000000, 14350000},   /* 20 m */
    {18068000, 18168000},   /* 17 m */
    {21000000, 21450000},   /* 15 m */
    {24890000, 24990000},   /* 12 m */
    {28000000, 29700000},   /* 10 m */
    {50000000, 54000000},   /* 6 m */
};

#define KR_N ((int)(sizeof(want_kr) / sizeof(want_kr[0])))

#define TEST_DIR "test.d"

static ham_tx_t kr(int32_t hz) {
    return tx_ranges_check(TX_RANGES_KR, hz, true, 0, false);
}

static ham_tx_t own(int32_t hz, bool any) {
    return tx_ranges_check(TX_RANGES_OWN, hz, true, 0, any);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (!f || fputs(text, f) < 0 || fclose(f)) {
        printf("FAIL cannot write %s\n", path);
        exit(2);
    }
}

static void kr_table(void) {
    int32_t lo, hi, phi = 0, blo, bhi;

    check(tx_ranges_kr_count() == KR_N, "11 ranges", tx_ranges_kr_count());
    for (int i = 0; i < KR_N && i < tx_ranges_kr_count(); i++) {
        tx_ranges_kr_get(i, &lo, &hi);
        check(lo == want_kr[i].lo && hi == want_kr[i].hi, "a range as written", lo);
        check(i == 0 || lo > phi + 1, "sorted, not touching the one before", lo);
        check(ham_band_of(lo, &blo, &bhi) && hi <= bhi, "inside one amateur band", lo);
        phi = hi;
    }
    for (int i = 0; i < KR_N; i++) {
        int32_t wl = want_kr[i].lo, wh = want_kr[i].hi;

        check(kr(wl) == HAM_TX_OK && kr(wh) == HAM_TX_OK, "both edges in", wl);
        check(kr(wl - 1) == HAM_TX_OFF_RANGE, "1 Hz under the low edge out", wl - 1);
        check(kr(wh + 1) == HAM_TX_OFF_RANGE, "1 Hz over the high edge out", wh + 1);
        check(tx_ranges_of(TX_RANGES_KR, (wl + wh) / 2, &lo, &hi) && lo == wl && hi == wh, "its edges", wl);
    }

    /* The issue's own edges, and the frequencies a Korean inspector would try */
    check(kr(1825000) == HAM_TX_OK, "1.825 in", 1825000);
    check(kr(1826000) == HAM_TX_OFF_RANGE, "1.826 out", 1826000);
    check(kr(1810000) == HAM_TX_OK, "1.810 in", 1810000);
    check(kr(1900000) == HAM_TX_OFF_RANGE, "1.900 out (160 m elsewhere)", 1900000);
    check(kr(3550000) == HAM_TX_OK, "3.550 in", 3550000);
    check(kr(3551000) == HAM_TX_OFF_RANGE, "3.551 out", 3551000);
    check(kr(3700000) == HAM_TX_OFF_RANGE, "3.700 out, between the two 80 m pieces", 3700000);
    check(kr(3789999) == HAM_TX_OFF_RANGE, "3.789999 out", 3789999);
    check(kr(3790000) == HAM_TX_OK, "3.790 in", 3790000);
    check(kr(3800000) == HAM_TX_OK, "3.800 in", 3800000);
    check(kr(3850000) == HAM_TX_OFF_RANGE, "3.850 out", 3850000);
    check(kr(5357000) == HAM_TX_OFF_RANGE, "60 m out", 5357000);
    check(kr(5167500) == HAM_TX_OFF_RANGE, "the Alaska channel out", 5167500);
    check(kr(7074000) == HAM_TX_OK, "FT8 on 40 m in", 7074000);
    check(kr(7200000) == HAM_TX_OK, "7.200 in", 7200000);
    check(kr(7200500) == HAM_TX_OFF_RANGE, "7.2005 out", 7200500);
    check(kr(7250000) == HAM_TX_OFF_RANGE, "7.250 out (40 m in Region 2)", 7250000);
    check(kr(14074000) == HAM_TX_OK, "20 m in", 14074000);
    check(kr(27185000) == HAM_TX_OFF_RANGE, "CB out", 27185000);
    check(kr(40680000) == HAM_TX_OFF_RANGE, "8 m out", 40680000);
    check(kr(50000000) == HAM_TX_OK && kr(52000000) == HAM_TX_OK && kr(54000000) == HAM_TX_OK, "50-54 in", 50000000);
    check(kr(49999999) == HAM_TX_OFF_RANGE && kr(54000001) == HAM_TX_OFF_RANGE, "past 6 m out", 54000001);
    check(kr(137000) == HAM_TX_OFF_RANGE && kr(475000) == HAM_TX_OFF_RANGE, "2200 m and 630 m out", 137000);
}

/* TX outside ham bands, receive only and the transverters, with South Korea chosen */
static void kr_rules(void) {
    check(tx_ranges_check(TX_RANGES_KR, 7250000, true, 0, true) == HAM_TX_OFF_RANGE, "KR: 7.250 out, the switch on",
          7250000);
    check(tx_ranges_check(TX_RANGES_KR, 27185000, true, 0, true) == HAM_TX_OFF_RANGE, "KR: CB out, the switch on",
          27185000);
    check(tx_ranges_check(TX_RANGES_KR, 7100000, true, 0, true) == HAM_TX_OK, "KR: 7.100 in, the switch on", 7100000);
    check(tx_ranges_check(TX_RANGES_KR, 100000000, false, 0, true) == HAM_TX_RX_ONLY, "KR: receive only stays so",
          100000000);
    check(tx_ranges_check(TX_RANGES_KR, -850000, false, 0, false) == HAM_TX_RX_ONLY, "KR: under 0 Hz, receive only",
          -850000);
    /* A 2 m transverter on a 28 MHz IF (a shift of 116 MHz): the IF counts, 144.000-145.700 MHz through 10 m */
    check(tx_ranges_check(TX_RANGES_KR, 145000000, true, 116000000, false) == HAM_TX_OK, "KR: 145.0 on IF 29.0",
          145000000);
    check(tx_ranges_check(TX_RANGES_KR, 145700000, true, 116000000, false) == HAM_TX_OK, "KR: 145.7 on IF 29.7",
          145700000);
    check(tx_ranges_check(TX_RANGES_KR, 145700001, true, 116000000, false) == HAM_TX_OFF_RANGE,
          "KR: 145.700001 on IF past 10 m", 145700001);
    check(tx_ranges_check(TX_RANGES_KR, 145900000, true, 116000000, true) == HAM_TX_OFF_RANGE,
          "KR: 145.9 on IF 29.9, the switch on", 145900000);
    /* A transverter set up so that its IF is CB: the radio's own output would be 27.185 MHz */
    check(tx_ranges_check(TX_RANGES_KR, 70000000, true, 42815000, true) == HAM_TX_OFF_RANGE,
          "KR: a shift onto CB refused", 70000000);
    /* Amateur bands keep a transverter's range as before, whatever its IF */
    check(tx_ranges_check(TX_RANGES_BANDS, 145900000, true, 116000000, false) == HAM_TX_OK,
          "bands: 145.9 on IF 29.9 as before", 145900000);
    check(tx_ranges_check(TX_RANGES_BANDS, 70000000, true, 42815000, false) == HAM_TX_OK,
          "bands: a transverter's range transmits as set up", 70000000);
}

static void lines(void) {
    static const struct {
        const char *line;
        int         r;
        int32_t     lo, hi;
    } t[] = {
        {"7.000-7.200", 1, 7000000, 7200000},
        {"  7 - 7.2   # 40 m, Region 1", 1, 7000000, 7200000},
        {"\t14.000-14.350\r", 1, 14000000, 14350000},
        {"7.2005-7.2005", 1, 7200500, 7200500},
        {"1.810000-1.999999", 1, 1810000, 1999999},
        {"0.1-55", 1, 100000, 55000000},
        {"50.000-54.000#6m", 1, 50000000, 54000000},
        {"", 0, 0, 0},
        {"   ", 0, 0, 0},
        {"# a comment", 0, 0, 0},
        {"  # 7.000-7.200 left out", 0, 0, 0},
        {"\r", 0, 0, 0},
        {"7,000-7,200", -1, 0, 0},          /* a comma for the point */
        {"7000-7200", -1, 0, 0},            /* kHz */
        {"7.200-7.000", -1, 0, 0},          /* from above to */
        {"7.0-", -1, 0, 0},
        {"-7.2", -1, 0, 0},
        {"7.0 7.2", -1, 0, 0},              /* no dash */
        {"7.0--7.2", -1, 0, 0},
        {"7.-7.2", -1, 0, 0},
        {".5-1.0", -1, 0, 0},
        {"14.0000001-14.1", -1, 0, 0},      /* under 1 Hz */
        {"50-60", -1, 0, 0},                /* past the radio's 55 MHz */
        {"144-146", -1, 0, 0},              /* a transverter's band: its IF is what the list holds */
        {"0.05-0.1", -1, 0, 0},             /* under 0.1 MHz */
        {"7.0-7.2 MHz", -1, 0, 0},
        {"40m 7.0-7.2", -1, 0, 0},
        {"7.0-7.2 7.3-7.4", -1, 0, 0},
        {"99999999999-1", -1, 0, 0},
    };

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int32_t lo = 0, hi = 0;
        int     r  = tx_ranges_line(t[i].line, &lo, &hi);
        char    what[96];

        snprintf(what, sizeof(what), "line \"%s\"", t[i].line);
        check(r == t[i].r, what, r);
        check(r != 1 || (lo == t[i].lo && hi == t[i].hi), what, lo);
    }
}

static void own_text(void) {
    tx_ranges_own_t st;
    int32_t         lo, hi;

    st = tx_ranges_own_set("# mine\n7.000-7.100\n7.050-7.200\nnot a range\n14.000-14.350\n7.200001-7.250\n\n"
                           "14.100-14.200 # inside 20 m\n");
    check(st.count == 2 && st.bad == 1 && st.first_bad == 4 && !st.missing, "text: 2 ranges, line 4 not read",
          st.count);
    check(tx_ranges_own_get(0, &lo, &hi) && lo == 7000000 && hi == 7250000, "text: 7.000-7.250 joined (overlap, touch)",
          hi);
    check(tx_ranges_own_get(1, &lo, &hi) && lo == 14000000 && hi == 14350000, "text: 14.000-14.350, 14.1 inside", lo);
    check(!tx_ranges_own_get(2, &lo, &hi) && !tx_ranges_own_get(-1, &lo, &hi), "text: no third range", 2);
    check(tx_ranges_of(TX_RANGES_OWN, 7225000, &lo, &hi) && lo == 7000000 && hi == 7250000, "text: 7.225's range", lo);
    check(own(7250000, false) == HAM_TX_OK && own(7250001, false) == HAM_TX_OFF_RANGE, "text: 7.250 in, 7.250001 out",
          7250001);
    check(own(10120000, true) == HAM_TX_OFF_RANGE, "text: 30 m out, the switch on", 10120000);
    check(tx_ranges_own_state().count == 2, "the state kept", tx_ranges_own_state().count);
    st = tx_ranges_own_set("7.000-7.200\n7.201-7.250\n");
    check(st.count == 2 && own(7200500, false) == HAM_TX_OFF_RANGE, "a 1 kHz gap: two ranges, 7.2005 out", st.count);

    /* Outside the amateur bands the own list needs the switch too: the narrower of the two wins */
    st = tx_ranges_own_set("26.900-27.500\n7.000-7.200\n");
    check(st.count == 2 && !st.bad, "CB list: 2 ranges", st.count);
    check(own(27185000, false) == HAM_TX_OFF_BAND, "CB list: 27.185 outside the amateur bands, switch off", 27185000);
    check(own(27185000, true) == HAM_TX_OK, "CB list: 27.185 with the switch on", 27185000);
    check(own(7100000, false) == HAM_TX_OK, "CB list: 7.100", 7100000);
    check(own(7250000, true) == HAM_TX_OFF_RANGE, "CB list: 7.250 out, the switch on", 7250000);
    check(tx_ranges_check(TX_RANGES_OWN, 100000000, false, 0, true) == HAM_TX_RX_ONLY, "CB list: receive only",
          100000000);

    /* The order lines come in, and one range inside another */
    st = tx_ranges_own_set("50.000-52.000\n1.810-2.000\n50.100-50.200\n3.500-3.800");
    check(st.count == 3, "unsorted: 3 ranges", st.count);
    check(tx_ranges_own_get(0, &lo, &hi) && lo == 1810000 && tx_ranges_own_get(2, &lo, &hi) && hi == 52000000,
          "unsorted: sorted, 50.1-50.2 inside 50-52", hi);

    /* Nothing read: the list is empty, the radio transmits nowhere with Own list (receive only stays receive only) */
    st = tx_ranges_own_set("# nothing yet\n\n7,0-7,2\n");
    check(st.count == 0 && st.bad == 1 && st.first_bad == 3 && !st.missing, "no valid line: empty, line 3", st.count);
    check(own(7074000, false) == HAM_TX_NO_RANGES && own(7074000, true) == HAM_TX_NO_RANGES,
          "no valid line: no TX, the switch on or off", 7074000);
    check(tx_ranges_check(TX_RANGES_OWN, 145000000, true, 116000000, true) == HAM_TX_NO_RANGES,
          "no valid line: a transverter neither", 145000000);
    check(tx_ranges_check(TX_RANGES_OWN, 100000000, false, 0, false) == HAM_TX_RX_ONLY, "no valid line: receive only",
          100000000);
    check(!tx_ranges_of(TX_RANGES_OWN, 7074000, &lo, &hi), "no valid line: no range", 7074000);
    st = tx_ranges_own_set(NULL);
    check(st.count == 0 && st.bad == 0 && !st.missing, "no text: empty", st.count);
    /* Amateur bands and South Korea do not look at the own list */
    check(tx_ranges_check(TX_RANGES_BANDS, 7074000, true, 0, false) == HAM_TX_OK, "empty own list: bands as before",
          7074000);
    check(kr(7074000) == HAM_TX_OK, "empty own list: South Korea as before", 7074000);
}

static void own_file(void) {
    tx_ranges_own_t st;
    int32_t         lo, hi;
    char            text[64 * 24 + 512];
    int             l = 0;

    mkdir(TEST_DIR, 0755);
    st = tx_ranges_own_load(TEST_DIR "/none.conf");
    check(st.missing && st.count == 0, "missing file: empty", st.count);
    check(own(7074000, false) == HAM_TX_NO_RANGES, "missing file: no TX", 7074000);

    /* CRLF line ends, a line longer than the buffer (bad, the next line still read), a comment line */
    l += snprintf(text + l, sizeof(text) - l, "# Own list\r\n7.000-7.200\r\n");
    for (int i = 0; i < 300; i++) text[l++] = 'x';
    l += snprintf(text + l, sizeof(text) - l, "\r\n14.000-14.350\r\n");
    write_file(TEST_DIR "/tx_ranges.conf", text);
    st = tx_ranges_own_load(TEST_DIR "/tx_ranges.conf");
    check(!st.missing && st.count == 2 && st.bad == 1 && st.first_bad == 3, "file: 2 ranges, the long line 3 bad",
          st.count);
    check(own(14074000, false) == HAM_TX_OK && own(7300000, false) == HAM_TX_OFF_RANGE, "file: as read", 14074000);
    check(tx_ranges_own_get(1, &lo, &hi) && lo == 14000000 && hi == 14350000, "file: line 4 after the long one", lo);

    /* More ranges than it keeps: the first 64 lines that are ranges, the rest counted as not read */
    l = 0;
    for (int i = 0; i < 70; i++) {
        l += snprintf(text + l, sizeof(text) - l, "%d.000-%d.100\n", 1 + (i % 50), 1 + (i % 50));
    }
    write_file(TEST_DIR "/tx_ranges.conf", text);
    st = tx_ranges_own_load(TEST_DIR "/tx_ranges.conf");
    check(st.bad == 6 && st.first_bad == 65, "70 lines: 6 past the 64th not read, from line 65", st.bad);
    check(st.count == 50, "70 lines: 50 different ranges", st.count);

    /* Removed again: missing, empty */
    unlink(TEST_DIR "/tx_ranges.conf");
    st = tx_ranges_own_load(TEST_DIR "/tx_ranges.conf");
    check(st.missing && st.count == 0 && tx_ranges_own_state().missing, "removed: missing again", st.count);
    rmdir(TEST_DIR);
}

static void scan(const char *what, tx_ranges_t p, int32_t freq, int32_t shift, int32_t lo, int32_t hi, bool want_ok,
                 int32_t want_lo, int32_t want_hi) {
    bool ok = tx_ranges_narrow(p, freq, shift, &lo, &hi);

    check(ok == want_ok, what, ok);
    check(!ok || (lo == want_lo && hi == want_hi), what, lo);
    check(!ok || hi == want_hi, what, hi);
}

/* The SWR scan's sweep (cfg_tx_range): no wider than the profile's range */
static void narrow(void) {
    scan("bands: as given", TX_RANGES_BANDS, 7100000, 0, 7000000, 7300000, true, 7000000, 7300000);
    scan("KR 40 m: 7.000-7.200, not 7.300", TX_RANGES_KR, 7100000, 0, 7000000, 7300000, true, 7000000, 7200000);
    scan("KR 80 m low piece", TX_RANGES_KR, 3520000, 0, 3500000, 4000000, true, 3500000, 3550000);
    scan("KR 80 m high piece", TX_RANGES_KR, 3795000, 0, 3500000, 4000000, true, 3790000, 3800000);
    scan("KR 160 m", TX_RANGES_KR, 1810000, 0, 1800000, 2000000, true, 1800000, 1825000);
    scan("KR with the switch on: HF's 0.1-55 to 20 m", TX_RANGES_KR, 14200000, 0, 100000, 55000000, true, 14000000,
         14350000);
    scan("KR outside: nothing", TX_RANGES_KR, 7250000, 0, 7000000, 7300000, false, 0, 0);
    scan("KR only narrows", TX_RANGES_KR, 7100000, 0, 7050000, 7150000, true, 7050000, 7150000);
    scan("KR 2 m transverter: 144.000-145.700", TX_RANGES_KR, 145000000, 116000000, 144000000, 146000000, true,
         144000000, 145700000);
    tx_ranges_own_set("7.000-7.100\n");
    scan("own 40 m piece", TX_RANGES_OWN, 7050000, 0, 7000000, 7300000, true, 7000000, 7100000);
    scan("own outside: nothing", TX_RANGES_OWN, 7150000, 0, 7000000, 7300000, false, 0, 0);
    tx_ranges_own_set("");
    scan("own empty: nothing", TX_RANGES_OWN, 7050000, 0, 7000000, 7300000, false, 0, 0);
}

/* Amateur bands: what ham_tx_check said before #247, every 1 kHz over HF and a few transverter frequencies */
static void bands_as_before(void) {
    int diff = 0;

    for (int32_t f = 0; f <= 56000000; f += 1000) {
        for (int any = 0; any < 2; any++) {
            diff += tx_ranges_check(TX_RANGES_BANDS, f, f >= 100000 && f <= 55000000, 0, any) !=
                    ham_tx_check(f, f >= 100000 && f <= 55000000, false, any);
        }
    }
    check(diff == 0, "bands: as ham_tx_check over HF", diff);
    check(tx_ranges_check(TX_RANGES_BANDS, 432100000, true, 404000000, false) == HAM_TX_OK, "bands: 70 cm transverter",
          432100000);
}

static void words(void) {
    check(!tx_ranges_why(HAM_TX_OK), "OK: no words", 0);
    check(!strcmp(tx_ranges_why(HAM_TX_RX_ONLY), "receive only"), "receive only", 0);
    check(!strcmp(tx_ranges_why(HAM_TX_OFF_BAND), "outside the amateur bands (Settings > TX Cal)"), "off band", 0);
    check(!strcmp(tx_ranges_why(HAM_TX_OFF_RANGE), "outside the TX ranges (Settings > TX Cal)"), "off range", 0);
    check(!strcmp(tx_ranges_why(HAM_TX_NO_RANGES), "no TX ranges in the own list (Settings > TX Cal)"), "no ranges", 0);
}

/* A read of the own list on one thread while others check: each check sees one list or the other, never half of one
 * (40 m only, or 20 m only: never empty between them, never a range with one edge of each) */
static atomic_bool stop;
static atomic_int  torn;

static void *checker(void *arg) {
    int32_t lo, hi;

    (void)arg;
    while (!atomic_load(&stop)) {
        if (own(7100000, false) == HAM_TX_NO_RANGES ||
            (tx_ranges_of(TX_RANGES_OWN, 7100000, &lo, &hi) && (lo != 7000000 || hi != 7200000)) ||
            (tx_ranges_of(TX_RANGES_OWN, 14100000, &lo, &hi) && (lo != 14000000 || hi != 14350000))) {
            atomic_fetch_add(&torn, 1);
        }
    }
    return NULL;
}

static void threads(void) {
    pthread_t th[3];

    tx_ranges_own_set("7.000-7.200\n");
    for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, checker, NULL);
    for (int i = 0; i < 20000; i++) {
        tx_ranges_own_set(i % 2 ? "7.000-7.200\n" : "14.000-14.350\n");
    }
    atomic_store(&stop, true);
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    check(atomic_load(&torn) == 0, "reads while checking: never empty, never both", atomic_load(&torn));
}

int main(void) {
    kr_table();
    kr_rules();
    lines();
    own_text();
    own_file();
    narrow();
    bands_as_before();
    words();
    threads();

    printf("tx_ranges: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
