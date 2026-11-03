/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/wr12.c, the X6200's WR12 driver (#134), against a stand-in WR12 on a pty that answers each
 *  command OK (or ERROR, late, nothing, or unplugs itself on one), found by its USB id in a stand-in sysfs
 *  (test.d/sys), with gpsd's control socket stood in too. In order: a WR12 the GUI before left out of its bypass (the
 *  mark), WR12 on HOST off (nothing sent), on (gpsd told to let go, the set-up in order, the keepalive), 98 MHz
 *  (AT+FREQ, AT+BYPASS=0, not while the radio is in TX), 98.1 MHz, 7 MHz, off while receiving, ERROR answers (in the
 *  set-up, to AT+FREQ, to AT+BYPASS=1), a WR12 that stops answering, unplugged in the middle of a command (receiving,
 *  and on its way back to the bypass), plugged in again as ttyACM1 beside a GPS on ttyACM0, an echo before the
 *  answers, the serial paddle's question, and the log: each answer once, not one line a command.
 *
 *  The TX rules are wr12_tx_block's: TX held from the moment the radio asks the WR12 out of its bypass until the WR12
 *  is seen back in it (its OK to AT+BYPASS=1), or is unplugged; free at once when it never left the bypass.
 */

#define _GNU_SOURCE
#include "wr12.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
static int             n_cmds;
static struct {
    int      master;            /* the pty's own side, -1: none */
    char     name[16];          /* "ttyACM0" */
    bool     silent;            /* answers nothing */
    bool     echo;              /* sends each command back before its answer */
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

        if (poll(&p, 1, 20) <= 0 || !(p.revents & POLLIN)) {
            sleep_ms(2);    /* the driver let go of its side: hung up until it opens it again */
            continue;
        }
        if ((k = read(m, buf, sizeof(buf))) <= 0) {
            sleep_ms(2);
            continue;
        }
        for (ssize_t i = 0; i < k; i++) {
            const char *ans = "OK\r\n";
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
            if (n_cmds < MAX_CMDS) snprintf(cmds[n_cmds++], sizeof(cmds[0]), "%.39s", line);
            unplug = starts(line, fw.unplug);
            if (starts(line, fw.error)) ans = "ERROR\r\n";
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
            if (echo && (write(m, line, strlen(line)) < 0 || write(m, "\r\n", 2) < 0)) continue;
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

int main(void) {
    int m;

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

    /* 7 MHz: back in its bypass, TX held until its OK (late here, within the time an answer has), free after */
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

    /* Unplugged on its way back to the bypass: TX held until then, free once it is gone */
    fw_text(fw.unplug, "AT+BYPASS=1");
    wr12_want(true, false, 145000000);
    check(is(wr12_tx_block(), "WR12 not in bypass"), "unplugged on the way back: held at first");
    WAIT_FOR(!wr12_tx_block(), 2000);
    check(!wr12_tx_block() && !wr12_ready(), "unplugged on the way back: TX free, let go");
    check(notes("WR12 unplugged") == 2, "unplugged on the way back: said");
    fw_text(fw.unplug, "");

    /* A WR12 that echoes the command before its answer */
    fw_set(NULL, &fw.echo, true);
    plug("ttyACM1");
    WAIT_FOR(wr12_ready(), 3000);
    check(wr12_ready(), "an echo before the answers: set up");
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
        check(log_lines("wr12: AT+BYPASS=0: OK") == 3 && log_lines("wr12: ttyACM0 let go") == 0,
              "the log: AT+BYPASS=0's OK once a plug-in (%d), no line for each set-up that failed",
              log_lines("wr12: AT+BYPASS=0: OK"));
        check(log_lines("wr12: AT+FREQ=98100000") == 0, "the log: an AT+FREQ answered as the last one, not logged");
        check(log_lines("wr12: AT+LNAGAIN=15: ERROR") == 1, "the log: the set-up's ERROR, once (%d)",
              log_lines("wr12: AT+LNAGAIN=15: ERROR"));
        check(log_lines("wr12: none on USB") <= 2, "the log: none on USB, once a time");
    }

    printf("%d checks, %d failed\n", checks, fails);
    if (!fails) {
        unlink("wr12_test.log");
        if (system("rm -rf test.d") != 0) return 1;
    }
    return fails != 0;
}
