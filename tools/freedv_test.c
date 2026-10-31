/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test for src/freedv_rx.c (not part of the image): feeds a 16-bit mono WAV, recorded
 *  or resampled to 11025 Hz, through freedv_rx_put_audio() in the pieces the GUI's audio
 *  thread would deliver, and writes the decoded speech (44100 Hz, what audio_play() gets) to
 *  a WAV. Compare SNR, sync and length with rade_c's rade_rx_wav on the same file.
 *
 *    tools/freedv_test.sh in.wav out.wav [speed]     (builds, resamples, runs)
 *
 *  speed 1 = real time (the radio's pace), 10 = ten times that (the decoder does about 30x on a PC);
 *  0 = unthrottled, which drops input the decoder can't take: only for a quick look.
 */

#include "freedv_rx.h"
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The audio.c this test does not link: the "headphones" are a file */
static FILE *out_fp;
static long  out_samples;

bool audio_play_bt_begin(void) { return true; }
void audio_play_bt_end(void) {}
void audio_play_flush(void) {}
int  audio_play(int16_t *buf, size_t n)
{
    if (out_fp && fwrite(buf, 2, n, out_fp) == n)
        out_samples += (long)n;
    return 0;
}

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
        fprintf(stderr, "usage: freedv_test in-11025.wav out.wav [speed]\n");
        return 2;
    }
    double speed = argc > 3 ? atof(argv[3]) : 0;
    FILE  *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 44, SEEK_SET);        /* ffmpeg's plain 44-byte header */

    out_fp = fopen(argv[2], "wb");
    if (!out_fp) { perror(argv[2]); return 1; }
    wav_header(out_fp, AUDIO_PLAY_RATE, 0);

    if (!freedv_rx_available() || !freedv_rx_start()) {
        fprintf(stderr, "freedv_rx_start failed (RADE built in?)\n");
        return 1;
    }
    enum { CHUNK = 1024 };      /* what dsp.cpp delivers: 4096 samples at 44.1 kHz, /4 */
    int16_t raw[CHUNK];
    float   f[CHUNK];
    size_t  n, total = 0;
    int     sync_polls = 0, polls = 0;
    float   snr_sum = 0;
    struct timespec t0, t;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while ((n = fread(raw, 2, CHUNK, in)) > 0) {
        for (size_t i = 0; i < n; i++)
            f[i] = raw[i] / 32768.0f;
        freedv_rx_put_audio((unsigned)n, f);
        total += n;
        if (speed > 0) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            double want = (double)total / FREEDV_RX_IN_RATE / speed;
            double have = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
            if (want > have)
                usleep((useconds_t)((want - have) * 1e6));
        } else {
            usleep(300);        /* unthrottled: faster than the decoder, the module drops input */
        }
        if (total % (FREEDV_RX_IN_RATE / 2) < CHUNK) {
            freedv_rx_stats_t s;
            freedv_rx_get_stats(&s);
            polls++;
            if (s.sync) { sync_polls++; snr_sum += s.snr_db; }
        }
    }
    sleep(2);                   /* the rings run empty */
    freedv_rx_stats_t s;
    freedv_rx_get_stats(&s);
    freedv_rx_stop();

    fseek(out_fp, 0, SEEK_SET);
    wav_header(out_fp, AUDIO_PLAY_RATE, (uint32_t)(out_samples * 2));
    fclose(out_fp);

    printf("in: %.1f s  out: %.1f s  sync %d/%d polls  mean SNR %.1f dB\n",
           (double)total / FREEDV_RX_IN_RATE, (double)out_samples / AUDIO_PLAY_RATE, sync_polls, polls,
           sync_polls ? snr_sum / sync_polls : 0.0);
    printf("eoo %u  in_dropped %u  speech_dropped %u  underruns %u  decode_ms_max %d  synth_ms_max %d\n",
           s.eoo_count, s.in_dropped, s.speech_dropped, s.underruns, s.decode_ms_max, s.synth_ms_max);
    return 0;
}
