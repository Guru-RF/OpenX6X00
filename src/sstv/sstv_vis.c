/*
 * VIS header finder (see sstv_vis.h).
 *
 * The header: leader 1900 Hz 300 ms, break 1200 Hz 10 ms, leader 300 ms,
 * start bit 1200 Hz, 7 data bits LSB first (1100 Hz = 1, 1300 Hz = 0), even
 * parity, stop bit 1200 Hz, bits of 30 ms.
 *
 * How it is found, block by block:
 *  - the block goes to complex baseband around 1500 Hz (97-tap Blackman
 *    low-pass at 750 Hz, one sample in 7 kept: 1575 Hz);
 *  - leader test: the two stretches that are leader whatever the start time
 *    (0.10-0.30 s and 0.41-0.61 s) must each hold one narrow tone near
 *    1900 Hz, at the same frequency.  The tone also gives the tuning error.
 *    Nearly every block of plain audio ends here;
 *  - timing: the leader -> start bit step (1900 -> 1200 Hz) on the 1575-Hz
 *    grid, before any data bit is read, so a frame shifted in time cannot
 *    pass as another code.  A visible break adds to the score; without it
 *    the start bit has to be cleaner;
 *  - bits: tone shares for bit lengths 29.1..30.9 ms, then the closest code
 *    of the list of real VIS codes (soft decision over 7 bits + parity, the
 *    stop bit counts too).  It must fit well and beat the next code clearly.
 *    A gap of 3..36 ms in the middle of the bits (audio drop-out at the
 *    sender) is also tried, but such a frame must fit much better.
 */
#include "sstv_vis.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#define DECIM        7
#define BB_RATE      ((float)SSTV_RATE / DECIM)    /* 1575 Hz */
#define MIX_HZ       1500.0f
#define MIX_LEN      147                           /* 1500 Hz: 20 periods in 147 samples */
#define NTAPS        97
#define BB_MAX       (2 * SSTV_RATE / DECIM + 8)   /* 2-s blocks */

#define LEADER_MIN   0.035f     /* leader tone share of the energy           */
#define SEARCH_HZ    150.0f     /* tuning error searched                      */
#define LEADER_DIFF  8.0f       /* the two leaders may differ this much, Hz  */
#define STEP_MIN     0.40f      /* tone shares at the leader -> start step    */
#define START_CLEAN  0.70f      /* start-bit share asked when no break shows  */
#define BREAK_PLUS   0.30f
#define TRIM         0.05f      /* part of each bit edge not looked at        */
#define LONGEST_S    0.937f     /* a header 3% slow                           */
#define FIT_MIN      0.50f      /* mean symbol score                          */
#define FIT_NOBREAK  0.60f
#define FIT_GAP      0.75f      /* with a gap in the bits                     */
#define LEAD_MIN     0.30f      /* best code over the next one, score x 11    */
#define GAP_MAX_S    0.036f
#define GAP_STEP_S   0.003f
#define GAP_COST     0.02f      /* equal fits: no gap wins                    */
#define GAP_WIN      0.08f      /* gap fit this much better: it wins          */

static float taps[NTAPS], osc_re[MIX_LEN], osc_im[MIX_LEN];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void tables_make(void)
{
    const double cut = 750.0 / SSTV_RATE;
    double       total = 0.0;

    for (int k = 0; k < NTAPS; ++k) {
        int    m = k - NTAPS / 2;
        double v = m ? sin(2.0 * M_PI * cut * m) / (M_PI * m) : 2.0 * cut;
        v *= 0.42 - 0.5 * cos(2.0 * M_PI * k / (NTAPS - 1)) + 0.08 * cos(4.0 * M_PI * k / (NTAPS - 1));
        taps[k] = (float)v;
        total += v;
    }
    for (int k = 0; k < NTAPS; ++k)
        taps[k] /= (float)total;
    for (int i = 0; i < MIX_LEN; ++i) {
        osc_re[i] = (float)cos(2.0 * M_PI * MIX_HZ * i / SSTV_RATE);
        osc_im[i] = (float)-sin(2.0 * M_PI * MIX_HZ * i / SSTV_RATE);
    }
}

/* Mixed down, filtered, decimated; returns the number of baseband samples */
static int baseband(const float *x, int n, float complex *z)
{
    int nz = 0;

    for (int o = NTAPS; o + DECIM <= n && nz < BB_MAX; o += DECIM) {
        float re = 0.0f, im = 0.0f;
        for (int k = 0; k < NTAPS; ++k) {
            int   i = o - k;
            float v = x[i] * taps[k];
            re += v * osc_re[i % MIX_LEN];
            im += v * osc_im[i % MIX_LEN];
        }
        z[nz++] = re + I * im;
    }
    return nz;
}

/* Energy of z[a .. a+len) at audio frequency hz (one DFT bin) */
static float tone(const float complex *z, int a, int len, float hz)
{
    const float   w = -2.0f * (float)M_PI * (hz - MIX_HZ) / BB_RATE;
    float complex step = cexpf(I * w), rot = 1.0f, sum = 0.0f;

    for (int i = 0; i < len; ++i) {
        sum += z[a + i] * rot;
        rot *= step;
    }
    return crealf(sum) * crealf(sum) + cimagf(sum) * cimagf(sum);
}

static float energy(const float complex *z, int a, int len)
{
    float e = 0.0f;
    for (int i = 0; i < len; ++i)
        e += crealf(z[a + i]) * crealf(z[a + i]) + cimagf(z[a + i]) * cimagf(z[a + i]);
    return e;
}

/* The four VIS tones over one symbol, edges trimmed by part of its length */
typedef struct { float f1100, f1200, f1300, f1900; } tones_t;

static bool measure(const float complex *z, int nz, float t, float len_s, float trim, float tune, tones_t *s)
{
    int a   = (int)lroundf((t + len_s * trim) * BB_RATE);
    int len = (int)lroundf(len_s * (1.0f - 2.0f * trim) * BB_RATE);

    if (a < 0 || len < 4 || a + len > nz) return false;
    s->f1100 = tone(z, a, len, 1100.0f + tune);
    s->f1200 = tone(z, a, len, 1200.0f + tune);
    s->f1300 = tone(z, a, len, 1300.0f + tune);
    s->f1900 = tone(z, a, len, 1900.0f + tune);
    return true;
}

static inline float share(float e, const tones_t *s)
{
    float all = s->f1100 + s->f1200 + s->f1300 + s->f1900;
    return all > 1e-20f ? e / all : 0.0f;
}

/* Both leaders: one narrow tone each, same frequency.  *tune: its offset from
 * 1900 Hz (parabolic peak on a 2-Hz grid), *q: its share of the energy. */
static bool leader_ok(const float complex *z, int nz, float *tune, float *q)
{
    const int a1 = (int)(0.10f * BB_RATE), a2 = (int)(0.41f * BB_RATE), len = (int)(0.20f * BB_RATE);

    *q = 0.0f;
    if (a2 + len > nz) return false;
    float e1 = energy(z, a1, len) * len, e2 = energy(z, a2, len) * len;
    if (e1 + e2 <= 1e-20f) return false;

    /* sum of the two stretches without phase: the break may jump it */
    float peak = -1.0f, peak_hz = 0.0f, below = 0.0f, above = 0.0f, last2 = -1.0f, last1 = -1.0f;
    for (float hz = 1900.0f - SEARCH_HZ; hz <= 1900.0f + SEARCH_HZ + 0.1f; hz += 2.0f) {
        float e = tone(z, a1, len, hz) + tone(z, a2, len, hz);
        if (last1 > peak && last1 >= last2 && last1 >= e) {
            peak = last1;
            peak_hz = hz - 2.0f;
            below = last2;
            above = e;
        }
        last2 = last1;
        last1 = e;
    }
    if (peak <= 0.0f) return false;
    float curve = below - 2.0f * peak + above;
    float frac = curve < 0.0f ? 0.5f * (below - above) / curve : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < -1.0f) frac = -1.0f;
    float hz0 = peak_hz + 2.0f * frac;
    *tune = hz0 - 1900.0f;
    *q = peak / (e1 + e2);
    if (*q < LEADER_MIN) return false;

    if (tone(z, a1, len, hz0) / e1 < 0.5f * LEADER_MIN) return false;
    if (tone(z, a2, len, hz0) / e2 < 0.5f * LEADER_MIN) return false;
    float m1 = 0, hz1 = hz0, m2 = 0, hz2 = hz0;
    for (float hz = hz0 - 20.0f; hz <= hz0 + 20.1f; hz += 1.0f) {
        float t1 = tone(z, a1, len, hz), t2 = tone(z, a2, len, hz);
        if (t1 > m1) { m1 = t1; hz1 = hz; }
        if (t2 > m2) { m2 = t2; hz2 = hz; }
    }
    return fabsf(hz1 - hz2) <= LEADER_DIFF;
}

/* Every VIS code in use, decoded or not: deciding among real codes only is
 * what makes the parity bit worth something */
static const unsigned char codes[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 40, 44, 48, 51, 52, 55, 56, 60, 63, 76,
    93, 94, 95, 96, 97, 98, 99, 100, 104, 105, 113, 114, 115,
};

/* Best and runner-up code of one family of hypotheses */
typedef struct {
    float best, next, bit_s, gap_s;
    int   code;
} pick_t;

static void offer(pick_t *p, int code, float score, float bit_s, float gap_s)
{
    if (score > p->best) {
        if (code != p->code) p->next = p->best;
        p->best = score;
        p->code = code;
        p->bit_s = bit_s;
        p->gap_s = gap_s;
    } else if (code != p->code && score > p->next) {
        p->next = score;
    }
}

bool sstv_vis_find(const float *x, int n, sstv_vis_t *res)
{
    float complex z[BB_MAX];      /* ~25 kB of stack, nothing shared */
    sstv_vis_t    r = { .mode = SSTV_UNKNOWN, .unsupported = -1, .score = -1.0f };
    bool          found = false;
    tones_t       s;

    pthread_once(&tables_once, tables_make);
    if (!x || n <= NTAPS) goto out;

    int         nz = baseband(x, n, z);
    const float t0 = (float)(NTAPS - NTAPS / 2) / SSTV_RATE;    /* time of z[0] */

    float tune = 0.0f;
    if (!leader_ok(z, nz, &tune, &r.leader)) goto out;
    r.offset_hz = tune;

    /* leader -> start bit, every baseband sample */
    const float last = (float)nz / BB_RATE + t0 - LONGEST_S;
    float       step_best = -1.0f, step_t = 0.0f, step_q = 0.0f;
    bool        step_brk = false;
    for (float t = 0.0f; t <= last; t += 1.0f / BB_RATE) {
        float tb = t - t0, start = tb + 0.610f;
        bool  brk = measure(z, nz, tb + 0.300f, 0.010f, 0.15f, tune, &s) && s.f1200 >= s.f1900;
        if (!measure(z, nz, start - 0.024f, 0.024f, 0.0f, tune, &s)) continue;
        float lead = share(s.f1900, &s);
        if (!measure(z, nz, start, 0.024f, 0.0f, tune, &s)) continue;
        float st = share(s.f1200, &s);
        if (lead < STEP_MIN || st < (brk ? STEP_MIN : START_CLEAN)) continue;
        float m = lead + st + (brk ? BREAK_PLUS : 0.0f);
        if (m > step_best) {
            step_best = m;
            step_t = t;
            step_q = lead + st;
            step_brk = brk;
        }
    }
    if (step_best < 0.0f) goto out;

    /* Bits.  plain: no gap, the usual case; any: every gap hypothesis
     * (no gap included), used only when plain fails or clearly loses. */
    static const float bit_lengths[] = { 0.0291f, 0.0297f, 0.0303f, 0.0309f };
    const float start = step_t - t0 + 0.610f;
    const int   n_gaps = (int)lroundf(GAP_MAX_S / GAP_STEP_S);
    pick_t      plain = { -1.0f, -1.0f, 0.0f, 0.0f, -1 }, any = plain;

    for (unsigned b = 0; b < sizeof(bit_lengths) / sizeof(bit_lengths[0]); ++b) {
        const float T = bit_lengths[b];
        if (!measure(z, nz, start, T, TRIM, tune, &s)) continue;
        const float base = 0.5f * step_q + share(s.f1200, &s);

        /* one/zero shares of the 8 bits: [0] on time, [1] after the gap */
        float one[2][8], zero[2][8];
        int   i;
        for (i = 0; i < 8; ++i) {
            if (!measure(z, nz, start + (i + 1) * T, T, TRIM, tune, &s)) break;
            one[0][i] = share(s.f1100, &s);
            zero[0][i] = share(s.f1300, &s);
        }
        if (i < 8) continue;

        for (int g = 0; g <= n_gaps; ++g) {
            const float gap = g * GAP_STEP_S;
            int         late_ok;
            for (i = 0; i < 8; ++i) {
                if (!g) {
                    one[1][i] = one[0][i];
                    zero[1][i] = zero[0][i];
                    continue;
                }
                if (!measure(z, nz, start + (i + 1) * T + gap, T, TRIM, tune, &s)) break;
                one[1][i] = share(s.f1100, &s);
                zero[1][i] = share(s.f1300, &s);
            }
            late_ok = i;
            /* the stop bit may be lost in the gap or past the block: bonus only */
            float fixed = base;
            if (measure(z, nz, start + 9 * T + gap, T, TRIM, tune, &s))
                fixed += share(s.f1200, &s);

            /* the gap falls before bit k (k = 8: before the stop bit) */
            for (int k = g ? 0 : 8; k <= 8; ++k) {
                if (k < 8 && late_ok < 8) continue;
                float o[8], zr[8];
                for (int j = 0; j < 8; ++j) {
                    o[j] = one[j >= k][j];
                    zr[j] = zero[j >= k][j];
                }
                for (unsigned c = 0; c < sizeof(codes); ++c) {
                    int   code = codes[c], parity = __builtin_popcount(code) & 1;
                    float sc = fixed;
                    for (int j = 0; j < 8; ++j) {
                        int bit = j < 7 ? (code >> j) & 1 : parity;
                        sc += bit ? o[j] : zr[j];
                    }
                    sc = sc / 11.0f - (g ? GAP_COST : 0.0f);
                    if (!g) offer(&plain, code, sc, T, gap);
                    offer(&any, code, sc, T, gap);
                }
            }
        }
    }

    /* A gap frame must be much cleaner (real headers with a gap score over
     * 0.9, picture content that happens to fit one about 0.5) */
    const pick_t *pk[2] = { &plain, &any };
    const float   need[2] = { FIT_MIN, FIT_GAP };
    bool          ok[2];
    for (int h = 0; h < 2; ++h) {
        float m = need[h];
        if (!step_brk && m < FIT_NOBREAK) m = FIT_NOBREAK;
        ok[h] = pk[h]->code >= 0 && pk[h]->best >= m && (pk[h]->best - pk[h]->next) * 11.0f >= LEAD_MIN;
    }
    /* a gap frame clearly better than every on-time one: the on-time
     * decision is reading shifted bits */
    int h;
    if (ok[1] && any.gap_s > 0.0f && any.best - plain.best >= GAP_WIN) h = 1;
    else if (ok[0]) h = 0;
    else if (ok[1]) h = 1;
    else h = 0;

    const pick_t *w = pk[h];
    r.gap_s = w->gap_s;
    r.score = w->best;
    r.start_s = step_t;
    r.bit_ms = w->bit_s * 1000.0f;
    if (w->code < 0 || w->best < need[h]) goto out;
    if (!step_brk && w->best < FIT_NOBREAK) goto out;
    if ((w->best - w->next) * 11.0f < LEAD_MIN) goto out;

    const sstv_mode_info_t *mi = sstv_mode_by_vis(w->code);
    if (mi) {
        r.mode = mi->mode;
        found = true;
    } else {
        r.unsupported = w->code;
    }

out:
    if (res) *res = r;
    return found;
}
