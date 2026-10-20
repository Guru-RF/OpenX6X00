/*
 * Line-sync follower (see sstv_sync.h).
 *
 * Pulses: a 2-ms sliding correlation with 1200 Hz; the score is the share of
 * the window energy at 1200 Hz, so it does not depend on the level.  A pulse
 * starts above 0.55 and ends below 0.40.  Real 4.9-10 ms syncs measure 6-12
 * ms here; anything under 5 ms is dropped (noise makes many short pulses,
 * almost no long ones).
 *
 * Mode: for every new pulse and every mode, is there a pulse one line period
 * earlier (or two, if one sync was lost)?  The periods are far apart against
 * the timing error (Martin M1 446.4 ms, Scottie S1 428.2 ms, tolerance 3 ms),
 * so the period alone names the mode.  A pulse extends the chain of the one
 * it links to: random pulses pair up now and then but hardly ever chain.
 * Most links must be one line long, otherwise Robot 72 (300 ms) would pass
 * as Robot 36 (150 ms) with every other sync missing.  Five chained syncs
 * lock; if several modes qualify the shortest line wins.
 *
 * Locked: a pulse on the line grid (any number of lines later) and as long
 * as the syncs seen so far (2 ms) is a sync.  The line period is a least
 * squares fit of the last syncs, so long fades keep the phase.
 *
 * End: no sync for max(8 lines, 10 s) AND no picture for 10 s.  On real
 * recordings the syncs can drown while the picture still reads.  Picture
 * means: of the energy above 300 Hz, at least 65% in 1100-2400 Hz over a
 * 1-s block (4th-order Butterworth filters).  Flat SSB noise gives up to
 * 0.55, pictures about 0.8.
 */
#include "sstv_sync.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#define OSC_LEN     147         /* 1200 Hz: 16 periods in 147 samples */
#define RISE        0.55
#define FALL        0.40
#define SHORTEST_MS 5.0
#define LONGEST_MS  14.0
#define TO_LOCK     5
#define TOL_MS      3.0
#define TOL_SHARE   0.006       /* of the line, if larger than TOL_MS */
#define TOL_DRIFT   0.0003      /* more per sample of distance        */
#define SAME_LEN_MS 2.0
#define LOST_LINES  8
#define LOST_S      10.0
#define PICTURE     0.65
#define QUIET_S     10.0

/* Mode order of the chain tables.  It matters only for ties. */
static const sstv_mode_t order[SSTV_SYNC_MODES] = {
    SSTV_ROBOT8, SSTV_ROBOT36, SSTV_ROBOT72, SSTV_MARTIN2, SSTV_MARTIN1, SSTV_SCOTTIE2, SSTV_SCOTTIE1, SSTV_SCOTTIE_DX,
};

typedef struct { float b0, b1, b2, a1, a2; } biquad_t;

static biquad_t       filters[6];      /* high-pass 1100 x2, low-pass 2400 x2, high-pass 300 x2 */
static float          osc_c[OSC_LEN], osc_s[OSC_LEN];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

/* Audio EQ cookbook (R. Bristow-Johnson), divided by a0 */
static void biquad(biquad_t *f, double hz, bool high, double q)
{
    double w = 2.0 * M_PI * hz / SSTV_RATE, c = cos(w), alpha = sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    double b0 = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
    double b1 = high ? -(1.0 + c) : (1.0 - c);

    f->b0 = (float)(b0 / a0);
    f->b1 = (float)(b1 / a0);
    f->b2 = (float)(b0 / a0);
    f->a1 = (float)(-2.0 * c / a0);
    f->a2 = (float)((1.0 - alpha) / a0);
}

/* transposed direct form II */
static inline float run(const biquad_t *f, float *z, float x)
{
    float y = f->b0 * x + z[0];
    z[0] = f->b1 * x - f->a1 * y + z[1];
    z[1] = f->b2 * x - f->a2 * y;
    return y;
}

static void tables_make(void)
{
    static const double q[2] = { 0.5411961, 1.3065630 };   /* 4th-order Butterworth */

    for (int k = 0; k < 2; ++k) {
        biquad(&filters[k], 1100.0, true, q[k]);
        biquad(&filters[2 + k], 2400.0, false, q[k]);
        biquad(&filters[4 + k], 300.0, true, q[k]);
    }
    for (int i = 0; i < OSC_LEN; ++i) {
        osc_c[i] = (float)cos(2.0 * M_PI * 1200.0 * i / SSTV_RATE);
        osc_s[i] = (float)sin(2.0 * M_PI * 1200.0 * i / SSTV_RATE);
    }
}

static int row_of(sstv_mode_t mode)
{
    for (int i = 0; i < SSTV_SYNC_MODES; ++i)
        if (order[i] == mode) return i;
    return -1;
}

static double line_ms(int row) { return sstv_mode_info(order[row])->line_ms; }
static double line_len(int row) { return line_ms(row) * SSTV_RATE / 1000.0; }

static double tolerance(int row)
{
    double t = TOL_MS * SSTV_RATE / 1000.0, share = TOL_SHARE * line_len(row);
    return t > share ? t : share;
}

double sstv_sync_line_s(sstv_mode_t mode)
{
    int r = row_of(mode);
    return r < 0 ? 0.0 : line_ms(r) / 1000.0;
}

double sstv_sync_picture_s(sstv_mode_t mode)
{
    int r = row_of(mode);
    return r < 0 ? 0.0 : line_ms(r) * sstv_mode_info(mode)->height / 1000.0;
}

void sstv_sync_init(sstv_sync_t *s, sstv_mode_t only)
{
    pthread_once(&tables_once, tables_make);
    memset(s, 0, sizeof(*s));
    s->only = row_of(only) >= 0 ? only : SSTV_UNKNOWN;
    s->cur = -1;
    s->st.state = SSTV_SYNC_SEARCH;
    s->st.mode = SSTV_UNKNOWN;
}

/* One more (line, time) point; the period is refitted from 6 points on and
 * kept within 5000 ppm of the nominal one */
static void fit_add(sstv_sync_t *s, double line, double at)
{
    s->fit_line[s->fit_head] = line;
    s->fit_at[s->fit_head] = at;
    s->fit_head = (s->fit_head + 1) % SSTV_SYNC_FIT;
    if (s->fit_n < SSTV_SYNC_FIT) ++s->fit_n;
    if (s->fit_n < 6) return;

    double sl = 0, st = 0, sll = 0, slt = 0;
    for (int k = 0; k < s->fit_n; ++k) {
        sl += s->fit_line[k];
        st += s->fit_at[k];
        sll += s->fit_line[k] * s->fit_line[k];
        slt += s->fit_line[k] * s->fit_at[k];
    }
    double d = s->fit_n * sll - sl * sl;
    if (fabs(d) < 1e-9) return;
    double p = (s->fit_n * slt - sl * st) / d, nominal = line_len(s->cur);
    if (p < nominal * 0.995) p = nominal * 0.995;
    if (p > nominal * 1.005) p = nominal * 1.005;
    s->st.period = p;
    s->st.clock_ppm = (p / nominal - 1.0) * 1e6;
}

/* The latest remembered pulse one period (+- tol) before t, or -1 */
static int earlier(const sstv_sync_t *s, uint64_t t, double period, double tol)
{
    for (int k = 1; k < s->count; ++k) {
        int i = (s->head - 1 - k + SSTV_SYNC_PULSES) % SSTV_SYNC_PULSES;
        if (s->at[i] >= t) continue;               /* lock() starts from older pulses */
        double d = (double)(t - s->at[i]);
        if (d > period + tol) return -1;           /* older ones are further away */
        if (fabs(d - period) <= tol) return i;
    }
    return -1;
}

static void lock(sstv_sync_t *s, int row, uint64_t t)
{
    double far = LOST_LINES * line_len(row), min = LOST_S * SSTV_RATE;

    s->cur = row;
    s->st.state = SSTV_SYNC_LOCKED;
    s->st.mode = order[row];
    s->st.period = line_len(row);
    s->st.last_sync = t;
    s->st.hits = 1;
    s->lost_after = (uint64_t)(far > min ? far : min);

    /* the chain that locked starts the period fit */
    double   line = 0.0;
    uint64_t at = t;
    fit_add(s, 0.0, (double)t);
    for (int k = 0; k < 8; ++k) {
        int i = earlier(s, at, line_len(row), tolerance(row));
        if (i < 0) break;
        line -= 1.0;
        at = s->at[i];
        fit_add(s, line, (double)at);
    }
    s->st.first_sync = at;
    s->line = 0.0;
    s->avg_ms = s->ms[(s->head - 1 + SSTV_SYNC_PULSES) % SSTV_SYNC_PULSES];
}

static void pulse(sstv_sync_t *s, uint64_t t, double ms)
{
    if (ms < SHORTEST_MS || ms > LONGEST_MS) return;
    int me = s->head;
    s->at[me] = t;
    s->ms[me] = (float)ms;
    s->head = (s->head + 1) % SSTV_SYNC_PULSES;
    if (s->count < SSTV_SYNC_PULSES) ++s->count;

    /* chains for every mode, kept up after the lock too (classify) */
    for (int m = 0; m < SSTV_SYNC_MODES; ++m) {
        s->chain[me][m] = 1;
        s->single[me][m] = 0;
        if (s->only != SSTV_UNKNOWN && order[m] != s->only) continue;
        int one = 1, prev = earlier(s, t, line_len(m), tolerance(m));
        if (prev < 0) {
            prev = earlier(s, t, 2.0 * line_len(m), tolerance(m));
            one = 0;
        }
        if (prev < 0) continue;
        int c = s->chain[prev][m] + 1, d = s->single[prev][m] + one;
        s->chain[me][m] = (uint8_t)(c > 250 ? 250 : c);
        s->single[me][m] = (uint8_t)(d > 250 ? 250 : d);
        if (one) ++s->links[m];
    }

    if (s->st.state == SSTV_SYNC_SEARCH) {
        int best = -1;
        for (int m = 0; m < SSTV_SYNC_MODES; ++m)
            if (s->chain[me][m] >= TO_LOCK && 2 * s->single[me][m] >= s->chain[me][m] &&
                (best < 0 || line_ms(m) < line_ms(best)))
                best = m;
        if (best >= 0) lock(s, best, t);
        return;
    }
    if (s->st.state != SSTV_SYNC_LOCKED) return;

    /* on the grid? */
    double since = (double)(t - s->st.last_sync);
    double lines = floor(since / s->st.period + 0.5);
    if (lines < 1.0) return;
    double off = since - lines * s->st.period;
    if (fabs(off) > tolerance(s->cur) + TOL_DRIFT * since) return;
    if (s->st.hits >= 4 && fabs(ms - s->avg_ms) > SAME_LEN_MS) return;
    s->avg_ms += 0.1 * (ms - s->avg_ms);
    s->line += lines;
    s->st.last_sync = t;
    ++s->st.hits;
    fit_add(s, s->line, (double)t);
}

void sstv_sync_feed(sstv_sync_t *s, const float *x, size_t n)
{
    const int W = SSTV_SYNC_WIN;

    for (size_t i = 0; i < n; ++i) {
        float v = x[i];
        float re = v * osc_c[s->osc], im = v * osc_s[s->osc], pw = v * v;
        s->osc = (s->osc + 1) % OSC_LEN;
        s->sre += re - s->wre[s->wpos];
        s->sim += im - s->wim[s->wpos];
        s->spow += pw - s->wpow[s->wpos];
        s->wre[s->wpos] = re;
        s->wim[s->wpos] = im;
        s->wpow[s->wpos] = pw;
        s->wpos = (s->wpos + 1) % W;
        uint64_t now = s->st.samples++;

        /* picture monitor */
        float band = run(&filters[0], s->flt[0], v);
        band = run(&filters[1], s->flt[1], band);
        band = run(&filters[2], s->flt[2], band);
        band = run(&filters[3], s->flt[3], band);
        float all = run(&filters[4], s->flt[4], v);
        all = run(&filters[5], s->flt[5], all);
        s->e_band += (double)band * band;
        s->e_all += (double)all * all;
        if (++s->e_count >= SSTV_RATE) {
            float ratio = s->e_all > 1e-12 ? (float)(s->e_band / s->e_all) : 0.0f;
            if (ratio >= PICTURE) s->picture_at = now + 1;
            s->e_band = s->e_all = 0.0;
            s->e_count = 0;
        }

        if (s->wpos == 0) {             /* fresh sums now and then: no float drift */
            double a = 0, b = 0, c = 0;
            for (int k = 0; k < W; ++k) {
                a += s->wre[k];
                b += s->wim[k];
                c += s->wpow[k];
            }
            s->sre = a;
            s->sim = b;
            s->spow = c;
        }
        double score = s->spow > 1e-12 && now + 1 >= (uint64_t)W
                     ? 2.0 * (s->sre * s->sre + s->sim * s->sim) / ((double)W * s->spow) : 0.0;
        if (!s->inside) {
            if (score >= RISE) {
                s->inside = true;
                s->rise = now;
            }
        } else if (score < FALL) {
            /* the window was all inside the pulse while the score was high */
            s->inside = false;
            uint64_t start = s->rise + 1 >= (uint64_t)W ? s->rise + 1 - W : 0;
            pulse(s, start, (double)(now - s->rise + W - 1) * 1000.0 / SSTV_RATE);
        }
    }
    if (s->st.state == SSTV_SYNC_LOCKED && s->st.samples - s->st.last_sync > s->lost_after &&
        s->st.samples - s->picture_at > (uint64_t)(QUIET_S * SSTV_RATE))
        s->st.state = SSTV_SYNC_LOST;
}

sstv_mode_t sstv_sync_classify(const float *x, long n, sstv_mode_t prefer, long *links)
{
    sstv_sync_t s;              /* ~3 kB of stack */

    if (links) *links = 0;
    if (!x || n <= 0) return SSTV_UNKNOWN;
    sstv_sync_init(&s, SSTV_UNKNOWN);
    sstv_sync_feed(&s, x, (size_t)n);

    /* Most line intervals wins.  A shorter mode whose line divides the
     * winner's (Robot 36 also links Robot 72 two lines back, Scottie DX
     * seven) takes over with at least half of its evidence. */
    int best = -1;
    for (int m = 0; m < SSTV_SYNC_MODES; ++m)
        if (best < 0 || s.links[m] > s.links[best]) best = m;
    if (best < 0 || s.links[best] < TO_LOCK) return SSTV_UNKNOWN;
    int p = row_of(prefer);
    if (p >= 0 && p != best && 2 * s.links[p] >= s.links[best] && s.links[p] >= TO_LOCK)
        best = p;
    for (int m = 0; m < SSTV_SYNC_MODES; ++m) {
        double ratio = line_ms(best) / line_ms(m);
        if (ratio > 1.5 && fabs(ratio - floor(ratio + 0.5)) < 0.01 && 2 * s.links[m] >= s.links[best] &&
            line_ms(m) < line_ms(best))
            best = m;
    }
    if (links) *links = s.links[best];
    return order[best];
}
