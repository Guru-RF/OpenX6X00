/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cpu_power.c, Settings > General > CONTROLS > Processor: what it writes into the kernel's cpufreq
 *  files for Full speed and Saves power, on a folder laid out as the radios' /sys/devices/system/cpu/cpufreq (one
 *  policy for the four cores, the radios' frequencies), and what it leaves alone. The setting and its subscription are
 *  stood in for here.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cpu_power.h"

#include "cfg/cfg_api.h"

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

/* ---- The setting, as cfg_api.h has it --------------------------------------------- */

static int32_t     cpu_save_val;
static observer_cb subscribed;
static int         subscriptions;
ParamInt          *cfg_cpu_save = (ParamInt *)&cpu_save_val;

int32_t param_i_get(const ParamInt *p) {
    return p == cfg_cpu_save ? cpu_save_val : -1;
}

Observer *subject_subscribe(Subject *subj, observer_cb fn, void *user_data) {
    (void)user_data;
    if (subj == (Subject *)cfg_cpu_save) {
        subscribed = fn;
        subscriptions++;
    }
    return NULL;
}

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

/* A policy as the kernel starts it: performance, the floor at the bottom step */
static void policy(const char *name, const char *govs, const char *freqs, const char *min, const char *max) {
    char path[256];

    snprintf(path, sizeof(path), DIR_ "/%s", name);
    mkdir(path, 0755);
    put(name, "scaling_available_governors", govs);
    if (freqs)
        put(name, "scaling_available_frequencies", freqs);
    put(name, "cpuinfo_min_freq", min);
    put(name, "cpuinfo_max_freq", max);
    put(name, "scaling_min_freq", min);
    put(name, "scaling_max_freq", max);
    put(name, "scaling_governor", "performance\n");
}

static void fresh(void) {
    if (system("rm -rf test.d && mkdir -p " DIR_) != 0)
        exit(2);
}

/* ---- The cases ------------------------------------------------------------------------ */

static void floor_cases(void) {
    CHECK(cpu_power_floor_khz(RADIO_FREQS, 120000, 1008000) == 480000, "the radios' table: 480 MHz");
    CHECK(cpu_power_floor_khz("1008000 408000 600000 816000", 408000, 1008000) == 600000,
          "no 480 MHz step, in any order: the next one up, 600 MHz");
    CHECK(cpu_power_floor_khz("120000 240000 312000", 120000, 312000) == 312000, "all under 480 MHz: the top one");
    CHECK(cpu_power_floor_khz("", 120000, 1008000) == 480000, "no list: 480 MHz");
    CHECK(cpu_power_floor_khz(NULL, 120000, 400000) == 400000, "no list, a top under 480 MHz: the top");
    CHECK(cpu_power_floor_khz("", 600000, 1008000) == 600000, "no list, a bottom over 480 MHz: the bottom");
    CHECK(cpu_power_floor_khz("garbage", 120000, 1008000) == 480000, "a list that is no list: as none");
}

static void apply_cases(void) {
    fresh();
    policy("policy0", RADIO_GOVS, RADIO_FREQS, "120000\n", "1008000\n");

    CHECK(cpu_power_apply(DIR_, true), "Saves power applied");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "ondemand"), "Saves power: ondemand, not %s",
          get("policy0", "scaling_governor"));
    CHECK(!strcmp(get("policy0", "scaling_min_freq"), "480000"), "Saves power: from 480 MHz, not %s",
          get("policy0", "scaling_min_freq"));
    CHECK(!strcmp(get("policy0", "scaling_max_freq"), "1008000"), "the top is left as it is");

    CHECK(cpu_power_apply(DIR_, true), "Saves power again");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "ondemand"), "Saves power twice: the same");

    CHECK(cpu_power_apply(DIR_, false), "Full speed applied");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "performance"), "Full speed: performance, not %s",
          get("policy0", "scaling_governor"));
    CHECK(!strcmp(get("policy0", "scaling_min_freq"), "120000"), "Full speed: the kernel's floor back, not %s",
          get("policy0", "scaling_min_freq"));

    /* A kernel without ondemand: nothing written, the policy stays at full speed */
    fresh();
    policy("policy0", "userspace powersave performance\n", RADIO_FREQS, "120000\n", "1008000\n");
    CHECK(!cpu_power_apply(DIR_, true), "no ondemand: not applied");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "performance"), "no ondemand: still performance");
    CHECK(!strcmp(get("policy0", "scaling_min_freq"), "120000"), "no ondemand: the floor untouched");

    /* "ondemand" only as part of another word is not ondemand */
    fresh();
    policy("policy0", "notondemand performance\n", RADIO_FREQS, "120000\n", "1008000\n");
    CHECK(!cpu_power_apply(DIR_, true), "a governor named like it is not it");

    /* Two policies (a kernel with a clock per cluster): both, each with its own table */
    fresh();
    policy("policy0", RADIO_GOVS, RADIO_FREQS, "120000\n", "1008000\n");
    policy("policy4", RADIO_GOVS, "300000 600000 1200000\n", "300000\n", "1200000\n");
    mkdir(DIR_ "/ondemand", 0755);  /* the governor's own folder, no policy */
    CHECK(cpu_power_apply(DIR_, true), "two policies applied");
    CHECK(!strcmp(get("policy4", "scaling_governor"), "ondemand"), "the second policy: ondemand");
    CHECK(!strcmp(get("policy4", "scaling_min_freq"), "600000"), "the second policy: its own floor, not %s",
          get("policy4", "scaling_min_freq"));
    CHECK(cpu_power_apply(DIR_, false), "two policies back");
    CHECK(!strcmp(get("policy4", "scaling_min_freq"), "300000"), "the second policy: its own floor back");

    /* A policy whose files cannot be read: false, the others still done */
    fresh();
    policy("policy0", RADIO_GOVS, RADIO_FREQS, "120000\n", "1008000\n");
    mkdir(DIR_ "/policy1", 0755);
    CHECK(!cpu_power_apply(DIR_, true), "a policy without its files: not all applied");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "ondemand"), "the good policy applied all the same");

    /* No cpufreq at all, or no policy in it; the PC build passes none */
    fresh();
    CHECK(!cpu_power_apply(DIR_, true), "no policy: false");
    CHECK(!cpu_power_apply("test.d/none", true), "no folder: false");
    CHECK(!cpu_power_apply(NULL, true), "NULL: false");
}

static void start_cases(void) {
    /* Full speed at the start: the kernel's setting left as it is (one set by hand over SSH stays) */
    fresh();
    policy("policy0", RADIO_GOVS, RADIO_FREQS, "120000\n", "1008000\n");
    put("policy0", "scaling_governor", "schedutil\n");
    cpu_save_val  = 0;
    subscriptions = 0;
    cpu_power_start();
    CHECK(subscriptions == 1, "the setting followed");
    CHECK(!strcmp(get("policy0", "scaling_governor"), "schedutil"), "Full speed at the start: nothing written");

    /* Chosen in Settings: applied at once, either way */
    cpu_save_val = 1;
    subscribed((Subject *)cfg_cpu_save, NULL);
    CHECK(!strcmp(get("policy0", "scaling_governor"), "ondemand"), "Saves power chosen: ondemand");
    cpu_save_val = 0;
    subscribed((Subject *)cfg_cpu_save, NULL);
    CHECK(!strcmp(get("policy0", "scaling_governor"), "performance"), "Full speed chosen: performance");

    /* Saves power at the start: applied before anything is chosen */
    fresh();
    policy("policy0", RADIO_GOVS, RADIO_FREQS, "120000\n", "1008000\n");
    cpu_save_val = 1;
    cpu_power_start();
    CHECK(!strcmp(get("policy0", "scaling_governor"), "ondemand"), "Saves power at the start: ondemand");
    CHECK(!strcmp(get("policy0", "scaling_min_freq"), "480000"), "Saves power at the start: from 480 MHz");
}

int main(void) {
    floor_cases();
    apply_cases();
    start_cases();
    if (system("rm -rf test.d") != 0)
        fails++;
    printf("cpu_power: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
