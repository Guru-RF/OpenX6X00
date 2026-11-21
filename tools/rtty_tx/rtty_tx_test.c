/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of RTTY's transmitter (src/rtty_tx.c, #312) and its macro list (src/rtty_macro.c):
 *
 *  - ITA2: each character's code, the LTRS and FIGS shifts where the case changes, FIGS again after a space sent in
 *    figures (a receiver that unshifts on space is in letters by then), CR LF for a line end, lower case as capitals,
 *    what ITA2 lacks left out; what is sent read back by a receiver that unshifts on space and by one that doesn't
 *  - the tones: the mark on the higher radio frequency in USB and LSB with Reverse off, the other way with it on
 *  - the presets (RTTY 5:5's Preset): Ham 45.45/170/Reverse off and Weather 50/450/Reverse on found, a setting off by
 *    one of the three none, a press Ham from Weather or none and Weather from Ham; Weather the defaults of params.c
 *  - the waveform: the mark idle at the start, the LTRS diddles, CR LF, the text framed as 1 start bit, 5 data bits
 *    (the first in bit 0) and 1.5 stop bits, then CR LF and the tail, each bit checked with a Goertzel filter at the
 *    two tones in the middle of the bit; the length to the sample; the idle's frequency from its zero crossings, with
 *    Reverse too; the phase running on across the bit edges; the fades at both ends; the same samples whatever the
 *    size of the blocks asked for; the characters told (echo) in order and before their first sample
 *  - the macros: the starter set written when the file is missing, read back, changed, deleted, tabs as spaces
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "rtty_tx.h"
#include "rtty_macro.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE    44100.0f
#define BAUD    45.45f
#define CENTER  800
#define SHIFT   170

static int fails, checks;

char rtty_test_macros_path[256];    /* test_path.h */

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fails++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

/* ---- ITA2 ---- */

/* The codes of a whole text, from a fresh encoder (in letters, as after the diddles) */
static int encode(const char *text, uint8_t *codes) {
    rtty_enc_t e;
    int        n = 0;

    rtty_enc_init(&e);
    for (const char *p = text; *p; p++) {
        n += rtty_enc_char(&e, *p, codes + n);
    }
    return n;
}

static void expect_codes(const char *text, const uint8_t *want, int want_n) {
    uint8_t codes[256];
    int     n = encode(text, codes);
    bool    same = n == want_n && !memcmp(codes, want, (size_t)n);

    CHECK(same, "\"%s\": %d codes, %d wanted", text, n, want_n);
    if (!same) {
        for (int i = 0; i < n; i++) printf(" %d", codes[i]);
        printf("\n");
    }
}

/* A receiver: the case it is in, LTRS and FIGS change it, a space puts it in letters when it unshifts on space */
static void decode(const uint8_t *codes, int n, bool usos, char *out) {
    bool letters = true;
    int  o = 0;

    for (int i = 0; i < n; i++) {
        uint8_t c = codes[i];
        char    ch;

        if (c == RTTY_ITA2_LTRS) {
            letters = true;
            continue;
        }
        if (c == RTTY_ITA2_FIGS) {
            letters = false;
            continue;
        }
        ch = letters ? rtty_ita2_letters[c] : rtty_ita2_figures[c];
        if (c == RTTY_ITA2_SPACE && usos) letters = true;
        if (ch) out[o++] = ch;
    }
    out[o] = '\0';
}

static void test_ita2(void) {
    const uint8_t F = RTTY_ITA2_FIGS, L = RTTY_ITA2_LTRS;

    /* C 14, Q 23: letters already, no shift */
    expect_codes("CQ", (const uint8_t[]){14, 23}, 2);
    expect_codes("cq", (const uint8_t[]){14, 23}, 2);
    /* 5 on T 16, 9 on O 24, 0 on P 22, 1 on Q 23: FIGS once, again after the space */
    expect_codes("599 001", (const uint8_t[]){F, 16, 24, 24, 4, F, 22, 22, 23}, 9);
    /* A 3, figure 1 on Q 23, B 25 back in letters */
    expect_codes("A1B", (const uint8_t[]){3, F, 23, L, 25}, 5);
    /* a space in letters keeps letters: no shift for the B */
    expect_codes("A B", (const uint8_t[]){3, 4, 25}, 3);
    /* after a space in figures a letter gets its LTRS again, so does a figure its FIGS */
    expect_codes("5 A", (const uint8_t[]){F, 16, 4, L, 3}, 5);
    expect_codes("5 5", (const uint8_t[]){F, 16, 4, F, 16}, 5);
    /* a line end: CR 8, LF 2, in either case; '\r' is nothing, a tab a space */
    expect_codes("A\r\nB", (const uint8_t[]){3, 8, 2, 25}, 4);
    expect_codes("A\tB", (const uint8_t[]){3, 4, 25}, 3);
    /* what ITA2 has not: left out, the shift not spent on it */
    expect_codes("A@%*<>B", (const uint8_t[]){3, 25}, 2);
    expect_codes("\xc3\xa9", (const uint8_t[]){0}, 0);
    /* the figures of the US teletype: ? on B, / on X, . on M, ' on J, $ on D, ! on F */
    expect_codes("?/.'$!", (const uint8_t[]){F, 25, 29, 28, 11, 9, 13}, 7);

    CHECK(rtty_enc_shown('q') == 'Q', "q shown as %c", rtty_enc_shown('q'));
    CHECK(rtty_enc_shown('@') == 0, "@ shown");
    CHECK(rtty_enc_shown('\t') == ' ', "tab shown");
    CHECK(rtty_enc_shown('\r') == 0, "CR shown");

    /* Read back by both receivers: the text in capitals, what ITA2 lacks gone */
    static const struct {
        const char *in, *out;
    } texts[] = {
        {"CQ CQ DE ON6URE ON6URE K", "CQ CQ DE ON6URE ON6URE K"},
        {"ur rst 599 599 - name joe, qth ghent.", "UR RST 599 599 - NAME JOE, QTH GHENT."},
        {"5 A 5 B 55 CC 1/2 ?", "5 A 5 B 55 CC 1/2 ?"},
        {"73 es gl\nde pa3xyz sk", "73 ES GL\nDE PA3XYZ SK"},
        {"mail@host 100% <ok>", "MAILHOST 100 OK"},
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        uint8_t codes[512];
        char    a[256], b[256];
        int     n = encode(texts[i].in, codes);

        decode(codes, n, true, a);
        decode(codes, n, false, b);
        CHECK(!strcmp(a, texts[i].out), "USOS receiver: \"%s\", \"%s\" wanted", a, texts[i].out);
        CHECK(!strcmp(b, texts[i].out), "receiver without USOS: \"%s\", \"%s\" wanted", b, texts[i].out);
    }

    /* The table the decoder prints with: each character of either case once, so the encoder's choice is the one */
    for (int c = 1; c < 128; c++) {
        int seen = 0;

        for (int i = 0; i < 32; i++) {
            seen += rtty_ita2_letters[i] == c;
            seen += rtty_ita2_figures[i] == c && rtty_ita2_letters[i] != c;
        }
        CHECK(seen <= 1, "'%c' is %d codes", c, seen);
    }
}

/* ---- tones ---- */

static void test_tones(void) {
    float mark, space;

    CHECK(rtty_mark_high(1, false), "USB: mark high");
    CHECK(!rtty_mark_high(-1, false), "LSB: mark low (the higher radio frequency)");
    CHECK(!rtty_mark_high(1, true), "USB, Reverse: mark low");
    CHECK(rtty_mark_high(-1, true), "LSB, Reverse: mark high");
    CHECK(!rtty_mark_high(0, false) && !rtty_mark_high(0, true), "another mode: mark low, as the decoder has it");

    rtty_tones(CENTER, SHIFT, true, &mark, &space);
    CHECK(mark == 885.0f && space == 715.0f, "mark high: %.1f %.1f", mark, space);
    rtty_tones(CENTER, SHIFT, false, &mark, &space);
    CHECK(mark == 715.0f && space == 885.0f, "mark low: %.1f %.1f", mark, space);
    rtty_tones(1500, 850, true, &mark, &space);
    CHECK(mark == 1925.0f && space == 1075.0f, "850 Hz: %.1f %.1f", mark, space);
}

/* ---- the presets ---- */

/* The number after ".name =" in params.c's defaults, -1 none */
static long params_default(const char *src, const char *name) {
    char        key[64];
    const char *p;

    snprintf(key, sizeof(key), ".%s", name);
    for (p = strstr(src, key); p; p = strstr(p + 1, key)) {
        const char *q = p + strlen(key);

        while (*q == ' ') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ') q++;
        if (!strncmp(q, "true", 4)) return 1;
        if (!strncmp(q, "false", 5)) return 0;
        return strtol(q, NULL, 10);
    }
    return -1;
}

static void test_presets(void) {
    static char src[1 << 20];
    FILE       *f = fopen(PARAMS_C, "r");
    size_t      n = f ? fread(src, 1, sizeof(src) - 1, f) : 0;

    CHECK(rtty_preset_find(4545, 170, false) == RTTY_PRESET_HAM, "45.45/170/off: Ham");
    CHECK(rtty_preset_find(5000, 450, true) == RTTY_PRESET_WEATHER, "50/450/on: Weather");
    CHECK(rtty_preset_find(4545, 170, true) == -1, "Ham with Reverse on: none");
    CHECK(rtty_preset_find(4545, 450, false) == -1, "45.45/450/off: none");
    CHECK(rtty_preset_find(5000, 170, false) == -1, "50/170/off: none");
    CHECK(rtty_preset_find(5000, 450, false) == -1, "Weather with Reverse off: none");

    CHECK(rtty_preset_next(-1) == RTTY_PRESET_HAM, "none: a press gives Ham");
    CHECK(rtty_preset_next(RTTY_PRESET_WEATHER) == RTTY_PRESET_HAM, "Weather: a press gives Ham");
    CHECK(rtty_preset_next(RTTY_PRESET_HAM) == RTTY_PRESET_WEATHER, "Ham: a press gives Weather");
    CHECK(!strcmp(rtty_presets[RTTY_PRESET_HAM].name, "Ham") && !strcmp(rtty_presets[RTTY_PRESET_WEATHER].name,
          "Weather"), "names");

    /* Weather is what the app starts on: the decoder's defaults */
    CHECK(n > 0, "%s not read", PARAMS_C);
    src[n] = '\0';
    if (f) fclose(f);
    CHECK(params_default(src, "rtty_rate") == rtty_presets[RTTY_PRESET_WEATHER].rate &&
          params_default(src, "rtty_shift") == rtty_presets[RTTY_PRESET_WEATHER].shift &&
          params_default(src, "rtty_reverse") == rtty_presets[RTTY_PRESET_WEATHER].reverse,
          "params.c's defaults %ld/%ld/%ld are not Weather", params_default(src, "rtty_rate"),
          params_default(src, "rtty_shift"), params_default(src, "rtty_reverse"));
}

/* ---- the waveform ---- */

typedef struct {
    const char *text;
    size_t      pos;
    char        echo[512];
    uint32_t    echo_at[512];   /* afsk.n when told */
    int         echo_n;
    rtty_tx_t  *tx;
} source_t;

static int next_cb(void *ctx) {
    source_t *s = ctx;

    return s->text[s->pos] ? (unsigned char)s->text[s->pos++] : -1;
}

static void echo_cb(char c, void *ctx) {
    source_t *s = ctx;

    s->echo_at[s->echo_n] = s->tx->afsk.n;
    s->echo[s->echo_n++] = c;
    s->echo[s->echo_n] = '\0';
}

/* A whole transmission of text into samples, asked for block samples at a time */
static int16_t *transmit(const char *text, float mark, float space, size_t block, size_t *count, source_t *src) {
    static rtty_tx_t tx;
    rtty_tx_cfg_t    cfg = {.rate = RATE, .baud = BAUD, .mark_hz = mark, .space_hz = space};
    size_t           cap = 1 << 22, n = 0, got;
    int16_t         *buf = malloc(cap * sizeof(int16_t));

    memset(src, 0, sizeof(*src));
    src->text = text;
    src->tx = &tx;
    rtty_tx_start(&tx, &cfg, next_cb, echo_cb, src);
    while ((got = rtty_tx_fill(buf + n, n + block <= cap ? block : cap - n, &tx)) > 0) {
        n += got;
    }
    *count = n;
    return buf;
}

static float goertzel(const int16_t *x, size_t n, float hz) {
    float w = 2.0f * (float)M_PI * hz / RATE, c = 2.0f * cosf(w), s1 = 0, s2 = 0;

    for (size_t i = 0; i < n; i++) {
        float s0 = x[i] + c * s1 - s2;

        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* The tones the transmission should be: (mark, bits), each code framed */
typedef struct {
    bool  mark;
    float bits;
} tone_t;

static int frame(tone_t *t, int n, uint8_t code) {
    t[n++] = (tone_t){false, 1.0f};
    for (int b = 0; b < 5; b++) t[n++] = (tone_t){(code >> b) & 1, 1.0f};
    t[n++] = (tone_t){true, RTTY_TX_STOP_BITS};
    return n;
}

static void test_wave(bool reverse) {
    const char *text = "cq de on6ure 599";
    float       mark, space;
    size_t      count;
    source_t    src;
    tone_t      want[1024];
    uint8_t     codes[256];
    int         nw = 0, nc;

    rtty_tones(CENTER, SHIFT, rtty_mark_high(1, reverse), &mark, &space);
    int16_t *x = transmit(text, mark, space, 2048, &count, &src);

    /* What should go out: the idle, the diddles, CR LF, the text's codes, the tail */
    want[nw++] = (tone_t){true, RTTY_TX_IDLE_S * BAUD};
    for (int i = 0; i < RTTY_TX_DIDDLES; i++) nw = frame(want, nw, RTTY_ITA2_LTRS);
    nw = frame(want, nw, RTTY_ITA2_CR);
    nw = frame(want, nw, RTTY_ITA2_LF);
    nc = encode(text, codes);
    for (int i = 0; i < nc; i++) nw = frame(want, nw, codes[i]);
    want[nw++] = (tone_t){true, RTTY_TX_TAIL_BITS};

    double total = 0;
    for (int i = 0; i < nw; i++) total += want[i].bits;
    long want_n = lround(total * RATE / BAUD);
    CHECK(labs((long)count - want_n) <= 1, "%zu samples, %ld wanted", count, want_n);

    /* Each tone in the middle of each bit (the long idle and tail in bit-long pieces) */
    double spb = RATE / BAUD, at = 0;
    int    wrong = 0, bits = 0;
    float  worst = 1e9f;

    for (int i = 0; i < nw; i++) {
        double end = at + want[i].bits * spb;

        for (double b = at; b + 0.5 * spb <= end + 1e-6; b += spb) {
            size_t from = (size_t)(b + 0.2 * spb), len = (size_t)(0.6 * spb);
            float  pm, ps, db;

            if (b + spb > end + 1e-6 && want[i].bits - floor(want[i].bits) > 0.01) len = (size_t)(0.3 * spb);
            if (from + len > count) break;
            pm = goertzel(x + from, len, mark);
            ps = goertzel(x + from, len, space);
            db = 10.0f * log10f((want[i].mark ? pm : ps) / (want[i].mark ? ps : pm));
            if (db < worst) worst = db;
            if (db < 10.0f) wrong++;
            bits++;
        }
        at = end;
    }
    CHECK(bits > 200 && wrong == 0, "reverse %d: %d of %d bits not on their tone", reverse, wrong, bits);
    CHECK(worst > 15.0f, "reverse %d: the weakest bit only %.1f dB over the other tone", reverse, worst);

    /* The idle: its zero crossings give the mark (the fade in left out) */
    size_t from = (size_t)(0.02f * RATE), to = (size_t)(0.45f * RATE);
    int    cross = 0;
    double first = 0, last = 0;

    for (size_t i = from; i < to; i++) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            double t = i - 1 + (double)-x[i - 1] / (x[i] - x[i - 1]);

            if (!cross) first = t;
            last = t;
            cross++;
        }
    }
    double hz = (cross - 1) / ((last - first) / RATE);
    CHECK(fabs(hz - mark) < 0.5, "reverse %d: the idle at %.2f Hz, the mark %.1f", reverse, hz, mark);

    /* No jump at a bit edge: from one sample to the next at most what the higher tone moves */
    float step = 2.0f * (float)M_PI * fmaxf(mark, space) / RATE * RTTY_TX_AMP * 1.02f + 2.0f;
    int   peak = 0, jumps = 0;

    for (size_t i = 1; i < count; i++) {
        if (abs(x[i] - x[i - 1]) > step) jumps++;
        if (abs(x[i]) > peak) peak = abs(x[i]);
    }
    CHECK(jumps == 0, "reverse %d: %d jumps in the phase", reverse, jumps);
    CHECK(peak <= RTTY_TX_AMP + 1 && peak > RTTY_TX_AMP * 0.99f, "peak %d", peak);

    /* The fades: from 0 up over the first 5 ms, down to 0 over the last */
    size_t ramp = (size_t)lroundf(RTTY_TX_RAMP_S * RATE);
    int    head = 0, tail = 0;

    for (size_t i = 0; i < ramp / 4; i++) head = abs(x[i]) > head ? abs(x[i]) : head;
    for (size_t i = count - ramp / 4; i < count; i++) tail = abs(x[i]) > tail ? abs(x[i]) : tail;
    CHECK(abs(x[0]) < 50 && head < RTTY_TX_AMP * 0.2f, "the start: %d, %d in its first ms", x[0], head);
    CHECK(abs(x[count - 1]) < 50 && tail < RTTY_TX_AMP * 0.2f, "the end: %d, %d in its last ms", x[count - 1], tail);

    /* Told: the text as the far end prints it, each character before its first sample, after the one before's */
    CHECK(!strcmp(src.echo, "CQ DE ON6URE 599"), "told \"%s\"", src.echo);
    double start = (RTTY_TX_IDLE_S * BAUD + (RTTY_TX_DIDDLES + 2) * 7.5) * spb;
    bool   order = true;

    for (int i = 1; i < src.echo_n; i++) order = order && src.echo_at[i] >= src.echo_at[i - 1];
    CHECK(src.echo_n > 0 && src.echo_at[0] <= start + 1, "the first character told at %u, it starts at %.0f",
          src.echo_n ? src.echo_at[0] : 0, start);
    CHECK(order, "told out of order");
    free(x);
}

/* The same samples whatever the blocks: the state carries over from one fill to the next */
static void test_blocks(void) {
    const char *text = "RYRYRY 123 THE QUICK BROWN FOX\n";
    size_t      n1, n2, n3;
    source_t    s1, s2, s3;
    int16_t    *a = transmit(text, 885, 715, 2048, &n1, &s1);
    int16_t    *b = transmit(text, 885, 715, 7, &n2, &s2);
    int16_t    *c = transmit(text, 885, 715, 1, &n3, &s3);

    CHECK(n1 == n2 && n1 == n3, "%zu, %zu and %zu samples", n1, n2, n3);
    CHECK(n1 == n2 && !memcmp(a, b, n1 * sizeof(int16_t)), "blocks of 2048 and of 7 differ");
    CHECK(n1 == n3 && !memcmp(a, c, n1 * sizeof(int16_t)), "blocks of 2048 and of 1 differ");
    CHECK(!strcmp(s1.echo, "RYRYRY 123 THE QUICK BROWN FOX\n"), "told \"%s\"", s1.echo);
    free(a);
    free(b);
    free(c);
}

/* Nothing to send: the idle, the diddles, CR LF and the tail, then the end */
static void test_empty(void) {
    size_t   n;
    source_t s;
    int16_t *x = transmit("", 885, 715, 2048, &n, &s);
    double   bits = RTTY_TX_IDLE_S * BAUD + (RTTY_TX_DIDDLES + 2) * 7.5 + RTTY_TX_TAIL_BITS;

    CHECK(labs((long)n - lround(bits * RATE / BAUD)) <= 1, "%zu samples", n);
    CHECK(s.echo_n == 0, "told %d", s.echo_n);
    free(x);
}

/* ---- the macros ---- */

static void test_macros(void) {
    char dir[] = "/tmp/rtty-tx-test-XXXXXX";

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(rtty_test_macros_path, sizeof(rtty_test_macros_path), "%s/rtty_macros.txt", dir);
    rtty_macros_load();
    CHECK(rtty_macros_count() == RTTY_MACRO_MAX, "%d starter macros", rtty_macros_count());
    CHECK(!strcmp(rtty_macro_get(0)->name, "CQ"), "first: %s", rtty_macro_get(0)->name);
    CHECK(strstr(rtty_macro_get(0)->text, "<MYCALL>") != NULL, "the CQ without the callsign");
    CHECK(access(RTTY_MACROS_PATH, F_OK) == 0, "the starter set not written");
    for (int i = 0; i < rtty_macros_count(); i++) {
        uint8_t codes[600];
        rtty_enc_t e;
        int        dropped = 0;

        rtty_enc_init(&e);
        for (const char *p = rtty_macro_get(i)->text; *p; p++) {
            if (*p != '<' && *p != '>' && rtty_enc_char(&e, *p, codes) == 0) dropped++;
        }
        CHECK(!dropped, "macro %d has %d characters ITA2 lacks", i, dropped);
    }

    CHECK(rtty_macro_add("X", "Y") == -1, "a seventh macro taken");
    rtty_macro_delete(5);
    CHECK(rtty_macro_add("Contest", "<CALL> 599\t599") == 5, "not added at the end");
    rtty_macro_set(1, "Call", NULL);
    rtty_macro_delete(0);
    CHECK(rtty_macros_save(), "not saved");

    rtty_macros_load();
    CHECK(rtty_macros_count() == 5, "%d after the delete", rtty_macros_count());
    CHECK(!strcmp(rtty_macro_get(0)->name, "Call"), "renamed: %s", rtty_macro_get(0)->name);
    CHECK(!strcmp(rtty_macro_get(4)->text, "<CALL> 599 599"), "the tab: \"%s\"", rtty_macro_get(4)->text);
    CHECK(rtty_macro_get(5) == NULL && rtty_macro_get(-1) == NULL, "past the end");

    /* A file there is taken as it is: lines without a tab skipped, an empty file no macros */
    FILE *f = fopen(RTTY_MACROS_PATH, "w");
    fputs("A\tAAA\nno tab here\n\nB\tBBB\r\n", f);
    fclose(f);
    rtty_macros_load();
    CHECK(rtty_macros_count() == 2 && !strcmp(rtty_macro_get(1)->text, "BBB"), "%d read", rtty_macros_count());
    f = fopen(RTTY_MACROS_PATH, "w");
    fclose(f);
    rtty_macros_load();
    CHECK(rtty_macros_count() == 0, "%d from an empty file", rtty_macros_count());
    unlink(RTTY_MACROS_PATH);
    rmdir(dir);
}

int main(void) {
    test_ita2();
    test_tones();
    test_presets();
    test_wave(false);
    test_wave(true);
    test_blocks();
    test_empty();
    test_macros();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
