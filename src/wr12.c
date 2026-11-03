/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the WR12, Xiegu's receiver for the X6200's HOST port (#134). It is a USB serial port (STM's 0483:5740,
 *  cdc_acm: /dev/ttyACM*) that takes text commands ending in \r\n and answers each with a line OK or ERROR (AT+HELP
 *  lists them; an R820T-type tuner inside). Set up with AT+INIT, its gains and AGCs, and AT+BYPASS=1: the antenna
 *  straight through to the radio, as it starts up. To receive, AT+FREQ=<Hz> and AT+BYPASS=0, the base meanwhile on
 *  the WR12's IF (radio.c); AT+AYT keeps the line alive. The commands and their order as ea4iln reported them in #134.
 *
 *  One thread has the port: the GUI says what it wants (wr12_want) and reads the state, and never waits for the WR12.
 *  Each answer goes into the log when it differs from the last one to that command, so a knob turned or the
 *  keepalive add nothing; the screen hears once that the WR12 is found, lost, or answered ERROR. Stopped with SIGTERM,
 *  the GUI puts it back in its bypass on its way out (on_term).
 *
 *  Copyright (c) 2026 RF.Guru
 */

#define _GNU_SOURCE
#include "wr12.h"

#ifdef BOARD_X6200

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Where it is looked for and what it leaves behind; the host test (tools/wr12) has its own */
#ifndef WR12_SYS
#define WR12_SYS    "/sys/class/tty"
#endif
#ifndef WR12_DEV
#define WR12_DEV    "/dev"
#endif
#ifndef WR12_GPSD
#define WR12_GPSD   "/var/run/gpsd.sock"    /* gpsd's control socket (/etc/default/gpsd) */
#endif
#ifndef WR12_MARK
#define WR12_MARK   "/tmp/wr12-out"         /* out of its bypass, for the GUI after this one: tmpfs, gone at a reboot */
#endif

#define WR12_VID    "0483"
#define WR12_PID    "5740"

#ifndef ANSWER_MS
#define ANSWER_MS   500     /* an answer within this, or none */
#endif
#ifndef AYT_MS
#define AYT_MS      1000    /* AT+AYT this long after the last command */
#endif
#ifndef RETRY_MS
#define RETRY_MS    2000    /* none found, or a set-up that failed: looked for again */
#endif
#ifndef GONE_MS
#define GONE_MS     3000    /* gone from USB this long out of its bypass: taken for unplugged, not reset */
#endif
#define BACK_MS     250     /* gone from USB out of its bypass: looked for again this often meanwhile */
#define STEP_MS     100     /* between the commands of the set-up */
#define DEFER_MS    50      /* the radio in TX: AT+BYPASS=0 tried again */
#define TERM_MS     20      /* SIGTERM: AT+BYPASS=1 this long on its way over USB before the GUI ends */
#define FAILS       3       /* commands unanswered in a row: not answering */
#define NOTES       4

typedef enum { ANS_OK, ANS_ERROR, ANS_NONE, ANS_GONE } answer_t;

static const char *const answer_words[] = {"OK", "ERROR", "no answer", "the port went"};

/* The set-up, in order; all of it answered OK or it is done again */
static const char *const setup[] = {
    "AT+INIT", "AT+LNAGAIN=15", "AT+VGAGAIN=0", "AT+MIXERGAIN=0", "AT+LNAAGCENA=1", "AT+MIXERAGCENA=1", "AT+BYPASS=1",
};

/* Under mux: what the GUI wants (wr12_want; gen counts the changes), out (the WR12 may be out of its bypass), the
 * lines for the screen */
static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond;
static bool            want_on, want_rx, out;
static int32_t         want_hz;
static unsigned        want_gen;
static char            notes[NOTES][64];
static int             n_notes;

static atomic_bool     ready;
static atomic_bool     notify_due;
static bool          (*notify_fn)(void);
static bool          (*keyed_fn)(void);

/* The port for on_term, -1 none open; dying: on_term under way, the thread sends nothing more */
static atomic_int      sig_fd = -1;
static atomic_bool     dying;

/* The thread's own: the port, the WR12 as its answers left it */
static int             fd = -1;
static char            port[32];        /* "ttyACM0" */
static int             bypass = -1;     /* 1 in its bypass, 0 out, -1 not known */
static int32_t         freq_sent;       /* the last AT+FREQ answered OK, 0 none */
static uint64_t        sent_ms;         /* the last command */
static int             fails;           /* unanswered in a row */
static bool            quiet_said;      /* "not answering" on the screen, until it answers or goes */
static int             quiet_sets;      /* set-ups unanswered since: the first one may come before it is up */
static bool            none_said;       /* "none on USB" in the log, until one is found or the switch goes off */
static int             open_err;        /* the errno of the last open that failed, logged once */
static uint64_t        gone_ms;         /* gone from USB out of its bypass since, 0: not */

/* The last answer logged for each command (AT+FREQ whatever its frequency), and whether its ERROR is on the screen yet */
static struct {
    char     name[20];
    answer_t last;
    bool     err_said;
} said[16];
static int n_said;

static uint64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};

    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/* The GUI told (notify_fn) that something changed; again from the loop while its queue is full */
static void tell(void) {
    atomic_store(&notify_due, notify_fn && !notify_fn());
}

static void note(const char *fmt, const char *arg) {
    pthread_mutex_lock(&mux);
    if (n_notes < NOTES) {
        snprintf(notes[n_notes++], sizeof(notes[0]), fmt, arg);
    }
    pthread_mutex_unlock(&mux);
    tell();
}

/* An answer into the log when it is not the last one this command had; an ERROR on the screen once, until an OK
 * (screen false: the log only) */
static void said_answer(const char *cmd, answer_t a, bool screen) {
    size_t len = strncmp(cmd, "AT+FREQ=", 8) == 0 ? 7 : strlen(cmd);
    int    i;

    for (i = 0; i < n_said && (strlen(said[i].name) != len || strncmp(said[i].name, cmd, len) != 0); i++) {
    }
    if (i == n_said) {
        if (n_said == (int)(sizeof(said) / sizeof(said[0]))) {
            return;
        }
        snprintf(said[n_said].name, sizeof(said[0].name), "%.*s", (int)len, cmd);
        said[n_said].last = (answer_t)-1;
        said[n_said++].err_said = false;
    }
    if (said[i].last != a) {
        said[i].last = a;
        fprintf(stderr, "wr12: %s: %s\n", cmd, answer_words[a]);
    }
    if (a == ANS_OK) {
        said[i].err_said = false;
    } else if (a == ANS_ERROR && screen && !said[i].err_said) {
        said[i].err_said = true;
        note("WR12: ERROR to %s", cmd);
    }
}

/* A sysfs attribute of the tty's USB device is `want` */
static bool attr_is(const char *name, const char *attr, const char *want) {
    char  path[160], v[16] = "";
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), WR12_SYS "/%s/device/../%s", name, attr);
    if (!(f = fopen(path, "r"))) {
        return false;
    }
    ok = fgets(v, sizeof(v), f) && strncmp(v, want, 4) == 0 && (v[4] == '\n' || v[4] == '\0');
    fclose(f);
    return ok;
}

/* name ("ttyACM0") is a WR12: its USB device's id, not its name, which another serial port can take */
static bool is_wr12(const char *name) {
    return attr_is(name, "idVendor", WR12_VID) && attr_is(name, "idProduct", WR12_PID);
}

/* The first WR12 among the ttyACM ports, its name into name; false: none plugged in */
static bool find(char *name, size_t size) {
    glob_t g;
    bool   found = false;

    if (glob(WR12_SYS "/ttyACM*", 0, NULL, &g) != 0) {
        return false;
    }
    for (size_t i = 0; i < g.gl_pathc && !found; i++) {
        const char *n = strrchr(g.gl_pathv[i], '/') + 1;

        if (is_wr12(n)) {
            snprintf(name, size, "%s", n);
            found = true;
        }
    }
    globfree(&g);
    return found;
}

/* gpsd told to let go of the port should it have it (one started before S50gpsd left the WR12 out, a gpsdctl add by
 * hand): two programs on one port each get part of what it says. Without gpsd, nothing */
static void gpsd_release(const char *name) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    char               msg[64], ans[16] = "";
    int                s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int                n;

    if (s < 0) {
        return;
    }
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", WR12_GPSD);
    n = snprintf(msg, sizeof(msg), "-" WR12_DEV "/%s\r\n", name);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 && write(s, msg, n) == n) {
        struct pollfd p = {s, POLLIN, 0};
        ssize_t       k;

        if (poll(&p, 1, 200) > 0 && (k = read(s, ans, sizeof(ans) - 1)) > 0) {
            ans[k] = 0;
            ans[strcspn(ans, "\r\n")] = 0;
        }
        said_answer("gpsd, let go", strcmp(ans, "OK") == 0 ? ANS_OK : ANS_ERROR, false);   /* ERROR: it had none */
    }
    close(s);
}

static bool port_open(const char *name) {
    char           path[64];
    struct termios t;

    snprintf(path, sizeof(path), WR12_DEV "/%s", name);
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno != open_err) {
            open_err = errno;
            fprintf(stderr, "wr12: %s: %s\n", path, strerror(errno));
        }
        return false;
    }
    open_err = 0;
    /* 115200 8N1, no flow control, raw. VMIN 1: a read with nothing there is EAGAIN, 0 only once it is unplugged */
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(CSTOPB | CRTSCTS);
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    tcflush(fd, TCIOFLUSH);
    snprintf(port, sizeof(port), "%s", name);
    atomic_store(&sig_fd, fd);
    return true;
}

/* on_term under way: this thread neither sends nor lets the port go any more, until the GUI ends */
static void stay_if_dying(void) {
    while (atomic_load(&dying)) {
        pause();
    }
}

static void port_close(void) {
    stay_if_dying();
    atomic_store(&sig_fd, -1);
    close(fd);
    fd = -1;
}

/* What waits on the port, dropped: an answer too late for its command is not the next one's. False: it went */
static bool drain(void) {
    char    buf[256];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
    }
    return n < 0 && (errno == EAGAIN || errno == EINTR);
}

/* One command, and its answer: the first line with ERROR or OK in it (an echo before it is passed over), a line ending
 * in \r, \n or both */
static answer_t command(const char *cmd) {
    char     out[64], line[128];
    size_t   len = 0;
    int      n = snprintf(out, sizeof(out), "%s\r\n", cmd);
    uint64_t end;
    answer_t a = ANS_NONE;

    stay_if_dying();
    sent_ms = now_ms();
    end = sent_ms + ANSWER_MS;
    if (!drain() || write(fd, out, n) != n) {
        a = ANS_GONE;
    }
    while (a == ANS_NONE) {
        int64_t       left = (int64_t)(end - now_ms());
        struct pollfd p = {fd, POLLIN, 0};
        char          buf[64];
        ssize_t       k;
        int           r;

        if (left <= 0) {
            break;
        }
        r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r == 0) {
            break;
        }
        if (r < 0 || !(p.revents & POLLIN)) {
            a = ANS_GONE;   /* hung up: unplugged */
            break;
        }
        k = read(fd, buf, sizeof(buf));
        if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
            a = ANS_GONE;
            break;
        }
        for (ssize_t i = 0; i < k && a == ANS_NONE; i++) {
            if (buf[i] != '\n' && buf[i] != '\r') {
                if (len < sizeof(line) - 1) {
                    line[len++] = buf[i];
                }
                continue;
            }
            line[len] = 0;
            len = 0;
            a = strstr(line, "ERROR") ? ANS_ERROR : strstr(line, "OK") ? ANS_OK : ANS_NONE;
        }
    }
    fails = a == ANS_NONE ? fails + 1 : 0;
    said_answer(cmd, a, true);
    return a;
}

/* Marked in /tmp (in memory) while it may be out of its bypass: a GUI started after this one stopped knows */
static void mark(bool on) {
    if (on) {
        FILE *f = fopen(WR12_MARK, "w");

        if (f) {
            fclose(f);
        }
    } else {
        unlink(WR12_MARK);
    }
}

static void set_out(bool o) {
    bool was;

    pthread_mutex_lock(&mux);
    was = out;
    out = o;
    pthread_mutex_unlock(&mux);
    if (o != was) {
        mark(o);
        tell();
    }
}

/* Out of its bypass only while the GUI still wants it, decided with out set under mux: a wr12_want that takes rx back
 * after this finds out set, and TX stays held until the bypass is seen again */
static bool go_out(void) {
    bool go;

    pthread_mutex_lock(&mux);
    go = want_on && want_rx;
    if (go) {
        out = true;
    }
    pthread_mutex_unlock(&mux);
    if (go) {
        mark(true);
        tell();
    }
    return go;
}

/* The port let go. Unplugged (its USB id gone): out goes once it has stayed away GONE_MS (run); back before that it
 * was only reset on USB, still powered and maybe out of its bypass, and is set up again first. Still there: what it
 * was asked last may still hold, out stays, and TX with it. quiet: it stopped answering, said once on the screen */
static void lose(bool quiet) {
    bool gone = !is_wr12(port);
    bool was  = atomic_exchange(&ready, false);

    port_close();
    bypass    = -1;
    freq_sent = 0;
    fails     = 0;
    if (gone || was) {      /* a set-up that failed again is in the log by its answer */
        fprintf(stderr, "wr12: %s %s\n", port, gone ? "unplugged" : quiet ? "not answering, let go" : "let go");
    }
    if (gone) {
        n_said     = 0;     /* plugged in again: its answers logged afresh */
        quiet_said = false;
        quiet_sets = 0;
        if (was) {
            note("%s", "WR12 unplugged");
        }
    } else if (quiet && !quiet_said && (was || ++quiet_sets >= 2)) {
        quiet_said = true;
        note("%s", "WR12 not answering");
    }
    tell();
}

/* Found: set up, in its bypass (said on the screen with WR12 on HOST on). False: not all of it answered OK, the port
 * let go */
static bool start(const char *name, bool on) {
    answer_t a = ANS_OK;

    gpsd_release(name);
    if (!port_open(name)) {
        return false;
    }
    bypass    = -1;
    freq_sent = 0;
    fails     = 0;
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]) && a == ANS_OK; i++) {
        if (i) {
            sleep_ms(STEP_MS);
        }
        a = command(setup[i]);
    }
    if (a != ANS_OK) {
        lose(a != ANS_ERROR);   /* an ERROR was said already */
        return false;
    }
    bypass     = 1;
    quiet_said = false;
    quiet_sets = 0;
    none_said  = false;
    set_out(false);
    atomic_store(&ready, true);
    fprintf(stderr, "wr12: found on " WR12_DEV "/%s, set up, in its bypass\n", port);
    if (on) {
        note("%s", "WR12 found on HOST");
    }
    tell();
    return true;
}

/* An answer that is none, or the port gone: let go once that happens FAILS times in a row, or at once */
static bool answered(answer_t a) {
    if (a == ANS_GONE || fails >= FAILS) {
        lose(a != ANS_GONE);
        return false;
    }
    return true;
}

static void *run(void *arg) {
    unsigned gen = 0;
    unsigned wait = AYT_MS;
    uint64_t retry_at = 0;

    (void)arg;
    for (;;) {
        bool     on, rx, o;
        int32_t  hz;
        uint64_t now;

        pthread_mutex_lock(&mux);
        if (want_gen == gen) {
            struct timespec t;

            clock_gettime(CLOCK_MONOTONIC, &t);
            t.tv_sec += wait / 1000;
            t.tv_nsec += (long)(wait % 1000) * 1000000;
            if (t.tv_nsec >= 1000000000) {
                t.tv_sec++;
                t.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&cond, &mux, &t);
        }
        gen = want_gen;
        on  = want_on;
        rx  = want_rx;
        hz  = want_hz;
        o   = out;
        pthread_mutex_unlock(&mux);
        if (atomic_load(&notify_due)) {
            tell();
        }
        now  = now_ms();
        wait = atomic_load(&notify_due) ? 200 : AYT_MS;

        /* None open: looked for while WR12 on HOST is on, or while one may still be out of its bypass */
        if (fd < 0) {
            char name[32];

            if (!on && !o) {
                none_said = false;
                wait      = 5000;
                continue;
            }
            if (now < retry_at) {
                wait = (unsigned)(retry_at - now);
                continue;
            }
            retry_at = now + RETRY_MS;
            if (!find(name, sizeof(name))) {
                if (!none_said) {
                    fprintf(stderr, "wr12: none on USB (%s:%s)\n", WR12_VID, WR12_PID);
                    none_said = true;
                }
                quiet_said = false;
                if (o && !gone_ms) {
                    gone_ms = now;
                }
                if (o && now - gone_ms < GONE_MS) {
                    retry_at = now + BACK_MS;   /* back soon if it was only reset on USB */
                    wait     = BACK_MS;
                    continue;
                }
                if (o) {
                    fprintf(stderr, "wr12: gone from USB out of its bypass for %u ms: taken for unplugged, TX free\n",
                            (unsigned)(now - gone_ms));
                }
                gone_ms = 0;
                set_out(false);     /* unplugged: none to bring back */
                continue;
            }
            gone_ms = 0;
            if (!start(name, on)) {
                continue;
            }
            retry_at = 0;
            wait     = 0;       /* what is wanted now, looked at afresh */
            continue;
        }
        if (!is_wr12(port)) {
            lose(false);    /* unplugged between two commands */
            continue;
        }

        /* Out of its bypass on hz where the GUI wants it (and the radio is not in TX), else in it */
        if (on && rx) {
            if (hz != freq_sent) {
                char     cmd[32];
                answer_t a;

                snprintf(cmd, sizeof(cmd), "AT+FREQ=%d", (int)hz);
                a = command(cmd);
                if (!answered(a)) {
                    continue;
                }
                if (a == ANS_NONE) {
                    wait = STEP_MS;     /* again, after a breath */
                    continue;
                }
                freq_sent = hz;         /* an ERROR too: said, not asked again until the dial moves */
            }
            if (bypass != 0) {
                if (keyed_fn && keyed_fn()) {
                    wait = DEFER_MS;
                    continue;
                }
                if (go_out()) {
                    answer_t a = command("AT+BYPASS=0");

                    if (!answered(a)) {
                        continue;
                    }
                    bypass = a == ANS_OK ? 0 : -1;
                    if (a != ANS_OK) {
                        wait = AYT_MS;  /* not answered: tried again at the keepalive's pace */
                    }
                }
            }
        } else if (bypass != 1 || o) {
            answer_t a = command("AT+BYPASS=1");

            if (!answered(a)) {
                continue;
            }
            if (a == ANS_OK) {
                bypass = 1;
                set_out(false);
            } else {
                bypass = -1;
                continue;       /* again at the keepalive's pace, TX held meanwhile */
            }
        }

        /* WR12 on HOST off: in its bypass, the port let go */
        if (!on) {
            pthread_mutex_lock(&mux);
            o = out;
            pthread_mutex_unlock(&mux);
            if (!o && bypass == 1) {
                port_close();
                atomic_store(&ready, false);
                fprintf(stderr, "wr12: WR12 on HOST off, %s let go\n", port);
                tell();
                continue;
            }
        }

        /* The keepalive, once nothing else has gone for a while */
        now = now_ms();
        if (now - sent_ms >= AYT_MS) {
            answered(command("AT+AYT"));
        } else {
            wait = (unsigned)(AYT_MS - (now - sent_ms));
        }
    }
    return NULL;
}

/* SIGTERM, which S95gui stops the GUI with (a restart, an update or its rollback, a shutdown): the WR12 back in its
 * bypass on the GUI's way out. The GUI after this one may be a version that knows no WR12, and would transmit into
 * it. The thread stopped first (dying), a command of its own on its way let go, then AT+BYPASS=1 and TERM_MS for it
 * to leave over USB; its answer is not waited for: the mark stays, and the next GUI that knows the WR12 holds TX
 * until it has seen it. Then the GUI ends as SIGTERM ends it (exit code 143 in x6100_daemon). Only calls a signal
 * handler may make */
static void on_term(int sig) {
    static const char cmd[] = "AT+BYPASS=1\r\n";
    int               f;

    atomic_store(&dying, true);
    poll(NULL, 0, 5);
    f = atomic_load(&sig_fd);
    if (f >= 0 && write(f, cmd, sizeof(cmd) - 1) > 0) {
        poll(NULL, 0, TERM_MS);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void wr12_init(bool (*notify)(void), bool (*keyed)(void)) {
    pthread_condattr_t a;
    pthread_t          t;
    struct sigaction   sa = {.sa_handler = on_term, .sa_flags = SA_RESTART};

    notify_fn = notify;
    keyed_fn  = keyed;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&cond, &a);
#ifndef HOST_BUILD
    /* A GUI before this one took it out of its bypass and stopped (an update, a crash): TX held until it is seen
     * back in it, or found unplugged. Not on a PC, which has no HOST port */
    out = access(WR12_MARK, F_OK) == 0;
    if (out) {
        fprintf(stderr, "wr12: left out of its bypass by the GUI before: TX held until it is back in it\n");
    }
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    pthread_create(&t, NULL, run, NULL);
    pthread_detach(t);
#else
    (void)t;
    (void)sa;
#endif
}

void wr12_want(bool on, bool rx, int32_t hz) {
    pthread_mutex_lock(&mux);
    if (on != want_on || rx != want_rx || (rx && hz != want_hz)) {
        want_on = on;
        want_rx = rx;
        want_hz = rx ? hz : want_hz;
        want_gen++;
        pthread_cond_signal(&cond);
    }
    pthread_mutex_unlock(&mux);
}

bool wr12_ready(void) {
    return atomic_load(&ready);
}

const char *wr12_tx_block(void) {
    const char *why;

    pthread_mutex_lock(&mux);
    why = want_rx ? "WR12 receiving" : out ? "WR12 not in bypass" : NULL;
    pthread_mutex_unlock(&mux);
    return why;
}

bool wr12_note(char *buf, size_t size) {
    bool got;

    pthread_mutex_lock(&mux);
    got = n_notes > 0;
    if (got) {
        snprintf(buf, size, "%s", notes[0]);
        memmove(notes[0], notes[1], sizeof(notes[0]) * --n_notes);
    }
    pthread_mutex_unlock(&mux);
    return got;
}

bool wr12_port(const char *dev) {
    const char *n = strrchr(dev, '/');
    bool        on;

    pthread_mutex_lock(&mux);
    on = want_on || out;
    pthread_mutex_unlock(&mux);
    return on && n && strncmp(n + 1, "ttyACM", 6) == 0 && is_wr12(n + 1);
}

#endif
