/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax: the saved charts read back, and their previews (thumb.h). From OlivLoze's fork (F4OLI).
 */

#include "thumb.h"

#include "tilt.h"

#include <dirent.h>
#include <errno.h>
#include <png.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAGIC   "WFP1"      /* then the width and the height, 16 bits each, little-endian, and the grey pixels */

uint8_t *wefax_png_read_grey(const char *path, unsigned *w_out, unsigned *h_out)
{
    FILE *fp = fopen(path, "rb");

    if (!fp) {
        printf("WEFAX: can't open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    png_structp png  = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    uint8_t *volatile img = NULL, *volatile line = NULL;      /* freed after a longjmp */

    if (!png || !info) {
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        fclose(fp);
        return NULL;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        free(img);
        free(line);
        printf("WEFAX: %s is not a readable PNG\n", path);
        return NULL;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int         depth = png_get_bit_depth(png, info), type = png_get_color_type(png, info);

    if (!w || !h || w > 10000 || h > 10000) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        printf("WEFAX: %s: bad size\n", path);
        return NULL;
    }

    /* everything to 8-bit RGB(A), then grey */
    if (depth == 16)
        png_set_strip_16(png);
    if (type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    type = png_get_color_type(png, info);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    size_t rowbytes = png_get_rowbytes(png, info);
    int    ch       = png_get_channels(png, info);

    img  = malloc((size_t)w * h);
    line = malloc(rowbytes);
    if (ch < 3 || !rowbytes || !img || !line) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        free(img);
        free(line);
        printf("WEFAX: %s: unsupported or no memory\n", path);
        return NULL;
    }

    for (png_uint_32 y = 0; y < h; y++) {
        png_read_row(png, line, NULL);
        for (png_uint_32 x = 0; x < w; x++) {
            const uint8_t *p = line + (size_t)x * ch;

            img[(size_t)y * w + x] = (uint8_t)((77U * p[0] + 150U * p[1] + 29U * p[2]) >> 8);
        }
    }
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    free(line);
    *w_out = w;
    *h_out = h;
    return img;
}

bool wefax_png_write_fixed(const char *path, const uint8_t *img, unsigned w, unsigned h, int align, int tilt)
{
    char tmp[512];

    if (snprintf(tmp, sizeof(tmp), "%s.part", path) >= (int)sizeof(tmp) || !w || !h)
        return false;

    FILE *fp = fopen(tmp, "wb");

    if (!fp) {
        printf("WEFAX: can't write %s: %s\n", tmp, strerror(errno));
        return false;
    }

    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    uint8_t *volatile line = malloc(w);     /* freed after a longjmp */
    volatile bool ok = false;

    if (png && info && line && !setjmp(png_jmpbuf(png))) {
        png_init_io(png, fp);
        png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                     PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *src = img + (size_t)y * w;
            /* Tilt's shift of this row, from the newest; then where its pixel x comes from */
            int64_t        sh  = ((int64_t)y - (int64_t)(h - 1)) * tilt * WEFAX_TILT_UNIT_W / (1000LL * w);
            int            off = (int)((-(int64_t)align - sh) % (int64_t)w);

            if (off < 0)
                off += (int)w;
            for (unsigned x = 0; x < w; x++) {
                unsigned from = x + (unsigned)off;

                line[x] = src[from >= w ? from - w : from];
            }
            png_write_row(png, line);
        }
        png_write_end(png, NULL);
        ok = true;
    }
    png_destroy_write_struct(&png, info ? &info : NULL);
    free(line);
    if (fclose(fp) != 0)
        ok = false;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        printf("WEFAX: writing %s failed\n", path);
        return false;
    }
    return true;
}

/* dir/thumbs/<name without .png>.prv */
static void preview_path(char *out, size_t n, const char *dir, const char *name)
{
    size_t len = strlen(name);

    if (len > 4 && !strcmp(name + len - 4, ".png"))
        len -= 4;
    snprintf(out, n, "%s/thumbs/%.*s.prv", dir, (int)len, name);
}

bool wefax_preview_make(const char *dir, const char *name)
{
    char     path[300], out[320], tmp[352];
    unsigned w, h;
    uint8_t *img;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(img = wefax_png_read_grey(path, &w, &h)))
        return false;

    /* the whole chart, each pixel the mean of its box */
    unsigned pw = WEFAX_PREVIEW_W, ph = (unsigned)((uint64_t)h * pw / w);

    if (ph > WEFAX_PREVIEW_H) {
        ph = WEFAX_PREVIEW_H;
        pw = (unsigned)((uint64_t)w * ph / h);
    }
    if (!pw)
        pw = 1;
    if (!ph)
        ph = 1;

    uint8_t *prv = malloc((size_t)pw * ph);

    if (!prv) {
        free(img);
        return false;
    }
    for (unsigned y = 0; y < ph; y++) {
        unsigned y0 = (unsigned)((uint64_t)y * h / ph), y1 = (unsigned)((uint64_t)(y + 1) * h / ph);

        if (y1 <= y0)
            y1 = y0 + 1;
        for (unsigned x = 0; x < pw; x++) {
            unsigned x0 = (unsigned)((uint64_t)x * w / pw), x1 = (unsigned)((uint64_t)(x + 1) * w / pw);
            uint32_t sum = 0;

            if (x1 <= x0)
                x1 = x0 + 1;
            for (unsigned yy = y0; yy < y1; yy++)
                for (unsigned xx = x0; xx < x1; xx++)
                    sum += img[(size_t)yy * w + xx];
            prv[(size_t)y * pw + x] = (uint8_t)(sum / ((y1 - y0) * (x1 - x0)));
        }
    }
    free(img);

    snprintf(out, sizeof(out), "%s/thumbs", dir);
    if (mkdir(out, 0775) != 0 && errno != EEXIST) {
        free(prv);
        return false;
    }
    preview_path(out, sizeof(out), dir, name);
    snprintf(tmp, sizeof(tmp), "%s.%lx", out, (unsigned long)pthread_self());   /* each thread its own */

    FILE   *fp = fopen(tmp, "wb");
    uint8_t head[8] = {MAGIC[0], MAGIC[1], MAGIC[2], MAGIC[3], pw & 0xff, pw >> 8, ph & 0xff, ph >> 8};
    bool    ok = fp && fwrite(head, 1, sizeof(head), fp) == sizeof(head) && fwrite(prv, 1, (size_t)pw * ph, fp) == (size_t)pw * ph;

    if (fp && fclose(fp) != 0)
        ok = false;
    free(prv);
    if (!ok || rename(tmp, out) != 0) {
        remove(tmp);
        printf("WEFAX: can't write %s\n", out);
        return false;
    }
    return true;
}

void wefax_preview_remove(const char *dir, const char *name)
{
    char path[320];

    preview_path(path, sizeof(path), dir, name);
    remove(path);
}

int wefax_preview_sweep(const char *dir)
{
    char path[320];
    int  n = 0;

    snprintf(path, sizeof(path), "%s/thumbs", dir);

    DIR *d = opendir(path);

    if (!d)
        return 0;

    struct dirent *de;
    struct stat    st;

    while ((de = readdir(d))) {
        size_t len = strlen(de->d_name);
        char   chart[sizeof(path) + sizeof(de->d_name)], prv[sizeof(path) + sizeof(de->d_name) + 1];

        if (len < 5 || strcmp(de->d_name + len - 4, ".prv"))
            continue;       /* a .prv.<thread> being written is not one yet */
        snprintf(chart, sizeof(chart), "%s/%.*s.png", dir, (int)(len - 4), de->d_name);
        if (stat(chart, &st) == 0)
            continue;
        snprintf(prv, sizeof(prv), "%s/%s", path, de->d_name);
        if (remove(prv) == 0)
            n++;
    }
    closedir(d);
    return n;
}

uint8_t *wefax_preview_load(const char *dir, const char *name, unsigned *w, unsigned *h)
{
    char    path[320];
    uint8_t head[8];

    preview_path(path, sizeof(path), dir, name);

    FILE *fp = fopen(path, "rb");

    if (!fp)
        return NULL;

    uint8_t *img = NULL;

    if (fread(head, 1, sizeof(head), fp) == sizeof(head) && !memcmp(head, MAGIC, 4)) {
        unsigned pw = head[4] | head[5] << 8, ph = head[6] | head[7] << 8;

        if (pw && ph && pw <= WEFAX_PREVIEW_W && ph <= WEFAX_PREVIEW_H && (img = malloc((size_t)pw * ph))) {
            if (fread(img, 1, (size_t)pw * ph, fp) == (size_t)pw * ph) {
                *w = pw;
                *h = ph;
            } else {
                free(img);
                img = NULL;
            }
        }
    }
    fclose(fp);
    return img;
}
