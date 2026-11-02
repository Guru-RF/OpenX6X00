/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test for src/freedv_tx.c (not part of the image): a 44100 Hz 16-bit mono speech WAV goes in
 *  as the hand mic would deliver it, the over comes out as tx_player would play it (44100 Hz), to a
 *  WAV. tools/freedv_tx_test.sh runs it and then decodes the result with freedv_test.
 *
 *    freedv_tx_test in-44100.wav out-44100.wav [CALLSIGN]
 */

#include "freedv_tx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void wav_header(FILE *f, uint32_t rate, uint32_t bytes)
{
    uint32_t v;
    uint16_t s;

    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); fwrite(&s, 2, 1, f);
    fwrite(&rate, 4, 1, f); v = rate * 2; fwrite(&v, 4, 1, f);
    s = 2; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: freedv_tx_test in-44100.wav out.wav [CALLSIGN]\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");

    if (!in || !out) { perror("open"); return 1; }
    fseek(in, 44, SEEK_SET);
    if (!freedv_tx_available() || !freedv_tx_begin(argc > 3 ? argv[3] : "TEST")) {
        fprintf(stderr, "freedv_tx_begin failed (RADE built in?)\n");
        return 1;
    }
    int16_t buf[1024];
    size_t  n, total = 0;
    freedv_tx_stats_t s;

    while ((n = fread(buf, 2, 1024, in)) > 0) {
        freedv_tx_put_mic(n, buf);
        total += n;
        usleep(200);
        freedv_tx_get_stats(&s);
        if (s.full)
            break;
    }
    freedv_tx_stop();
    for (int i = 0; i < 6000; i++) {        /* 60 s at most */
        freedv_tx_get_stats(&s);
        if (s.done)
            break;
        usleep(10000);
    }
    if (!s.done) { fprintf(stderr, "the encoder did not finish\n"); return 1; }

    wav_header(out, FREEDV_TX_MIC_RATE, 0);
    int16_t o[2048];
    size_t  got, out_n = 0;

    while ((got = freedv_tx_read(o, 2048, NULL)) > 0) {
        fwrite(o, 2, got, out);
        out_n += got;
    }
    fseek(out, 0, SEEK_SET);
    wav_header(out, FREEDV_TX_MIC_RATE, (uint32_t)(out_n * 2));
    fclose(out);
    printf("mic %.1f s  recorded %d ms  encoded %d ms  over %d ms (8 kHz store)  out %.2f s  encode_ms_max %d  full %d\n",
           (double)total / FREEDV_TX_MIC_RATE, s.rec_ms, s.encoded_ms, s.over_ms,
           (double)out_n / FREEDV_TX_MIC_RATE, s.encode_ms_max, s.full);
    freedv_tx_discard();
    return 0;
}
