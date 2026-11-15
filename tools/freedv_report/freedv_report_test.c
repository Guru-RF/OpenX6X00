/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/freedv_report.c, the RADE app's FreeDV Reporter on the GUI's side (#37): the helper started with
 *  its arguments, the lines it gets (each event once, the bad ones not at all), its lines back, and that nothing waits:
 *  a helper that reads nothing takes a flood of lines without a write blocking, the stop returns at once and the
 *  helper is gone after it (by itself at the end of its stdin, by SIGTERM at 1 s, by SIGKILL at 2 s), a helper that
 *  died costs no SIGPIPE, a missing one no descriptor. Last, the real helper with no server to reach: one line, and
 *  gone at the stop without a signal. fake_helper.py stands in for the helper; this process keeps SIGPIPE's default,
 *  so a write to a dead helper that raised it would end the test.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "freedv_report.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails;

#define CHECK(c, ...)                                                                                                 \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            fails++;                                                                                                  \
            printf("  FAIL ");                                                                                        \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static char fake[PATH_MAX], real_helper[PATH_MAX], logf[PATH_MAX];

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void nap(double s) { usleep((useconds_t)(s * 1e6)); }

static const char *log_text(void)
{
    static char buf[16384];
    FILE       *f = fopen(logf, "r");
    size_t      n = 0;

    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

static bool wait_log(const char *needle, double secs)
{
    for (double end = now() + secs; now() < end; nap(0.02))
        if (strstr(log_text(), needle))
            return true;
    return strstr(log_text(), needle) != NULL;
}

/* Children of this process, the zombies too: 0 once the reaper has waited for the helper */
static int children(void)
{
    DIR           *d = opendir("/proc");
    struct dirent *e;
    int            n = 0;
    char           path[300], stat[512];

    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");

        if (!f)
            continue;
        size_t k = fread(stat, 1, sizeof(stat) - 1, f);

        fclose(f);
        stat[k] = '\0';
        char *paren = strrchr(stat, ')');
        int   ppid = 0;
        char  state;

        if (paren && sscanf(paren + 1, " %c %d", &state, &ppid) == 2 && ppid == getpid())
            n++;
    }
    if (d)
        closedir(d);
    return n;
}

/* The time until no child is left, or -1 */
static double wait_gone(double secs)
{
    double t0 = now();

    for (double end = t0 + secs; now() < end; nap(0.01))
        if (!children())
            return now() - t0;
    return -1;
}

static int fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int  n = 0;

    while (d && readdir(d))
        n++;
    if (d)
        closedir(d);
    return n;
}

static bool poll_for(const char *want, double secs)
{
    char line[256];

    for (double end = now() + secs; now() < end; nap(0.02))
        while (freedv_report_poll(line, sizeof(line)))
            if (!strcmp(line, want))
                return true;
    return false;
}

static void start_fake(const char *mode)
{
    unlink(logf);
    setenv("FAKE_MODE", mode, 1);
    setenv("FAKE_LOG", logf, 1);
    freedv_report_set_helper(fake);
    CHECK(freedv_report_start("N0CALL", "FN31pr", false, 14236000, "OpenX6X00 0.3.12"), "%s: started", mode);
    CHECK(freedv_report_running(), "%s: running", mode);
}

static void case_echo(void)
{
    int    fd0 = fds();
    double t0;

    printf("the arguments, each event once, the lines back, the stop\n");
    start_fake("echo");
    CHECK(wait_log("args N0CALL|FN31pr|0|14236000|OpenX6X00 0.3.12\n", 3), "the arguments: %s", log_text());
    CHECK(poll_for("connected", 3), "\"connected\" back");
    CHECK(poll_for("qsy K1ABC 7177000 pse QSY", 1), "a QSY line back");

    freedv_report_freq(14236000);   /* the start's own: not again */
    freedv_report_freq(7177000);
    freedv_report_freq(7177000);
    freedv_report_freq(0);
    freedv_report_tx(false);        /* the start's own */
    freedv_report_tx(true);
    freedv_report_tx(true);
    freedv_report_tx(false);
    freedv_report_rx("DL1ABC", -3);
    freedv_report_rx("DL1ABC", -3);  /* every End-of-Over counts */
    freedv_report_rx("bad call", 1);
    freedv_report_rx("", 1);
    freedv_report_rx(NULL, 1);
    freedv_report_rx("ABCDEFGHIJKLM", 1);
    freedv_report_message("  hello\t  world \n");
    freedv_report_message("hello world");
    freedv_report_message("Gr\xc3\xbc\xc3\x9f" "e aus K\xc3\xb6ln \xc3");     /* a broken last character */
    freedv_report_message("0123456789012345678901234567890123456789012345678901234567890123456789");
    freedv_report_message("");
    CHECK(wait_log("line msg \n", 3), "all the lines");
    const char *want = "line freq 7177000\n"
                       "line tx 1\n"
                       "line tx 0\n"
                       "line rx DL1ABC -3\n"
                       "line rx DL1ABC -3\n"
                       "line msg hello world\n"
                       "line msg Gr\xc3\xbc\xc3\x9f" "e aus K\xc3\xb6ln\n"
                       "line msg 0123456789012345678901234567890123456789012345678901234567890123\n"
                       "line msg \n";
    const char *got = strstr(log_text(), "line ");

    CHECK(got && !strcmp(got, want), "the lines, once each:\n%s", got ? got : "(none)");

    t0 = now();
    freedv_report_stop();
    CHECK(now() - t0 < 0.005, "the stop returns at once (%.4f s)", now() - t0);
    CHECK(!freedv_report_running(), "not running after the stop");
    freedv_report_tx(true);         /* nothing to send to: nothing happens */
    char line[64];

    CHECK(!freedv_report_poll(line, sizeof(line)), "no line after the stop");
    CHECK(wait_log("eof\n", 2), "the helper sees the end of its stdin");
    double g = wait_gone(2);

    CHECK(g >= 0 && g < 0.9, "it exits by itself and is reaped, no signal (%.2f s)", g);
    CHECK(fds() == fd0, "no descriptor left (%d, %d before)", fds(), fd0);
}

static void tick(bool sync, float snr, uint32_t eoo, const char *call)
{
    freedv_rx_stats_t st = {.running = true, .sync = sync, .snr_db = snr, .eoo_count = eoo};

    snprintf(st.callsign, sizeof(st.callsign), "%s", call);
    freedv_report_rx_stats(&st);
}

static void case_eoo(void)
{
    printf("End-of-Overs: the callsign with the last SNR of the sync, once; late ones, a restart\n");
    freedv_report_rx_stats(NULL);   /* not running: nothing */
    start_fake("echo");
    CHECK(poll_for("connected", 3), "connected");
    tick(true, 7.4f, 3, "OLD1");    /* heard before the start: not reported */
    tick(true, 7.4f, 3, "OLD1");
    tick(false, 0.0f, 4, "DL1ABC"); /* the End-of-Over ended the sync: the SNR before it */
    tick(false, 0.0f, 4, "DL1ABC");
    tick(true, -2.6f, 4, "DL1ABC");
    tick(false, 0.0f, 5, "");       /* the callsign comes two ticks later */
    tick(false, 0.0f, 5, "");
    tick(false, 0.0f, 5, "K1ABC");
    tick(false, 0.0f, 5, "K1ABC");
    tick(false, 0.0f, 0, "");       /* the receiver started again */
    tick(true, 12.0f, 0, "");
    tick(false, 0.0f, 1, "");       /* no callsign for five ticks: none */
    for (int i = 0; i < 4; i++)
        tick(false, 0.0f, 1, "");
    tick(false, 0.0f, 1, "G4XYZ");
    tick(false, 0.0f, 2, "");       /* the fifth tick: still */
    for (int i = 0; i < 3; i++)
        tick(false, 0.0f, 2, "");
    tick(false, 0.0f, 2, "VK2XYZ");
    tick(false, 0.0f, 3, "bad call");   /* not a callsign: not sent */
    freedv_report_message("end");
    CHECK(wait_log("line msg end\n", 3), "all the lines");
    const char *want = "line rx DL1ABC 7\n"
                       "line rx K1ABC -3\n"
                       "line rx VK2XYZ 12\n"
                       "line msg end\n";
    const char *got = strstr(log_text(), "line ");

    CHECK(got && !strcmp(got, want), "the rx lines:\n%s", got ? got : "(none)");
    freedv_report_stop();
    CHECK(wait_gone(2) >= 0, "gone");
}

static void case_deaf(void)
{
    sigset_t term, old;
    double   t0;
    int      sent = 0;

    printf("a helper that reads nothing: the writes never wait, SIGTERM at 1 s (blocked here, not there)\n");
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &term, &old);    /* the GUI's threads block some: the helper must not inherit it */
    start_fake("deaf");
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    CHECK(wait_log("asleep", 3), "the helper runs");
    t0 = now();
    for (int i = 0; i < 20000; i++, sent++)
        freedv_report_freq(1000000 + (uint32_t)i);
    for (int i = 0; i < 2000; i++)
        freedv_report_rx("K1ABC", i % 20);
    double t = now() - t0;

    CHECK(t < 0.5, "%d lines into a full socket without waiting (%.3f s)", sent + 2000, t);
    t0 = now();
    freedv_report_stop();
    CHECK(now() - t0 < 0.005, "the stop returns at once (%.4f s)", now() - t0);
    double g = wait_gone(3);

    CHECK(g >= 0.9 && g < 1.6, "SIGTERM ends it at 1 s (%.2f s)", g);
}

static void case_stubborn(void)
{
    printf("a helper that ignores SIGTERM: SIGKILL at 2 s\n");
    start_fake("stubborn");
    CHECK(wait_log("asleep", 3), "the helper runs");
    double t0 = now();

    freedv_report_stop();
    CHECK(now() - t0 < 0.005, "the stop returns at once");
    double g = wait_gone(4);

    CHECK(g >= 1.9 && g < 2.6, "SIGKILL ends it at 2 s (%.2f s)", g);
}

static void case_quit(void)
{
    char line[64];

    printf("a helper that died: no SIGPIPE, \"gone\" once\n");
    start_fake("quit");
    CHECK(wait_log("args", 3), "the helper ran");
    nap(0.3);
    freedv_report_freq(3643000);    /* to a closed socket: SIGPIPE would end this test */
    freedv_report_tx(true);
    freedv_report_rx("K1ABC", 3);
    freedv_report_message("x");
    CHECK(poll_for("gone", 2), "\"gone\" when it is gone");
    CHECK(!freedv_report_running(), "not running");
    CHECK(!freedv_report_poll(line, sizeof(line)), "and only once");
    double g = wait_gone(2);

    CHECK(g >= 0 && g < 0.5, "reaped (%.2f s)", g);
}

static void case_missing(void)
{
    int fd0 = fds();

    printf("no helper there\n");
    freedv_report_set_helper("/nonexistent/x6100-freedv-reporter");
    CHECK(!freedv_report_start("N0CALL", "FN31pr", false, 14236000, "OpenX6X00 0.3.12"), "not started");
    CHECK(!freedv_report_running(), "not running");
    freedv_report_tx(true);
    freedv_report_stop();
    CHECK(wait_gone(1) >= 0, "no child");
    CHECK(fds() == fd0, "no descriptor left (%d, %d before)", fds(), fd0);
}

static void case_real_offline(void)
{
    char line[256];
    int  s = socket(AF_INET, SOCK_STREAM, 0), lines = 0;

    printf("the real helper, no server to reach: one line, the app's events cost nothing, gone at the stop\n");
    /* A port nothing listens on */
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t          al = sizeof(a);
    char               url[128];

    bind(s, (struct sockaddr *)&a, sizeof(a));
    getsockname(s, (struct sockaddr *)&a, &al);
    close(s);
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/socket.io/?EIO=4&transport=websocket", ntohs(a.sin_port));
    setenv("X6100_FDVR_URL", url, 1);
    setenv("X6100_FDVR_RETRY", "0.3", 1);
    setenv("X6100_FDVR_RETRY_MAX", "0.6", 1);
    freedv_report_set_helper(real_helper);
    CHECK(freedv_report_start("N0CALL", "FN31pr", false, 14236000, "OpenX6X00 0.3.12"), "started");
    CHECK(poll_for("offline refused", 5), "\"offline refused\"");
    double t0 = now();

    while (now() - t0 < 2.5) {
        double t1 = now();

        freedv_report_freq(7177000 + (uint32_t)(lines * 100));
        freedv_report_tx(lines % 2);
        freedv_report_rx("K1ABC", 5);
        freedv_report_message(lines % 2 ? "a" : "b");
        CHECK(now() - t1 < 0.01, "the events never wait");
        while (freedv_report_poll(line, sizeof(line))) {
            lines++;
            CHECK(0, "no other line while it tries again (%s)", line);
        }
        nap(0.2);
    }
    CHECK(freedv_report_running(), "still running, trying quietly");
    t0 = now();
    freedv_report_stop();
    CHECK(now() - t0 < 0.005, "the stop returns at once");
    double g = wait_gone(3);

    CHECK(g >= 0 && g < 0.9, "gone by itself at the end of its stdin, before any signal (%.2f s)", g);
    unsetenv("X6100_FDVR_URL");
}

static void case_message_file(void)
{
    char m[FREEDV_REPORT_MSG_MAX + 1];

    printf("the status message file\n");
    freedv_report_message_load(m, sizeof(m));
    CHECK(!strcmp(m, ""), "none yet: \"%s\"", m);
    CHECK(freedv_report_message_save("  QRV\t40 m  "), "saved");
    freedv_report_message_load(m, sizeof(m));
    CHECK(!strcmp(m, "QRV 40 m"), "read back: \"%s\"", m);
    CHECK(freedv_report_message_save(""), "saved empty");
    freedv_report_message_load(m, sizeof(m));
    CHECK(!strcmp(m, ""), "read back empty: \"%s\"", m);
}

int main(void)
{
    if (!realpath("fake_helper.py", fake) || !realpath("../../rootfs/usr/sbin/x6100-freedv-reporter", real_helper)) {
        printf("run it in tools/freedv_report\n");
        return 2;
    }
    snprintf(logf, sizeof(logf), "%s/data/fake.log", getcwd(NULL, 0));
    signal(SIGPIPE, SIG_DFL);
    case_echo();
    case_eoo();
    case_deaf();
    case_stubborn();
    case_quit();
    case_missing();
    case_real_offline();
    case_message_file();
    CHECK(children() == 0, "no child left at the end");
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
