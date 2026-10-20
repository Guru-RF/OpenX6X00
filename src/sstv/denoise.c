#include "denoise.h"

#include <stdlib.h>

#define DN_IMPULSE  40  /* levels off the 3x3 median that count as noise */
#define DN_SIGMA    12  /* neighbours within this many levels get averaged */

#define SORT2(a, b) do { if ((a) > (b)) { uint8_t t_ = (a); (a) = (b); (b) = t_; } } while (0)

/* Median of 9: the 19-exchange network (Paeth / Devillard opt_med9) */
static uint8_t med9(uint8_t *p)
{
    SORT2(p[1], p[2]); SORT2(p[4], p[5]); SORT2(p[7], p[8]);
    SORT2(p[0], p[1]); SORT2(p[3], p[4]); SORT2(p[6], p[7]);
    SORT2(p[1], p[2]); SORT2(p[4], p[5]); SORT2(p[7], p[8]);
    SORT2(p[0], p[3]); SORT2(p[5], p[8]); SORT2(p[4], p[7]);
    SORT2(p[3], p[6]); SORT2(p[1], p[4]); SORT2(p[2], p[5]);
    SORT2(p[4], p[7]); SORT2(p[4], p[2]); SORT2(p[6], p[4]);
    SORT2(p[4], p[2]);
    return p[4];
}

/* The 9 values of channel c around (x, y), edges repeated */
static void window(const uint8_t *img, int w, int h, int x, int y, int c, uint8_t *v)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        int yy = y + dy < 0 ? 0 : y + dy >= h ? h - 1 : y + dy;
        for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
            v[n++] = img[((size_t)yy * w + xx) * 3 + c];
        }
    }
}

void sstv_denoise(uint8_t *rgb, uint8_t *tmp, int w, int h)
{
    uint8_t v[9];

    if (!rgb || !tmp || w <= 0 || h <= 0)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                size_t o = ((size_t)y * w + x) * 3 + c;
                int p = rgb[o];
                int up = rgb[o - (y > 0 ? (size_t)w * 3 : 0)];
                int dn = rgb[o + (y < h - 1 ? (size_t)w * 3 : 0)];
                tmp[o] = p;
                /* Noise stands out from the pixels above and below it; a
                 * corner or an edge does not, so those are never touched */
                if ((p - up > DN_IMPULSE && p - dn > DN_IMPULSE) ||
                    (up - p > DN_IMPULSE && dn - p > DN_IMPULSE)) {
                    window(rgb, w, h, x, y, c, v);
                    tmp[o] = med9(v);
                }
            }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                size_t o = ((size_t)y * w + x) * 3 + c;
                int sum = 0, n = 0;
                window(tmp, w, h, x, y, c, v);
                for (int i = 0; i < 9; i++)
                    if (abs(v[i] - tmp[o]) <= DN_SIGMA) { sum += v[i]; n++; }
                rgb[o] = (uint8_t)((sum + n / 2) / n);   /* n >= 1: the centre */
            }
}
