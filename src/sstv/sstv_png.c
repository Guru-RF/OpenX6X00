#include "sstv_png.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *sstv_png_read(const char *path, unsigned max_side, bool over_white, unsigned *w, unsigned *h)
{
    FILE       *f = fopen(path, "rb");
    png_structp png = NULL;
    png_infop   info = NULL;
    uint8_t    *volatile rgb = NULL, *volatile raw = NULL;
    png_bytep  *volatile rows = NULL;

    *w = *h = 0;
    if (!f) return NULL;
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png ? png_create_info_struct(png) : NULL;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        free(rgb);
        free(raw);
        free(rows);
        return NULL;
    }
    png_init_io(png, f);
    png_read_info(png, info);

    png_uint_32 pw = png_get_image_width(png, info), ph = png_get_image_height(png, info);
    int         depth = png_get_bit_depth(png, info), type = png_get_color_type(png, info);
    if (!pw || !ph || pw > max_side || ph > max_side) longjmp(png_jmpbuf(png), 1);

    /* everything to 8-bit RGB, with alpha if there is any */
    if (depth == 16) png_set_strip_16(png);
    if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    int    ch = png_get_channels(png, info);
    size_t stride = png_get_rowbytes(png, info);
    if (ch < 3) longjmp(png_jmpbuf(png), 1);
    rgb = malloc((size_t)pw * ph * 3);
    raw = malloc(stride * ph);
    rows = malloc(ph * sizeof(png_bytep));
    if (!rgb || !raw || !rows) longjmp(png_jmpbuf(png), 1);
    for (png_uint_32 y = 0; y < ph; ++y) rows[y] = raw + y * stride;
    png_read_image(png, rows);          /* interlaced files too */
    for (png_uint_32 y = 0; y < ph; ++y) {
        for (png_uint_32 x = 0; x < pw; ++x) {
            const uint8_t *s = rows[y] + (size_t)x * ch;
            uint8_t       *d = rgb + ((size_t)y * pw + x) * 3;
            unsigned       a = ch == 4 && over_white ? s[3] : 255;
            for (int c = 0; c < 3; ++c)
                d[c] = a == 255 ? s[c] : (uint8_t)((s[c] * a + 255u * (255u - a)) / 255u);
        }
    }
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    free(raw);
    free(rows);
    *w = pw;
    *h = ph;
    return rgb;
}

int sstv_load_png_scaled(const char *path, uint8_t *out, int out_w, int out_h)
{
    unsigned w, h;
    uint8_t *src = sstv_png_read(path, 10000, false, &w, &h);

    if (!src) return -1;
    for (int y = 0; y < out_h; ++y) {
        unsigned sy = (unsigned)((uint64_t)y * h / (uint64_t)out_h);
        for (int x = 0; x < out_w; ++x) {
            unsigned       sx = (unsigned)((uint64_t)x * w / (uint64_t)out_w);
            const uint8_t *s = src + ((size_t)(sy < h ? sy : h - 1) * w + (sx < w ? sx : w - 1)) * 3;
            uint8_t       *d = out + ((size_t)y * out_w + x) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    free(src);
    return 0;
}

bool sstv_png_write(const char *path, const uint8_t *rgb, unsigned w, unsigned h)
{
    FILE       *f = path && rgb && w && h ? fopen(path, "wb") : NULL;
    png_structp png = NULL;
    png_infop   info = NULL;

    if (!f) return false;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png ? png_create_info_struct(png) : NULL;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (unsigned y = 0; y < h; ++y) png_write_row(png, (png_bytep)(rgb + (size_t)y * w * 3));
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    return fclose(f) == 0;
}
