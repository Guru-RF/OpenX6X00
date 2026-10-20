/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 */

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi_writer.h"

#define HDR_MAX         512
#define FILE_MAX        0xF0000000u     /* RIFF sizes are 32 bit */
#define AVIIF_KEYFRAME  0x10

typedef struct {
    uint32_t    id;
    uint32_t    size;
    uint32_t    offset;
} idx_t;

struct avi_t {
    FILE        *f;
    bool        err;
    uint32_t    pos;                /* file size so far */
    uint32_t    movi;               /* offset of the "movi" fourcc */
    uint32_t    frames, samples, max_chunk;
    uint32_t    p_riff, p_frames, p_vlen, p_alen, p_movi, p_buf;    /* fields to patch */
    uint32_t    p_arate, p_fmt_rate;    /* audio rate in strh and in WAVEFORMATEX */
    uint16_t    fps;
    idx_t       *idx;
    size_t      idx_n, idx_cap;
};

static uint8_t  *hp, *hb;              /* header cursor and start */

static void put4(const char *s)     { memcpy(hp, s, 4); hp += 4; }
static void put32(uint32_t x)       { for (int i = 0; i < 4; i++) *hp++ = x >> (8 * i); }
static void put16(uint16_t x)       { *hp++ = x; *hp++ = x >> 8; }

static void put_list(const char *type, uint32_t size) {
    put4("LIST"); put32(size); put4(type);
}

static void put_strh(const char *type, const char *handler, uint32_t rate, uint32_t sample_size, uint16_t w, uint16_t h, uint32_t *p_len) {
    put4("strh"); put32(56);
    put4(type); put4(handler);
    put32(0); put32(0); put32(0);           /* flags, priority + language, initial frames */
    put32(1); put32(rate);                  /* scale, rate */
    put32(0);                               /* start */
    *p_len = hp - hb;
    put32(0);                               /* length */
    put32(0);                               /* suggested buffer */
    put32(0xFFFFFFFF);                      /* quality */
    put32(sample_size);
    put16(0); put16(0); put16(w); put16(h); /* rcFrame */
}

static uint32_t build_header(avi_t *a, uint8_t *hdr, uint16_t w, uint16_t h, uint16_t fps, uint32_t rate) {
    hp = hb = hdr;
    put4("RIFF"); a->p_riff = hp - hdr; put32(0); put4("AVI ");
    put_list("hdrl", 4 + 64 + (12 + 64 + 48) + (12 + 64 + 26));

    put4("avih"); put32(56);
    put32(1000000 / fps);                   /* us per frame */
    put32(0); put32(0);                     /* max bytes/s, padding */
    put32(0x10 | 0x100);                    /* has index, interleaved */
    a->p_frames = hp - hdr; put32(0);       /* total frames */
    put32(0); put32(2);                     /* initial frames, streams */
    a->p_buf = hp - hdr; put32(0);          /* suggested buffer */
    put32(w); put32(h);
    for (int i = 0; i < 4; i++) put32(0);

    put_list("strl", 4 + 64 + 48);
    put_strh("vids", "MJPG", fps, 0, w, h, &a->p_vlen);
    put4("strf"); put32(40);                /* BITMAPINFOHEADER */
    put32(40); put32(w); put32(h); put16(1); put16(24);
    put4("MJPG"); put32(w * h * 3);
    put32(0); put32(0); put32(0); put32(0);

    put_list("strl", 4 + 64 + 26);
    put_strh("auds", "\0\0\0\0", rate, 2, 0, 0, &a->p_alen);
    a->p_arate = a->p_alen - 8;
    put4("strf"); put32(18);                /* WAVEFORMATEX */
    put16(1); put16(1);
    a->p_fmt_rate = hp - hdr;
    put32(rate); put32(rate * 2); put16(2); put16(16); put16(0);

    a->p_movi = hp - hdr + 4;
    put_list("movi", 4);
    a->movi = hp - hdr - 4;
    return hp - hdr;
}

static void wr(avi_t *a, const void *p, size_t n) {
    if (a->err || a->pos + n > FILE_MAX || fwrite(p, 1, n, a->f) != n) {
        a->err = true;
        return;
    }
    a->pos += n;
}

static void patch32(avi_t *a, uint32_t at, uint32_t x) {
    uint8_t b[4] = { x, x >> 8, x >> 16, x >> 24 };

    if (fseeko(a->f, at, SEEK_SET) != 0 || fwrite(b, 1, 4, a->f) != 4) {
        a->err = true;
    }
}

avi_t * avi_open(const char *path, uint16_t width, uint16_t height, uint16_t fps, uint32_t rate) {
    avi_t   *a = calloc(1, sizeof(*a));
    uint8_t hdr[HDR_MAX];

    if (!a) {
        return NULL;
    }

    a->f = fopen(path, "wb");

    if (!a->f) {
        free(a);
        return NULL;
    }

    setvbuf(a->f, NULL, _IOFBF, 64 * 1024);
    a->fps = fps;
    wr(a, hdr, build_header(a, hdr, width, height, fps, rate));

    if (a->err) {
        fclose(a->f);
        free(a);
        return NULL;
    }
    return a;
}

static void chunk(avi_t *a, const char *id, const void *data, size_t size) {
    uint8_t     h[8] = { id[0], id[1], id[2], id[3], size, size >> 8, size >> 16, size >> 24 };
    uint32_t    pad = size & 1;

    if (a->err) {
        return;
    }

    if (a->idx_n == a->idx_cap) {
        size_t  cap = a->idx_cap ? a->idx_cap * 2 : 4096;
        idx_t   *p = realloc(a->idx, cap * sizeof(idx_t));

        if (!p) {
            a->err = true;
            return;
        }
        a->idx = p;
        a->idx_cap = cap;
    }

    idx_t *e = &a->idx[a->idx_n++];

    memcpy(&e->id, id, 4);
    e->size = size;
    e->offset = a->pos - a->movi;

    wr(a, h, 8);
    wr(a, data, size);
    if (pad) {
        wr(a, "", 1);
    }
    if (size > a->max_chunk) {
        a->max_chunk = size;
    }
}

bool avi_write_video(avi_t *a, const uint8_t *jpeg, size_t size) {
    chunk(a, "00dc", jpeg, size);
    a->frames++;
    return !a->err;
}

bool avi_write_audio(avi_t *a, const int16_t *samples, size_t nsamples) {
    chunk(a, "01wb", samples, nsamples * 2);    /* little endian target and host */
    a->samples += nsamples;
    return !a->err;
}

uint32_t avi_size(avi_t *a) {
    return a->pos;
}

bool avi_close(avi_t *a) {
    uint32_t    movi_end = a->pos;
    uint8_t     h[8] = { 'i', 'd', 'x', '1' };
    uint32_t    n = a->idx_n * 16;

    for (int i = 0; i < 4; i++) h[4 + i] = n >> (8 * i);
    wr(a, h, 8);

    for (size_t i = 0; i < a->idx_n; i++) {
        uint32_t    e[4] = { 0, AVIIF_KEYFRAME, a->idx[i].offset, a->idx[i].size };

        memcpy(&e[0], &a->idx[i].id, 4);
        wr(a, e, 16);                       /* little endian target and host */
    }

    if (!a->err) {
        patch32(a, a->p_riff, a->pos - 8);
        patch32(a, a->p_movi, movi_end - a->movi);
        patch32(a, a->p_frames, a->frames);
        patch32(a, a->p_buf, a->max_chunk);
        patch32(a, a->p_vlen, a->frames);
        patch32(a, a->p_alen, a->samples);

        /* The codec clock is not exactly the nominal rate (+0.6% measured): the rate
         * that fits the video length keeps sound and picture together to the end */
        if (a->frames >= a->fps * 10) {
            uint32_t rate = ((uint64_t) a->samples * a->fps + a->frames / 2) / a->frames;

            patch32(a, a->p_arate, rate);
            patch32(a, a->p_fmt_rate, rate);
            patch32(a, a->p_fmt_rate + 4, rate * 2);
        }
    }

    bool ok = !a->err;

    if (fclose(a->f) != 0) {
        ok = false;
    }
    free(a->idx);
    free(a);
    return ok;
}
