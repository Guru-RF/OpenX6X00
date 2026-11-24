/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/audio_meter.c, the Internet radio's station on the S-meter box (#360):
 *  - a piece's level: silence and nothing at the floor, a full-scale square wave 0 dB RMS, a full-scale sine -3, one
 *    a tenth of it -23, its highest sample as the peak (-32768 too), a hiss under the floor at the floor;
 *  - the motion: the bar up at once, down 20 dB a second, never under the floor nor over 0; the peak held 1.5 s, then
 *    falling as fast, never under the bar; several levels due at one tick taken as the loudest;
 *  - the scale: -60, -40, -20, -10 and 0 dB of full scale on S1, S5, S9, +20 and +40, every bar of the bars (3 dB to
 *    S9, 5 over it) 2.5 dB and every tick of the needle (an S unit, 10 dB over S9) 5 dB, clamped at both ends, rising;
 *  - the queue: levels taken in order once due and not before, 64 at most, an older turn's dropped at once (due or
 *    not) so the next station's do not wait behind them, the counters past their wrap, and one thread putting while
 *    another takes, nothing lost or out of order;
 *  - src/smeter.c's marks (meter_audio_marks, which every style of the box and Ops Room draw): each one's text the dB
 *    of full scale of its place, and the manual's Internet radio part naming them as they are.
 *    audio_meter_test <smeter.c> <MANUAL.md>
 */

#include "audio_meter.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

static void check(int ok, const char *what, double v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %g\n", what, v);
    }
}

static int near(double a, double b, double tol) {
    return fabs(a - b) <= tol;
}

/* smeter.h's S scale */
#define S1    (-121)
#define S9    (-73)
#define S9_20 (S9 + 20)
#define S9_40 (S9 + 40)

static void test_measure(void) {
    static int16_t s[4410];
    audio_level_t  l;

    l = audio_meter_measure(s, 0);
    check(l.rms == AUDIO_METER_FLOOR && l.peak == AUDIO_METER_FLOOR, "nothing: the floor", l.rms);
    l = audio_meter_measure(s, 2048);
    check(l.rms == AUDIO_METER_FLOOR && l.peak == AUDIO_METER_FLOOR, "silence: the floor", l.rms);

    for (int i = 0; i < 2048; i++) s[i] = i % 2 ? 32767 : -32767;
    l = audio_meter_measure(s, 2048);
    check(near(l.rms, 0.0, 0.01), "full-scale square: RMS 0 dB", l.rms);
    check(near(l.peak, 0.0, 0.01), "full-scale square: peak 0 dB", l.peak);

    for (int i = 0; i < 4410; i++) s[i] = (int16_t)lrint(32767 * sin(2 * M_PI * 1000 * i / 44100.0));
    l = audio_meter_measure(s, 4410);
    check(near(l.rms, -3.01, 0.05), "full-scale sine: RMS -3 dB", l.rms);
    check(near(l.peak, 0.0, 0.01), "full-scale sine: peak 0 dB", l.peak);

    for (int i = 0; i < 4410; i++) s[i] = (int16_t)lrint(3277 * sin(2 * M_PI * 1000 * i / 44100.0));
    l = audio_meter_measure(s, 4410);
    check(near(l.rms, -23.01, 0.05), "sine a tenth of full scale: RMS -23 dB", l.rms);
    check(near(l.peak, -20.0, 0.05), "sine a tenth of full scale: peak -20 dB", l.peak);

    memset(s, 0, sizeof(s));
    s[100] = -32768;
    l = audio_meter_measure(s, 2048);
    check(near(l.peak, 0.0, 0.001), "a sample at -32768: peak 0 dB", l.peak);
    check(near(l.rms, 20 * log10(1 / sqrt(2048)), 0.05), "one loud sample in 2048: its RMS", l.rms);

    for (int i = 0; i < 2048; i++) s[i] = i % 2 ? 8 : -8;     /* -72 dB */
    l = audio_meter_measure(s, 2048);
    check(l.rms == AUDIO_METER_FLOOR && l.peak == AUDIO_METER_FLOOR, "a hiss under -60 dB: the floor", l.rms);
}

static void test_motion(void) {
    audio_meter_t m;
    audio_level_t loud = {-10.0f, -2.0f}, soft = {-30.0f, -25.0f}, over = {5.0f, 6.0f};

    audio_meter_reset(&m, 1000);
    check(m.bar == AUDIO_METER_FLOOR && m.peak == AUDIO_METER_FLOOR, "reset: the floor", m.bar);
    audio_meter_step(&m, &loud, 1000);
    check(m.bar == -10.0f && m.peak == -2.0f, "up at once", m.bar);
    audio_meter_step(&m, NULL, 1500);
    check(near(m.bar, -20.0, 0.001), "down 20 dB a second (half a second: 10 dB)", m.bar);
    check(m.peak == -2.0f, "the peak held", m.peak);
    audio_meter_step(&m, &soft, 1500);
    check(near(m.bar, -20.0, 0.001), "a softer level: the bar falls on", m.bar);
    audio_meter_step(&m, NULL, 2500);
    check(m.peak == -2.0f, "the peak held 1.5 s", m.peak);
    audio_meter_step(&m, NULL, 2600);
    check(near(m.peak, -4.0, 0.001), "then falling as the bar", m.peak);
    audio_meter_step(&m, NULL, 9000);
    check(m.bar == AUDIO_METER_FLOOR && m.peak == AUDIO_METER_FLOOR, "silence: both at the floor", m.bar);
    check(m.peak >= m.bar, "the peak never under the bar", m.peak);

    audio_meter_step(&m, &over, 9000);
    check(m.bar == 0.0f && m.peak == 0.0f, "never over 0 dB", m.bar);

    /* Several due at one tick: the loudest, the peak the highest */
    audio_meter_reset(&m, 0);
    audio_meter_step(&m, &soft, 100);
    audio_meter_step(&m, &loud, 100);
    audio_meter_step(&m, &soft, 100);
    check(m.bar == -10.0f && m.peak == -2.0f, "three at one tick: the loudest", m.bar);

    /* The peak never under the bar, also when the bar's RMS tops a held peak's fall */
    audio_meter_reset(&m, 0);
    audio_level_t flat = {-12.0f, -12.0f};
    audio_meter_step(&m, &flat, 0);
    audio_meter_step(&m, NULL, 3000);
    check(m.peak >= m.bar, "peak and bar falling together", m.peak - m.bar);

    /* A clock that went back (two ticks at one ms) takes nothing off */
    audio_meter_reset(&m, 500);
    audio_meter_step(&m, &loud, 500);
    audio_meter_step(&m, NULL, 400);
    check(m.bar == -10.0f, "no time back", m.bar);
}

static int16_t bar_db(int i) {     /* smeter.c's bars */
    return i <= 16 ? S1 + 3 * i : S9 + 5 * (i - 16);
}

static void test_scale(void) {
    check(near(audio_meter_to_s(-60), S1, 1e-4), "-60 dB on S1", audio_meter_to_s(-60));
    check(near(audio_meter_to_s(-40), S1 + 24, 1e-4), "-40 dB on S5", audio_meter_to_s(-40));
    check(near(audio_meter_to_s(-20), S9, 1e-4), "-20 dB on S9", audio_meter_to_s(-20));
    check(near(audio_meter_to_s(-10), S9_20, 1e-4), "-10 dB on S9+20", audio_meter_to_s(-10));
    check(near(audio_meter_to_s(0), S9_40, 1e-4), "0 dB on S9+40", audio_meter_to_s(0));
    check(near(audio_meter_to_s(-90), S1, 1e-4), "under the floor: S1", audio_meter_to_s(-90));
    check(near(audio_meter_to_s(6), S9_40, 1e-4), "over 0 dB: S9+40", audio_meter_to_s(6));
    for (int i = 0; i <= 24; i++) {
        check(near(audio_meter_to_s(-60 + 2.5f * i), bar_db(i), 1e-3), "a bar 2.5 dB", i);
    }
    for (int k = 0; k <= 8; k++) {
        check(near(audio_meter_to_s(-60 + 5.0f * k), S1 + 6 * k, 1e-3), "an S unit's tick 5 dB", k);
    }
    for (int k = 1; k <= 4; k++) {
        check(near(audio_meter_to_s(-20 + 5.0f * k), S9 + 10 * k, 1e-3), "a tick over S9 5 dB", k);
    }
    float last = -1000;
    int   rising = 1;

    for (float d = -70; d <= 5; d += 0.25f) {
        float s = audio_meter_to_s(d);

        rising &= s >= last;
        last = s;
    }
    check(rising, "rising along the scale", last);
}

static audio_level_t lv(float rms) {
    return (audio_level_t){rms, rms + 3};
}

static void test_ring(void) {
    static audio_meter_ring_t r;
    audio_level_t             l;
    int                       n;

    memset(&r, 0, sizeof(r));
    check(!audio_meter_take(&r, 1000, 1, &l), "empty: nothing", 0);
    audio_meter_put(&r, 100, lv(-30), 1);
    audio_meter_put(&r, 150, lv(-20), 1);
    audio_meter_put(&r, 200, lv(-10), 1);
    check(!audio_meter_take(&r, 99, 1, &l), "not due: not taken", 99);
    check(audio_meter_take(&r, 160, 1, &l) && l.rms == -30, "due: the oldest first", l.rms);
    check(audio_meter_take(&r, 160, 1, &l) && l.rms == -20, "then the next", l.rms);
    check(!audio_meter_take(&r, 160, 1, &l), "the third not due yet", 160);
    check(audio_meter_take(&r, 200, 1, &l) && l.rms == -10, "due at its ms", l.rms);

    /* Full at 64 */
    for (n = 0; audio_meter_put(&r, 1000 + n, lv(-40), 1); n++) {
    }
    check(n == AUDIO_METER_RING, "64 queued at most", n);
    for (n = 0; audio_meter_take(&r, 5000, 1, &l); n++) {
    }
    check(n == AUDIO_METER_RING, "all 64 taken", n);

    /* An older turn's dropped at once, due or not; the new turn's next */
    audio_meter_put(&r, 9000, lv(-5), 1);
    audio_meter_put(&r, 9000, lv(-6), 1);
    audio_meter_put(&r, 300, lv(-15), 2);
    check(audio_meter_take(&r, 300, 2, &l) && l.rms == -15, "the old turn's gone, the new one's level due", l.rms);
    check(!audio_meter_take(&r, 100000, 2, &l), "nothing else", 0);
    audio_meter_put(&r, 10, lv(-25), 2);
    check(!audio_meter_take(&r, 100000, 3, &l), "a level of turn 2 after the end (turn 3): dropped", 0);

    /* The counters wrap */
    atomic_store(&r.head, 0xfffffff0u);
    atomic_store(&r.tail, 0xfffffff0u);
    for (int i = 0; i < 40; i++) {
        audio_meter_put(&r, i, lv(-50 + i), 4);
    }
    int order = 1;

    for (int i = 0; i < 40; i++) {
        order &= audio_meter_take(&r, 1000, 4, &l) && l.rms == -50 + i;
    }
    check(order, "past the counters' wrap: in order", 0);
}

/* One thread puts 200000 levels as fast as it can (they say their number), the other takes them: none lost, none
 * twice, in order */
#define MANY 200000
static audio_meter_ring_t big;

static void *putter(void *arg) {
    (void)arg;
    for (int i = 0; i < MANY; i++) {
        while (!audio_meter_put(&big, 0, (audio_level_t){(float)i, 0}, 7)) {
        }
    }
    return NULL;
}

static void test_threads(void) {
    pthread_t     t;
    audio_level_t l;
    int           next = 0, ok = 1;

    memset(&big, 0, sizeof(big));
    pthread_create(&t, NULL, putter, NULL);
    while (next < MANY) {
        if (audio_meter_take(&big, 1, 7, &l)) {
            ok &= l.rms == (float)next;
            next++;
        }
    }
    pthread_join(t, NULL);
    check(ok, "two threads: every level once, in order", next);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    char *s;
    long  n;

    if (!f) {
        printf("FAIL cannot read %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    s = malloc(n + 1);
    s[fread(s, 1, n, f)] = 0;
    fclose(f);
    return s;
}

/* The place of a mark as smeter.c writes it */
static int s_place(const char *e, int *db) {
    static const struct {
        const char *name;
        int         db;
    } names[] = {{"S1 + 24", S1 + 24}, {"S9_20", S9_20}, {"S9_40", S9_40}, {"S1", S1}, {"S9", S9}};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strncmp(e, names[i].name, strlen(names[i].name)) && e[strlen(names[i].name)] == ',') {
            *db = names[i].db;
            return 1;
        }
    }
    return 0;
}

static void test_marks(const char *smeter_c, const char *manual) {
    char       *src = slurp(smeter_c), *man = slurp(manual);
    const char *p   = strstr(src, "meter_audio_marks[METER_AUDIO_MARKS] = {");
    char        words[128] = "";
    int         n = 0;

    check(p != NULL, "smeter.c has meter_audio_marks", 0);
    if (!p) {
        return;
    }
    p = strchr(p, '{') + 1;
    const char *end = strstr(p, "};");

    for (const char *q = strchr(p, '{'); q && q < end; q = strchr(q + 1, '{')) {
        int  db;
        char txt[16];

        q++;
        while (*q == ' ' || *q == '\n') q++;
        if (!s_place(q, &db) || sscanf(strchr(q, '"'), "\"%15[^\"]\"", txt) != 1) {
            check(0, "a mark smeter.c writes as this test does not read it", n);
            continue;
        }
        check(near(audio_meter_to_s((float)atoi(txt)), db, 1e-3), "a mark's text its place's dB of full scale", n);
        snprintf(words + strlen(words), sizeof(words) - strlen(words), "%s%s", n ? ", " : "", txt);
        n++;
    }
    check(n == 5, "five marks", n);

    /* "-60, -40, -20, -10 and 0 dB of full scale", within the Internet radio part, its lines taken as one: every run
     * of spaces and line ends one space */
    char *last = strrchr(words, ',');

    if (last) {
        memmove(last + 4, last + 1, strlen(last + 1) + 1);
        memcpy(last, " and", 4);
    }
    strcat(words, " dB of full scale");

    char *part = strstr(man, "\n### Internet radio\n"), *next = part ? strstr(part + 1, "\n### ") : NULL;

    check(part != NULL, "the manual has its Internet radio part", 0);
    if (part) {
        char *w = part;

        if (next) {
            *next = 0;
        }
        for (const char *r = part; *r; r++) {
            if (*r == ' ' || *r == '\n') {
                if (w > part && w[-1] == ' ') {
                    continue;
                }
                *w++ = ' ';
            } else {
                *w++ = *r;
            }
        }
        *w = 0;
        check(strstr(part, words) != NULL, "the manual's Internet radio part names the marks", 0);
        if (!strstr(part, words)) {
            printf("     (looked for \"%s\")\n", words);
        }
    }
    free(src);
    free(man);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "audio_meter_test <smeter.c> <MANUAL.md>\n");
        return 2;
    }
    test_measure();
    test_motion();
    test_scale();
    test_ring();
    test_threads();
    test_marks(argv[1], argv[2]);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
