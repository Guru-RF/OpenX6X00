#ifndef SSTV_DENOISE_H
#define SSTV_DENOISE_H

#include <stdint.h>

/*
 * Clean-up of a received SSTV picture (RGB, 3 bytes per pixel), in place.
 * 1. Switching 3x3 median, per channel: a value that stands out from both
 *    the pixel above and the one below (same direction, > 40 levels) is
 *    replaced by the median of its 3x3 neighbourhood.  Removes speckles and
 *    the one-row streaks QRM and fading leave along a line; edges, corners
 *    and lines 2 px or thicker of a clean picture are left alone.
 * 2. Light sigma filter, per channel: average of the 3x3 neighbours within a
 *    few levels of the pixel.  Smooths grain, does not blur across edges.
 * tmp: caller's scratch of width*height*3 bytes (no allocation here).
 */
void sstv_denoise(uint8_t *rgb, uint8_t *tmp, int width, int height);

#endif
