/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later AND BSD-2-Clause
 *
 *  Host test of src/freedv_text.c, the reader of the callsign in FreeDV's own form in RADE's
 *  End-of-Over (not part of the image; no RADE library, no radio). `make` in this folder runs it.
 *
 *  FreeDV's sender is ported here for the test only (rade_text_generate_tx_string() and
 *  ldpc_encode() of freedv-backend 49006b02; FreeDV's notice is in src/freedv_text.c). What it
 *  sends is checked first against reference symbols that FreeDV's own code made: freedv-backend's
 *  rade_text.cpp, ldpc_encode.cpp and ldpc_decode.cpp built on a PC, each frame also read back by
 *  FreeDV's own decoder (the text after "got" below). Then the frames go to the reader:
 *  without noise, scaled and turned, with noise, with symbols wrong, after them garbage of every
 *  kind, our own form, and frames FreeDV's sender never makes. It must read FreeDV's text or
 *  nothing, never another text.
 */

#include "freedv_text.c"
#include "freedv_text_frames.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the reference: FreeDV's own encoder, each frame read back by FreeDV's own decoder ---- */

/* Which of the four points a clean symbol is */
static int symbol_of(const float *s)
{
    return s[0] > 0.5f ? 0 : s[1] > 0.5f ? 1 : s[1] < -0.5f ? 2 : 3;
}

static const struct {
    const char *in, *got, *sym;
} ref[] = {
    {"K6AQ",         "K6AQ",     "13102310010000220310321303030000000010233312132000012221"},
    {"W1AW",         "W1AW",     "20201002100000121133013113300000010313132020211000012011"},
    {"VK2DVZ",       "VK2DVZ",   "22020300130031233131223030222002230121201233322001120211"},
    {"KG5SWP",       "KG5SWP",   "01100101010001231322023131000003113110033212033001110221"},
    {"VK4RGG1",      "VK4RGG1",  "11323201230111233122311121022031121122022310232002210212"},
    {"AA1ZZZZZ",     "AA1ZZZZZ", "22013113003333312230021021001222223001100332101111110222"},
    {"A",            "A",        "23300132030000000233333230133000000001303133023000000021"},
    {"",             "",         "00000000000000000000000000000000000000000000000000000000"},
    {"ON6URE",       "ON6URE",   "30221001210012100010031211102000200212222113200002112110"},
    {"on4abc",       "ON4ABC",   "13322133210022230033320213221001201210302012231002220111"},
    {"2E0JFS",       "2E0JFS",   "32121202010021311322333103321001221001320200120001220203"},
    {"K6AQ/5",       "K6AQ/5",   "21030203300033220303222200113003100012213121031000112220"},
    {"F/ON6URE",     "F/ON6URE", "23231133301210003103233220002020021123130130113211211122"},
    {"DL1ABC/P",     "DL1ABC/P", "11223032020322213230212303113311203332012232020112220221"},
    {"LONGCALLSIGN", "LONGCALL", "10303110223322100323310103310330112133003322113222221120"},
    {"ON6 URE",      "ON6URE",   "30221001210012100010031211102000200212222113200002112110"},
    {"N0CALL-1",     "N0CALL-1", "00222321001133221022023013212303301121103220032002222011"},
    {"M7GQH",        "M7GQH",    "02001221330001210033311312202000301202222210102000212210"},
    {"&'()*+,-.",    "&'()*+,-", "20231201201222200020130031230031203123113021213000000002"},
    {"9A1A/MM",      "9A1A/MM",  "12333022230003212013312032330000103032230221132011120220"},
};
#define NREF ((int)(sizeof(ref) / sizeof(ref[0])))

static void test_reference(void)
{
    int ok = 0;

    for (int r = 0; r < NREF; r++) {
        float eoo[EOO_N];
        char  sym[NSYM + 1], text[FREEDV_TEXT_MAX + 1];
        bool  fill = true;

        fdv_encode(ref[r].in, eoo);
        for (int k = 0; k < NSYM; k++)
            sym[k] = (char)('0' + symbol_of(&eoo[2 * k]));
        sym[NSYM] = '\0';
        for (int i = NBITS; i < EOO_N; i++)
            fill &= eoo[i] == (i % 2 ? 0.0f : 1.0f);
        CHECK(!strcmp(sym, ref[r].sym) && fill, "\"%s\": sends %s, FreeDV %s", ref[r].in, sym, ref[r].sym);
        freedv_text_result_t res = freedv_text_read(eoo, EOO_N, text, NULL);
        CHECK(res == FREEDV_TEXT_READ && !strcmp(text, ref[r].got), "\"%s\": read %d \"%s\", FreeDV read \"%s\"",
              ref[r].in, res, text, ref[r].got);
        ok += !strcmp(sym, ref[r].sym) && res == FREEDV_TEXT_READ && !strcmp(text, ref[r].got);
    }
    printf("reference: %d of %d frames as FreeDV's encoder sends them, each read as FreeDV's decoder reads it\n",
           ok, NREF);
}

/* Without noise: any amplitude, a phase error short of 45 degrees, just 112 floats, rade_c's junk after 120 */
static void test_clean(void)
{
    static const float scale[] = {1e-4f, 0.03f, 1.0f, 70.0f, 3e4f};
    static const float turn[] = {-30.0f, -10.0f, 0.0f, 10.0f, 30.0f};
    int n = 0, ok = 0;

    for (int r = 0; r < NREF; r++)
        for (int a = 0; a < 5; a++)
            for (int t = 0; t < 5; t++) {
                float  eoo[EOO_N], c = cosf(turn[t] * (float)M_PI / 180.0f), s = sinf(turn[t] * (float)M_PI / 180.0f);
                char   text[FREEDV_TEXT_MAX + 1];

                fdv_encode(ref[r].in, eoo);
                for (int k = 0; k < EOO_N / 2; k++) {
                    float x = eoo[2 * k], y = eoo[2 * k + 1];

                    eoo[2 * k] = scale[a] * (x * c - y * s);
                    eoo[2 * k + 1] = scale[a] * (x * s + y * c);
                }
                junk_tail(eoo);
                int m = (a + t) % 2 ? EOO_N : FREEDV_TEXT_FLOATS;
                freedv_text_result_t res = freedv_text_read(eoo, m, text, NULL);

                n++;
                ok += res == FREEDV_TEXT_READ && !strcmp(text, ref[r].got);
                CHECK(res == FREEDV_TEXT_READ && !strcmp(text, ref[r].got), "\"%s\" x%g turned %g: %d \"%s\"",
                      ref[r].in, scale[a], turn[t], res, text);
            }
    printf("clean: %d of %d read (scaled 1e-4 to 3e4, turned up to 30 degrees, 112 or 180 floats)\n", ok, n);
}

/* Noise on each float, the symbols of amplitude 1: sigma 0.3 is Es/N0 7.4 dB */
static void test_noise(void)
{
    static const float sigma[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 1.0f, 1.5f, 2.0f};
    static const int   need[]  = {100,  100,  100,  97,   80,   0,    0,    0,    0,    0,    0};   /* % read */
    const int trials = 400;

    printf("noise   Es/N0    read  nothing  wrong  (of %d, %d callsigns)\n", trials, NREF);
    for (int g = 0; g < (int)(sizeof(sigma) / sizeof(sigma[0])); g++) {
        int read = 0, none = 0, wrong = 0;

        for (int t = 0; t < trials; t++) {
            int   r = t % NREF;
            float eoo[EOO_N];
            char  text[FREEDV_TEXT_MAX + 1];

            fdv_encode(ref[r].in, eoo);
            for (int i = 0; i < FILL_END; i++)
                eoo[i] += sigma[g] * (float)gauss();
            junk_tail(eoo);
            freedv_text_result_t res = freedv_text_read(eoo, EOO_N, text, NULL);

            if (res != FREEDV_TEXT_READ)
                none++;
            else if (!strcmp(text, ref[r].got))
                read++;
            else
                wrong++;
            /* deep in the noise the shape is the noise's, and may look like ours: our reader then reads noise */
            CHECK(res != FREEDV_TEXT_OTHER || sigma[g] > 0.75f, "sigma %g: FreeDV's form not taken for FreeDV's",
                  sigma[g]);
        }
        printf("%5.2f  %5.1f dB  %5d  %7d  %5d\n", sigma[g], 10.0 * log10(1.0 / (2.0 * sigma[g] * sigma[g])),
               read, none, wrong);
        CHECK(wrong == 0, "sigma %g: %d wrong", sigma[g], wrong);
        CHECK(read * 100 >= need[g] * trials, "sigma %g: %d read, %d%% needed", sigma[g], read, need[g]);
    }
}

/* Symbols on the wrong point, the rest clean */
static void test_symbol_errors(void)
{
    printf("wrong symbols  read  nothing  wrong  (of 400)\n");
    for (int e = 1; e <= 16; e *= 2) {
        int read = 0, none = 0, wrong = 0;

        for (int t = 0; t < 400; t++) {
            int   r = t % NREF;
            float eoo[EOO_N];
            char  text[FREEDV_TEXT_MAX + 1];
            bool  hit[NSYM] = {false};

            fdv_encode(ref[r].in, eoo);
            for (int k = 0; k < e; k++) {
                int s;

                do
                    s = (int)(rnd32() % NSYM);
                while (hit[s]);
                hit[s] = true;
                /* to one of the three other points */
                static const float pts[4][2] = {{1, 0}, {0, 1}, {0, -1}, {-1, 0}};
                int now = symbol_of(&eoo[2 * s]), to = (now + 1 + (int)(rnd32() % 3)) % 4;

                eoo[2 * s] = pts[to][0];
                eoo[2 * s + 1] = pts[to][1];
            }
            freedv_text_result_t res = freedv_text_read(eoo, EOO_N, text, NULL);

            if (res != FREEDV_TEXT_READ)
                none++;
            else if (!strcmp(text, ref[r].got))
                read++;
            else
                wrong++;
        }
        printf("%13d  %4d  %7d  %5d\n", e, read, none, wrong);
        CHECK(wrong == 0, "%d wrong symbols: %d wrong", e, wrong);
        CHECK(e != 1 || read == 400, "one wrong symbol: only %d read", read);
        CHECK(e != 2 || read >= 390, "two wrong symbols: only %d read", read);
    }
}

/* Garbage of every kind. The code converges on some of it (a short code: a few in a hundred of pure noise), and its
   CRC-8 lets one in 256 of those through, the padding and the character set fewer still: a few in 100000 are read
   here. freedv_rx.c then shows only what reads as a callsign (tools/freedv_text/freedv_text_e2e.c) */
static void test_garbage(void)
{
    const int trials = 30000;
    int read[6] = {0}, conv[6] = {0}, other[6] = {0};
    static const char *what[6] = {
        "Gaussian noise in all 120 floats", "random points on the axes (not a code word)",
        "random +-1 in all 120 floats", "noise, a constant added (an unbalanced demodulator)",
        "all zeros", "random code words (decodes; the CRC and the rest must catch them)",
    };

    for (int kind = 0; kind < 6; kind++)
        for (int t = 0; t < (kind == 4 ? 1 : trials); t++) {
            float eoo[EOO_N];
            char  text[FREEDV_TEXT_MAX + 1];
            freedv_text_diag_t dg;

            for (int i = 0; i < FILL_END; i++) {
                switch (kind) {
                case 0: eoo[i] = (float)gauss(); break;
                case 2: eoo[i] = rnd32() & 1 ? 1.0f : -1.0f; break;
                case 3: eoo[i] = 0.7f + (float)gauss(); break;
                default: eoo[i] = 0.0f; break;
                }
            }
            if (kind == 1)
                for (int s = 0; s < FILL_END / 2; s++) {
                    static const float pts[4][2] = {{1, 0}, {0, 1}, {0, -1}, {-1, 0}};
                    int v = (int)(rnd32() % 4);

                    eoo[2 * s] = pts[v][0];
                    eoo[2 * s + 1] = pts[v][1];
                }
            if (kind == 5) {
                uint8_t code[8];
                uint32_t w = rnd32();

                for (int c = 0; c < 8; c++)
                    code[c] = (uint8_t)(rnd32() & 63);
                fdv_frame((uint8_t)w, code, eoo);
            }
            junk_tail(eoo);
            freedv_text_result_t res = freedv_text_read(eoo, EOO_N, text, &dg);

            read[kind] += res == FREEDV_TEXT_READ;
            conv[kind] += dg.converged;
            other[kind] += res == FREEDV_TEXT_OTHER;
            if (kind == 5 && res == FREEDV_TEXT_READ && read[kind] <= 3)
                printf("  a random code word that passes every check, as FreeDV's sender could make it: \"%s\"\n", text);
        }
    for (int kind = 0; kind < 6; kind++) {
        int n = kind == 4 ? 1 : trials;

        printf("garbage, %s: %d of %d decoded, %d read, %d taken as our form\n", what[kind], conv[kind], n,
               read[kind], other[kind]);
        if (kind < 5)
            CHECK(read[kind] * 5000 <= n, "%s: %d read, more than one in 5000", what[kind], read[kind]);
    }
    /* Random code words pass the CRC-8 one time in 256, and then the padding and the character set must hold too */
    CHECK(read[5] * 2000 <= trials, "random code words: %d read, more than one in 2000", read[5]);
}

/* Our form must go to our reader (FREEDV_TEXT_OTHER), with noise too; FreeDV's never */
static void test_our_form(void)
{
    static const char *calls[] = {"ON6URE", "K6AQ", "W1AW", "OM0WT", "2E0JFS", "VK2DVZ/P", "A", "", "ZZZZZZZZ",
                                  "00000000", "TEST"};
    int n = 0, other = 0, read = 0;
    float worst = 0.0f;

    for (int c = 0; c < (int)(sizeof(calls) / sizeof(calls[0])); c++)
        for (int g = 0; g <= 4; g++)
            for (int t = 0; t < 200; t++) {
                float eoo[EOO_N];
                char  text[FREEDV_TEXT_MAX + 1];
                freedv_text_diag_t dg;

                our_encode(calls[c], eoo);
                for (int i = 0; i < FILL_END; i++)
                    eoo[i] += 0.1f * (float)g * (float)gauss();
                junk_tail(eoo);
                freedv_text_result_t res = freedv_text_read(eoo, EOO_N, text, &dg);

                n++;
                other += res == FREEDV_TEXT_OTHER;
                read += res == FREEDV_TEXT_READ;
                if (dg.fill > worst)
                    worst = dg.fill;
            }
    printf("our form (noise up to 0.4 on +-1): %d of %d to our reader, %d read as FreeDV's; fill at most %.2f\n",
           other, n, read, worst);
    CHECK(other == n && read == 0, "our form: %d of %d to our reader, %d read as FreeDV's", other, n, read);
}

/* Frames FreeDV's sender never makes, and FreeDV's older '/' */
static void test_crafted(void)
{
    uint8_t k6aq[8] = {to_code('K'), to_code('6'), to_code('A'), to_code('Q')};
    uint8_t crc = crc8(k6aq, 8);
    float   eoo[EOO_N];
    char    text[FREEDV_TEXT_MAX + 1];

    for (int b = 0; b < 8; b++) {
        fdv_frame(crc ^ (1u << b), k6aq, eoo);
        CHECK(freedv_text_read(eoo, EOO_N, text, NULL) == FREEDV_TEXT_UNREAD, "CRC bit %d wrong: read \"%s\"", b, text);
    }
    for (uint8_t c = 47; c < 64; c++) {
        uint8_t code[8] = {to_code('K'), c, to_code('6'), to_code('A'), 0};

        fdv_frame(crc8(code, 8), code, eoo);
        CHECK(freedv_text_read(eoo, EOO_N, text, NULL) == FREEDV_TEXT_UNREAD, "code %u: read \"%s\"", c, text);
    }
    {
        uint8_t code[8] = {to_code('K'), to_code('6'), 0, to_code('A'), to_code('Q')};

        fdv_frame(crc8(code, 8), code, eoo);
        CHECK(freedv_text_read(eoo, EOO_N, text, NULL) == FREEDV_TEXT_UNREAD, "text after the 0: read \"%s\"", text);
    }
    {
        /* FreeDV before July 2026: '/' went out as 10, the code of '0' */
        uint8_t code[8] = {to_code('K'), to_code('6'), to_code('A'), to_code('Q'), 10, to_code('5')};

        fdv_frame(crc8(code, 8), code, eoo);
        CHECK(freedv_text_read(eoo, EOO_N, text, NULL) == FREEDV_TEXT_READ && !strcmp(text, "K6AQ05"),
              "the old '/': \"%s\"", text);
    }
    fdv_encode("K6AQ", eoo);
    CHECK(freedv_text_read(eoo, FREEDV_TEXT_FLOATS - 1, text, NULL) == FREEDV_TEXT_OTHER, "111 floats");
    CHECK(freedv_text_read(NULL, EOO_N, text, NULL) == FREEDV_TEXT_OTHER, "no floats");
    eoo[5] = NAN;
    CHECK(freedv_text_read(eoo, EOO_N, text, NULL) == FREEDV_TEXT_UNREAD, "a NaN in the frame");
    printf("crafted: wrong CRC, codes 47-63, text after the end refused; the old '/' reads as '0'\n");
}

int main(void)
{
    test_reference();
    test_clean();
    test_noise();
    test_symbol_errors();
    test_garbage();
    test_our_form();
    test_crafted();
    printf(fails ? "freedv_text_test: %d FAILED\n" : "freedv_text_test: all passed\n", fails);
    return fails != 0;
}
