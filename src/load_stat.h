/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the processor's and the memory's load, from /proc (load_stat.c, #233)
 *
 *  For the Load meter (load_meter.h): band keys that went missing now and then, to be held against what the radio
 *  was doing at that moment. The processor's load is the time all its cores worked between two reads of /proc/stat;
 *  the GUI thread's is its own time from /proc/<pid>/task/<tid>/stat, as a part of one core, since one thread can
 *  never use more; the memory in use is what /proc/meminfo does not count as available. Here only the parsing, the
 *  sums and the rules the meter draws by; no LVGL, tools/load_meter tests them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The processor since its start, in clock ticks of every core summed: the first line of /proc/stat */
typedef struct {
    uint64_t busy;      /* user, nice, system, irq, softirq, steal */
    uint64_t total;     /* busy and idle, iowait among the idle: waiting for the card is no work */
    int      cores;     /* the cpuN lines: the cores online */
} load_cpu_t;

/* /proc/stat's text (its start is enough: the cpu lines come first). False without a cpu line */
bool load_cpu_parse(const char *text, load_cpu_t *out);

/* The load between two reads, 0 to 100 % of all cores; -1 when no tick passed between them */
int load_cpu_pct(const load_cpu_t *was, const load_cpu_t *now);

/* /proc/meminfo, in kB */
typedef struct {
    uint64_t total;         /* MemTotal */
    uint64_t avail;         /* MemAvailable; a kernel without it: MemFree + Buffers + Cached */
    uint64_t swap_total;    /* 0: no swap (the radios' images have none) */
    uint64_t swap_free;
} load_mem_t;

/* False without MemTotal */
bool load_mem_parse(const char *text, load_mem_t *out);

/* The memory in use, 0 to 100 %; the swap in use, -1 with no swap */
int load_mem_pct(const load_mem_t *m);
int load_swap_pct(const load_mem_t *m);

/* A thread's /proc/<pid>/task/<tid>/stat: its user and system time, clock ticks. Its name, in brackets, may hold
 * spaces and brackets: the fields are counted from the last ')' */
bool load_task_parse(const char *text, uint64_t *ticks);

/* The thread's share between two reads, 0 to 100 % of one core; -1 when no tick passed */
int load_task_pct(uint64_t was, uint64_t now, const load_cpu_t *cpu_was, const load_cpu_t *cpu_now);

/* How a load is drawn: in the theme's colour for it, raised from LOAD_MID %, high from LOAD_HIGH % */
#define LOAD_MID  70
#define LOAD_HIGH 90

typedef enum { LOAD_LEVEL_OK, LOAD_LEVEL_MID, LOAD_LEVEL_HIGH } load_level_t;

load_level_t load_level(int pct);

/* The GUI loop's longest stretch of work between two waits for input, ms: a key pressed meanwhile is read after it.
 * Late from LOAD_LATE_MS (a few frames: the screen lags), very late from LOAD_LATE_HIGH_MS (half a band key's first
 * repeat and a long press: front_keys.c then has to tell a tap from a hold by the events still queued) */
#define LOAD_LATE_MS      100
#define LOAD_LATE_HIGH_MS 500

load_level_t load_late_level(uint32_t ms);

/* The highest of the last LOAD_HOLD seconds: the meter's peak marks, and how long a late loop stays marked, so a
 * peak is still there when the operator looks after a key did nothing */
#define LOAD_HOLD 10

typedef struct {
    int16_t v[LOAD_HOLD];
    int     n;          /* values in it, up to LOAD_HOLD */
    int     next;       /* where the next one goes */
} load_hold_t;

/* One second's value in; the highest of the last LOAD_HOLD seconds, this one among them */
int load_hold_push(load_hold_t *h, int v);

/* A value 0 to 100 as a bar's height in pixels, 0 to h: rounded, so 1 % is no pixel on a short bar, and a value
 * outside its range is held to it */
int load_px(int pct, int h);

/* A file of /proc into buf, at most size - 1 bytes, ended with a zero; false when it cannot be read */
bool load_read(const char *path, char *buf, size_t size);

#ifdef __cplusplus
}
#endif
