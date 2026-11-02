/*
 * olivia_test.c - host test of the Olivia modem (src/olivia*.c)
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Golden values: hashes of the symbols and the audio of fixed blocks, taken
 * from the code before it learned a second protocol. A change here means
 * Olivia changed on the air. Loopback: text through the modulator, noise and
 * a tuning error, into the receiver, and back out.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "olivia.h"
#include "olivia_mod.h"
#include "olivia_rx.h"

#define RATE   8000
#define CENTER 1500

#define MOD_INIT(m, p, t, b, e)    olivia_mod_init(m, p, t, b, RATE, e)
#define RX_INIT(r, p, t, b, e, mg) olivia_rx_init(r, p, t, b, RATE, e, mg)

/* --sweep: a lock threshold to try, 0 for the receiver's own. */
static float try_threshold = 0.0f;

static int failures = 0;

#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
            failures++;                       \
        }                                     \
    } while (0)

/* --- Noise ------------------------------------------------------------- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static double uniform(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) / 9007199254740992.0;
}

static double gauss(void)
{
    double u = uniform();
    double v = uniform();

    if (u < 1e-12)
        u = 1e-12;
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

/* --- Golden ------------------------------------------------------------ */

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;

    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

typedef struct {
    int tones, bw;
    uint32_t sym, audio;
} golden_t;

#include "olivia_golden.inc"

static void golden_one(int tones, int bw, uint32_t *sym_h, uint32_t *audio_h)
{
    olivia_mod_t  mod;
    uint8_t       chars[OLIVIA_MAX_BITS];
    uint8_t       symbols[64];
    float        *audio;
    int           n, i;

    *sym_h = *audio_h = 2166136261u;
    CHECK(MOD_INIT(&mod, OLIVIA_PROTO_OLIVIA, tones, bw, CENTER - bw / 2.0) == 0,
          "mod init %d/%d", tones, bw);
    olivia_mod_seed(&mod, 12345);
    audio = malloc(sizeof(float) * olivia_mod_block_samples(&mod));

    /* Every 7-bit code, in blocks. */
    for (int c = 0; c < 128; c += mod.cfg.bits) {
        for (i = 0; i < mod.cfg.bits; i++)
            chars[i] = (uint8_t)((c + i) & 0x7f);
        olivia_encode_block(&mod.cfg, chars, symbols);
        *sym_h = fnv(*sym_h, symbols, 64);
        n = olivia_mod_block(&mod, chars, audio);
        for (i = 0; i < n; i++) {
            int32_t v = (int32_t)lrintf(audio[i] * 4096.0f);

            *audio_h = fnv(*audio_h, &v, sizeof(v));
        }
    }
    free(audio);
    olivia_mod_free(&mod);
}

/* --- Loopback ---------------------------------------------------------- */

typedef struct {
    char   text[4096];
    size_t len;
} sink_t;

static void on_text(void *user, const uint8_t *chars, int count)
{
    sink_t *s = user;

    for (int i = 0; i < count; i++) {
        if (chars[i] && s->len + 1 < sizeof(s->text)) {
            s->text[s->len++] = (char)chars[i];
            s->text[s->len] = '\0';
        }
    }
}

/*
 * Send msg with `shift_hz` of tuning error at a level of snr_db measured in
 * 2500 Hz (the usual way of quoting these modes). Returns 1 when the receiver
 * printed msg whole.
 */
static int loopback(int proto, int tones, int bw, const char *msg,
                    double shift_hz, double snr_db, int *chars_ok)
{
    olivia_mod_t mod;
    olivia_rx_t  rx;
    sink_t       sink = {.len = 0};
    double       edge = CENTER - bw / 2.0;
    double       noise_sd = 0.0, sig_pow;
    float       *audio;
    int          bits, n, blocks, ok;
    size_t       pos = 0, msg_len = strlen(msg);

    if (MOD_INIT(&mod, proto, tones, bw, edge + shift_hz) < 0)
        return -1;
    if (RX_INIT(&rx, proto, tones, bw, edge, 4) < 0) {
        olivia_mod_free(&mod);
        return -1;
    }
    if (try_threshold > 0.0f)
        rx.lock_threshold = try_threshold;
    olivia_rx_set_callback(&rx, on_text, &sink);
    olivia_mod_seed(&mod, 777);

    bits = mod.cfg.bits;
    n = olivia_mod_block_samples(&mod);
    audio = malloc(sizeof(float) * n);

    /* 4 blocks of run-in, the text, 2 closing blocks, as olivia_tx.c does. */
    blocks = 4 + (int)((msg_len + bits - 1) / bits) + 3;
    sig_pow = 0.0;

    for (int b = 0; b < blocks; b++) {
        uint8_t chars[OLIVIA_MAX_BITS] = {0};

        if (b >= 4) {
            for (int i = 0; i < bits && pos < msg_len; i++)
                chars[i] = olivia_char_encode(&mod.cfg, (uint8_t)msg[pos++]);
        }
        olivia_mod_block(&mod, chars, audio);

        if (b == 0) {
            for (int i = n / 2; i < n; i++)
                sig_pow += (double)audio[i] * audio[i];
            sig_pow /= (n - n / 2);
            /* Noise power in 4000 Hz of real samples, for the SNR in 2500. */
            noise_sd = sqrt(sig_pow / pow(10.0, snr_db / 10.0) * 4000.0 / 2500.0);
        }
        if (snr_db < 100.0) {
            for (int i = 0; i < n; i++)
                audio[i] += (float)(noise_sd * gauss());
        }
        /* Odd chunks: the remainder handling is part of the test. */
        for (int i = 0; i < n; i += 701) {
            int k = n - i < 701 ? n - i : 701;

            olivia_rx_process(&rx, audio + i, k);
        }
    }

    /* Decoded back through the alphabet for the comparison. */
    for (size_t i = 0; i < sink.len; i++)
        sink.text[i] = (char)olivia_char_decode(&mod.cfg, (uint8_t)sink.text[i]);

    ok = strstr(sink.text, msg) != NULL;
    if (chars_ok) {
        int good = 0;

        for (size_t i = 0; i + msg_len <= sink.len; i++) {
            int m = 0;

            for (size_t j = 0; j < msg_len; j++)
                m += sink.text[i + j] == msg[j];
            if (m > good)
                good = m;
        }
        *chars_ok = good;
    }

    free(audio);
    olivia_mod_free(&mod);
    olivia_rx_free(&rx);
    return ok;
}

/*
 * False locks on noise, as a count of receiver starts. Starts are what count:
 * the score has only two blocks of averaging when lock first becomes possible,
 * later it has four and rarely gets there.
 */
static int noise_locks(int proto, int tones, int bw, int runs)
{
    olivia_rx_t rx;
    float       buf[2048];
    int         locks = 0;

    RX_INIT(&rx, proto, tones, bw, CENTER - bw / 2.0, 4);
    if (try_threshold > 0.0f)
        rx.lock_threshold = try_threshold;
    for (int run = 0; run < runs; run++) {
        int total = (int)(10.0 * olivia_block_period(&rx.cfg) * RATE);
        int locked = 0;

        olivia_rx_reset(&rx);
        for (int s = 0; s < total; s += 2048) {
            for (int k = 0; k < 2048; k++)
                buf[k] = (float)(0.1 * gauss());
            olivia_rx_process(&rx, buf, 2048);
            locked |= olivia_rx_locked(&rx);
        }
        locks += locked;
    }
    olivia_rx_free(&rx);
    return locks;
}

/* Measurements, not checks: false locks on noise and sensitivity. */
static void sweep(int only_tones)
{
    static const struct { int proto, tones, bw; } m[] = {
        {0, 8, 250}, {0, 16, 500}, {0, 32, 1000},
        {1, 4, 250}, {1, 8, 250}, {1, 4, 500}, {1, 8, 500}, {1, 16, 500},
        {1, 8, 1000}, {1, 16, 1000}, {1, 32, 1000},
    };
    const char *msg = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789";

    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        int         locks, runs = 200;
        double      thr = 99;

        if (only_tones && m[i].tones != only_tones)
            continue;

        locks = noise_locks(m[i].proto, m[i].tones, m[i].bw, runs);

        for (double db = 0.0; db >= -18.0; db -= 1.0) {
            int good = 0;

            for (int r = 0; r < 8; r++) {
                int ok = 0;

                loopback(m[i].proto, m[i].tones, m[i].bw, msg, 7.0, db, &ok);
                good += ok >= (int)strlen(msg) * 9 / 10;
            }
            if (good >= 6)
                thr = db;
            else
                break;
        }
        printf("%-9s %3d/%-4d thr %.1f: false lock in %d of %d starts, 75%% copy at %.0f dB\n",
               m[i].proto ? "Contestia" : "Olivia", m[i].tones, m[i].bw,
               try_threshold, locks, runs, thr);
    }
}

int main(int argc, char **argv)
{
    static const struct { int tones, bw; } olivia_modes[] = {
        {8, 250}, {16, 500}, {32, 1000}, {4, 250}, {4, 500}, {8, 500},
        {8, 1000}, {16, 1000}, {64, 2000},
    };

    static const struct { int tones, bw; } contestia_modes[] = {
        {4, 250}, {8, 250}, {4, 500}, {8, 500}, {16, 500}, {8, 1000}, {16, 1000}, {32, 1000},
    };

    if (argc > 1 && strcmp(argv[1], "--sweep") == 0) {
        if (argc > 2)
            try_threshold = (float)atof(argv[2]);
        sweep(argc > 3 ? atoi(argv[3]) : 0);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--golden") == 0) {
        for (size_t i = 0; i < sizeof(olivia_modes) / sizeof(olivia_modes[0]); i++) {
            uint32_t s, a;

            golden_one(olivia_modes[i].tones, olivia_modes[i].bw, &s, &a);
            printf("    {%d, %d, 0x%08xu, 0x%08xu},\n", olivia_modes[i].tones,
                   olivia_modes[i].bw, s, a);
        }
        return failures != 0;
    }

    for (size_t i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
        uint32_t s, a;

        golden_one(golden[i].tones, golden[i].bw, &s, &a);
        CHECK(s == golden[i].sym, "Olivia %d/%d symbols changed: %08x", golden[i].tones,
              golden[i].bw, s);
        CHECK(a == golden[i].audio, "Olivia %d/%d audio changed: %08x", golden[i].tones,
              golden[i].bw, a);
    }

    {
        const char *msg = "CQ CQ DE N0CALL N0CALL K";

        for (size_t i = 0; i < sizeof(olivia_modes) / sizeof(olivia_modes[0]); i++) {
            int t = olivia_modes[i].tones, b = olivia_modes[i].bw;

            CHECK(loopback(OLIVIA_PROTO_OLIVIA, t, b, msg, 0.0, 200.0, NULL) == 1,
                  "Olivia %d/%d clean loopback", t, b);
            CHECK(loopback(OLIVIA_PROTO_OLIVIA, t, b, msg, 20.0, 0.0, NULL) == 1,
                  "Olivia %d/%d loopback, 0 dB, 20 Hz off", t, b);
        }
    }

    /* The alphabet: every printable character there and back. */
    {
        olivia_cfg_t c;

        CHECK(olivia_cfg_init(&c, OLIVIA_PROTO_CONTESTIA, 8, 250) == 0, "cfg");
        CHECK(c.block_symbols == 32, "block length");
        for (int ch = '!'; ch <= 'Z'; ch++)
            CHECK(olivia_char_decode(&c, olivia_char_encode(&c, (uint8_t)ch)) == ch,
                  "alphabet %c", ch);
        CHECK(olivia_char_encode(&c, 'a') == olivia_char_encode(&c, 'A'), "lower case");
        CHECK(olivia_char_decode(&c, olivia_char_encode(&c, ' ')) == ' ', "space");
        CHECK(olivia_char_decode(&c, olivia_char_encode(&c, '\r')) == '\r', "CR");
        CHECK(olivia_char_encode(&c, '~') == olivia_char_encode(&c, '?'), "no code");
        CHECK(olivia_char_encode(&c, 0) == 0 && olivia_char_decode(&c, 0) == 0, "filler");
        for (int ch = 0; ch < 256; ch++)
            CHECK(olivia_char_encode(&c, (uint8_t)ch) < 64, "code range %d", ch);
    }

    /* Every code through a block of each width, no noise. */
    for (int tones = 4; tones <= 64; tones *= 2) {
        olivia_cfg_t c;
        uint8_t in[OLIVIA_MAX_BITS], out[OLIVIA_MAX_BITS], sym[OLIVIA_MAX_SYMBOLS];

        olivia_cfg_init(&c, OLIVIA_PROTO_CONTESTIA, tones, 500);
        for (int code = 0; code < 64; code++) {
            for (int i = 0; i < c.bits; i++)
                in[i] = (uint8_t)((code + i * 7) & 63);
            olivia_encode_block(&c, in, sym);
            olivia_decode_block_hard(&c, sym, out, NULL);
            CHECK(memcmp(in, out, (size_t)c.bits) == 0, "block %d tones, code %d", tones, code);
        }
    }

    {
        const char *msg = "CQ CQ DE N0CALL N0CALL K";

        for (size_t i = 0; i < sizeof(contestia_modes) / sizeof(contestia_modes[0]); i++) {
            int t = contestia_modes[i].tones, b = contestia_modes[i].bw;

            CHECK(loopback(OLIVIA_PROTO_CONTESTIA, t, b, msg, 0.0, 200.0, NULL) == 1,
                  "Contestia %d/%d clean loopback", t, b);
            CHECK(loopback(OLIVIA_PROTO_CONTESTIA, t, b, msg, 20.0, 0.0, NULL) == 1,
                  "Contestia %d/%d loopback, 0 dB, 20 Hz off", t, b);
        }
    }

    /* Noise alone must not lock Contestia: the modes with the fewest tones are
     * the worst, and every receiver start is a chance. */
    for (size_t i = 0; i < sizeof(contestia_modes) / sizeof(contestia_modes[0]); i++) {
        int t = contestia_modes[i].tones, b = contestia_modes[i].bw, locks = 0;

        locks = noise_locks(OLIVIA_PROTO_CONTESTIA, t, b, 40);
        CHECK(locks <= 1, "Contestia %d/%d locked on noise in %d of 40 starts", t, b, locks);
    }

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
