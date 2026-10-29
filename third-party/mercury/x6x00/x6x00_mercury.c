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
 *  stop it.
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

/* The Fyne bridge's Android hooks (gui_interface/fyne-ui/engine/mercury_bridge.c), which upstream's daemon
 * build does not have: never called here */
void radio_io_set_usb_fd(const char *device, int dev_fd) {
    (void)device;
    (void)dev_fd;
}

void audioio_set_android_devices(const char *list) {
    (void)list;
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

    /* The exchange ends before Mercury does (host/mercury_host.h). A stop that hangs ends at SIGALRM, as
     * Mercury's own main() has it */
    atomic_store(&quit, true);
    poke();
    pthread_join(th, NULL);
    alarm(10);
    mercury_request_shutdown();
    mercury_shutdown();
    return 0;
}
