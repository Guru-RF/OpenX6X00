/*
 * WeFax transmitter, see encoder.h
 */

#include "encoder.h"

#include "../sstv/sstv_png.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APT_START_S     5.0
#define APT_START_HZ    300.0
#define PHASING_LINES   60
#define PHASING_PULSE   0.05
#define APT_STOP_S      5.0
#define APT_STOP_HZ     450.0
#define BLACK_S         10.0
#define LINE_S          (60.0 / WEFAX_LPM)
#define WEFAX_TX_PI     3.14159265358979323846

struct wefax_encoder {
    uint8_t  *img;      /* WEFAX_WIDTH x rows, grey */
    unsigned  rows;
    unsigned  rate;
    uint64_t  pos;
    uint64_t  total;
    double    phase;
};

/* Width and height from the PNG header (IHDR is always the first chunk) */
static int png_size(const char *path, uint32_t *w, uint32_t *h)
{
    uint8_t b[24];
    FILE   *fp = fopen(path, "rb");

    if (!fp)
        return -1;
    size_t got = fread(b, 1, sizeof(b), fp);
    fclose(fp);
    if (got != sizeof(b) || memcmp(b + 1, "PNG", 3) || memcmp(b + 12, "IHDR", 4))
        return -2;
    *w = (uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19];
    *h = (uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23];
    return *w && *h && *w <= 10000 && *h <= 10000 ? 0 : -4;
}

wefax_encoder_t *wefax_encoder_open(const char *png_path, unsigned sample_rate, int *rc)
{
    uint32_t w, h;

    *rc = png_size(png_path, &w, &h);
    if (*rc < 0)
        return NULL;

    /* 1809 wide, or narrower when 800 lines are not enough (square pixels) */
    unsigned out_w = WEFAX_WIDTH;
    unsigned out_h = (unsigned)(((uint64_t)h * WEFAX_WIDTH + w / 2) / w);

    if (out_h > WEFAX_TX_MAX_ROWS) {
        out_h = WEFAX_TX_MAX_ROWS;
        out_w = (unsigned)(((uint64_t)w * WEFAX_TX_MAX_ROWS + h / 2) / h);
    }
    if (out_h < 1)
        out_h = 1;
    if (out_w < 1)
        out_w = 1;

    wefax_encoder_t *enc = calloc(1, sizeof(*enc));
    uint8_t         *rgb = malloc((size_t)out_w * out_h * 3);

    if (enc)
        enc->img = malloc((size_t)WEFAX_WIDTH * out_h);
    if (!enc || !rgb || !enc->img) {
        *rc = -5;
        goto fail;
    }
    *rc = sstv_load_png_scaled(png_path, rgb, (int)out_w, (int)out_h);
    if (*rc < 0)
        goto fail;

    memset(enc->img, 255, (size_t)WEFAX_WIDTH * out_h);
    for (unsigned y = 0; y < out_h; y++) {
        uint8_t       *d = enc->img + (size_t)y * WEFAX_WIDTH + (WEFAX_WIDTH - out_w) / 2;
        const uint8_t *s = rgb + (size_t)y * out_w * 3;

        for (unsigned x = 0; x < out_w; x++, s += 3)
            d[x] = (uint8_t)((s[0] * 299 + s[1] * 587 + s[2] * 114 + 500) / 1000);
    }
    free(rgb);

    enc->rows  = out_h;
    enc->rate  = sample_rate;
    enc->total = (uint64_t)((APT_START_S + (PHASING_LINES + out_h) * LINE_S + APT_STOP_S + BLACK_S) * sample_rate);
    return enc;

fail:
    free(rgb);
    wefax_encoder_close(enc);
    return NULL;
}

void wefax_encoder_close(wefax_encoder_t *enc)
{
    if (enc) {
        free(enc->img);
        free(enc);
    }
}

/* Grey level at time t (s) of the transmission */
static uint8_t level(const wefax_encoder_t *enc, double t)
{
    if (t < APT_START_S)
        return (uint64_t)(t * APT_START_HZ * 2) & 1 ? 0 : 255;
    t -= APT_START_S;

    double   line = t / LINE_S;
    unsigned n    = (unsigned)line;
    double   x    = line - n;

    if (n < PHASING_LINES)   /* the white pulse ends the line: the picture starts right after it */
        return x >= 1.0 - PHASING_PULSE ? 255 : 0;
    n -= PHASING_LINES;
    if (n < enc->rows) {
        unsigned px = (unsigned)(x * WEFAX_WIDTH);

        return enc->img[(size_t)n * WEFAX_WIDTH + (px < WEFAX_WIDTH ? px : WEFAX_WIDTH - 1)];
    }
    t -= (PHASING_LINES + enc->rows) * LINE_S;
    if (t < APT_STOP_S)
        return (uint64_t)(t * APT_STOP_HZ * 2) & 1 ? 0 : 255;
    return 0;
}

size_t wefax_encoder_fill(int16_t *buf, size_t n, void *ctx)
{
    wefax_encoder_t *enc = ctx;
    size_t           i;

    for (i = 0; i < n && enc->pos < enc->total; i++, enc->pos++) {
        double hz = WEFAX_CENTER_HZ + WEFAX_SHIFT_HZ * (level(enc, (double)enc->pos / enc->rate) / 255.0 - 0.5);

        buf[i]      = (int16_t)lrint(sin(enc->phase) * 24575.0);
        enc->phase += 2.0 * WEFAX_TX_PI * hz / enc->rate;
        if (enc->phase >= 2.0 * WEFAX_TX_PI)
            enc->phase -= 2.0 * WEFAX_TX_PI;
    }
    return i;
}

unsigned wefax_encoder_rows(const wefax_encoder_t *enc)
{
    return enc->rows;
}

int wefax_encoder_progress(const wefax_encoder_t *enc)
{
    return (int)(enc->pos * 1000 / enc->total);
}
