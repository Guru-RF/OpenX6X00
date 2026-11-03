/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/wr12.c, the X6200's WR12 driver (#134), against a stand-in WR12 on a pty that answers each
 *  command OK (or ERROR, late, nothing, or unplugs itself on one), found by its USB id in a stand-in sysfs
 *  (test.d/sys), with gpsd's control socket stood in too. In order: a WR12 the GUI before left out of its bypass (the
 *  mark), WR12 on HOST off (nothing sent), on (gpsd told to let go, the set-up in order, the keepalive), 98 MHz
 *  (AT+FREQ, AT+BYPASS=0, not while the radio is in TX), 98.1 MHz, 7 MHz, off while receiving, ERROR answers (in the
 *  set-up, to AT+FREQ, to AT+BYPASS=1), a WR12 that stops answering, unplugged in the middle of a command (receiving,
 *  and on its way back to the bypass, for good or back within GONE_MS as after a reset on USB), plugged in again as
 *  ttyACM1 beside a GPS on ttyACM0, an echo before the answers, the serial paddle's question, and the log: each
 *  answer once, not one line a command. A WR12 that takes the set-up but answers AT+AYT too late, again and again:
 *  said once, set up again later each time. One that never stops sending. Last, a GUI stopped with SIGTERM while the
 *  WR12 is out of its bypass (this program again, as `wr12_test term`, its log in wr12_term.log), and the start
 *  loop's AT+BYPASS=1 after a GUI that ended any way (rootfs/usr/sbin/x6100-wr12-bypass, run by sh on the stand-ins).
 *
 *  The TX rules are wr12_tx_block's, which cfg_tx_refusal says first and so radio.c's refusals and its hold at 0 W
 *  follow: TX held from the moment the radio asks the WR12 out of its bypass until the WR12 is seen back in it (its OK
 *  to AT+BYPASS=1), or is unplugged; free at once when it never left the bypass.
 */

#define _GNU_SOURCE
#include "wr12.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MARK "test.d/wr12-out"

static int fails, checks;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    checks++;
    if (!ok) {
        fails++;
        printf("FAIL ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

static uint64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms) {
    usleep(ms * 1000);
}

#define WAIT_FOR(cond, ms)                                      \
    do {                                                        \
        uint64_t _end = now_ms() + (ms);                        \
        while (!(cond) && now_ms() < _end) sleep_ms(5);         \
    } while (0)

static bool is(const char *a, const char *b) {
    return a && b ? strcmp(a, b) == 0 : a == b;
}

/* ---- The stand-in WR12 ------------------------------------------------------------------------------------------- */

#define MAX_CMDS 2048

static pthread_mutex_t fw_mux = PTHREAD_MUTEX_INITIALIZER;
static char            cmds[MAX_CMDS][40];      /* what it was sent, in order */
static uint64_t        cmd_ms[MAX_CMDS];        /* and when */
static int             n_cmds;
static struct {
    int      master;            /* the pty's own side, -1: none */
    char     name[16];          /* "ttyACM0" */
    bool     silent;            /* answers nothing */
    bool     echo;              /* sends each command back before its answer */
    bool     cr;                /* ends its lines with \r alone */
    bool     endless;           /* sends lines of its own, without end, as fast as the port takes them */
    char     error[40];         /* a command starting with this answered ERROR */
    char     unplug[40];        /* unplugged at the first command starting with this, unanswered */
    char     slow[40];          /* a command starting with this answered slow_ms late */
    unsigned slow_ms;
} fw = {.master = -1};
static pthread_t   fw_thread;
static bool        fw_running;
static atomic_bool fw_stop;

static bool starts(const char *s, const char *prefix) {
    return prefix[0] && strncmp(s, prefix, strlen(prefix)) == 0;
}

static void sys_path(char *buf, size_t size, const char *name, const char *what) {
    snprintf(buf, size, "test.d/sys/%s%s%s", name, what[0] ? "/" : "", what);
}

/* Out of the stand-in sysfs and /dev, as the kernel does it at an unplug */
static void sys_remove(const char *name) {
    char p[96];

    sys_path(p, sizeof(p), name, "idVendor");
    unlink(p);
    sys_path(p, sizeof(p), name, "idProduct");
    unlink(p);
    sys_path(p, sizeof(p), name, "device");
    rmdir(p);
    sys_path(p, sizeof(p), name, "");
    rmdir(p);
    snprintf(p, sizeof(p), "test.d/dev/%s", name);
    unlink(p);
}

static void sys_add(const char *name, const char *vid, const char *pid) {
    char  p[96];
    FILE *f;

    sys_path(p, sizeof(p), name, "");
    mkdir(p, 0755);
    sys_path(p, sizeof(p), name, "device");
    mkdir(p, 0755);
    sys_path(p, sizeof(p), name, "idVendor");
    if ((f = fopen(p, "w"))) {
        fprintf(f, "%s\n", vid);
        fclose(f);
    }
    sys_path(p, sizeof(p), name, "idProduct");
    if ((f = fopen(p, "w"))) {
        fprintf(f, "%s\n", pid);
        fclose(f);
    }
}

/* Unplugged: from its own thread in the middle of a command, or from the test's */
static void fw_gone(void) {
    pthread_mutex_lock(&fw_mux);
    sys_remove(fw.name);
    close(fw.master);
    fw.master = -1;
    pthread_mutex_unlock(&fw_mux);
}

static void *fw_run(void *arg) {
    char   line[64];
    size_t len = 0;
    int    m = fw.master;

    (void)arg;
    while (!atomic_load(&fw_stop)) {
        struct pollfd p = {m, POLLIN, 0};
        char          buf[64];
        ssize_t       k;
        bool          endless;

        pthread_mutex_lock(&fw_mux);
        endless = fw.endless;
        pthread_mutex_unlock(&fw_mux);
        if (endless) {
            static const char dump[] = "REG 05 = 1F\r\nREG 06 = 80\r\nREG 07 = 00\r\nREG 08 = C0\r\n";
            char              lots[4096];

            for (size_t i = 0; i + sizeof(dump) - 1 <= sizeof(lots); i += sizeof(dump) - 1) {
                memcpy(lots + i, dump, sizeof(dump) - 1);
            }
            if (write(m, lots, sizeof(lots) - sizeof(lots) % (sizeof(dump) - 1)) < 0) {
            }
        }
        if (poll(&p, 1, endless ? 0 : 20) <= 0 || !(p.revents & POLLIN)) {
            if (endless) continue;
            sleep_ms(2);    /* the driver let go of its side: hung up until it opens it again */
            continue;
        }
        if ((k = read(m, buf, sizeof(buf))) <= 0) {
            sleep_ms(2);
            continue;
        }
        for (ssize_t i = 0; i < k; i++) {
            const char *ans = "OK\r\n";
            const char *eol = "\r\n";
            bool        unplug, silent, echo;
            unsigned    delay;

            if (buf[i] == '\r') continue;
            if (buf[i] != '\n') {
                if (len < sizeof(line) - 1) line[len++] = buf[i];
                continue;
            }
            line[len] = 0;
            len = 0;
            pthread_mutex_lock(&fw_mux);
            if (n_cmds < MAX_CMDS) {
                cmd_ms[n_cmds] = now_ms();
                snprintf(cmds[n_cmds++], sizeof(cmds[0]), "%.39s", line);
            }
            unplug = starts(line, fw.unplug);
            if (starts(line, fw.error)) ans = "ERROR\r\n";
            if (fw.cr) {
                ans = starts(line, fw.error) ? "ERROR\r" : "OK\r";
                eol = "\r";
            }
            delay  = starts(line, fw.slow) ? fw.slow_ms : 0;
            silent = fw.silent;
            echo   = fw.echo;
            pthread_mutex_unlock(&fw_mux);
            if (unplug) {
                fw_gone();
                return NULL;
            }
            if (silent) continue;
            if (delay) sleep_ms(delay);
            if (echo && (write(m, line, strlen(line)) < 0 || write(m, eol, strlen(eol)) < 0)) continue;
            if (write(m, ans, strlen(ans)) < 0) continue;
        }
    }
    return NULL;
}

/* A WR12 plugged in as name: its USB id in sysfs, its port a pty */
static void plug(const char *name) {
    char p[96];
    int  m = posix_openpt(O_RDWR | O_NOCTTY);

    if (fw_running) {
        atomic_store(&fw_stop, true);
        pthread_join(fw_thread, NULL);
        fw_running = false;
    }
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) {
        printf("FAIL no pty: %s\n", strerror(errno));
        exit(1);
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);     /* sending without end: never held up by it */
    snprintf(fw.name, sizeof(fw.name), "%s", name);
    fw.master = m;
    sys_add(name, "0483", "5740");
    snprintf(p, sizeof(p), "test.d/dev/%s", name);
    unlink(p);
    if (symlink(ptsname(m), p) != 0) {
        printf("FAIL %s: %s\n", p, strerror(errno));
        exit(1);
    }
    atomic_store(&fw_stop, false);
    pthread_create(&fw_thread, NULL, fw_run, NULL);
    fw_running = true;
}

static void fw_set(const char **field_or_null, bool *flag, bool on) {
    (void)field_or_null;
    pthread_mutex_lock(&fw_mux);
    *flag = on;
    pthread_mutex_unlock(&fw_mux);
}

static void fw_text(char *field, const char *text) {
    pthread_mutex_lock(&fw_mux);
    snprintf(field, 40, "%s", text);
    pthread_mutex_unlock(&fw_mux);
}

static int cmds_mark(void) {
    int n;

    pthread_mutex_lock(&fw_mux);
    n = n_cmds;
    pthread_mutex_unlock(&fw_mux);
    return n;
}

/* When the commands from `from` on starting with prefix came, into t (max of them); how many */
static int cmd_times(int from, const char *prefix, uint64_t *t, int max) {
    int n = 0;

    pthread_mutex_lock(&fw_mux);
    for (int i = from; i < n_cmds && n < max; i++) {
        if (starts(cmds[i], prefix)) t[n++] = cmd_ms[i];
    }
    pthread_mutex_unlock(&fw_mux);
    return n;
}

/* The last command it was sent is this one */
static bool last_is(const char *cmd) {
    bool ok;

    pthread_mutex_lock(&fw_mux);
    ok = n_cmds > 0 && strcmp(cmds[n_cmds - 1], cmd) == 0;
    pthread_mutex_unlock(&fw_mux);
    return ok;
}

static int cmds_since(int from, const char *prefix) {
    int n = 0;

    pthread_mutex_lock(&fw_mux);
    for (int i = from; i < n_cmds; i++) n += starts(cmds[i], prefix);
    pthread_mutex_unlock(&fw_mux);
    return n;
}

/* The commands from `from` on are these, in this order, and no more than these (more: more) */
static void expect(const char *what, int from, const char *const *want, int n, bool more) {
    bool ok;

    pthread_mutex_lock(&fw_mux);
    ok = more ? n_cmds - from >= n : n_cmds - from == n;
    for (int i = 0; i < n && ok; i++) ok = strcmp(cmds[from + i], want[i]) == 0;
    if (!ok) {
        printf("FAIL %s, sent:", what);
        for (int i = from; i < n_cmds; i++) printf(" %s", cmds[i]);
        printf("\n");
        fails++;
    }
    checks++;
    pthread_mutex_unlock(&fw_mux);
}

static const char *const SETUP[] = {
    "AT+INIT", "AT+LNAGAIN=15", "AT+VGAGAIN=0", "AT+MIXERGAIN=0", "AT+LNAAGCENA=1", "AT+MIXERAGCENA=1", "AT+BYPASS=1",
};
#define N_SETUP 7

/* ---- The stand-in gpsd: its control socket answers ERROR (none of its devices), what it was told kept --------- */

static char            gpsd_said[64];
static pthread_mutex_t gpsd_mux = PTHREAD_MUTEX_INITIALIZER;

static void *gpsd_run(void *arg) {
    int s = *(int *)arg;

    for (;;) {
        int     c = accept(s, NULL, NULL);
        char    buf[64];
        ssize_t k;

        if (c < 0) continue;
        if ((k = read(c, buf, sizeof(buf) - 1)) > 0) {
            buf[k] = 0;
            pthread_mutex_lock(&gpsd_mux);
            snprintf(gpsd_said, sizeof(gpsd_said), "%s", buf);
            pthread_mutex_unlock(&gpsd_mux);
            if (write(c, "ERROR\n", 6) < 0) {
            }
        }
        close(c);
    }
    return NULL;
}

static void gpsd_start(void) {
    static int         s;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    pthread_t          t;

    snprintf(a.sun_path, sizeof(a.sun_path), "test.d/gpsd.sock");
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        printf("FAIL gpsd's socket: %s\n", strerror(errno));
        exit(1);
    }
    pthread_create(&t, NULL, gpsd_run, &s);
    pthread_detach(t);
}

static bool gpsd_told(const char *what) {
    bool ok;

    pthread_mutex_lock(&gpsd_mux);
    ok = strcmp(gpsd_said, what) == 0;
    gpsd_said[0] = 0;
    pthread_mutex_unlock(&gpsd_mux);
    return ok;
}

/* ---- The GUI's side ------------------------------------------------------------------------------------------------ */

static atomic_int  notified;
static atomic_bool keyed;
static char        notes_seen[64][64];
static int         n_notes;

static bool on_notify(void) {
    atomic_fetch_add(&notified, 1);
    return true;
}

static bool on_keyed(void) {
    return atomic_load(&keyed);
}

static int notes(const char *text) {
    char buf[64];
    int  n = 0;

    while (wr12_note(buf, sizeof(buf)) && n_notes < 64) snprintf(notes_seen[n_notes++], sizeof(buf), "%s", buf);
    for (int i = 0; i < n_notes; i++) n += strcmp(notes_seen[i], text) == 0;
    return n;
}

static int log_lines(const char *text) {
    FILE *f = fopen("wr12_test.log", "r");
    char  line[256];
    int   n = 0;

    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) n += strstr(line, text) != NULL;
    fclose(f);
    return n;
}

static bool marked(void) {
    return access(MARK, F_OK) == 0;
}

/* The start loop's x6100-wr12-bypass on the stand-ins; what it said into out */
static void loop_bypass(char *out, size_t size) {
    FILE  *f;
    size_t k = 0;

    out[0] = 0;
    if (system("X6100_WR12_SYS=test.d/sys X6100_WR12_DEV=test.d/dev X6100_WR12_MARK=" MARK
               " sh ../../rootfs/usr/sbin/x6100-wr12-bypass > test.d/bypass.out 2>&1") != 0) {
        snprintf(out, size, "(it failed)");
        return;
    }
    if ((f = fopen("test.d/bypass.out", "r"))) {
        k = fread(out, 1, size - 1, f);
        fclose(f);
    }
    out[k] = 0;
}

/* `wr12_test term`: a GUI receiving 98 MHz through the WR12, until the test stops it with SIGTERM */
static int term_child(void) {
    if (!freopen("wr12_term.log", "w", stderr)) return 1;
    setvbuf(stderr, NULL, _IONBF, 0);
    wr12_init(on_notify, on_keyed);
    wr12_want(true, true, 98000000);
    for (;;) pause();
}

int main(int argc, char **argv) {
    int m;

    if (argc > 1 && strcmp(argv[1], "term") == 0) return term_child();
    if (system("rm -rf test.d && mkdir -p test.d/sys test.d/dev") != 0) return 1;
    if (!freopen("wr12_test.log", "w", stderr)) return 1;
    setvbuf(stderr, NULL, _IONBF, 0);
    gpsd_start();
    plug("ttyACM0");

    /* A GUI before this one took it out of its bypass and stopped: held from the start, WR12 on HOST off, until the
     * WR12 is set up again and in its bypass; then its port is let go */
    fclose(fopen(MARK, "w"));
    wr12_init(on_notify, on_keyed);
    check(is(wr12_tx_block(), "WR12 not in bypass"), "the mark: TX held from the start (%s)", wr12_tx_block());
    WAIT_FOR(!wr12_tx_block(), 3000);
    check(!wr12_tx_block(), "the mark: TX free once AT+BYPASS=1 is answered OK");
    expect("the mark: the set-up, WR12 on HOST off", 0, SETUP, N_SETUP, false);
    check(!marked(), "the mark: gone with it");
    WAIT_FOR(!wr12_ready(), 1000);
    check(!wr12_ready(), "the mark: the port let go, WR12 on HOST off");
    check(notes("WR12 found on HOST") == 0, "the mark: nothing on the screen, WR12 on HOST off");

    /* Off: nothing sent, the port the serial paddle's */
    m = cmds_mark();
    sleep_ms(800);
    check(cmds_mark() == m, "off: nothing sent");
    check(!wr12_port("/dev/ttyACM0"), "off: the serial paddle may take its port");

    /* On: gpsd told to let go of the port, the set-up, in its bypass, TX free, the keepalive */
    m = cmds_mark();
    wr12_want(true, false, 0);
    WAIT_FOR(wr12_ready(), 3000);
    check(wr12_ready(), "on: found and set up");
    expect("on: the set-up", m, SETUP, N_SETUP, true);
    check(gpsd_told("-test.d/dev/ttyACM0\r\n"), "on: gpsd told to let go of the port");
    check(notes("WR12 found on HOST") == 1, "on: said once on the screen");
    check(!wr12_tx_block(), "on, in its bypass: TX free");
    check(wr12_port("/dev/ttyACM0"), "on: the serial paddle leaves its port alone");
    check(!wr12_port("/dev/ttyACM1") && !wr12_port("/dev/ttyUSB0"), "on: other ports are the paddle's");
    m = cmds_mark();
    sleep_ms(2100);
    check(cmds_since(m, "AT+AYT") >= 4 && cmds_since(m, "AT+AYT") == cmds_mark() - m, "on: AT+AYT, nothing else");

    /* 98 MHz, the radio in TX at first: TX held at once, AT+FREQ, and AT+BYPASS=0 only once the radio is out of TX */
    atomic_store(&keyed, true);
    m = cmds_mark();
    wr12_want(true, true, 98000000);
    check(is(wr12_tx_block(), "WR12 receiving"), "98 MHz: TX held at once (%s)", wr12_tx_block());
    WAIT_FOR(cmds_since(m, "AT+FREQ=98000000") == 1, 1000);
    sleep_ms(300);
    check(cmds_since(m, "AT+BYPASS=0") == 0, "98 MHz: not out of its bypass while the radio is in TX");
    atomic_store(&keyed, false);
    WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 1000);
    expect("98 MHz", m, (const char *const[]){"AT+FREQ=98000000", "AT+BYPASS=0"}, 2, true);
    check(marked(), "98 MHz: marked out of its bypass");
    check(is(wr12_tx_block(), "WR12 receiving"), "98 MHz: TX held");

    /* 98.1 MHz: AT+FREQ alone */
    m = cmds_mark();
    wr12_want(true, true, 98100000);
    WAIT_FOR(cmds_since(m, "AT+FREQ=98100000") == 1, 1000);
    sleep_ms(100);
    check(cmds_since(m, "AT+FREQ=98100000") == 1 && cmds_since(m, "AT+BYPASS") == 0, "98.1 MHz: AT+FREQ alone");

    /* 7 MHz: back in its bypass, TX held until its OK (late here, within the time an answer has), free 50 ms after
     * (SETTLE_MS, for its relay) */
    fw.slow_ms = 100;
    fw_text(fw.slow, "AT+BYPASS=1");
    m = cmds_mark();
    wr12_want(true, false, 7074000);
    check(is(wr12_tx_block(), "WR12 not in bypass"), "7 MHz: TX held until the bypass is answered (%s)",
          wr12_tx_block());
    WAIT_FOR(cmds_since(m, "AT+BYPASS=1") == 1, 1000);
    sleep_ms(30);
    check(is(wr12_tx_block(), "WR12 not in bypass"), "7 MHz: still held while its answer is on its way");
    WAIT_FOR(!wr12_tx_block(), 1000);
    check(!wr12_tx_block(), "7 MHz: TX free after its OK");
    {
        uint64_t sent, free_at = now_ms();

        check(cmd_times(m, "AT+BYPASS=1", &sent, 1) == 1 && free_at - sent >= 100 + 50 - 10,
              "7 MHz: TX free 50 ms after its OK, not at once (%lld ms after AT+BYPASS=1, its OK at 100)",
              (long long)(free_at - sent));
    }
    check(!marked(), "7 MHz: the mark gone");
    fw_text(fw.slow, "");
    expect("7 MHz", m, (const char *const[]){"AT+BYPASS=1"}, 1, true);
    check(cmds_since(m, "AT+FREQ") == 0, "7 MHz: no AT+FREQ");

    /* Asked out while the radio keys and back before it stops: never out, TX free at once */
    atomic_store(&keyed, true);
    m = cmds_mark();
    wr12_want(true, true, 145500000);
    WAIT_FOR(cmds_since(m, "AT+FREQ=145500000") == 1, 1000);
    wr12_want(true, false, 145500000);
    check(!wr12_tx_block(), "asked out and back while the radio keys: TX free at once (%s)", wr12_tx_block());
    atomic_store(&keyed, false);
    sleep_ms(300);
    check(cmds_since(m, "AT+BYPASS=0") == 0, "asked out and back while the radio keys: never out of its bypass");

    /* WR12 on HOST off while receiving: back in its bypass first, TX held until then, then the port let go */
    m = cmds_mark();
    wr12_want(true, true, 98000000);
    WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 1000);
    wr12_want(false, false, 0);
    check(is(wr12_tx_block(), "WR12 not in bypass"), "off while receiving: TX held until the bypass");
    WAIT_FOR(!wr12_ready(), 1500);
    check(!wr12_ready() && !wr12_tx_block(), "off while receiving: in its bypass, TX free, the port let go");
    expect("off while receiving", m, (const char *const[]){"AT+FREQ=98000000", "AT+BYPASS=0", "AT+BYPASS=1"}, 3,
           false);

    /* An ERROR in the set-up: not set up, said once, the set-up again until it all answers OK */
    fw_text(fw.error, "AT+LNAGAIN");
    m = cmds_mark();
    wr12_want(true, false, 0);
    sleep_ms(1200);
    check(!wr12_ready(), "an ERROR in the set-up: not set up");
    check(cmds_since(m, "AT+INIT") >= 2, "an ERROR in the set-up: the set-up again");
    check(cmds_since(m, "AT+VGAGAIN") == 0, "an ERROR in the set-up: none of it after the ERROR");
    check(notes("WR12: ERROR to AT+LNAGAIN=15") == 1, "an ERROR in the set-up: said once on the screen");
    check(notes("WR12 not answering") == 0, "an ERROR in the set-up: an answer, not none");
    fw_text(fw.error, "");
    WAIT_FOR(wr12_ready(), 2000);
    check(wr12_ready() && !wr12_tx_block(), "an ERROR in the set-up, then OK: set up, TX free");

    /* An ERROR to AT+FREQ: said once, not asked again, out of its bypass all the same; said again only after an OK */
    fw_text(fw.error, "AT+FREQ");
    m = cmds_mark();
    wr12_want(true, true, 433920000);
    WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 1000);
    wr12_want(true, true, 434000000);
    WAIT_FOR(cmds_since(m, "AT+FREQ=434000000") == 1, 1000);
    sleep_ms(200);
    check(cmds_since(m, "AT+FREQ=433920000") == 1 && cmds_since(m, "AT+FREQ=434000000") == 1,
          "an ERROR to AT+FREQ: not asked again until the frequency changes");
    check(notes("WR12: ERROR to AT+FREQ=433920000") == 1 && notes("WR12: ERROR to AT+FREQ=434000000") == 0,
          "an ERROR to AT+FREQ: said once until an OK");
    check(is(wr12_tx_block(), "WR12 receiving"), "an ERROR to AT+FREQ: TX held");
    fw_text(fw.error, "");
    wr12_want(true, true, 435000000);
    WAIT_FOR(cmds_since(m, "AT+FREQ=435000000") == 1, 1000);

    /* An ERROR to AT+BYPASS=1: TX held, asked again at the keepalive's pace until it answers OK */
    fw_text(fw.error, "AT+BYPASS=1");
    m = cmds_mark();
    wr12_want(true, false, 435000000);
    sleep_ms(1000);
    check(cmds_since(m, "AT+BYPASS=1") >= 2, "an ERROR to AT+BYPASS=1: asked again");
    check(is(wr12_tx_block(), "WR12 not in bypass"), "an ERROR to AT+BYPASS=1: TX held meanwhile");
    check(wr12_ready(), "an ERROR to AT+BYPASS=1: an answer, still set up");
    fw_text(fw.error, "");
    WAIT_FOR(!wr12_tx_block(), 1500);
    check(!wr12_tx_block(), "an ERROR to AT+BYPASS=1, then OK: TX free");
    check(notes("WR12: ERROR to AT+BYPASS=1") == 1, "an ERROR to AT+BYPASS=1: said once on the screen");

    /* Stops answering, out of its bypass: let go, said once, TX held (what it was asked last may hold); answering
     * again: set up, in its bypass, TX free */
    m = cmds_mark();
    wr12_want(true, true, 98000000);
    WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 1000);
    fw_set(NULL, &fw.silent, true);
    wr12_want(true, false, 98000000);
    WAIT_FOR(!wr12_ready(), 3000);
    check(!wr12_ready(), "not answering: let go");
    check(is(wr12_tx_block(), "WR12 not in bypass"), "not answering: TX held (%s)", wr12_tx_block());
    check(notes("WR12 not answering") == 1, "not answering: said once on the screen");
    sleep_ms(1000);
    check(notes("WR12 not answering") == 1, "not answering, the set-up again and again: said once");
    check(is(wr12_tx_block(), "WR12 not in bypass"), "not answering: still held");
    fw_set(NULL, &fw.silent, false);
    WAIT_FOR(wr12_ready() && !wr12_tx_block(), 3000);
    check(wr12_ready() && !wr12_tx_block(), "answering again: set up, in its bypass, TX free");

    /* Unplugged in the middle of AT+FREQ while receiving: said, out of its bypass no more, but TX held for as long
     * as the radio asks for it (until it puts the base back on the dial and takes rx back) */
    fw_text(fw.unplug, "AT+FREQ=99000000");
    m = cmds_mark();
    wr12_want(true, true, 99000000);
    WAIT_FOR(!wr12_ready(), 2000);
    check(!wr12_ready(), "unplugged in the middle of a command: let go");
    check(notes("WR12 unplugged") == 1, "unplugged: said on the screen");
    check(is(wr12_tx_block(), "WR12 receiving"), "unplugged, the radio still asking: TX held");
    wr12_want(true, false, 99000000);
    check(!wr12_tx_block(), "unplugged, rx taken back: TX free at once (%s)", wr12_tx_block());
    check(!marked(), "unplugged: no mark");
    fw_text(fw.unplug, "");

    /* Plugged in again as ttyACM1, a GPS on ttyACM0 (another USB id), the radio asking for 145 MHz: found by its id,
     * gpsd told about the right port, set up, then out of its bypass */
    int found = notes("WR12 found on HOST");

    sys_add("ttyACM0", "1546", "01a7");
    plug("ttyACM1");
    m = cmds_mark();
    wr12_want(true, true, 145000000);
    WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 3000);
    expect("plugged in again", m, SETUP, N_SETUP, true);
    expect("plugged in again, after the set-up", m + N_SETUP,
           (const char *const[]){"AT+FREQ=145000000", "AT+BYPASS=0"}, 2, true);
    check(gpsd_told("-test.d/dev/ttyACM1\r\n"), "plugged in again: gpsd told about ttyACM1");
    check(wr12_port("/dev/ttyACM1") && !wr12_port("/dev/ttyACM0"), "the WR12 by its USB id, not its name");
    check(notes("WR12 found on HOST") == found + 1, "plugged in again: said");

    /* Gone from USB on its way back to the bypass, and back within GONE_MS (a reset on USB, the WR12 still powered
     * and maybe out of its bypass): TX held throughout, the WR12 set up again first, then free */
    int unplugged = notes("WR12 unplugged");

    fw_text(fw.unplug, "AT+BYPASS=1");
    wr12_want(true, false, 145000000);
    check(is(wr12_tx_block(), "WR12 not in bypass"), "gone on the way back: held at first");
    WAIT_FOR(!wr12_ready(), 1000);
    sleep_ms(100);
    check(!wr12_ready() && is(wr12_tx_block(), "WR12 not in bypass"), "gone from USB out of its bypass: held a while");
    check(notes("WR12 unplugged") == unplugged + 1, "gone on the way back: said");
    fw_text(fw.unplug, "");
    m = cmds_mark();
    plug("ttyACM1");
    WAIT_FOR(wr12_ready() && !wr12_tx_block(), 2000);
    check(wr12_ready() && !wr12_tx_block(), "back within the time: set up, in its bypass, then TX free");
    expect("back within the time", m, SETUP, N_SETUP, true);

    /* Unplugged on its way back to the bypass for good: TX held for GONE_MS, then free */
    m = cmds_mark();
    wr12_want(true, true, 145000000);
    WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 1000);
    fw_text(fw.unplug, "AT+BYPASS=1");
    wr12_want(true, false, 145000000);
    WAIT_FOR(!wr12_ready(), 1000);
    check(is(wr12_tx_block(), "WR12 not in bypass"), "unplugged on the way back: held a while");
    WAIT_FOR(!wr12_tx_block(), 2000);
    check(!wr12_tx_block() && !wr12_ready(), "unplugged on the way back for good: TX free, let go");
    check(notes("WR12 unplugged") == unplugged + 2, "unplugged on the way back: said");
    fw_text(fw.unplug, "");

    /* A WR12 that echoes the command before its answer, its lines ending in \r alone */
    fw_set(NULL, &fw.echo, true);
    fw_set(NULL, &fw.cr, true);
    plug("ttyACM1");
    WAIT_FOR(wr12_ready(), 3000);
    check(wr12_ready(), "an echo before the answers, \\r alone: set up");
    m = cmds_mark();
    wr12_want(true, true, 70200000);
    WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 1000);
    wr12_want(true, false, 70200000);
    WAIT_FOR(!wr12_tx_block(), 1000);
    check(!wr12_tx_block(), "an echo before the answers: out of its bypass and back");
    check(wr12_ready(), "an echo before the answers: still set up");

    /* Off: the port let go */
    wr12_want(false, false, 0);
    WAIT_FOR(!wr12_ready(), 1500);
    check(!wr12_ready() && !wr12_tx_block(), "off: let go, TX free");
    check(atomic_load(&notified) > 0, "the GUI told");

    /* The log: an answer once, not a line for each command */
    {
        int ayt = cmds_since(0, "AT+AYT"), ok = log_lines("wr12: AT+AYT: OK");

        check(ayt >= 5 && ok == 1, "the log: AT+AYT sent %d times, its OK logged %d times", ayt, ok);
        check(log_lines("wr12: AT+BYPASS=0: OK") == 4 && log_lines("wr12: ttyACM0 let go") == 0,
              "the log: AT+BYPASS=0's OK once a plug-in (%d), no line for each set-up that failed",
              log_lines("wr12: AT+BYPASS=0: OK"));
        check(log_lines("wr12: AT+FREQ=98100000") == 0, "the log: an AT+FREQ answered as the last one, not logged");
        check(log_lines("wr12: AT+LNAGAIN=15: ERROR") == 1, "the log: the set-up's ERROR, once (%d)",
              log_lines("wr12: AT+LNAGAIN=15: ERROR"));
        check(log_lines("wr12: none on USB") <= 2, "the log: none on USB, once a time");
    }

    /* A WR12 that takes the set-up but answers AT+AYT too late, again and again: let go each time, set up again
     * later each time (RETRY_MS, twice that, then no later than BACKOFF_MS), said once on the screen and once in the
     * log. One that worked STABLE_MS before it stopped answering: a new trouble, said again, RETRY_MS again. The
     * driver's loop looks again AYT_MS after a let-go at the soonest: RETRY_MS (300) counts as 400 here, so the set-ups
     * come 2.65 s, 2.85 s, 2.85 s apart (2.65 s each time before) */
    {
        uint64_t t[8], ayt[64];
        int      found = notes("WR12 found on HOST"), quiet = notes("WR12 not answering");
        int      log_found = log_lines("wr12: found on"), log_lost = log_lines("not answering, let go");
        int      n, k;

        fw_set(NULL, &fw.echo, false);
        fw_set(NULL, &fw.cr, false);
        pthread_mutex_lock(&fw_mux);
        fw.slow_ms = 200;
        pthread_mutex_unlock(&fw_mux);
        fw_text(fw.slow, "AT+AYT");
        m = cmds_mark();
        wr12_want(true, false, 0);
        WAIT_FOR(cmds_since(m, "AT+INIT") >= 4, 12000);
        n = cmd_times(m, "AT+INIT", t, 8);
        check(n >= 4, "AYT late: set up again and again (%d)", n);
        if (n >= 4) {
            int64_t g1 = t[1] - t[0], g2 = t[2] - t[1], g3 = t[3] - t[2];

            check(g2 - g1 >= 150 && g3 - g2 > -100 && g3 - g2 < 100,
                  "AYT late: set up again later each time, up to BACKOFF_MS (%lld, %lld, %lld ms)", (long long)g1,
                  (long long)g2, (long long)g3);
        }
        check(notes("WR12 found on HOST") == found + 1, "AYT late: found said once on the screen (%d)",
              notes("WR12 found on HOST") - found);
        check(notes("WR12 not answering") == quiet + 1, "AYT late: not answering said once on the screen (%d)",
              notes("WR12 not answering") - quiet);
        check(log_lines("wr12: found on") == log_found + 1 && log_lines("not answering, let go") == log_lost + 1,
              "AYT late: found and let go, once each in the log (%d, %d)", log_lines("wr12: found on") - log_found,
              log_lines("not answering, let go") - log_lost);

        /* Answering in time again for longer than STABLE_MS, then late again: said again, set up again after
         * RETRY_MS (400 ms after the let-go as above, which comes ANSWER_MS after the third AT+AYT unanswered: 550 ms;
         * 750 ms with BACKOFF_MS) */
        fw_text(fw.slow, "");
        WAIT_FOR(wr12_ready(), 2000);
        sleep_ms(2800);
        check(wr12_ready(), "AYT in time again: stays set up");
        m = cmds_mark();
        fw_text(fw.slow, "AT+AYT");
        WAIT_FOR(cmds_since(m, "AT+INIT") >= 1, 3000);
        n = cmd_times(m, "AT+INIT", t, 1);
        k = cmd_times(m, "AT+AYT", ayt, 64);
        check(n == 1 && k >= 3 && t[0] - ayt[k - 1] < 650,
              "late again after STABLE_MS: set up again after RETRY_MS (%lld ms after the last AT+AYT)",
              n == 1 && k ? (long long)(t[0] - ayt[k - 1]) : -1LL);
        WAIT_FOR(wr12_ready(), 2000);
        check(notes("WR12 not answering") == quiet + 2 && notes("WR12 found on HOST") == found + 2,
              "late again after STABLE_MS: said again (%d, %d)", notes("WR12 not answering") - quiet,
              notes("WR12 found on HOST") - found);
        fw_text(fw.slow, "");
        wr12_want(false, false, 0);
        WAIT_FOR(!wr12_ready() && !wr12_tx_block(), 3000);
        check(!wr12_ready() && !wr12_tx_block(), "AYT late, then off: let go, TX free");
    }

    /* A device with the WR12's USB id that never stops sending (a WR12 stuck in a DUMP): the driver is not held
     * draining it, takes it for one that does not answer and keeps to WR12 on HOST; once it is quiet again, set up */
    {
        int quiet = notes("WR12 not answering");

        fw_set(NULL, &fw.endless, true);
        wr12_want(true, false, 0);
        WAIT_FOR(notes("WR12 not answering") > quiet, 3000);
        check(notes("WR12 not answering") == quiet + 1 && !wr12_ready(), "sending without end: not answering, said");
        check(log_lines("keeps sending") == 1, "sending without end: in the log once (%d)", log_lines("keeps sending"));
        fw_set(NULL, &fw.endless, false);
        WAIT_FOR(wr12_ready(), 3000);
        check(wr12_ready(), "quiet again: set up");
        wr12_want(false, false, 0);
        WAIT_FOR(!wr12_ready() && !wr12_tx_block(), 3000);
        check(!wr12_ready() && !wr12_tx_block(), "sending without end, then off: let go, TX free");
    }

    /* A GUI stopped with SIGTERM while the WR12 is out of its bypass (S95gui stop: a restart, an update or its
     * rollback, a shutdown): AT+BYPASS=1 last thing, the GUI ended as SIGTERM ends it, the mark kept for the GUI
     * after it, which holds TX until it has seen the WR12 back in its bypass */
    {
        pid_t p;
        int   st = 0;

        m = cmds_mark();
        p = fork();
        if (p == 0) {
            execl("/proc/self/exe", "wr12_test", "term", (char *)NULL);
            _exit(127);
        }
        WAIT_FOR(cmds_since(m, "AT+BYPASS=0") == 1, 3000);
        sleep_ms(50);
        check(cmds_since(m, "AT+BYPASS=0") == 1 && marked(), "SIGTERM: a GUI receiving through the WR12");
        m = cmds_mark();
        kill(p, SIGTERM);
        waitpid(p, &st, 0);
        sleep_ms(50);
        check(cmds_since(m, "AT+BYPASS=1") == 1 && last_is("AT+BYPASS=1"), "SIGTERM: AT+BYPASS=1, last thing");
        check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "SIGTERM: the GUI ends as SIGTERM ends it (%#x)", st);
        check(marked(), "SIGTERM: the mark kept for the GUI after it");
    }

    /* The start loop after a GUI that ended any way (a crash), and S95gui stop: with the mark, AT+BYPASS=1 to the
     * port with the WR12's USB id and to no other (the GPS on ttyACM0, a file here, stays empty), the mark kept for
     * the next GUI; without the mark (the WR12 seen in its bypass, an X6100), nothing */
    {
        char        said[256];
        struct stat st;

        unlink("test.d/dev/ttyACM0");
        fclose(fopen("test.d/dev/ttyACM0", "w"));
        m = cmds_mark();
        loop_bypass(said, sizeof(said));
        sleep_ms(50);
        check(cmds_since(m, "AT+BYPASS=1") == 1 && cmds_mark() - m == 1, "the start loop: AT+BYPASS=1 to the WR12");
        check(is(said, "x6100-wr12-bypass: ttyACM1: AT+BYPASS=1 sent\n"), "the start loop: its line (%s)", said);
        check(stat("test.d/dev/ttyACM0", &st) == 0 && st.st_size == 0, "the start loop: nothing to the GPS");
        check(marked(), "the start loop: the mark kept");
        unlink(MARK);
        m = cmds_mark();
        loop_bypass(said, sizeof(said));
        sleep_ms(50);
        check(cmds_mark() == m && said[0] == 0, "the start loop, no mark: nothing (%s)", said);
    }

    printf("%d checks, %d failed\n", checks, fails);
    if (!fails) {
        unlink("wr12_test.log");
        unlink("wr12_term.log");
        if (system("rm -rf test.d") != 0) return 1;
    }
    return fails != 0;
}
