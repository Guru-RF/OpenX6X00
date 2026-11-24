/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Load meter's sums (src/load_stat.c, #233). /proc/stat: the busy and total ticks of the first
 *  line (iowait idle, the guests not counted twice), the cores from the cpuN lines, a line cut off at the end of the
 *  read not counted, an old kernel's four fields, a text with no cpu line or too few fields refused. The processor's
 *  load between two reads, rounded, and -1 when no tick passed or a counter went back. /proc/meminfo: in use is
 *  MemTotal less MemAvailable, a kernel without MemAvailable takes MemFree + Buffers + Cached, SwapCached is not
 *  Cached, no swap is -1, no MemTotal is refused. A thread's stat line: utime + stime counted from the last ')', a
 *  name with spaces and brackets, a tpgid of -1, a cut line refused; its share of one core on four cores, at most
 *  100 %. The colours' steps, the late loop's steps, the 10 s peak, the bars' pixels. And this PC's own /proc read
 *  and parsed, as the radio reads its own.
 */

#include "load_stat.h"

#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static void int_is(long got, long want, const char *what) {
    char s[160];

    snprintf(s, sizeof(s), "%s: %ld, wanted %ld", what, got, want);
    check(got == want, s);
}

/* An X6100 (four Cortex-A7 cores), a second apart: 400 ticks, 100 of them busy */
static const char stat_a[] =
    "cpu  1000 10 500 8000 200 30 20 0 0 0\n"
    "cpu0 250 2 125 2000 50 8 5 0 0 0\n"
    "cpu1 250 3 125 2000 50 7 5 0 0 0\n"
    "cpu2 250 2 125 2000 50 8 5 0 0 0\n"
    "cpu3 250 3 125 2000 50 7 5 0 0 0\n"
    "intr 123456 0 0 0\n"
    "ctxt 987654\n";
static const char stat_b[] =
    "cpu  1070 10 520 8290 210 35 25 0 0 0\n"
    "cpu0 270 2 130 2070 52 9 6 0 0 0\n"
    "cpu1 270 3 130 2075 53 8 6 0 0 0\n"
    "cpu2 265 2 130 2070 52 9 6 0 0 0\n"
    "cpu3 265 3 130 2075 53 9 7 0 0 0\n"
    "intr 123999 0 0 0\n";

static const char meminfo_radio[] =
    "MemTotal:         505688 kB\n"
    "MemFree:          120000 kB\n"
    "MemAvailable:     320000 kB\n"
    "Buffers:           10000 kB\n"
    "Cached:           150000 kB\n"
    "SwapCached:            0 kB\n"
    "Active:           200000 kB\n"
    "SwapTotal:             0 kB\n"
    "SwapFree:              0 kB\n"
    "Dirty:                12 kB\n";

int main(void) {
    load_cpu_t a, b, c;
    load_mem_t m;
    uint64_t   t;
    char       buf[4096], path[64];

    /* /proc/stat */
    check(load_cpu_parse(stat_a, &a), "the radio's /proc/stat not read");
    int_is((long)a.busy, 1000 + 10 + 500 + 30 + 20, "busy ticks: user nice system irq softirq steal");
    int_is((long)a.total, 1560 + 8000 + 200, "total ticks: busy, idle and iowait");
    int_is(a.cores, 4, "cores of four cpuN lines");
    check(load_cpu_parse(stat_b, &b), "the second read not read");
    int_is(load_cpu_pct(&a, &b), 25, "load of 100 busy ticks in 400");     /* 1660 - 1560 of 10160 - 9760 */
    int_is(load_cpu_pct(&a, &a), -1, "no tick between two reads");
    int_is(load_cpu_pct(&b, &a), -1, "a counter that went back");
    check(load_cpu_parse("cpu  100 0 50 850\ncpu0 100 0 50 850\n", &c), "an old kernel's four fields not read");
    int_is((long)c.busy, 150, "four fields: busy");
    int_is((long)c.total, 1000, "four fields: total");
    int_is(c.cores, 1, "one core");
    check(!load_cpu_parse("cpu  100 0 50\n", &c), "three fields read");
    check(!load_cpu_parse("intr 1 2 3\nctxt 5\n", &c), "a text without a cpu line read");
    check(!load_cpu_parse("", &c), "an empty text read");
    check(load_cpu_parse("cpu  1 2 3 4 5 6 7 8 9 10\ncpu0 1 2 3 4\ncpu1 1 2 3 4\ncpu2 1 2", &c), "a cut text not read");
    int_is(c.cores, 2, "a cpuN line cut off at the end counted");
    /* The numbers stop at the end of the line: four fields, then a line that starts with digits is not a fifth */
    check(load_cpu_parse("cpu  10 0 10 80\n5 5 5 5\n", &c), "a line of four");
    int_is((long)c.total, 100, "numbers read on into the next line");
    /* 100 % at most, rounded to the nearest */
    a = (load_cpu_t){.busy = 0, .total = 0, .cores = 4};
    b = (load_cpu_t){.busy = 400, .total = 400, .cores = 4};
    int_is(load_cpu_pct(&a, &b), 100, "all busy");
    b = (load_cpu_t){.busy = 1, .total = 200, .cores = 4};
    int_is(load_cpu_pct(&a, &b), 1, "half a per cent rounds up");

    /* /proc/meminfo */
    check(load_mem_parse(meminfo_radio, &m), "the radio's /proc/meminfo not read");
    int_is((long)m.total, 505688, "MemTotal");
    int_is((long)m.avail, 320000, "MemAvailable");
    int_is(load_mem_pct(&m), 37, "memory in use");     /* 185688 of 505688 */
    int_is(load_swap_pct(&m), -1, "no swap");
    check(load_mem_parse("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nSwapCached: 999 kB\nCached: 250 kB\n"
                         "SwapTotal: 2000 kB\nSwapFree: 1500 kB\n", &m), "a meminfo without MemAvailable not read");
    int_is((long)m.avail, 400, "without MemAvailable: free, buffers and cached (not SwapCached)");
    int_is(load_mem_pct(&m), 60, "in use without MemAvailable");
    int_is(load_swap_pct(&m), 25, "swap in use");
    check(load_mem_parse("MemTotal: 1000 kB\nMemAvailable: 5000 kB\nSwapTotal: 10 kB\nSwapFree: 20 kB\n", &m),
          "an odd meminfo not read");
    int_is(load_mem_pct(&m), 0, "more available than there is");
    int_is(load_swap_pct(&m), 0, "more swap free than there is");
    check(!load_mem_parse("MemFree: 100 kB\nCached: 250 kB\n", &m), "a meminfo without MemTotal read");
    check(!load_mem_parse("MemTotal: 0 kB\n", &m), "a MemTotal of 0 read");

    /* A thread's stat line */
    check(load_task_parse("812 (x6100_gui) S 1 812 812 0 -1 4194560 9021 0 3 0 1234 567 0 0 20 0 9 0 300 1 2\n",
                          &t), "the GUI thread's stat not read");
    int_is((long)t, 1234 + 567, "utime + stime");
    check(load_task_parse("812 (a b) (c)) R 1 812 812 0 -1 4194560 9021 0 3 0 70 30 0 0 20 0 9 0 300\n", &t),
          "a name with spaces and brackets not read");
    int_is((long)t, 100, "utime + stime after a name with brackets");
    check(!load_task_parse("812 (x6100_gui) S 1 812 812 0 -1 4194560 9021 0 3 0 1234", &t), "a cut line read");
    check(!load_task_parse("812 x6100_gui S 1", &t), "a line without brackets read");
    check(!load_task_parse("812 (x6100_gui)", &t), "a line that ends after the name read");
    /* Four cores, 400 ticks a second: 25 ticks of the thread are a quarter of one core; 100 all of it */
    a = (load_cpu_t){.busy = 0, .total = 0, .cores = 4};
    b = (load_cpu_t){.busy = 200, .total = 400, .cores = 4};
    int_is(load_task_pct(1000, 1025, &a, &b), 25, "a quarter of a core");
    int_is(load_task_pct(1000, 1100, &a, &b), 100, "a whole core");
    int_is(load_task_pct(1000, 1150, &a, &b), 100, "more than a core");
    int_is(load_task_pct(1000, 1000, &a, &b), 0, "an idle thread");
    int_is(load_task_pct(1000, 990, &a, &b), -1, "a thread's counter that went back");
    int_is(load_task_pct(1000, 1025, &b, &b), -1, "no tick between two reads");
    b.cores = 0;
    int_is(load_task_pct(1000, 1025, &a, &b), 6, "no core counted: one");

    /* The colours and the dot */
    int_is(load_level(0), LOAD_LEVEL_OK, "0 %");
    int_is(load_level(LOAD_MID - 1), LOAD_LEVEL_OK, "under raised");
    int_is(load_level(LOAD_MID), LOAD_LEVEL_MID, "raised");
    int_is(load_level(LOAD_HIGH - 1), LOAD_LEVEL_MID, "under high");
    int_is(load_level(LOAD_HIGH), LOAD_LEVEL_HIGH, "high");
    int_is(load_level(100), LOAD_LEVEL_HIGH, "100 %");
    int_is(load_late_level(0), LOAD_LEVEL_OK, "no time");
    int_is(load_late_level(LOAD_LATE_MS - 1), LOAD_LEVEL_OK, "under late");
    int_is(load_late_level(LOAD_LATE_MS), LOAD_LEVEL_MID, "late");
    int_is(load_late_level(LOAD_LATE_HIGH_MS - 1), LOAD_LEVEL_MID, "under very late");
    int_is(load_late_level(LOAD_LATE_HIGH_MS), LOAD_LEVEL_HIGH, "very late");
    int_is(LOAD_LATE_HIGH_MS, 500, "very late is not half the band keys' first repeat (front_keys.c, 500 ms)");

    /* The peak: held LOAD_HOLD seconds, the second it came among them, then gone */
    load_hold_t h = {0};

    int_is(load_hold_push(&h, 10), 10, "the first second");
    int_is(load_hold_push(&h, 80), 80, "a peak");
    for (int i = 0; i < LOAD_HOLD - 2; i++) {
        load_hold_push(&h, 20);
    }
    int_is(load_hold_push(&h, 20), 80, "the peak in its last second");
    int_is(load_hold_push(&h, 20), 20, "the peak after LOAD_HOLD seconds");
    int_is(load_hold_push(&h, 5), 20, "a lower second under the ones held");

    /* The bars */
    int_is(load_px(0, 82), 0, "0 % is no pixel");
    int_is(load_px(100, 82), 82, "100 % fills the bar");
    int_is(load_px(50, 82), 41, "half");
    int_is(load_px(1, 82), 1, "1 % on 82 px rounds to one");
    int_is(load_px(1, 40), 0, "1 % on 40 px is none");
    int_is(load_px(-1, 82), 0, "under 0 % held to 0");
    int_is(load_px(150, 82), 82, "over 100 % held to the top");

    /* This PC's own /proc, as the meter reads the radio's: once a second, the first 2 kB of /proc/stat */
    char small[2048];

    check(load_read("/proc/stat", small, sizeof(small)) && load_cpu_parse(small, &a), "this PC's /proc/stat");
    check(a.cores >= 1 && a.total > a.busy, "this PC's processor");
    check(load_read("/proc/meminfo", buf, sizeof(buf)) && load_mem_parse(buf, &m), "this PC's /proc/meminfo");
    check(load_mem_pct(&m) > 0 && load_mem_pct(&m) <= 100, "this PC's memory in use");
    snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", (long)syscall(SYS_gettid));
    check(load_read(path, buf, sizeof(buf)) && load_task_parse(buf, &t), "this thread's stat");
    check(!load_read("/proc/no-such-file", buf, sizeof(buf)), "a file that is not there read");

    printf("%s: %d cases, %d wrong\n", bad ? "FAIL" : "ok", cases, bad);
    return bad ? 1 : 0;
}
