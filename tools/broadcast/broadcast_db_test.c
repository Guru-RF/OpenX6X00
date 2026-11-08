/* broadcast_db_find_matches() by the frequency index against the scan of the whole list (the index switched off), on
 * the EiBi list the image ships: every 500 Hz from 0 to 30 MHz and around every station, three tolerances, max 1, 4
 * and 16. Same stations in the same order, or a FAIL line. Then the time of one lookup each way.
 *
 * broadcast_db.c is included, for its index. The list is copied to ./broadcast.csv (BOARD_DATA) on the first run.
 * `broadcast_db_test bench`: the times only (the comparison scans the whole list 800000 times: minutes on a radio). */
#include "broadcast_db.c"

#include <time.h>

static int same(const broadcast_match_t *a, size_t na, const broadcast_match_t *b, size_t nb) {
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++)
        if (a[i].freq_khz != b[i].freq_khz || strcmp(a[i].station, b[i].station) || a[i].start_minute != b[i].start_minute)
            return 0;
    return 1;
}

static size_t scan(int32_t f, int tol, broadcast_match_t *out, size_t max) {
    by_freq_t *keep = by_freq;
    size_t     n;

    by_freq = NULL;
    n = broadcast_db_find_matches(f, tol, out, max);
    by_freq = keep;
    return n;
}

static int check(int32_t f, int tol, size_t max) {
    static broadcast_match_t a[BROADCAST_MAX_MATCHES], b[BROADCAST_MAX_MATCHES];

    for (int tries = 0; tries < 2; tries++) {       /* again if the minute turned between the two */
        size_t na = broadcast_db_find_matches(f, tol, a, max), nb = scan(f, tol, b, max);

        if (same(a, na, b, nb)) return 1;
    }
    printf("FAIL f=%d tol=%d max=%zu\n", f, tol, max);
    return 0;
}

static double now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    static const int    tols[] = {0, 500, 2000};
    static const size_t maxes[] = {1, 4, BROADCAST_MAX_MATCHES};
    static broadcast_match_t m[BROADCAST_MAX_MATCHES];
    size_t              cases = 0, bad = 0;

    if (!broadcast_db_init() || !by_freq) {
        printf("FAIL: no list (%zu entries) or no index\n", list_len);
        return 1;
    }
    for (int t = 0; t < 3 && !(argc > 1 && !strcmp(argv[1], "bench")); t++) {
        for (size_t k = 0; k < 3; k++) {
            for (int32_t f = 0; f <= 30000000; f += 500, cases++) bad += !check(f, tols[t], maxes[k]);
            for (size_t i = 0; i < list_len; i++) {
                int32_t f = (int32_t) (list[i].m.freq_khz * 1000.0);

                for (int d = -tols[t] - 1; d <= tols[t] + 1; d += tols[t] + 1, cases++)
                    bad += !check(f + d, tols[t], maxes[k]);
            }
        }
    }

    int     reps = 2000;
    double  t0 = now_ms();

    for (int i = 0; i < reps; i++) broadcast_db_find_matches(5000000 + i * 3000, 2000, m, BROADCAST_MAX_MATCHES);
    double  t1 = now_ms();

    for (int i = 0; i < reps; i++) scan(5000000 + i * 3000, 2000, m, BROADCAST_MAX_MATCHES);
    double  t2 = now_ms();

    printf("%s: %zu lookups, %zu stations; one lookup %.1f us indexed, %.1f us scanned\n", bad ? "FAIL" : "OK", cases,
           list_len, (t1 - t0) * 1e3 / reps, (t2 - t1) * 1e3 / reps);
    return bad ? 1 : 0;
}
