/*
 * Line-sync follower for SSTV audio as it arrives (SSTV_RATE).
 *
 * It finds the 1200-Hz line syncs, recognises the mode from their spacing
 * (no VIS needed), follows the line grid with its clock error and tells when
 * the picture is over.  Feed it blocks of any size; no allocation, no locks:
 * one follower per thread.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sstv_mode.h"

typedef enum {
    SSTV_SYNC_SEARCH = 0,     /* no line grid yet                               */
    SSTV_SYNC_LOCKED,         /* following the lines of .mode                    */
    SSTV_SYNC_LOST,           /* syncs and picture gone after a lock: the end    */
} sstv_sync_state_t;

#define SSTV_SYNC_WIN    22   /* 2-ms detector window                           */
#define SSTV_SYNC_PULSES 64   /* pulses remembered                              */
#define SSTV_SYNC_FIT    32   /* syncs in the line period fit                   */
#define SSTV_SYNC_MODES  8

typedef struct {
    sstv_sync_state_t state;
    sstv_mode_t mode;         /* SSTV_UNKNOWN while searching                    */
    uint64_t samples;         /* audio seen                                      */
    uint64_t first_sync;      /* oldest sync of the chain that gave the lock     */
    uint64_t last_sync;       /* start of the last sync on the grid              */
    uint32_t hits;            /* syncs on the grid since the lock                */
    double   period;          /* line period, samples                            */
    double   clock_ppm;       /* line period against the nominal one             */
} sstv_sync_status_t;

typedef struct {
    sstv_sync_status_t st;
    sstv_mode_t only;                     /* the mode chosen by hand, or SSTV_UNKNOWN */
    /* 1200-Hz detector: running sums over the last SSTV_SYNC_WIN samples */
    float    wre[SSTV_SYNC_WIN], wim[SSTV_SYNC_WIN], wpow[SSTV_SYNC_WIN];
    double   sre, sim, spow;
    int      wpos, osc;
    bool     inside;
    uint64_t rise;
    /* pulses: start, length, chain length and 1-line links per mode */
    uint64_t at[SSTV_SYNC_PULSES];
    float    ms[SSTV_SYNC_PULSES];
    uint8_t  chain[SSTV_SYNC_PULSES][SSTV_SYNC_MODES];
    uint8_t  single[SSTV_SYNC_PULSES][SSTV_SYNC_MODES];
    int      count, head;
    long     links[SSTV_SYNC_MODES];      /* 1-line intervals seen, whole run */
    /* locked */
    int      cur;                         /* table row of st.mode            */
    double   fit_line[SSTV_SYNC_FIT], fit_at[SSTV_SYNC_FIT];
    int      fit_n, fit_head;
    double   line;                        /* line number of st.last_sync     */
    double   avg_ms;                      /* mean length of the grid syncs   */
    uint64_t lost_after;                  /* no sync for this long: maybe over */
    uint64_t picture_at;                  /* end of the last 1-s picture block */
    /* picture monitor: band filters, energies over 1 s */
    float    flt[6][2];
    double   e_band, e_all;
    uint32_t e_count;
} sstv_sync_t;

/* only: mode chosen by hand, SSTV_UNKNOWN to recognise it.  Starts from 0. */
void sstv_sync_init(sstv_sync_t *s, sstv_mode_t only);
void sstv_sync_feed(sstv_sync_t *s, const float *x, size_t n);

/* Nominal line and picture length, seconds (0: unknown mode) */
double sstv_sync_line_s(sstv_mode_t mode);
double sstv_sync_picture_s(sstv_mode_t mode);

/* The mode of a whole recording from its line syncs, more reliable than the
 * first seconds.  prefer: the mode locked on while receiving (or
 * SSTV_UNKNOWN), kept with at least half the evidence of the best one.
 * SSTV_UNKNOWN when no mode has a believable chain; *links (optional): the
 * line intervals found for the result. */
sstv_mode_t sstv_sync_classify(const float *x, long n, sstv_mode_t prefer, long *links);
