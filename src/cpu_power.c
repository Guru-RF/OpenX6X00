/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The processor at full speed (cpu_power.h): the kernel's cpufreq files, written from the LVGL thread
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cpu_power.h"

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

/* 1: put back to full speed, 0: left as it is, -1: its files could not be read or written. The governor first, at the
 * top at once, then the kernel's own floor back */
static int policy_restore(const char *dir) {
    char gov[64], min[24], floor[24];

    if (!read_line(dir, "scaling_governor", gov, sizeof(gov)) || !read_line(dir, "scaling_min_freq", min, sizeof(min))
        || !read_line(dir, "cpuinfo_min_freq", floor, sizeof(floor))) {
        fprintf(stderr, "cpu: %s: no governor or frequencies\n", dir);
        return -1;
    }
    if (strcmp(gov, CPU_SAVE_GOV) || strtoul(min, NULL, 10) <= strtoul(floor, NULL, 10))
        return 0;
    if (!write_line(dir, "scaling_governor", CPU_FULL_GOV) || !write_line(dir, "scaling_min_freq", floor))
        return -1;
    fprintf(stderr, "cpu: %s, full speed (was %s from %s kHz: v0.3.11's Saves power)\n", CPU_FULL_GOV, gov, min);
    return 1;
}

int cpu_power_restore(const char *cpufreq_dir) {
    DIR           *d;
    struct dirent *e;
    int            found = 0, restored = 0;
    bool           ok = true;

    if (!cpufreq_dir || !(d = opendir(cpufreq_dir)))
        return -1;
    while ((e = readdir(d))) {
        char dir[512];

        if (strncmp(e->d_name, "policy", 6))
            continue;
        snprintf(dir, sizeof(dir), "%s/%s", cpufreq_dir, e->d_name);
        found++;
        int r = policy_restore(dir);
        if (r < 0)
            ok = false;
        else
            restored += r;
    }
    closedir(d);
    return found && ok ? restored : -1;
}

void cpu_power_start(void) {
    cpu_power_restore(CPUFREQ_DIR);
}
