/*
 * PNG in and out for the SSTV app (and the WeFax encoder): 8-bit RGB only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The picture as RGB, w*h*3 bytes, malloc'd (free() it); NULL if it cannot
 * be read or is larger than max_side.  over_white: transparent pixels are
 * mixed with white, else their colour is taken as it is. */
uint8_t *sstv_png_read(const char *path, unsigned max_side, bool over_white, unsigned *w, unsigned *h);

/* The picture stretched to out_w x out_h (nearest pixel) into out.
 * 0, or < 0 if it cannot be read. */
int sstv_load_png_scaled(const char *path, uint8_t *out, int out_w, int out_h);

bool sstv_png_write(const char *path, const uint8_t *rgb, unsigned w, unsigned h);
