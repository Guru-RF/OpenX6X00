/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later AND BSD-2-Clause
 *
 *  Host test of the RADE app's End-of-Over callsign end to end (not part of the image; no radio):
 *  src/freedv_rx.c's freedv_rx_eoo_callsign(), what the app shows, and the RADE modem in between,
 *  rade_c as tools/rade/build.sh builds it for this PC. `make` in this folder builds and runs it.
 *
 *    1. The app's reading on its own: FreeDV's form and ours with noise, and garbage. What it
 *       shows must be the callsign sent or nothing.
 *    2. Through the modem: rade_tx_set_eoo_bits() -> modem frames -> rade_tx_eoo() -> the real
 *       part (the radio's audio is real) with a frequency offset and noise at a 3 kHz SNR ->
 *       rade_rx() -> its EOO soft bits -> freedv_rx_eoo_callsign(). FreeDV's form and ours,
 *       an EOO without bits, and one of random bits.
 *    3. Through freedv_rx.c's threads, as the app runs them: the overs at 11025 Hz into
 *       freedv_rx_put_audio(), the callsign from freedv_rx_get_stats().
 */

#include "freedv_text.c"
#include "freedv_text_frames.h"

#include "audio.h"
#include "freedv_rx.h"
#include "olivia_resamp.h"
#include "rade_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The audio.c this test does not link: the headphones take the speech and drop it */
bool audio_play_bt_begin(void) { return true; }
void audio_play_bt_end(void) {}
void audio_play_flush(void) {}
int  audio_play(int16_t *buf, size_t n) { (void)buf; (void)n; return 0; }

enum { FORM_FREEDV, FORM_OURS, FORM_ZEROS, FORM_RANDOM };
static const char *form_name[] = {"FreeDV's", "ours", "no bits", "random bits"};

static const char *calls[] = {"ON6URE", "K6AQ/5", "W1AW", "AA1ZZZZZ", "2E0JFS", "VK2DVZ"};
#define NCALLS ((int)(sizeof(calls) / sizeof(calls[0])))

static void make_bits(int form, const char *call, float eoo[EOO_N])
{
    switch (form) {
    case FORM_FREEDV: fdv_encode(call, eoo); break;
    case FORM_OURS:   our_encode(call, eoo); break;
    case FORM_ZEROS:  memset(eoo, 0, sizeof(float) * EOO_N); break;
    default:
        for (int i = 0; i < EOO_N; i++)
            eoo[i] = rnd32() & 1 ? 1.0f : -1.0f;
        break;
    }
}

/* What the app should show: the callsign, or nothing */
static const char *expected(int form, const char *call)
{
    return form == FORM_FREEDV || form == FORM_OURS ? call : "";
}

/* ---- 1. the app's reading on its own ---- */

static void test_direct(void)
{
    static const float sig_fdv[] = {0.3f, 0.5f, 0.6f, 0.7f, 0.8f, 1.0f};
    static const float sig_our[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.6f};
    const int n = 2000;

    printf("1. freedv_rx_eoo_callsign() on its own (%d frames each, the symbols of amplitude 1)\n", n);
    for (int form = FORM_FREEDV; form <= FORM_OURS; form++) {
        const float *sig = form == FORM_FREEDV ? sig_fdv : sig_our;
        int          ns = form == FORM_FREEDV ? 6 : 5;

        for (int g = 0; g < ns; g++) {
            int right = 0, none = 0, wrong = 0;

            for (int t = 0; t < n; t++) {
                const char *c = calls[t % NCALLS];
                float       eoo[EOO_N];
                char        out[FREEDV_RX_CALL_MAX + 1];

                make_bits(form, c, eoo);
                for (int i = 0; i < FILL_END; i++)
                    eoo[i] += sig[g] * (float)gauss();
                junk_tail(eoo);
                freedv_rx_eoo_callsign(eoo, EOO_N, out);
                if (!out[0])
                    none++;
                else if (!strcmp(out, c))
                    right++;
                else if (++wrong <= 3 && form == FORM_FREEDV)
                    printf("   sent %s, shown %s\n", c, out);
            }
            printf("   %-8s noise %.1f: %4d right, %4d nothing, %3d wrong\n", form_name[form], sig[g], right, none, wrong);
            /* FreeDV's form: the CRC-8 lets one wrong code word in 256 through, and the padding, the character set and
               the look of a callsign about one in four of those: about one in 100000 frames as noisy as 0.6 to 0.8
               gives another callsign (1.5 million of them on a PC). None here where the code reads most frames */
            if (form == FORM_FREEDV)
                CHECK(sig[g] > 0.65f ? wrong * 1000 <= n : wrong == 0, "FreeDV's form, noise %g: %d wrong", sig[g], wrong);
            /* ours has no check: a bit error that still makes a callsign is shown */
            else if (sig[g] <= 0.2f)
                CHECK(right == n, "our form, noise %g: %d right", sig[g], right);
        }
    }
    static const char *what[] = {"Gaussian noise", "random points on the axes", "noise and a constant"};
    const int m = 50000;

    for (int kind = 0; kind < 3; kind++) {
        int shown = 0;

        for (int t = 0; t < m; t++) {
            float eoo[EOO_N];
            char  out[FREEDV_RX_CALL_MAX + 1];

            for (int s = 0; s < FILL_END / 2; s++) {
                static const float pts[4][2] = {{1, 0}, {0, 1}, {0, -1}, {-1, 0}};
                int v = (int)(rnd32() % 4);

                eoo[2 * s] = kind == 1 ? pts[v][0] : (kind == 2 ? 0.7f : 0.0f) + (float)gauss();
                eoo[2 * s + 1] = kind == 1 ? pts[v][1] : (float)gauss();
            }
            junk_tail(eoo);
            freedv_rx_eoo_callsign(eoo, EOO_N, out);
            if (out[0] && shown++ < 3)
                printf("   garbage shown as \"%s\"\n", out);
        }
        printf("   garbage, %s: %d of %d shown\n", what[kind], shown, m);
        CHECK(shown == 0, "garbage, %s: %d shown", what[kind], shown);
    }
}

/* ---- 2. through the modem ---- */

#define NFRAMES     12          /* modem frames of 120 ms before the EOO: 1.44 s */
#define GAP         4000        /* 0.5 s of noise after it */

typedef struct {
    float *x;
    int    n, sig_n;            /* samples, and how many of them carry the signal */
    double power;               /* of those */
} over_t;

/* An over at 8 kHz as the radio's receiver hands it on: the real part, at freedv_rx.c's scale (2 x the real part of
   rade_tx()'s output: rade_rx_wav takes int16 * 2 / 16384, the transmitter sends real * 16384) */
static over_t make_over(struct rade *r, int form, const char *call, double foff)
{
    int        n_tx = rade_n_tx_out(r), n_eoo = rade_n_tx_eoo_out(r), n_feat = rade_n_features_in_out(r);
    float     *feat = calloc((size_t)n_feat, sizeof(float));
    RADE_COMP *iq = malloc(sizeof(RADE_COMP) * (size_t)(n_tx > n_eoo ? n_tx : n_eoo));
    over_t     o = {0};
    float      bits[EOO_N];
    double     ph = 0.0, w = 2.0 * M_PI * foff / RADE_FS;

    o.x = malloc(sizeof(float) * (size_t)(NFRAMES * n_tx + n_eoo + GAP));
    make_bits(form, call, bits);
    rade_tx_set_eoo_bits(r, bits);
    for (int f = 0; f <= NFRAMES; f++) {
        int m = f < NFRAMES ? rade_tx(r, iq, feat) : rade_tx_eoo(r, iq);

        for (int i = 0; i < m; i++, ph += w) {
            float v = 2.0f * (float)(iq[i].real * cos(ph) - iq[i].imag * sin(ph));

            o.x[o.n++] = v;
            o.power += (double)v * v;
        }
    }
    o.sig_n = o.n;
    o.power /= o.sig_n;
    for (int i = 0; i < GAP; i++)
        o.x[o.n++] = 0.0f;
    free(feat);
    free(iq);
    return o;
}

/* Noise for a 3 kHz SNR: real noise of variance s2 at 8 kHz has 3/4 of it in 3 kHz */
static void add_noise(over_t *o, double snr_db)
{
    double s = sqrt(o->power / (0.75 * pow(10.0, snr_db / 10.0)));

    for (int i = 0; i < o->n; i++)
        o->x[i] += (float)(s * gauss());
}

/* Feeds an over to the receiver; the callsign of each EOO it finds into out, their count returned */
static int receive(struct rade *r, const over_t *o, char out[FREEDV_RX_CALL_MAX + 1], float *fill, float *snr)
{
    static RADE_COMP *in;
    static float     *feat, *eoo;
    int               eoos = 0, at = 0;

    if (!in) {
        in = malloc(sizeof(RADE_COMP) * (size_t)rade_nin_max(r));
        feat = malloc(sizeof(float) * (size_t)rade_n_features_in_out(r));
        eoo = malloc(sizeof(float) * (size_t)rade_n_eoo_bits(r));
    }
    out[0] = '\0';
    for (;;) {
        int nin = rade_nin(r), has_eoo = 0;

        if (at + nin > o->n)
            break;
        for (int i = 0; i < nin; i++) {
            in[i].real = o->x[at + i];
            in[i].imag = 0.0f;
        }
        at += nin;
        rade_rx(r, feat, &has_eoo, eoo, in);
        if (rade_sync(r))
            *snr = rade_snrdB_3k_est(r);
        if (has_eoo) {
            char               t[FREEDV_TEXT_MAX + 1];
            freedv_text_diag_t dg;

            eoos++;
            freedv_rx_eoo_callsign(eoo, rade_n_eoo_bits(r), out);
            freedv_text_read(eoo, rade_n_eoo_bits(r), t, &dg);
            *fill = dg.fill;
        }
    }
    return eoos;
}

static void test_modem(struct rade *r)
{
    static const double snr[] = {20.0, 15.0, 10.0, 6.0, 4.0, 2.0, 0.0};
    const int nsnr = (int)(sizeof(snr) / sizeof(snr[0]));
    static const double foff[] = {0.0, 37.0};
    int    total = 0, wrong_all = 0;
    float  fill_max[2] = {0.0f, 0.0f}, fill_min[2] = {1e9f, 1e9f};

    printf("2. through the RADE modem: %d modem frames, the EOO, 0.5 s; the real part, noise at a 3 kHz SNR\n", NFRAMES);
    printf("   SNR   offset  form          shown  nothing  wrong  no EOO   (est. SNR)\n");
    for (int si = 0; si < nsnr; si++)
        for (int fi = 0; fi < 2; fi++)
            for (int form = FORM_FREEDV; form <= FORM_RANDOM; form++) {
                int right = 0, none = 0, wrong = 0, missed = 0, runs = form <= FORM_OURS ? NCALLS : 2;
                float est = 0.0f;

                for (int c = 0; c < runs; c++) {
                    over_t o = make_over(r, form, calls[c], foff[fi]);
                    char   out[FREEDV_RX_CALL_MAX + 1];
                    float  fill = 0.0f, s = 0.0f;
                    const char *want = expected(form, calls[c]);

                    add_noise(&o, snr[si]);
                    int eoos = receive(r, &o, out, &fill, &s);

                    est += s / runs;
                    total++;
                    if (eoos != 1)
                        missed++;
                    if (eoos && form <= FORM_OURS && snr[si] >= 4.0) {
                        if (fill > fill_max[form]) fill_max[form] = fill;
                        if (fill < fill_min[form]) fill_min[form] = fill;
                    }
                    if (!out[0])
                        none++;
                    else if (!strcmp(out, want))
                        right++;
                    else {
                        wrong++;
                        printf("   wrong: sent %s (%s), shown %s\n", calls[c], form_name[form], out);
                    }
                    free(o.x);
                }
                if (form != FORM_OURS)
                    wrong_all += wrong;
                printf("   %4.0f dB %3.0f Hz  %-12s %5d  %7d  %5d  %6d   (%.1f dB)\n", snr[si], foff[fi], form_name[form],
                       right, none, wrong, missed, est);
                /* ours has no check, and the modem's EOO bits are noisy (a BER of 0.7 % at 10 dB): only FreeDV's form,
                   and no callsign at all, must never show a wrong one */
                if (form != FORM_OURS)
                    CHECK(wrong == 0, "SNR %g dB, %s: %d wrong", snr[si], form_name[form], wrong);
                if (snr[si] >= 6.0)
                    CHECK(missed == 0, "SNR %g dB %g Hz, %s: %d overs without one EOO", snr[si], foff[fi],
                          form_name[form], missed);
                /* FreeDV's form reads about 94 % of the overs at 6 dB (113 of 120 on a PC), ours none */
                if ((form == FORM_FREEDV && snr[si] >= 10.0) || (form == FORM_OURS && snr[si] >= 15.0))
                    CHECK(right == runs, "SNR %g dB %g Hz, %s: %d of %d shown", snr[si], foff[fi], form_name[form], right,
                          runs);
                if (form == FORM_FREEDV && snr[si] == 6.0)
                    CHECK(right >= runs - 2, "SNR 6 dB %g Hz, FreeDV's form: %d of %d shown", foff[fi], right, runs);
            }
    printf("   the shape from 4 dB up (floats 56-111 over 0-55): FreeDV's form %.2f to %.2f, ours %.3f to %.3f\n",
           fill_min[0], fill_max[0], fill_min[1], fill_max[1]);
    printf("   %d overs, %d wrong in FreeDV's form or without a callsign\n", total, wrong_all);
    CHECK(fill_max[1] < 0.5f && fill_min[0] > 0.5f, "the shape does not tell the forms apart through the modem");
}

/* ---- 3. through freedv_rx.c's threads ---- */

static void test_threads(struct rade *r)
{
    static const struct { int form; const char *call; } ov[] = {
        {FORM_FREEDV, "ON6URE"}, {FORM_OURS, "ON6URE"}, {FORM_FREEDV, "K6AQ/5"}, {FORM_ZEROS, ""},
        {FORM_OURS, "W1AW"}, {FORM_FREEDV, "2E0JFS"},
    };
    const int nov = (int)(sizeof(ov) / sizeof(ov[0]));
    float    *x[6];
    int       xn[6];
    olivia_resamp_t rs;

    /* the overs first: freedv_rx_start opens its own context */
    for (int i = 0; i < nov; i++) {
        over_t o = make_over(r, ov[i].form, ov[i].call, 11.0);
        int    cap;
        float *tmp;

        add_noise(&o, 20.0);
        olivia_resamp_init(&rs, RADE_FS, FREEDV_RX_IN_RATE);
        cap = olivia_resamp_max_out(&rs, o.n + OLIVIA_RESAMP_TAPS);
        x[i] = malloc(sizeof(float) * (size_t)cap);
        tmp = calloc((size_t)(o.n + OLIVIA_RESAMP_TAPS), sizeof(float));
        for (int k = 0; k < o.n; k++)
            tmp[k] = o.x[k] / 4.0f;     /* freedv_rx_put_audio takes +-1.0 and multiplies by 4 */
        xn[i] = olivia_resamp_process(&rs, tmp, o.n + OLIVIA_RESAMP_TAPS, x[i]);
        olivia_resamp_free(&rs);
        free(tmp);
        free(o.x);
    }
    rade_close(r);

    printf("3. through freedv_rx.c's threads, 11025 Hz, 20 dB, 11 Hz off, eight times real time\n");
    CHECK(freedv_rx_start(), "freedv_rx_start");
    uint32_t eoo_seen = 0;

    for (int i = 0; i < nov; i++) {
        struct timespec t0, t;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int at = 0; at < xn[i]; at += 1024) {
            int m = xn[i] - at < 1024 ? xn[i] - at : 1024;

            freedv_rx_put_audio((unsigned)m, &x[i][at]);
            clock_gettime(CLOCK_MONOTONIC, &t);
            double want = (at + m) / (double)FREEDV_RX_IN_RATE / 8.0;
            double have = (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;

            if (want > have)
                usleep((useconds_t)((want - have) * 1e6));
        }
        freedv_rx_stats_t s;

        for (int w = 0; w < 300; w++) {
            freedv_rx_get_stats(&s);
            if (s.eoo_count > eoo_seen)
                break;
            usleep(10000);
        }
        const char *want = expected(ov[i].form, ov[i].call);

        printf("   %-12s %-8s: End-of-Over %s, shown \"%s\"\n", form_name[ov[i].form], ov[i].call,
               s.eoo_count > eoo_seen ? "seen" : "NOT SEEN", s.callsign);
        CHECK(s.eoo_count == eoo_seen + 1, "over %d: %u End-of-Overs", i, s.eoo_count - eoo_seen);
        CHECK(!strcmp(s.callsign, want), "over %d: shown \"%s\", sent \"%s\"", i, s.callsign, want);
        CHECK(s.in_dropped == 0, "input dropped: %u", s.in_dropped);
        eoo_seen = s.eoo_count;
        free(x[i]);
    }
    freedv_rx_stop();
}

int main(void)
{
    test_direct();
    rade_initialize();
    struct rade *r = rade_open("", RADE_VERBOSE_0);

    if (!r || rade_n_eoo_bits(r) != EOO_N) {
        printf("FAIL: rade_open, or not %d EOO bits\n", EOO_N);
        return 1;
    }
    test_modem(r);
    test_threads(r);        /* closes r */
    printf(fails ? "freedv_text_e2e: %d FAILED\n" : "freedv_text_e2e: all passed\n", fails);
    return fails != 0;
}
