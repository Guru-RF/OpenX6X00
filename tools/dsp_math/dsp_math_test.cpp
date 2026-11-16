/*
 * Host test of dsp_db_to_pow() and dsp_pow_to_db() (src/dsp_math.h), the dB <-> power of every bin of the panorama
 * (src/dsp.cpp). Checked:
 *   - how far each is from 10^(dB/10) and 10 log10(p) worked out in double, next to how far powf and log10f are:
 *     every mantissa at the exponents the panorama has, a sweep over all the positive floats, the clamps;
 *   - that nothing on the screen changes: the waterfall's 256 colours (waterfall_data()), the spectrum's pixels and
 *     the noise level the S-meter and the waterfall's colours follow, taken from the same bins once with powf/log10f
 *     and once with the new ones, for the IQ path (the X6100) and the base's spectrum (the X6200);
 *   - the time per bin of both, in dsp.cpp's loops (printed, not checked: a busy PC would fail it).
 * `./dsp_math_test bench` only times them: built for the radio (make arm), it tells what the Cortex-A7 makes of it.
 */
#include "dsp_math.h"

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#define LEVEL_DB    (-34.0f)    /* dsp.cpp: the display level of a bin reading 1.0 */
#define WF_NFFT     1024        /* WATERFALL_NFFT */
#define SP_NFFT     800         /* SPECTRUM_NFFT */
#define PSD_BINS    512         /* the X6200 base's spectrum packet */

static int fails;

static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

static float from_bits(uint32_t b) {
    float f;

    memcpy(&f, &b, 4);
    return f;
}

/* A fixed series of numbers 0 .. 1, the same on every run */
static uint32_t lcg = 12345;

static double rnd(void) {
    lcg = lcg * 1664525u + 1013904223u;
    return (lcg >> 8) / 16777216.0;
}

/* ---- How far from the exact values ---- */

struct Err {
    double ours = 0, libm = 0;      /* the largest error of dsp_pow_to_db and of 10 log10f, dB */
    float  at = 0;
};

static void pow_err(float p, Err &e) {
    double exact = 10.0 * log10((double)p);
    double a = fabs(dsp_pow_to_db(p) - exact);
    double b = fabs(10.0f * log10f(p) - exact);

    if (a > e.ours) {
        e.ours = a;
        e.at = p;
    }
    e.libm = std::max(e.libm, b);
}

static void test_pow_to_db(void) {
    Err used, all;
    char line[160];

    /* Every mantissa at exponents of the range the panorama has: the floor clamp of PsdFrames 1e-20 (2^-67), the noise
     * of a bin, 1, and a carrier at full scale over the bins of an FFT */
    static const int exps[] = {-67, -50, -34, -20, -1, 0, 1, 10, 20, 33};

    for (int e : exps) {
        for (uint32_t m = 0; m < (1u << 23); m++) pow_err(from_bits((uint32_t)(e + 127) << 23 | m), used);
    }
    /* Every 101st positive float, 1.2e-38 .. 3.4e38 */
    for (uint32_t b = 0x00800000u; b < 0x7f800000u; b += 101) {
        float p = from_bits(b);

        pow_err(p, all);
        if (p >= 1e-20f && p <= 1e10f) pow_err(p, used);
    }
    snprintf(line, sizeof(line), "10 log10(p), 1e-20 .. 1e10: largest error %.2g dB at %.4g (10 log10f: %.2g dB)",
             used.ours, used.at, used.libm);
    check(used.ours < 1e-4, line);
    snprintf(line, sizeof(line), "10 log10(p), all positive floats: largest error %.2g dB (10 log10f: %.2g dB)",
             all.ours, all.libm);
    check(all.ours < 1e-4, line);

    /* Under the smallest normal float, 0 too: the clamp, not log10f's -inf */
    float z = dsp_pow_to_db(0.0f), d = dsp_pow_to_db(1e-40f), m = dsp_pow_to_db(FLT_MIN);

    snprintf(line, sizeof(line), "10 log10(0) and of a denormal read %.2f and %.2f dB, as FLT_MIN (%.2f dB)", z, d, m);
    check(std::isfinite(z) && z == m && d == m && fabs(m - 10.0 * log10((double)FLT_MIN)) < 1e-4, line);
    snprintf(line, sizeof(line), "10 log10(1) %.3g, of FLT_MAX %.4f dB", dsp_pow_to_db(1.0f), dsp_pow_to_db(FLT_MAX));
    check(fabs(dsp_pow_to_db(1.0f)) < 1e-4 && fabs(dsp_pow_to_db(FLT_MAX) - 10.0 * log10((double)FLT_MAX)) < 1e-4, line);
}

static void test_db_to_pow(void) {
    double ours = 0, libm = 0, trip = 0;
    float  at = 0;
    char   line[160];

    for (int32_t k = -376 * 4096; k <= 376 * 4096; k++) {
        float  db = k / 4096.0f;
        double exact = pow(10.0, db / 10.0);
        double a = fabs(dsp_db_to_pow(db) / exact - 1.0);
        double b = fabs(powf(10.0f, db * 0.1f) / exact - 1.0);

        if (a > ours) {
            ours = a;
            at = db;
        }
        libm = std::max(libm, b);
        if (db >= -200.0f && db <= 100.0f) trip = std::max(trip, fabs((double)dsp_pow_to_db(dsp_db_to_pow(db)) - db));
    }
    snprintf(line, sizeof(line), "10^(dB/10), -376 .. 376 dB: largest error %.2g of the value (%.2g dB) at %.3f dB "
             "(powf: %.2g)", ours, 10.0 * log10(1.0 + ours), at, libm);
    check(ours < 1e-5, line);
    snprintf(line, sizeof(line), "there and back, -200 .. 100 dB: largest error %.2g dB", trip);
    check(trip < 1e-4, line);

    float lo = dsp_db_to_pow(-1000.0f), hi = dsp_db_to_pow(1000.0f);

    snprintf(line, sizeof(line), "past the clamps: -1000 dB gives %.3g (2^-125), 1000 dB %.3g (2^125)", lo, hi);
    check(lo == ldexpf(1.0f, -125) && hi == ldexpf(1.0f, 125), line);
}

/* ---- What the screen shows ---- */

struct Old {
    static float pow_of(float db) { return powf(10.0f, db * 0.1f); }
    static float db_of(float p) { return 10.0f * log10f(p); }
};

struct New {
    static float pow_of(float db) { return dsp_db_to_pow(db); }
    static float db_of(float p) { return dsp_pow_to_db(p); }
};

/* A bin's colour of the 256 (waterfall_data()), and its row on a spectrum strip h pixels high (spectrum_view.c) */
static int colour(float db, float min, float max) {
    float v = (db - min) * (255.0f / (max - min));

    return v < 0.0f ? 0 : v > 255.0f ? 255 : (uint8_t)v;
}

static int row_px(float db, float min, float max, int h) {
    return (int)((1.0f - (db - min) / (max - min)) * h);
}

struct Shown {
    long   bins = 0, colour_moved = 0, px_moved = 0;
    int    colour_most = 0, px_most = 0;
    double noise_most = 0;      /* dB */
};

/* The levels of the same frame both ways: the colours for a 48 dB scale (the waterfall's own) and a 10 dB one (a
 * narrow Grid), the pixels of a 480 pixel strip (higher than any the GUI has), the noise floor */
static void compare(const float *a, const float *b, size_t n, const float *lin_a, const float *lin_b, Shown &s) {
    static const float ranges[][2] = {{-121.0f, -73.0f}, {-110.0f, -100.0f}};

    for (size_t i = 0; i < n; i++) {
        for (auto &r : ranges) {
            int d = abs(colour(a[i], r[0], r[1]) - colour(b[i], r[0], r[1]));

            s.colour_moved += d != 0;
            s.colour_most = std::max(s.colour_most, d);
        }
        int d = abs(row_px(a[i], -130.0f, -50.0f, 480) - row_px(b[i], -130.0f, -50.0f, 480));

        s.px_moved += d != 0;
        s.px_most = std::max(s.px_most, d);
    }
    s.bins += n;

    float fa = dsp_noise_floor(lin_a, n, 8, 0.25f), fb = dsp_noise_floor(lin_b, n, 8, 0.25f);

    s.noise_most = std::max(s.noise_most, fabs(10.0 * log10((double)fa / fb)));
}

/* The IQ path (X6100): a bin's power, mean |X|^2 of a unit-energy window; dsp.cpp shows 10 log10 of it */
template <class C> static void iq_frame(const float *lin, float *db) {
    for (size_t i = 0; i < WF_NFFT; i++) db[i] = C::db_of(lin[i]) + LEVEL_DB;
}

/* The base's spectrum (X6200): two packets of whole dB averaged in power, laid onto the 1024 bins (PsdFrames), and
 * the powers auto_levels() and the noise floor read back from them (dsp_psd()) */
template <class C> static void psd_frame(const float pkt[2][PSD_BINS], float frac, float *db, float *lin) {
    std::vector<float> acc(PSD_BINS, 0.0f);

    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; i < PSD_BINS; i++) acc[i] += C::pow_of(pkt[k][i]);
    }
    float width = (PSD_BINS - 1) * frac;
    float start = (PSD_BINS - 1 - width) * 0.5f;

    for (size_t j = 0; j < WF_NFFT; j++) {
        float  x = start + width * j / (WF_NFFT - 1);
        size_t i = (size_t)x;
        float  f = x - i;
        float  p = i + 1 < PSD_BINS ? acc[i] * (1.0f - f) + acc[i + 1] * f : acc[i];

        db[j] = C::db_of(std::max(p * 0.5f, 1e-20f));
        lin[j] = C::pow_of(db[j] - LEVEL_DB);
    }
}

static void test_screen(void) {
    static float lin[WF_NFFT], a[WF_NFFT], b[WF_NFFT], la[WF_NFFT], lb[WF_NFFT];
    static float pkt[2][PSD_BINS];
    Shown        iq, psd;
    char         line[200];

    for (int frame = 0; frame < 4000; frame++) {
        /* Noise of a random level with carriers: powers of 1e-14 .. 1e2 (-174 .. -14 dB on the screen), every
         * float of them as likely as the next on a dB scale */
        double floor_db = -160.0 + 100.0 * rnd();

        for (size_t i = 0; i < WF_NFFT; i++) {
            double db = rnd() < 0.02 ? floor_db + 60.0 * rnd() : floor_db + 10.0 * log10(-log(rnd() + 1e-12));

            lin[i] = (float)pow(10.0, std::min(db, 20.0) / 10.0);
        }
        iq_frame<Old>(lin, a);
        iq_frame<New>(lin, b);
        compare(a, b, WF_NFFT, lin, lin, iq);

        /* The X6200 base: 126 = 0 dBm, whole dB */
        for (int k = 0; k < 2; k++) {
            for (size_t i = 0; i < PSD_BINS; i++) pkt[k][i] = (float)(int)(-140.0 + 30.0 * rnd() + (rnd() < 0.03 ? 70 * rnd() : 0));
        }
        float frac = 0.25f + 0.75f * (frame % 4) / 3.0f;

        psd_frame<Old>(pkt, frac, a, la);
        psd_frame<New>(pkt, frac, b, lb);
        compare(a, b, WF_NFFT, la, lb, psd);
    }
    for (int k = 0; k < 2; k++) {
        Shown      &s = k ? psd : iq;
        const char *name = k ? "base spectrum (X6200)" : "IQ (X6100)";

        snprintf(line, sizeof(line), "%s, %ld bins: %ld colours of the two scales changed (by %d at most), %ld pixels "
                 "(by %d at most)", name, s.bins, s.colour_moved, s.colour_most, s.px_moved, s.px_most);
        check(s.colour_most <= 1 && s.px_most <= 1 && s.colour_moved * 1000 < s.bins && s.px_moved * 1000 < s.bins, line);
    }
    /* The S-meter: on the X6100 summed from the FFT's powers, which are not converted; on the X6200 the base's own
     * reading. What moves is the X6200's noise floor (the S-meter's noise mark, the waterfall's colour floor), read
     * from the powers converted back from the base's dB */
    snprintf(line, sizeof(line), "base spectrum (X6200): the noise floor moved by %.2g dB at most", psd.noise_most);
    check(psd.noise_most < 1e-3, line);
}

/* ---- Time per bin, as dsp.cpp's loops run ---- */

static double now_s(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

__attribute__((noinline)) static void to_db_libm(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = 10.0f * log10f(in[i]) + LEVEL_DB;
}

__attribute__((noinline)) static void to_db_ours(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = dsp_pow_to_db(in[i]) + LEVEL_DB;
}

__attribute__((noinline)) static void to_pow_libm(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = powf(10.0f, (in[i] - LEVEL_DB) * 0.1f);
}

__attribute__((noinline)) static void to_pow_ours(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = dsp_db_to_pow(in[i] - LEVEL_DB);
}

static double ns_per_bin(void (*fn)(const float *, float *, size_t), const float *in, float *out) {
    double best = 1e9;

    for (int run = 0; run < 5; run++) {
        int    reps = 2000;
        double t0 = now_s();

        for (int r = 0; r < reps; r++) {
            fn(in, out, WF_NFFT);
            __asm__ volatile("" : : "r"(out) : "memory");
        }
        best = std::min(best, (now_s() - t0) / reps / WF_NFFT * 1e9);
    }
    return best;
}

static void bench(void) {
    static float lin[WF_NFFT], db[WF_NFFT], out[WF_NFFT];

    for (size_t i = 0; i < WF_NFFT; i++) {
        lin[i] = (float)pow(10.0, (-140.0 + 120.0 * rnd()) / 10.0);
        db[i] = 10.0f * log10f(lin[i]) + LEVEL_DB;
    }
    double dl = ns_per_bin(to_db_libm, lin, out), dn = ns_per_bin(to_db_ours, lin, out);
    double pl = ns_per_bin(to_pow_libm, db, out), pn = ns_per_bin(to_pow_ours, db, out);

    printf("time  10 log10f %.2f ns a bin, dsp_pow_to_db %.2f ns (%.1fx)\n", dl, dn, dl / dn);
    printf("time  powf      %.2f ns a bin, dsp_db_to_pow %.2f ns (%.1fx)\n", pl, pn, pl / pn);
    /* The X6200's busiest case: 29 packets a second of 512 bins pushed twice, the spectrum's 800 bins at 15 frames
     * and the waterfall's 1024 at 30 (Speed 5) back to dB, the waterfall's powers again */
    double us_old = (2 * 29 * PSD_BINS * pl + (15 * SP_NFFT + 30 * WF_NFFT) * dl + 30 * WF_NFFT * pl) / 1000.0;
    double us_new = (2 * 29 * PSD_BINS * pn + (15 * SP_NFFT + 30 * WF_NFFT) * dn + 30 * WF_NFFT * pn) / 1000.0;

    printf("time  X6200 at waterfall Speed 5: %.0f us of every second before, %.0f us now\n", us_old, us_new);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        bench();
        return 0;
    }
    test_pow_to_db();
    test_db_to_pow();
    test_screen();
    bench();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
