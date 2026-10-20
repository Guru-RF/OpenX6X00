/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY receiver
 *
 *  The frame decoding follows WSJT-X 3.2.0-rc1 lib/jtty/jtty_mdecode.f90,
 *  jtty_peakup.f90 and jtty_payload_correlators.f90 (WSJT Development
 *  Group, K1JT et al., GPL-3.0): the 13 sync symbols' tone powers gate a
 *  candidate (more than 6 right and 4.6 dB in the Rx window), the 46 data
 *  symbols are correlated with the four tones on a frequency-corrected
 *  baseband and go to the list decoder, and frames following each other
 *  by one to three frame periods on the same frequency make one message.
 *
 *  What differs, for a small CPU: WSJT-X searches for the sync with a
 *  coherent 13-symbol correlation over FFTs of 8192 points every 2 ms,
 *  around the Rx frequency and over 1200-1800 Hz. Here the search is a
 *  spectrogram over the whole passband, one symbol long, every eighth of
 *  a symbol, 7.8 Hz bins, summing the sync tones without phase; each peak
 *  is then refined on the coherent sync (to a sample and 0.02 Hz) before
 *  the gate. Outside the Rx window the gate is stricter (more than 9
 *  right and 5.5 dB; WSJT-X: 8 and 5 dB over its 600 Hz), and a frame
 *  that measures below -17 dB with its own tones is dropped: on noise the
 *  CRC lets a wrong word through now and then. Signal subtraction and
 *  the retro sweeps of WSJT-X are left out.
 *
 *  Measured on the host (tests/test_jtty.cpp, one-frame message, noise in
 *  2500 Hz): 80 % decoded at -14 dB, half at -15, a quarter at -16; on
 *  noise alone nothing in an hour.
 *
 *  Chain: 11025 Hz -> 12000 Hz (olivia_resamp) -> complex, centred on
 *  1500 Hz, 4000 Hz (128 samples a symbol) -> ring of 8 s.
 */

#include "jtty_rx.h"

#include "jtty.h"
#include "olivia_resamp.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FS          4000.0f
#define CF          1500.0f         /* audio Hz at baseband 0 */
#define NSS         128             /* samples a symbol */
#define HOP         16              /* spectrogram hop: an eighth of a symbol */
#define NFFT        512             /* zero padded: 7.8 Hz bins, 4 a tone */
#define DF          (FS / NFFT)
#define KOFF        200             /* stored bins: baseband -200..199 */
#define NB          400
#define RING        32768
#define PH          128             /* spectrogram rows kept */
#define RH          32              /* score rows kept */
#define NFIR        64
#define FRAME_SAMP  (JTTY_NSYM * NSS)
#define FRAME_S     (FRAME_SAMP / FS)

#define THR_PRE     2.85f           /* spectrogram sync score for a candidate: noise peaks
                                       * reach 2.6-3, frames that decode at -16 dB 2.9 and up */
#define MAX_CAND    24
#define MAX_ACTIVE  30
#define MAX_RECENT  64
#define FIFO        65536

typedef struct {
    double t;           /* first sync sample, absolute */
    float  f;           /* baseband Hz of tone 0 */
    float  score;
} cand_t;

typedef struct {
    uint32_t id;
    float    f, t_last;
    char     text[81];
    bool     sep;
    char     call[14];
    int      call_kind;
    int      snr;
} active_t;

/* ---- State; the worker thread owns everything below the FIFO -------- */

static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static pthread_t       thread;
static bool            running = false, quit = false, busy = false, reset_req = false;
static float           fifo[FIFO];
static size_t          f_head = 0, f_tail = 0;
static jtty_rx_cb_t    out_cb = NULL;

static float           rng_lo = 200.0f, rng_hi = 2800.0f, rx_f0 = 1500.0f, rx_tol = 20.0f;

static olivia_resamp_t rs;
static float           fir[NFIR];
static float complex   fir_hist[2 * NFIR];
static int             fir_pos = 0, mix_ph = 0, dec_cnt = 0;

static float complex   ring[RING];
static int64_t         zn = 0;             /* samples written */
static int64_t         valid_from = 0;     /* first sample after a reset */

static float           spec[PH][NB];
static float           score[RH][NB];
static int64_t         nhop = 0;           /* spectrogram rows made */
static float complex   tw[NFFT / 2];
static int             rev[NFFT];

static cand_t          cand[MAX_CAND];
static int             ncand = 0;
static active_t        act[MAX_ACTIVE];
static int             nact = 0;
static struct { float f; double t; } recent[MAX_RECENT];
static int             nrecent = 0;
static uint32_t        next_id = 1;

/* ---- Small FFT ------------------------------------------------------ */

static void fft_init(void)
{
    int bits = 0;
    while ((1 << bits) < NFFT)
        bits++;
    for (int i = 0; i < NFFT; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        rev[i] = r;
    }
    for (int i = 0; i < NFFT / 2; i++)
        tw[i] = cexpf(-2.0f * (float)M_PI * I * (float)i / NFFT);
}

static void fft(float complex *x)
{
    for (int i = 0; i < NFFT; i++)
        if (rev[i] > i) {
            float complex t = x[i];
            x[i] = x[rev[i]];
            x[rev[i]] = t;
        }
    for (int len = 2; len <= NFFT; len <<= 1)
        for (int i = 0; i < NFFT; i += len)
            for (int j = 0; j < len / 2; j++) {
                float complex a = tw[j * (NFFT / len)], b = x[i + j + len / 2];
                float wr = crealf(a) * crealf(b) - cimagf(a) * cimagf(b);
                float wi = crealf(a) * cimagf(b) + cimagf(a) * crealf(b);
                x[i + j + len / 2] = (crealf(x[i + j]) - wr) + I * (cimagf(x[i + j]) - wi);
                x[i + j] = (crealf(x[i + j]) + wr) + I * (cimagf(x[i + j]) + wi);
            }
}

/* ---- Front end ------------------------------------------------------- */

static void front_init(void)
{
    double sum = 0;

    for (int i = 0; i < NFIR; i++) {
        double k = i - (NFIR - 1) / 2.0, fc = 2000.0 / 12000.0;
        double s = fabs(k) < 1e-9 ? 2 * fc : sin(2 * M_PI * fc * k) / (M_PI * k);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (NFIR - 1)) + 0.08 * cos(4 * M_PI * i / (NFIR - 1));
        fir[i] = (float)(s * w);
        sum += fir[i];
    }
    for (int i = 0; i < NFIR; i++)
        fir[i] = (float)(fir[i] / sum);
    memset(fir_hist, 0, sizeof(fir_hist));
    fir_pos = mix_ph = dec_cnt = 0;
}

static inline float complex at(int64_t n)
{
    return ring[n & (RING - 1)];
}

static void spectrum_row(void)
{
    float complex x[NFFT];
    float *row = spec[nhop % PH];

    for (int i = 0; i < NSS; i++)
        x[i] = at(zn - NSS + i);
    memset(x + NSS, 0, sizeof(float complex) * (NFFT - NSS));
    fft(x);
    for (int k = 0; k < NB; k++) {
        float complex v = x[(k - KOFF + NFFT) % NFFT];
        row[k] = crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
    }
}

/* Sync score of every frequency for the frame whose first sync symbol
 * is spectrogram row s, and candidates from the local peaks */
static void score_row(int64_t s)
{
    float *r = score[s % RH];
    int klo = (int)ceilf((rng_lo - CF) / DF) + KOFF, khi = (int)floorf((rng_hi - 100.0f - CF) / DF) + KOFF;

    if (klo < 1)
        klo = 1;
    if (khi > NB - 13)
        khi = NB - 13;
    for (int k = 0; k < NB; k++)
        r[k] = 0;
    if (s * HOP < valid_from)
        return;
    for (int k = klo; k <= khi; k++) {
        float sig = 0, tot = 0;
        for (int j = 0; j < JTTY_NSYNC; j++) {
            const float *p = spec[(s + 8 * j) % PH];
            sig += p[k + 4 * jtty_sync[j]];
            tot += p[k] + p[k + 4] + p[k + 8] + p[k + 12];
        }
        float noise = (tot - sig) / 3.0f;
        r[k] = noise > 0 ? sig / noise : 0;
    }

    /* Peaks of row c = s - 8 over +-8 rows (a symbol) and +-2 bins */
    int64_t c = s - 8;
    if (c - 8 < 0)
        return;
    const float *rc = score[c % RH];
    for (int k = klo; k <= khi; k++) {
        float v = rc[k];
        bool peak = v >= THR_PRE;
        for (int dt = -8; dt <= 8 && peak; dt++)
            for (int dk = -2; dk <= 2 && peak; dk++) {
                if ((dt == 0 && dk == 0) || k + dk < 0 || k + dk >= NB)
                    continue;
                float o = score[(c + dt) % RH][k + dk];
                if (o > v || (o == v && (dt < 0 || (dt == 0 && dk < 0))))
                    peak = false;
            }
        if (!peak)
            continue;
        /* Between the bins and the rows: the vertex of a parabola */
        float lo = rc[k - 1], hi = rc[k + 1], dk = 0, dt = 0;
        if (lo - 2 * v + hi < 0)
            dk = 0.5f * (lo - hi) / (lo - 2 * v + hi);
        lo = score[(c - 1) % RH][k];
        hi = score[(c + 1) % RH][k];
        if (lo - 2 * v + hi < 0)
            dt = 0.5f * (lo - hi) / (lo - 2 * v + hi);
        cand_t nc = {(c + dt) * HOP, (k + dk - KOFF) * DF, v};
        if (ncand < MAX_CAND) {
            cand[ncand++] = nc;
        } else {
            int w = 0;
            for (int i = 1; i < ncand; i++)
                if (cand[i].score < cand[w].score)
                    w = i;
            if (cand[w].score < v)
                cand[w] = nc;
        }
    }
}

/* ---- Frame decoding --------------------------------------------------- */

/* Sum of n samples from n0 turned by e^-j(ph0 + k dph): a DFT at any
 * frequency. Real arithmetic: C complex products check for NaN. */
static float complex rot_sum(int64_t n0, int n, double ph0, double dph)
{
    float cr = (float)cos(ph0), ci = (float)-sin(ph0), wr = (float)cos(dph), wi = (float)-sin(dph);
    float zr = 0, zi = 0;

    for (int k = 0; k < n; k++) {
        float complex x = at(n0 + k);
        float xr = crealf(x), xi = cimagf(x), t;
        zr += xr * cr - xi * ci;
        zi += xr * ci + xi * cr;
        t = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = t;
    }
    return zr + I * zi;
}

/* The 13 sync symbols' phasors at (t, f), on one phase reference over
 * the whole sync (continuous phase: the modulation index is 1) */
static void sync_phasors(int64_t t, float f, float complex y[JTTY_NSYNC])
{
    for (int j = 0; j < JTTY_NSYNC; j++)
        y[j] = rot_sum(t + (int64_t)j * NSS, NSS, 2 * M_PI * f * j * NSS / FS,
                       2 * M_PI * (f + jtty_sync[j] * JTTY_BAUD) / FS);
}

/*
 * Time and frequency of a candidate, from the spectrogram peak (within a
 * few hertz and samples), on the coherent sum over the 13 sync symbols
 * as jtty_peakup's second stage: it peaks within a fraction of a hertz,
 * and the decoder combines the data symbols coherently - at the limit of
 * sensitivity 1 Hz off costs most frames. A frequency step d moves the
 * phasor of symbol j by 2 pi d (j + 1/2) T, so each time step needs the
 * phasors once.
 */
static void refine(int64_t *t, float *f)
{
    int64_t bt = *t, ct;
    float bf = *f, cf, be = -1;

    for (int pass = 0; pass < 2; pass++) {
        int   nt = pass ? 2 : 8, nd = pass ? 25 : 45;
        float step = pass ? 0.02f : 0.1f;
        float complex y[JTTY_NSYNC];

        ct = bt;
        cf = bf;
        be = -1;
        for (int i = -nt; i <= nt; i++) {
            if (ct + i < valid_from)
                continue;
            sync_phasors(ct + i, cf, y);
            for (int d = -nd; d <= nd; d++) {
                double dph = 2 * M_PI * d * step * NSS / FS;
                float  zr = 0, zi = 0, tr;
                float  cr = (float)cos(dph / 2), ci = (float)-sin(dph / 2), wr = (float)cos(dph), wi = (float)-sin(dph);
                for (int j = 0; j < JTTY_NSYNC; j++) {
                    zr += crealf(y[j]) * cr - cimagf(y[j]) * ci;
                    zi += crealf(y[j]) * ci + cimagf(y[j]) * cr;
                    tr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = tr;
                }
                float e = zr * zr + zi * zi;
                if (e > be) {
                    be = e;
                    bt = ct + i;
                    bf = cf + d * step;
                }
            }
        }
    }
    *t = bt;
    *f = bf;
}

/* Correlations of symbol sym (0..58) of the frame at (t, f) with the four
 * tones, on a baseband shifted by f with continuous phase over the frame;
 * h gets the energy of the two half symbols. */
static void symbol_corr(int64_t t, float f, int sym, float complex c[4], float complex h[4])
{
    int64_t n0 = t + (int64_t)sym * NSS;
    double  ph = 2 * M_PI * f * sym * NSS / FS;

    for (int k = 0; k < 4; k++) {
        double dph = 2 * M_PI * (f / FS + (double)k / NSS);
        float complex a = rot_sum(n0, NSS / 2, ph, dph);
        float complex b = rot_sum(n0 + NSS / 2, NSS / 2, ph + dph * (NSS / 2), dph);
        c[k] = a + b;
        if (h)
            h[k] = sqrtf(crealf(a) * crealf(a) + cimagf(a) * cimagf(a) + crealf(b) * crealf(b) + cimagf(b) * cimagf(b));
    }
}

static float p2(float complex z)
{
    return crealf(z) * crealf(z) + cimagf(z) * cimagf(z);
}

static bool is_recent(float f, double t)
{
    for (int i = 0; i < nrecent; i++)
        if (fabsf(recent[i].f - f) < 12.0f && fabs(recent[i].t - t) < 0.1 * FS)
            return true;
    return false;
}

static void remember(float f, double t)
{
    if (nrecent == MAX_RECENT) {
        memmove(recent, recent + 1, sizeof(recent[0]) * (MAX_RECENT - 1));
        nrecent--;
    }
    recent[nrecent].f = f;
    recent[nrecent].t = t;
    nrecent++;
}

static void emit(const active_t *a, bool complete)
{
    jtty_rx_msg_t m = {0};
    const char *s = a->text;
    int o = 0;

    m.id = a->id;
    m.freq = a->f + CF;
    m.snr = a->snr;
    m.complete = complete;
    snprintf(m.call, sizeof(m.call), "%s", a->call);
    m.call_kind = a->call_kind;
    /* display_message_text: a gap of frames as " ... ", '~' is a space */
    while (*s && o < 80) {
        if (strncmp(s, "~~~~~", 5) == 0) {
            o += snprintf(m.text + o, sizeof(m.text) - (size_t)o, " ... ");
            s += 5;
        } else {
            m.text[o++] = *s == '~' ? ' ' : *s;
            s++;
        }
    }
    m.text[o < 80 ? o : 80] = '\0';
    char *p = m.text;
    while (*p == ' ')
        p++;
    memmove(m.text, p, strlen(p) + 1);
    for (o = (int)strlen(m.text); o > 0 && m.text[o - 1] == ' '; o--)
        m.text[o - 1] = '\0';
    if (out_cb && m.text[0])
        out_cb(&m);
}

static void append(char *dst, const char *src)
{
    size_t n = strlen(dst);
    snprintf(dst + n, 81 - n, "%s", src);
}

/* A decoded frame into the messages (start_message / append_active_message) */
static void merge(float f, double t, uint64_t frame, int snr)
{
    jtty_atom_t a;
    bool eom;
    char r[81], piece[81];
    double ts = t / FS;
    int best = -1, gap = 1;
    float bdf = 1e9f;

    if (!jtty_unpack_frame(frame, &a, &eom) || !jtty_render_atom(&a, r))
        return;
    if (a.kind == JTTY_ATOM_TEXT5)
        for (char *p = r; *p; p++)
            if (*p == ' ')
                *p = '~';
    snprintf(piece, sizeof(piece), "%s", r);

    for (int i = 0; i < nact; i++) {
        double dt = ts - act[i].t_last;
        int nfp = (int)lround(dt / FRAME_S);
        float df = fabsf(f - act[i].f);
        if (nfp < 1 || nfp > 3 || fabs(dt - FRAME_S * nfp) >= 0.1 || df >= 10.0f + 3.0f * (nfp - 1))
            continue;
        if (df < bdf) {
            bdf = df;
            best = i;
            gap = nfp;
        }
    }

    active_t *m;
    active_t one;
    if (best >= 0) {
        m = &act[best];
        if (gap > 1) {
            append(m->text, "~~~~~");
            append(m->text, piece[0] == '~' ? piece + 1 : piece);
        } else {
            if (m->sep)
                append(m->text, " ");
            append(m->text, piece);
        }
    } else {
        if (!eom && nact >= MAX_ACTIVE)
            return;
        m = eom ? &one : &act[nact++];
        memset(m, 0, sizeof(*m));
        m->id = next_id++;
        snprintf(m->text, sizeof(m->text), strncmp(piece, "599 ", 4) == 0 ? "~%s" : "%s", piece);
    }
    m->sep = a.kind != JTTY_ATOM_TEXT5;
    m->f = f;
    m->t_last = (float)ts;
    m->snr = snr;
    if (a.kind == JTTY_ATOM_CALL) {
        snprintf(m->call, sizeof(m->call), "%s", a.text);
        m->call_kind = a.subtype;
    }
    emit(m, eom);
    if (eom && m != &one) {
        *m = act[--nact];
    }
}

static void prune(void)
{
    float now = (float)(zn / FS);

    for (int i = 0; i < nact;) {
        if (now - act[i].t_last > 4 * FRAME_S + 0.1f + 2.0f) {
            act[i] = act[--nact];
        } else {
            i++;
        }
    }
}

static void try_candidate(const cand_t *c)
{
    int64_t t = (int64_t)c->t;
    float f = c->f, pow[JTTY_NSYNC][4];
    float complex corr[JTTY_NDATA][4], half[JTTY_NDATA][4], cs[4];
    float pt = 0, pa = 0;
    int nsync = 0;
    uint64_t frame;

    if (is_recent(f, c->t) || t < valid_from)
        return;
    refine(&t, &f);
    if (is_recent(f, (double)t) || t + FRAME_SAMP + 16 > zn)
        return;

    for (int j = 0; j < JTTY_NSYNC; j++) {
        int best = 0;
        symbol_corr(t, f, j, cs, NULL);
        for (int k = 0; k < 4; k++) {
            pow[j][k] = p2(cs[k]);
            if (pow[j][k] > pow[j][best])
                best = k;
        }
        nsync += best == jtty_sync[j];
        pt += pow[j][jtty_sync[j]];
        pa += pow[j][0] + pow[j][1] + pow[j][2] + pow[j][3];
    }
    float pn = (pa - pt) / 3.0f, snr = pn > 0 ? 10.0f * log10f(pt / pn) : -99.0f;
    bool in_win = fabsf(f + CF - rx_f0) < rx_tol;
    /* Outside the Rx window WSJT-X asks nsync > 8 and 5 dB, over 600 Hz;
     * here it is the whole passband, four times the noise to pick from */
    if (in_win ? (nsync <= 6 || snr < 4.6f) : (nsync <= 9 || snr < 5.5f))
        return;

    for (int s = 0; s < JTTY_NDATA; s++)
        symbol_corr(t, f, JTTY_NSYNC + s, corr[s], half[s]);
    if (!jtty_decode_frame(corr, half, &frame))
        return;
    jtty_atom_t a;
    bool eom;
    if (!jtty_unpack_frame(frame, &a, &eom))
        return;

    /* S/N over the whole frame with the decoded tones, in 2500 Hz */
    uint8_t tones[JTTY_NSYM];
    jtty_frame_tones(frame, tones);
    for (int s = 0; s < JTTY_NDATA; s++) {
        float complex *cc = corr[s];
        pt += p2(cc[tones[JTTY_NSYNC + s]]);
        pa += p2(cc[0]) + p2(cc[1]) + p2(cc[2]) + p2(cc[3]);
    }
    pn = (pa - pt) / 3.0f;
    float ratio = pn > 0 ? pt / pn - 1.0f : 0.001f;
    int snr2500 = (int)lroundf(10.0f * log10f(ratio > 0.001f ? ratio : 0.001f) - 19.0f);
    /* The CRC lets a wrong word through now and then on noise. Those
     * measure -18 dB or less over the frame with their own tones, where
     * nothing real decodes (a single frame: half at -15 dB, 1 in 30 at
     * -17); real frames at the limit measure -17 and up. */
    if (snr2500 < -17)
        return;

    remember(f, (double)t);
    merge(f, (double)t, frame, snr2500);
}

/* Candidates whose frame has fully arrived, strongest first */
static void run_candidates(void)
{
    for (;;) {
        int b = -1;
        for (int i = 0; i < ncand; i++)
            if ((int64_t)cand[i].t + FRAME_SAMP + 64 <= zn && (b < 0 || cand[i].score > cand[b].score))
                b = i;
        if (b < 0)
            break;
        cand_t c = cand[b];
        cand[b] = cand[--ncand];
        try_candidate(&c);
    }
}

/* ---- Sample path ------------------------------------------------------ */

static void put_baseband(float complex z)
{
    ring[zn & (RING - 1)] = z;
    zn++;
    if (zn % HOP == 0 && zn >= NSS) {
        spectrum_row();
        nhop++;
        /* row h starts at sample h * HOP; a frame whose sync starts at
         * row s has its last sync symbol at row s + 96 */
        if (nhop - 1 - 96 >= 0)
            score_row(nhop - 1 - 96);
        run_candidates();
        if (nhop % 64 == 0)
            prune();
    }
}

static void process(const float *x, int n)
{
    static const float complex mix[8] = {
        1, 0.70710678f - 0.70710678f * I, -I, -0.70710678f - 0.70710678f * I,
        -1, -0.70710678f + 0.70710678f * I, I, 0.70710678f + 0.70710678f * I,
    };   /* e^-j pi m / 4: 1500 Hz at 12000 Hz */
    float y[1024];

    while (n > 0) {
        int m = n > 512 ? 512 : n, k = olivia_resamp_process(&rs, x, m, y);
        for (int i = 0; i < k; i++) {
            float complex v = y[i] * mix[mix_ph];
            mix_ph = (mix_ph + 1) & 7;
            fir_hist[fir_pos] = fir_hist[fir_pos + NFIR] = v;
            fir_pos = (fir_pos + 1) % NFIR;
            if (++dec_cnt < 3)
                continue;
            dec_cnt = 0;
            float complex acc = 0;
            const float complex *h = fir_hist + fir_pos;     /* oldest first */
            for (int j = 0; j < NFIR; j++)
                acc += h[j] * fir[NFIR - 1 - j];
            put_baseband(acc);
        }
        x += m;
        n -= m;
    }
}

static void do_reset(void)
{
    memset(ring, 0, sizeof(ring));
    memset(spec, 0, sizeof(spec));
    memset(score, 0, sizeof(score));
    ncand = 0;
    valid_from = zn + NFIR;
}

static void *worker(void *arg)
{
    float buf[1024];

    (void)arg;
    pthread_mutex_lock(&mux);
    while (!quit) {
        if (reset_req) {
            reset_req = false;
            f_tail = f_head;
            do_reset();
        }
        if (f_tail == f_head) {
            busy = false;
            pthread_cond_wait(&cond, &mux);
            continue;
        }
        size_t n = 0;
        while (f_tail != f_head && n < 1024) {
            buf[n++] = fifo[f_tail];
            f_tail = (f_tail + 1) % FIFO;
        }
        busy = true;
        pthread_mutex_unlock(&mux);
        process(buf, (int)n);
        pthread_mutex_lock(&mux);
    }
    busy = false;
    pthread_mutex_unlock(&mux);
    return NULL;
}

/* ---- Public ----------------------------------------------------------- */

void jtty_rx_start(jtty_rx_cb_t cb)
{
    if (running)
        return;
    fft_init();
    front_init();
    olivia_resamp_init(&rs, JTTY_RX_RATE, 12000);
    zn = nhop = 0;
    valid_from = NFIR;
    ncand = nact = nrecent = 0;
    memset(ring, 0, sizeof(ring));
    memset(spec, 0, sizeof(spec));
    memset(score, 0, sizeof(score));
    f_head = f_tail = 0;
    out_cb = cb;
    quit = reset_req = false;
    running = pthread_create(&thread, NULL, worker, NULL) == 0;
}

void jtty_rx_stop(void)
{
    if (!running)
        return;
    pthread_mutex_lock(&mux);
    quit = true;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mux);
    pthread_join(thread, NULL);
    running = false;
    out_cb = NULL;
    olivia_resamp_free(&rs);
}

void jtty_rx_put_audio(size_t n, const float *x)
{
    if (!running)
        return;
    pthread_mutex_lock(&mux);
    for (size_t i = 0; i < n; i++) {
        size_t nh = (f_head + 1) % FIFO;
        if (nh == f_tail)
            break;              /* the worker is behind: drop */
        fifo[f_head] = x[i];
        f_head = nh;
    }
    busy = true;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mux);
}

void jtty_rx_drain(void)
{
    for (;;) {
        pthread_mutex_lock(&mux);
        bool idle = !running || (f_tail == f_head && !busy);
        pthread_mutex_unlock(&mux);
        if (idle)
            return;
        usleep(1000);
    }
}

void jtty_rx_set_range(float lo, float hi)
{
    rng_lo = lo;
    rng_hi = hi;
}

void jtty_rx_set_freq(float f0, float tol)
{
    rx_f0 = f0;
    rx_tol = tol;
}

void jtty_rx_reset(void)
{
    pthread_mutex_lock(&mux);
    reset_req = true;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mux);
}
