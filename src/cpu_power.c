/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The processor's speed (cpu_power.h): the kernel's cpufreq files, written from the LVGL thread
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cpu_power.h"

#include "cfg/cfg_api.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The kernel's cpufreq files. The PC build leaves a PC's own governor alone; the host test gives a folder of its own */
#ifndef CPUFREQ_DIR
#ifdef HOST_BUILD
#define CPUFREQ_DIR NULL
#else
#define CPUFREQ_DIR "/sys/devices/system/cpu/cpufreq"
#endif
#endif

static bool read_line(const char *dir, const char *name, char *buf, size_t size) {
    char  path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "r")))
        return false;
    bool ok = fgets(buf, (int)size, f) != NULL;
    fclose(f);
    if (ok)
        buf[strcspn(buf, "\n")] = 0;
    return ok;
}

/* One write(): the kernel takes or refuses the value there, which stdio's buffer would put off to the close */
static bool write_line(const char *dir, const char *name, const char *text) {
    char path[512];
    int  fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if ((fd = open(path, O_WRONLY | O_TRUNC)) < 0) {
        fprintf(stderr, "cpu: %s: %s\n", path, strerror(errno));
        return false;
    }
    size_t n  = strlen(text);
    bool   ok = write(fd, text, n) == (ssize_t)n;
    if (!ok)
        fprintf(stderr, "cpu: %s refused %s: %s\n", path, text, strerror(errno));
    close(fd);
    return ok;
}

static unsigned long read_khz(const char *dir, const char *name) {
    char buf[32];

    return read_line(dir, name, buf, sizeof(buf)) ? strtoul(buf, NULL, 10) : 0;
}

static bool has_word(const char *list, const char *word) {
    size_t n = strlen(word);

    for (const char *p = list; (p = strstr(p, word)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return true;
    return false;
}

unsigned long cpu_power_floor_khz(const char *avail, unsigned long min_khz, unsigned long max_khz) {
    unsigned long best = 0, top = 0;
    const char   *p    = avail;

    while (p && *p) {
        char         *end;
        unsigned long f = strtoul(p, &end, 10);

        if (end == p)
            break;
        p = end;
        if (f > top)
            top = f;
        if (f >= CPU_SAVE_MIN_KHZ && (!best || f < best))
            best = f;
    }
    if (best)
        return best;
    if (top)
        return top;
    return CPU_SAVE_MIN_KHZ < min_khz ? min_khz : CPU_SAVE_MIN_KHZ > max_khz ? max_khz : CPU_SAVE_MIN_KHZ;
}

/* Saves power: the floor first, which "performance" does not look at, then the governor, so it never goes under it.
 * Full speed: the governor first, at the top at once, then the kernel's own floor back */
static bool policy_apply(const char *dir, bool save) {
    unsigned long min = read_khz(dir, "cpuinfo_min_freq"), max = read_khz(dir, "cpuinfo_max_freq");
    char          govs[256], avail[512], khz[24];

    if (!min || !max) {
        fprintf(stderr, "cpu: %s: no frequencies\n", dir);
        return false;
    }
    if (!save) {
        snprintf(khz, sizeof(khz), "%lu", min);
        bool ok = write_line(dir, "scaling_governor", CPU_FULL_GOV);
        ok = write_line(dir, "scaling_min_freq", khz) && ok;
        if (ok)
            fprintf(stderr, "cpu: %s, full speed\n", CPU_FULL_GOV);
        return ok;
    }
    if (!read_line(dir, "scaling_available_governors", govs, sizeof(govs)) || !has_word(govs, CPU_SAVE_GOV)) {
        fprintf(stderr, "cpu: %s: no %s governor, left as it is\n", dir, CPU_SAVE_GOV);
        return false;
    }
    if (!read_line(dir, "scaling_available_frequencies", avail, sizeof(avail)))
        avail[0] = 0;
    unsigned long low = cpu_power_floor_khz(avail, min, max);
    snprintf(khz, sizeof(khz), "%lu", low);
    if (!write_line(dir, "scaling_min_freq", khz) || !write_line(dir, "scaling_governor", CPU_SAVE_GOV))
        return false;
    fprintf(stderr, "cpu: %s, %lu-%lu MHz\n", CPU_SAVE_GOV, low / 1000, max / 1000);
    return true;
}

bool cpu_power_apply(const char *cpufreq_dir, bool save) {
    DIR           *d;
    struct dirent *e;
    bool           any = false, ok = true;

    if (!cpufreq_dir || !(d = opendir(cpufreq_dir)))
        return false;
    while ((e = readdir(d))) {
        char dir[512];

        if (strncmp(e->d_name, "policy", 6))
            continue;
        snprintf(dir, sizeof(dir), "%s/%s", cpufreq_dir, e->d_name);
        any = true;
        ok  = policy_apply(dir, save) && ok;
    }
    closedir(d);
    return any && ok;
}

static void on_change(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    cpu_power_apply(CPUFREQ_DIR, param_i_get(cfg_cpu_save) != 0);
}

void cpu_power_start(void) {
    subject_subscribe((Subject *)cfg_cpu_save, on_change, NULL);
    if (param_i_get(cfg_cpu_save))
        cpu_power_apply(CPUFREQ_DIR, true);
}
