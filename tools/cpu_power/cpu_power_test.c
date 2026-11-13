/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cpu_power.c: a radio that v0.3.11's Saves power left on the kernel's ondemand governor from 480 MHz
 *  is put back to full speed at the start, and anything else is left alone, on a folder laid out as the radios'
 *  /sys/devices/system/cpu/cpufreq (one policy for the four cores, the radios' frequencies).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cpu_power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_ "test.d/cpufreq"

/* The radios' table (the boards' device tree), kHz, as the kernel lists it */
#define RADIO_FREQS "120000 240000 312000 408000 480000 504000 600000 648000 720000 816000 912000 1008000 "
#define RADIO_GOVS  "conservative ondemand userspace powersave performance schedutil "

static int fails, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

/* ---- A cpufreq folder ---------------------------------------------------------------- */

static void put(const char *policy, const char *name, const char *text) {
    char  path[256];
    FILE *f;

    snprintf(path, sizeof(path), DIR_ "/%s/%s", policy, name);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static const char *get(const char *policy, const char *name) {
    static char buf[256];
    char        path[256];
    FILE       *f;

    snprintf(path, sizeof(path), DIR_ "/%s/%s", policy, name);
    buf[0] = 0;
    if ((f = fopen(path, "r"))) {
        if (!fgets(buf, sizeof(buf), f))
            buf[0] = 0;
        fclose(f);
    }
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}

/* A policy with governor gov and floor min (kHz text), the kernel's own floor cpuinfo_min */
static void policy(const char *name, const char *gov, const char *min, const char *cpuinfo_min, const char *max) {
    char path[256];

    snprintf(path, sizeof(path), DIR_ "/%s", name);
    mkdir(path, 0755);
    put(name, "scaling_available_governors", RADIO_GOVS);
    put(name, "scaling_available_frequencies", RADIO_FREQS);
    put(name, "cpuinfo_min_freq", cpuinfo_min);
    put(name, "cpuinfo_max_freq", max);
    put(name, "scaling_min_freq", min);
    put(name, "scaling_max_freq", max);
    put(name, "scaling_governor", gov);
}

static void fresh(void) {
    if (system("rm -rf test.d && mkdir -p " DIR_) != 0)
        exit(2);
}

/* ---- The cases ------------------------------------------------------------------------ */

static void restore_cases(void) {
    /* What v0.3.11's Saves power left: ondemand from 480 MHz. Back to performance and the kernel's floor */
    fresh();
    policy("policy0", "ondemand\n", "480000\n", "120000\n", "1008000\n");
    CHECK(cpu_power_restore(DIR_) == 1, "Saves power's policy put back");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "performance"), "back to performance, not %s",
          get("policy0", "scaling_governor"));
    CHECK(!strcmp(get("policy0", "scaling_min_freq"), "120000"), "the kernel's floor back, not %s",
          get("policy0", "scaling_min_freq"));
    CHECK(!strcmp(get("policy0", "scaling_max_freq"), "1008000"), "the top left as it is");
    CHECK(cpu_power_restore(DIR_) == 0, "once back, nothing more to do");

    /* Full speed as the kernel starts it: nothing written */
    fresh();
    policy("policy0", "performance\n", "120000\n", "120000\n", "1008000\n");
    CHECK(cpu_power_restore(DIR_) == 0, "performance: left alone");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "performance"), "performance: still performance");

    /* Set by hand over SSH: left alone, ondemand at the kernel's own floor too (v0.3.11 never left that) */
    fresh();
    policy("policy0", "schedutil\n", "120000\n", "120000\n", "1008000\n");
    CHECK(cpu_power_restore(DIR_) == 0, "schedutil by hand: left alone");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "schedutil"), "schedutil by hand: still schedutil");
    fresh();
    policy("policy0", "ondemand\n", "120000\n", "120000\n", "1008000\n");
    CHECK(cpu_power_restore(DIR_) == 0, "ondemand at the kernel's floor: left alone");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "ondemand"), "ondemand at the kernel's floor: still ondemand");
    fresh();
    policy("policy0", "powersave\n", "480000\n", "120000\n", "1008000\n");
    CHECK(cpu_power_restore(DIR_) == 0, "another governor with a raised floor: left alone");

    /* Two policies (a kernel with a clock per cluster): each by its own floor */
    fresh();
    policy("policy0", "ondemand\n", "480000\n", "120000\n", "1008000\n");
    policy("policy4", "ondemand\n", "600000\n", "300000\n", "1200000\n");
    mkdir(DIR_ "/ondemand", 0755);  /* the governor's own folder, no policy */
    CHECK(cpu_power_restore(DIR_) == 2, "two policies put back");
    CHECK(!strcmp(get("policy4", "scaling_governor"), "performance"), "the second policy: performance");
    CHECK(!strcmp(get("policy4", "scaling_min_freq"), "300000"), "the second policy: its own floor, not %s",
          get("policy4", "scaling_min_freq"));

    /* A policy whose files cannot be read: -1, the others still done */
    fresh();
    policy("policy0", "ondemand\n", "480000\n", "120000\n", "1008000\n");
    mkdir(DIR_ "/policy1", 0755);
    CHECK(cpu_power_restore(DIR_) == -1, "a policy without its files: -1");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "performance"), "the good policy put back all the same");

    /* No cpufreq at all, or no policy in it; the PC build passes none */
    fresh();
    CHECK(cpu_power_restore(DIR_) == -1, "no policy: -1");
    CHECK(cpu_power_restore("test.d/none") == -1, "no folder: -1");
    CHECK(cpu_power_restore(NULL) == -1, "NULL: -1");
}

static void start_cases(void) {
    /* At the start, on the radio's folder (CPUFREQ_DIR, the test's here) */
    fresh();
    policy("policy0", "ondemand\n", "480000\n", "120000\n", "1008000\n");
    cpu_power_start();
    CHECK(!strcmp(get("policy0", "scaling_governor"), "performance"), "the start puts it back");
    CHECK(!strcmp(get("policy0", "scaling_min_freq"), "120000"), "the start: the kernel's floor back");
}

int main(void) {
    restore_cases();
    start_cases();
    if (system("rm -rf test.d") != 0)
        fails++;
    printf("cpu_power: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
