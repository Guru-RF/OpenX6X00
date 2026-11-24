/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the processor's and the memory's load, from /proc: see load_stat.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "load_stat.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Up to max numbers after p, on its line only: strtoull would go on into the next line */
static int numbers(const char *p, const char *eol, uint64_t *out, int max) {
    int n = 0;

    while (n < max) {
        char    *end;
        uint64_t v = strtoull(p, &end, 10);

        if (end == p || (eol && end > eol)) break;
        out[n++] = v;
        p = end;
    }
    return n;
}

bool load_cpu_parse(const char *text, load_cpu_t *out) {
    const char *p     = text;
    bool        found = false;

    memset(out, 0, sizeof(*out));
    /* "cpu  user nice system idle iowait irq softirq steal guest guest_nice", then a "cpuN ..." line a core. The
     * guests' time is in user's already */
    while (p && strncmp(p, "cpu", 3) == 0) {
        const char *eol = strchr(p, '\n');

        if (p[3] == ' ') {
            uint64_t f[8] = {0};

            if (numbers(p + 3, eol, f, 8) < 4) return false;
            out->busy  = f[0] + f[1] + f[2] + f[5] + f[6] + f[7];
            out->total = out->busy + f[3] + f[4];
            found      = true;
        } else if (isdigit((unsigned char)p[3]) && eol) {
            out->cores++;   /* a whole line: one cut off at the end of what was read is not counted */
        }
        p = eol ? eol + 1 : NULL;
    }
    if (found && !out->cores) out->cores = 1;
    return found;
}

static int pct_of(uint64_t part, uint64_t all) {
    uint64_t pct = (part * 100 + all / 2) / all;

    return pct > 100 ? 100 : (int)pct;
}

int load_cpu_pct(const load_cpu_t *was, const load_cpu_t *now) {
    if (now->total <= was->total || now->busy < was->busy) return -1;
    return pct_of(now->busy - was->busy, now->total - was->total);
}

/* The number of the line "<key>: <n> kB"; false when there is none */
static bool mem_field(const char *text, const char *key, uint64_t *v) {
    size_t      len = strlen(key);
    const char *p   = text;

    while (p && *p) {
        if (strncmp(p, key, len) == 0 && p[len] == ':') {
            const char *eol = strchr(p, '\n');

            return numbers(p + len + 1, eol, v, 1) == 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return false;
}

bool load_mem_parse(const char *text, load_mem_t *out) {
    uint64_t free_kb = 0, buffers = 0, cached = 0;

    memset(out, 0, sizeof(*out));
    if (!mem_field(text, "MemTotal", &out->total) || !out->total) return false;
    if (!mem_field(text, "MemAvailable", &out->avail)) {
        mem_field(text, "MemFree", &free_kb);
        mem_field(text, "Buffers", &buffers);
        mem_field(text, "Cached", &cached);
        out->avail = free_kb + buffers + cached;
    }
    if (out->avail > out->total) out->avail = out->total;
    mem_field(text, "SwapTotal", &out->swap_total);
    mem_field(text, "SwapFree", &out->swap_free);
    if (out->swap_free > out->swap_total) out->swap_free = out->swap_total;
    return true;
}

int load_mem_pct(const load_mem_t *m) {
    return m->total ? pct_of(m->total - m->avail, m->total) : 0;
}

int load_swap_pct(const load_mem_t *m) {
    return m->swap_total ? pct_of(m->swap_total - m->swap_free, m->swap_total) : -1;
}

bool load_task_parse(const char *text, uint64_t *ticks) {
    const char *p = strrchr(text, ')');
    uint64_t    f[12];

    /* After the name the state (field 3, a letter), then numbers from field 4: utime and stime are 14 and 15. tpgid
     * may be -1, which counts as a number as well */
    if (!p || p[1] != ' ' || !p[2]) return false;
    for (p += 2; *p && *p != ' '; p++) {
    }
    if (numbers(p, strchr(p, '\n'), f, 12) < 12) return false;
    *ticks = f[10] + f[11];
    return true;
}

int load_task_pct(uint64_t was, uint64_t now, const load_cpu_t *cpu_was, const load_cpu_t *cpu_now) {
    if (now < was || cpu_now->total <= cpu_was->total) return -1;
    /* All the cores' ticks passed, a core's part of them: the thread's ticks of that */
    return pct_of((now - was) * (uint64_t)(cpu_now->cores > 0 ? cpu_now->cores : 1),
                  cpu_now->total - cpu_was->total);
}

load_level_t load_level(int pct) {
    return pct >= LOAD_HIGH ? LOAD_LEVEL_HIGH : pct >= LOAD_MID ? LOAD_LEVEL_MID : LOAD_LEVEL_OK;
}

load_level_t load_late_level(uint32_t ms) {
    return ms >= LOAD_LATE_HIGH_MS ? LOAD_LEVEL_HIGH : ms >= LOAD_LATE_MS ? LOAD_LEVEL_MID : LOAD_LEVEL_OK;
}

int load_hold_push(load_hold_t *h, int v) {
    int top = v;

    h->v[h->next] = (int16_t)v;
    h->next       = (h->next + 1) % LOAD_HOLD;
    if (h->n < LOAD_HOLD) h->n++;
    for (int i = 0; i < h->n; i++) {
        if (h->v[i] > top) top = h->v[i];
    }
    return top;
}

int load_px(int pct, int h) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (pct * h + 50) / 100;
}

/* One read: /proc makes the text at the read, and these few lines come in one */
bool load_read(const char *path, char *buf, size_t size) {
    int     fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) return false;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}
