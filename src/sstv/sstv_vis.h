/*
 * VIS header finder.
 *
 * x: n samples at SSTV_RATE (n <= 2 s, normally 1 s looked at every 50 ms);
 * the whole header (0.91 s) must be inside.  Headers that start in the first
 * ~55 ms of the block are found, so blocks may be at most 50 ms apart.
 * No global state besides constant tables: safe from any thread.
 *
 * Returns true for a header of a mode we decode (res->mode).  A clean header
 * of a mode we do not decode gives false with res->unsupported = its code.
 */
#pragma once

#include <stdbool.h>
#include "sstv_mode.h"

typedef struct {
    sstv_mode_t mode;         /* SSTV_UNKNOWN unless found                     */
    int   unsupported;        /* code of a known mode we do not decode, or -1  */
    float leader;             /* share of the energy in the leader tone        */
    float offset_hz;          /* tuning error: leader frequency - 1900 Hz      */
    float score;              /* mean symbol score 0..1, -1: no frame tried    */
    float start_s;            /* header start in the block                     */
    float bit_ms;             /* bit length that fitted best                   */
    float gap_s;              /* audio gap found inside the data bits          */
} sstv_vis_t;

bool sstv_vis_find(const float *x, int n, sstv_vis_t *res);

/* Header start to the end of its stop bit, seconds */
#define SSTV_VIS_S 0.910
