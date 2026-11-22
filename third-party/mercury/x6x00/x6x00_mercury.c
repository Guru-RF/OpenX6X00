/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  OpenX6X00 - x6x00-mercury: Mercury, the HF modem, with the radio's GUI as its sound card and PTT
 *
 *  Mercury's daemon build (host/mercury_host.h) in a client of the GUI's Winlink modem socket
 *  (src/winlink.c), protocol version 2. The hello asks for 8000 Hz and keys of KEY_S at most; the GUI's R
 *  messages, the receiver in 20 ms blocks, go into Mercury; while Mercury keys, its audio goes back as T
 *  messages between a P 1 and a P 0, and the GUI keys the radio with the guards it has for ardopcf. Pat
 *  talks to Mercury on 127.0.0.1:8300 and 8301 (its varahf transport) and tunes the radio through the GUI's
 *  rigctld.
 *
 *    x6x00-mercury [SOCK:<path>] [Mercury's options, e.g. -C /run/winlink/mercury.ini]
 *
 *  <path> is the GUI's socket, /run/ardop.sock if not given. The rest is Mercury's command line, with -W
 *  (no spectrum: nothing shows it here) always added. The log is Mercury's, on stderr. SIGTERM or SIGINT
 *  stop it: at once with no session up; with one, after Mercury has ended it on the air (its DISCONNECT, 42 s
 *  at most), or at once at a second SIGTERM or SIGINT.
 *
 *  One thread owns the socket and Mercury's audio, so a key's messages go out in order: P 1, its T, P 0.
 *  Unkeyed, each R is one exchange with Mercury. Keyed, the GUI sends no R once the radio is on air, so a
 *  clock of our own takes Mercury's audio at 8 kHz of wall time, LEAD ahead, as ardopcf sends its frames;
 *  Mercury times the frame and unkeys on its own clock, and P 0 lets the GUI play out what it holds. The
 *  thread connects, and connects again once a second while the GUI is not there; Mercury runs on meanwhile,
 *  its keys going nowhere (the GUI cuts a transmission whose client goes).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#define _GNU_SOURCE     /* pipe2 */

#include "mercury_host.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PROTO       2           /* the GUI's socket protocol */
#define RATE        8000        /* Mercury's audio in the daemon build */
#define KEY_S       40          /* the longest key: a carousel round is up to 30 s of air, then its tail */
#define BLOCK       160         /* the clock's step while keyed: 20 ms */
#define LEAD        800         /* TX audio sent ahead of the wall clock, 100 ms: the GUI's player never waits */
#define T_MAX       1200        /* samples in a message at most */
#define SEND_MS     500         /* a message the GUI does not take this long: the connection is given up */
#define ARGS_MAX    64
#define GOODBYE_S   42          /* a session ended on the air at a stop: Mercury's cap for it is 40 s (ENGINE_GOODBYE_CAP_MS) */
#define TAIL_MS     300         /* after its last key, for the GUI to play what it holds before the modem goes */

typedef struct {
    uint8_t  type;
    uint8_t  arg;
    uint16_t n;                 /* samples in s */
    int16_t  s[T_MAX];
} msg_t;

#define MSG_HDR     offsetof(msg_t, s)

/* Set when Mercury stops by itself, a port it cannot open for one (common/mercury_engine.h: its own main()
 * polls it; it is not in host/mercury_host.h) */
extern _Atomic bool mercury_stop;
/* The end of a session on the air before Mercury stops (1.9.25; gui_interface/fyne-ui/engine/mercury_bridge.h,
 * common/mercury_engine.c), and whether there is one to end (datalink_arq/arq.h: 0 none, also while only calling
 * or answering a call; 1 up; 2 its DISCONNECT sent). Neither is in host/mercury_host.h */
void mercury_say_goodbye(void);
int  arq_goodbye_progress(void);

static atomic_bool        bye_done;             /* the goodbye returned */

static struct sockaddr_un gui     = {.sun_family = AF_UNIX};
static int                fd      = -1;         /* the GUI's socket: the audio thread's */
static bool               told;                 /* this connection had the P 1 of the key on: its T and P 0 go */
static int                wake[2] = {-1, -1};   /* key() and the end wake the audio thread */
static atomic_bool        quit;
static atomic_bool        key_want;             /* Mercury's last key() */
static atomic_uint        key_seq;              /* its key(true)s: one that ended and began again between two looks */

static uint64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static void poke(void) {
    char c = 0;

    if (write(wake[1], &c, 1) < 0) {
        /* full: a wake is waiting already */
    }
}

/* ---- Mercury's radio ---------------------------------------------------------------- */

/* Mercury's TX thread: at the start of a transmission, and at its end once the frame and its tail are out by
 * Mercury's clock. The audio thread tells the GUI */
static int key(bool tx, void *user) {
    (void)user;
    if (tx)
        atomic_fetch_add(&key_seq, 1);
    atomic_store(&key_want, tx);
    poke();
    return 0;
}

/* For Mercury's UI only, which has none here */
static bool get_frequency(uint64_t *hz, void *user) {
    (void)hz;
    (void)user;
    return false;
}

/* ---- The GUI's socket, on the audio thread ------------------------------------------ */

static void gui_close(const char *why) {
    if (fd < 0)
        return;
    fprintf(stderr, "x6x00-mercury: the GUI's socket closed: %s\n", why);
    close(fd);
    fd   = -1;
    told = false;   /* the rest of a key goes nowhere, not to the next connection */
}

/* Waits up to SEND_MS for room: an R is never sent while the GUI waits for T, so it is not stuck on us */
static bool gui_send(uint8_t type, uint8_t arg, const int16_t *s, size_t n) {
    msg_t m = {.type = type, .arg = arg, .n = (uint16_t)n};

    if (fd < 0)
        return false;
    if (n)
        memcpy(m.s, s, n * sizeof(int16_t));
    if (send(fd, &m, MSG_HDR + n * sizeof(int16_t), MSG_NOSIGNAL) < 0) {
        gui_close(errno == EAGAIN ? "the GUI took nothing for 0.5 s" : strerror(errno));
        return false;
    }
    return true;
}

static void gui_connect(void) {
    static bool    said;
    struct timeval tv    = {.tv_sec = 0, .tv_usec = SEND_MS * 1000};
    int16_t        hello[2] = {RATE, KEY_S};
    int            s     = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (s < 0)
        return;
    if (connect(s, (struct sockaddr *)&gui, sizeof(gui)) < 0) {
        if (!said)
            fprintf(stderr, "x6x00-mercury: %s is not there yet, trying again every second\n", gui.sun_path);
        said = true;
        close(s);
        return;
    }
    said = false;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    fd = s;
    if (gui_send('H', PROTO, hello, 2))
        fprintf(stderr, "x6x00-mercury: connected to %s\n", gui.sun_path);
}

/* The GUI's messages that came: each R one exchange with Mercury while unkeyed */
static void gui_read(bool keyed) {
    msg_t   m;
    int16_t tx[T_MAX];

    for (int i = 0; i < 64 && fd >= 0; i++) {
        ssize_t len = recv(fd, &m, sizeof(m), MSG_DONTWAIT);

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (len <= 0) {
            gui_close(len == 0 ? "the GUI went away" : strerror(errno));
            return;
        }
        if (m.type != 'R' || len < (ssize_t)MSG_HDR || len != (ssize_t)(MSG_HDR + m.n * sizeof(int16_t)))
            continue;   /* not the receiver, or cut */
        if (keyed)
            continue;   /* the receiver before the radio went on air: Mercury sends, the clock feeds it */
        mercury_host_audio_exchange(m.s, tx, m.n);  /* unkeyed: its TX side is silence */
    }
}

static void *audio_thread(void *arg) {
    int16_t  tx[BLOCK];
    bool     on       = false;  /* the GUI was sent P 1 and no P 0 since */
    unsigned seq      = 0;      /* the key(true) of that P 1 */
    uint64_t key_ms   = 0;      /* when it went */
    uint64_t sent     = 0;      /* samples of Mercury's taken since */
    uint64_t next_try = 0;
    (void)arg;

    while (!atomic_load(&quit)) {
        uint64_t now  = now_ms();
        bool     want = atomic_load(&key_want);
        unsigned s    = atomic_load(&key_seq);
        int      timeout;

        if (fd < 0 && now >= next_try) {
            next_try = now + 1000;
            gui_connect();
        }
        /* The key ended, or ended and began again since the last look: the GUI hears each one */
        if (on && (!want || s != seq)) {
            if (told)
                gui_send('P', 0, NULL, 0);
            on = told = false;
        }
        if (!on && want) {
            on     = true;
            seq    = s;
            key_ms = now;
            sent   = 0;
            told   = gui_send('P', 1, NULL, 0);
        }
        /* Keyed: Mercury's audio up to the wall clock and LEAD ahead of it. Without the GUI it goes nowhere,
         * but Mercury's TX drains only this way */
        timeout = fd < 0 ? 1000 : -1;
        if (on) {
            uint64_t due = (now - key_ms) * (RATE / 1000) + LEAD;

            while (sent < due) {
                mercury_host_audio_exchange(NULL, tx, BLOCK);
                if (told)
                    gui_send('T', 0, tx, BLOCK);
                sent += BLOCK;
            }
            timeout = (int)((sent - due) / (RATE / 1000)) + 1;  /* the next block's time */
        }

        struct pollfd pfd[2] = {{.fd = wake[0], .events = POLLIN}, {.fd = fd, .events = POLLIN}};

        if (poll(pfd, fd >= 0 ? 2 : 1, timeout) < 0) {
            if (errno != EINTR)
                usleep(10000);
            continue;
        }
        if (pfd[0].revents) {
            char c[16];

            while (read(wake[0], c, sizeof(c)) > 0)
                ;
        }
        if (fd >= 0 && pfd[1].revents)
            gui_read(on);
    }
    if (told)
        gui_send('P', 0, NULL, 0);
    return NULL;
}

/* ---- Main --------------------------------------------------------------------------- */

/* Mercury's goodbye: returns once the session's DISCONNECT is off the air or the session ended, at its own cap
 * (40 s), or within 50 ms of mercury_stop */
static void *goodbye(void *arg) {
    (void)arg;
    mercury_say_goodbye();
    atomic_store(&bye_done, true);
    return NULL;
}

int main(int argc, char **argv) {
    const char            *path = "/run/ardop.sock";
    char                  *margv[ARGS_MAX + 3];
    int                    margc = 0;
    sigset_t               stop;
    struct timespec        half = {.tv_sec = 0, .tv_nsec = 500000000L};
    pthread_t              th;
    mercury_host_radio_ops ops  = {.key = key, .get_frequency = get_frequency, .user = NULL};

    if (argc > 1 && !strncmp(argv[1], "SOCK:", 5)) {
        path = argv[1] + 5;
        argv++;
        argc--;
    }
    if (!path[0] || strlen(path) >= sizeof(gui.sun_path) || argc > ARGS_MAX) {
        fprintf(stderr, "usage: x6x00-mercury [SOCK:<path>] [Mercury's options]\n");
        return 2;
    }
    memcpy(gui.sun_path, path, strlen(path) + 1);
    margv[margc++] = argv[0];
    margv[margc++] = (char *)"-W";
    for (int i = 1; i < argc; i++)
        margv[margc++] = argv[i];
    margv[margc] = NULL;

    /* SIGTERM and SIGINT for this thread's sigtimedwait(): blocked before any thread, Mercury's too, is made.
     * A write to a client that left is an error, not the end */
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("x6x00-mercury: pipe");
        return 1;
    }

    mercury_host_set_radio(&ops);
    if (mercury_init(margc, margv, NULL, NULL) != 0) {
        fprintf(stderr, "x6x00-mercury: Mercury did not start\n");
        return 1;
    }
    if (pthread_create(&th, NULL, audio_thread, NULL) != 0) {
        fprintf(stderr, "x6x00-mercury: no audio thread\n");
        mercury_request_shutdown();
        mercury_shutdown();
        return 1;
    }
    while (!mercury_stop && sigtimedwait(&stop, NULL, &half) < 0)
        ;
    fprintf(stderr, "x6x00-mercury: stopping\n");

    /* A session that is up is ended on the air first (Mercury 1.9.25's goodbye: one DISCONNECT, in the peer's slot
     * or out of turn), while the audio thread still carries its key: the other station is let go at once, not by
     * its watchdog 240 s later. On a thread of its own, so this one sees a second SIGTERM or SIGINT, which ends
     * it at once (the key cut where it is), and the time: GOODBYE_S is Mercury's own cap and some to spare.
     * SIGALRM, whose default ends the process, is behind both, and whatever ends the process closes the socket,
     * at which the GUI unkeys */
    if (!mercury_stop && arq_goodbye_progress() != 0) {
        pthread_t       bye;
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 50000000L};
        uint64_t        t0   = now_ms();
        const char     *how  = "over";

        fprintf(stderr, "x6x00-mercury: a session is up, ending it on the air first (%d s at most)\n", GOODBYE_S);
        alarm(GOODBYE_S + 5);
        if (pthread_create(&bye, NULL, goodbye, NULL) == 0) {
            while (!atomic_load(&bye_done)) {
                if (sigtimedwait(&stop, NULL, &tick) >= 0) {
                    how = "cut short by a second stop";
                    break;
                }
                if (now_ms() - t0 >= GOODBYE_S * 1000ULL) {
                    how = "given up";
                    break;
                }
            }
            if (!atomic_load(&bye_done))
                mercury_stop = true;        /* its wait ends within 50 ms */
            pthread_join(bye, NULL);
            /* Mercury unkeyed by its clock; the GUI still plays what it was sent ahead (LEAD, and its own 100 ms),
             * and cuts that when the modem goes */
            if (!mercury_stop)
                usleep(TAIL_MS * 1000);
        }
        fprintf(stderr, "x6x00-mercury: the goodbye is %s, %.1f s\n", how, (now_ms() - t0) / 1000.0);
    }

    /* The exchange ends before Mercury does (host/mercury_host.h): the audio thread's P 0 unkeys what is still
     * keyed. A stop that hangs ends at SIGALRM, as Mercury's own main() has it. mercury_request_shutdown() would
     * begin a goodbye of its own (1.9.25), which nothing carries any more: Mercury is told it stops already */
    atomic_store(&quit, true);
    poke();
    pthread_join(th, NULL);
    alarm(10);
    mercury_stop = true;
    mercury_request_shutdown();
    mercury_shutdown();
    return 0;
}
