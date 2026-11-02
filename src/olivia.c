/*
 * olivia.c - FEC layer of Olivia MFSK
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * How it works (from the ARRL specification):
 *
 *  - a block is 64 symbols; each symbol carries K = log2(N) bits, so a
 *    block carries K 7-bit ASCII characters,
 *  - every character becomes a 64-bit Walsh vector: a biorthogonal code,
 *    characters 0..63 are rows of the Hadamard matrix, characters 64..127
 *    the same rows negated,
 *  - the vector is scrambled with the sequence 0xE257E6D0291574EC,
 *    rotated right by 13 bits for each further character in the block,
 *  - the interleave is diagonal: bit (i + t) mod K of symbol t comes from
 *    the vector of character i. A corrupted symbol therefore damages
 *    exactly one bit in each of the K vectors.
 *
 * Contestia differs in the numbers only (see olivia_cfg_init): a block of
 * 32 symbols, so 32 Walsh vectors and their negations give 64 codes, and
 * the sequence 0xEDB88320 rotated by 5 bits. Hence "block_symbols" below
 * where Olivia's specification says 64.
 */

#include "olivia.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static int log2_exact(int v)
{
    int n = 0;
    if (v < 2)
        return -1;
    while ((v & 1) == 0) {
        v >>= 1;
        n++;
    }
    return (v == 1) ? n : -1;
}

/* Olivia: the sequence from the specification. */
#define OLIVIA_SCRAMBLER    0xE257E6D0291574ECULL
#define CONTESTIA_SCRAMBLER 0xEDB88320ULL

int olivia_cfg_init(olivia_cfg_t *cfg, olivia_proto_t proto, int tones,
                    int bandwidth)
{
    int bits = log2_exact(tones);

    if (bits < 1 || bits > OLIVIA_MAX_BITS)
        return -1;

    if (bandwidth != 125 && bandwidth != 250 && bandwidth != 500 &&
        bandwidth != 1000 && bandwidth != 2000)
        return -1;

    cfg->tones = tones;
    cfg->bits = bits;
    cfg->bandwidth = bandwidth;
    cfg->proto = proto;

    switch (proto) {
    case OLIVIA_PROTO_OLIVIA:
        cfg->block_symbols = 64;
        cfg->rotate = 13;
        cfg->scrambler = OLIVIA_SCRAMBLER;
        break;
    case OLIVIA_PROTO_CONTESTIA:
        cfg->block_symbols = 32;
        cfg->rotate = 5;
        cfg->scrambler = CONTESTIA_SCRAMBLER;
        break;
    default:
        return -1;
    }
    return 0;
}

double olivia_baud(const olivia_cfg_t *cfg)
{
    return (double)cfg->bandwidth / (double)cfg->tones;
}

double olivia_block_period(const olivia_cfg_t *cfg)
{
    return (double)cfg->block_symbols / olivia_baud(cfg);
}

/* ------------------------------------------------------------------ */
/*
 * Contestia's 64 codes: 1..58 are '!'..'Z', then space, CR and backspace.
 * 62 and 63 are '^' and '_', by the same offset as the first range; 0 is the
 * filler. LF has no code: the app sends CR for a new line.
 */

#define CONTESTIA_SPACE 59
#define CONTESTIA_CR    60
#define CONTESTIA_BS    61

uint8_t olivia_char_encode(const olivia_cfg_t *cfg, uint8_t c)
{
    if (cfg->proto != OLIVIA_PROTO_CONTESTIA)
        return c > 127 ? '?' : c;

    if (c >= 'a' && c <= 'z')
        c = (uint8_t)(c - 'a' + 'A');

    if (c == 0 || c == '\n')
        return 0;
    if (c == ' ')
        return CONTESTIA_SPACE;
    if (c == '\r')
        return CONTESTIA_CR;
    if (c == '\b')
        return CONTESTIA_BS;
    if (c >= '!' && c <= 'Z')
        return (uint8_t)(c - 32);
    return '?' - 32;
}

uint8_t olivia_char_decode(const olivia_cfg_t *cfg, uint8_t code)
{
    if (cfg->proto != OLIVIA_PROTO_CONTESTIA)
        return code;

    switch (code) {
    case 0:              return 0;
    case CONTESTIA_SPACE: return ' ';
    case CONTESTIA_CR:   return '\r';
    case CONTESTIA_BS:   return '\b';
    default:             return (uint8_t)(code + 32);
    }
}

/* ------------------------------------------------------------------ */
/* Gray code. Tone 0 is the lowest frequency, tones go upwards.         */

uint8_t olivia_symbol_to_tone(uint8_t symbol)
{
    return (uint8_t)(symbol ^ (symbol >> 1));
}

uint8_t olivia_tone_to_symbol(uint8_t tone)
{
    tone ^= (uint8_t)(tone >> 4);
    tone ^= (uint8_t)(tone >> 2);
    tone ^= (uint8_t)(tone >> 1);
    return tone;
}

/* ------------------------------------------------------------------ */
/* Fast Hadamard transform, natural order, length n.                    */

/*
 * NOTE: Olivia does not use the textbook Hadamard transform. The forward
 * butterfly is (a,b) -> (a+b, b-a), not (a+b, a-b), and the inverse runs
 * the stages in the opposite order giving (a,b) -> (a-b, a+b). The two
 * are inverses of each other up to a scale of n, but NEITHER is
 * self-inverse. Matched to the SP9VRC reference code, because on-air
 * compatibility of the code words depends on it - do not "fix" it into
 * the textbook version.
 */

void olivia_fht_f(float *v, int n)
{
    int step, i, j;

    for (step = 1; step < n; step <<= 1) {
        for (i = 0; i < n; i += (step << 1)) {
            for (j = i; j < i + step; j++) {
                float a = v[j];
                float b = v[j + step];
                v[j] = b + a;
                v[j + step] = b - a;
            }
        }
    }
}

void olivia_ifht_f(float *v, int n)
{
    int step, i, j;

    for (step = n / 2; step; step >>= 1) {
        for (i = 0; i < n; i += (step << 1)) {
            for (j = i; j < i + step; j++) {
                float a = v[j];
                float b = v[j + step];
                v[j] = a - b;
                v[j + step] = a + b;
            }
        }
    }
}

/*
 * Build the Walsh vector of a character and scramble it straight away.
 * out[] gets +1.0 / -1.0.
 */
static void walsh_vector(const olivia_cfg_t *cfg, uint8_t chr, int char_index,
                         float *out)
{
    int n = cfg->block_symbols;
    int shift = (char_index * cfg->rotate) & (n - 1);
    int t;

    chr &= (uint8_t)(2 * n - 1);

    for (t = 0; t < n; t++)
        out[t] = 0.0f;

    /* Biorthogonal code: below n a Hadamard row, from n its negation. */
    if (chr < n)
        out[chr] = 1.0f;
    else
        out[chr - n] = -1.0f;

    olivia_ifht_f(out, n);

    /* Scrambling: bit (shift + t) mod n of the sequence flips sample t. */
    for (t = 0; t < n; t++) {
        int bit = (shift + t) & (n - 1);
        if ((cfg->scrambler >> bit) & 1ULL)
            out[t] = -out[t];
    }
}

/* ------------------------------------------------------------------ */

void olivia_encode_block(const olivia_cfg_t *cfg,
                         const uint8_t *chars,
                         uint8_t *symbols)
{
    float vec[OLIVIA_MAX_SYMBOLS];
    int n = cfg->block_symbols;
    int i, t;

    for (t = 0; t < n; t++)
        symbols[t] = 0;

    for (i = 0; i < cfg->bits; i++) {
        walsh_vector(cfg, chars[i], i, vec);

        for (t = 0; t < n; t++) {
            /* diagonal interleave */
            int bit = (i + t) % cfg->bits;
            /* a negative value in the Walsh vector means the bit is set */
            if (vec[t] < 0.0f)
                symbols[t] |= (uint8_t)(1u << bit);
        }
    }
}

/* ------------------------------------------------------------------ */

/*
 * Common decoder core. soft[t*bits + b] > 0 means bit = 0.
 */
static void decode_core(const olivia_cfg_t *cfg,
                        const float *soft,
                        uint8_t *chars,
                        float *snr)
{
    int n = cfg->block_symbols;
    int i, t;

    for (i = 0; i < cfg->bits; i++) {
        float vec[OLIVIA_MAX_SYMBOLS];
        int shift = (i * cfg->rotate) & (n - 1);
        float peak = 0.0f;
        float sqr_sum = 0.0f;
        int peak_pos = 0;
        float rest;

        /* de-interleave + descramble */
        for (t = 0; t < n; t++) {
            int bit = (i + t) % cfg->bits;
            int cbit = (shift + t) & (n - 1);
            float s = soft[t * cfg->bits + bit];

            if ((cfg->scrambler >> cbit) & 1ULL)
                s = -s;
            vec[t] = s;
        }

        olivia_fht_f(vec, n);

        for (t = 0; t < n; t++) {
            float s = vec[t];
            sqr_sum += s * s;
            if (fabsf(s) > fabsf(peak)) {
                peak = s;
                peak_pos = t;
            }
        }

        /* The sign of the peak decides which half of the biorthogonal code. */
        chars[i] = (uint8_t)(peak > 0.0f ? peak_pos : peak_pos + n);

        if (snr) {
            rest = sqr_sum - peak * peak;
            /*
             * A zero remainder does NOT mean perfect confidence - it means
             * no information. That is what silence at the input looks like:
             * all soft decisions are zero, the FHT puts everything in bin
             * 0 and the remainder comes out zero. Returning a high value
             * then gave a false lock showing "100%" and no text.
             */
            if (rest <= 1e-12f || sqr_sum <= 1e-12f) {
                snr[i] = 0.0f;
            } else {
                rest = sqrtf(rest / (float)(n - 1));
                snr[i] = fabsf(peak) / rest;
            }
        }
    }
}

void olivia_decode_block_soft(const olivia_cfg_t *cfg,
                              const float *soft,
                              uint8_t *chars,
                              float *snr)
{
    decode_core(cfg, soft, chars, snr);
}

void olivia_decode_block_hard(const olivia_cfg_t *cfg,
                              const uint8_t *symbols,
                              uint8_t *chars,
                              float *snr)
{
    float soft[OLIVIA_MAX_SYMBOLS * OLIVIA_MAX_BITS];
    int n = cfg->block_symbols;
    int t, b;

    for (t = 0; t < n; t++) {
        for (b = 0; b < cfg->bits; b++) {
            int set = (symbols[t] >> b) & 1;
            soft[t * cfg->bits + b] = set ? -1.0f : 1.0f;
        }
    }

    decode_core(cfg, soft, chars, snr);
}
