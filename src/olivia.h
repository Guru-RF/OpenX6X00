/*
 * olivia.h - FEC layer of Olivia MFSK (Walsh-Hadamard + interleave + scrambler)
 *
 * Written from scratch from the public Olivia specification (ARRL,
 * "The Draft Specification For The Olivia HF Transmission System").
 * The SP9VRC reference code is GPL-2 and was NOT copied - it was used
 * only to verify the results.
 *
 * Contestia is the same system with a shorter block: 32 symbols, 6-bit
 * characters, its own scrambling sequence. It has no published
 * specification; its parameters were checked against fldigi, whose code
 * was NOT copied either.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef OLIVIA_H
#define OLIVIA_H

#include <stdint.h>
#include <stddef.h>

#define OLIVIA_MAX_SYMBOLS   64   /* longest block: Olivia's Walsh vector    */
#define OLIVIA_MAX_BITS      8    /* max log2(tones), i.e. up to 256 tones   */

typedef enum {
    OLIVIA_PROTO_OLIVIA = 0,     /* 64 symbols a block, 7-bit ASCII           */
    OLIVIA_PROTO_CONTESTIA,      /* 32 symbols a block, a 6-bit alphabet      */
} olivia_proto_t;

typedef struct {
    int tones;       /* 2,4,8,...,256                                */
    int bits;        /* log2(tones) - characters per block            */
    int bandwidth;   /* Hz: 125,250,500,1000,2000                     */

    olivia_proto_t proto;
    int block_symbols;   /* Walsh vector length = symbols per block   */
    int rotate;          /* scrambler rotation per character, bits    */
    uint64_t scrambler;  /* scrambling sequence, block_symbols long   */
} olivia_cfg_t;

/* Fill in a configuration. Returns 0 when the parameters are valid, -1
 * when not. */
int olivia_cfg_init(olivia_cfg_t *cfg, olivia_proto_t proto, int tones,
                    int bandwidth);

/* Symbol rate [Bd] = bandwidth / tones. */
double olivia_baud(const olivia_cfg_t *cfg);

/* Block duration [s]. */
double olivia_block_period(const olivia_cfg_t *cfg);

/* ---- FEC layer ---- */

/*
 * Encode a block of cfg->bits character codes into cfg->block_symbols
 * symbols (values 0..tones-1, still WITHOUT the Gray code - the modulation
 * layer does that). Codes are what olivia_char_encode() returns.
 * chars[]   : cfg->bits input codes
 * symbols[] : cfg->block_symbols output symbols
 */
void olivia_encode_block(const olivia_cfg_t *cfg,
                         const uint8_t *chars,
                         uint8_t *symbols);

/*
 * Decode a block from hard symbols.
 * symbols[] : cfg->block_symbols symbols (without Gray)
 * chars[]   : cfg->bits output codes, for olivia_char_decode()
 * snr[]     : optional (may be NULL) - quality per character, FHT peak /
 *             RMS of the rest. Below about 3.0 decoding is unreliable.
 */
void olivia_decode_block_hard(const olivia_cfg_t *cfg,
                              const uint8_t *symbols,
                              uint8_t *chars,
                              float *snr);

/*
 * Decode a block from soft decisions - the version used on the radio.
 * soft : array [block_symbols][bits], soft[t*bits + b] is the soft value of bit
 *        b of symbol t. POSITIVE = bit is 0, NEGATIVE = bit is 1, the
 *        magnitude is the confidence. Zeros are allowed (no information).
 */
void olivia_decode_block_soft(const olivia_cfg_t *cfg,
                              const float *soft,
                              uint8_t *chars,
                              float *snr);

/* ---- Alphabet ---- */

/*
 * Character -> code of the block layer. Olivia sends 7-bit ASCII as it is.
 * Contestia has 64 codes and no lower case: lower case is sent as upper,
 * what has no code as '?'. In both a code of 0 is the idle filler.
 */
uint8_t olivia_char_encode(const olivia_cfg_t *cfg, uint8_t c);

/* Code -> character. 0 stays 0, the filler. */
uint8_t olivia_char_decode(const olivia_cfg_t *cfg, uint8_t code);

/* ---- Modulation layer: tone numbering ---- */

/* Symbol -> tone number (Gray code). Tone 0 = lowest frequency. */
uint8_t olivia_symbol_to_tone(uint8_t symbol);

/* Tone number -> symbol. */
uint8_t olivia_tone_to_symbol(uint8_t tone);

/* ---- Helpers (exposed for tests) ---- */

/* Fast Hadamard transform in place, n a power of 2. Olivia's forward and
 * inverse pair - see olivia.c; neither is self-inverse. */
void olivia_fht_f(float *v, int n);
void olivia_ifht_f(float *v, int n);

#endif /* OLIVIA_H */
