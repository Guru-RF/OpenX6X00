/*
 * btcat - Bluetooth SPP bridge for X6100 CAT (CI-V)
 *
 * - opens the real CAT UART (/dev/ttyS2, USB-CAT)
 * - creates a pty and bind-mounts its slave over /dev/ttyS2, so x6100_gui
 *   (unmodified) talks to btcat instead of the UART
 * - listens on RFCOMM channel 1 (SPP)
 * - multiplexes USB + BT clients: one request at a time to the GUI,
 *   replies routed to the requester, transceive (dst=0x00) broadcast to all
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* minimal bluetooth defs (no libbluetooth needed) */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_RFCOMM 3
typedef struct { uint8_t b[6]; } __attribute__((packed)) bdaddr_t;
struct sockaddr_rc { sa_family_t rc_family; bdaddr_t rc_bdaddr; uint8_t rc_channel; };
#define SOL_BLUETOOTH      274
#define BT_SECURITY        4
#define BT_SECURITY_MEDIUM 2
struct bt_security { uint8_t level; uint8_t key_size; };

#define UART_DEFAULT "/dev/ttyS2"
static const char *UART_PATH = UART_DEFAULT;
#define RF_CHANNEL 1
#define RADIO_ADDR 0xA4
#define MAX_EP     4            /* 0 = USB, 1..3 = BT clients */
#define BUF_SZ     2048
#define REQ_TMO_MS 300
#define CLIENTS_FILE "/tmp/btcat.clients"

typedef struct {
    int     fd;
    int     is_bt;
    uint8_t buf[BUF_SZ];
    size_t  len;
} ep_t;

static ep_t ep[MAX_EP];
static int  gui_fd = -1, slave_fd = -1, rf_fd = -1;
static uint8_t gbuf[BUF_SZ];
static size_t  glen;
static int  pending = -1;       /* endpoint awaiting reply */
static long pend_ts;
static int  rr;                 /* round-robin start */

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void set_raw(int fd, speed_t sp) {
    struct termios a;
    if (tcgetattr(fd, &a) < 0) return;
    cfmakeraw(&a);
    if (sp) { cfsetispeed(&a, sp); cfsetospeed(&a, sp); }
    tcsetattr(fd, TCSANOW, &a);
}

static void write_all(int fd, const uint8_t *p, size_t n) {
    long t0 = now_ms();
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) { p += w; n -= w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) return;
        if (now_ms() - t0 > 200) return;          /* peer stuck: drop */
        struct pollfd pf = { fd, POLLOUT, 0 };
        poll(&pf, 1, 50);
    }
}

/* number of BT clients, for the GUI status icon */
static void write_clients(void) {
    int n = 0;
    for (int i = 1; i < MAX_EP; i++) if (ep[i].fd >= 0) n++;
    FILE *f = fopen(CLIENTS_FILE, "w");
    if (f) { fprintf(f, "%d\n", n); fclose(f); }
}

static void ep_close(int i) {
    int was_bt = ep[i].is_bt && ep[i].fd >= 0;
    if (was_bt) {
        close(ep[i].fd);
        fprintf(stderr, "btcat: BT client %d disconnected\n", i);
    }
    ep[i].fd = -1; ep[i].len = 0;
    if (pending == i) pending = -1;
    if (was_bt) write_clients();
}

/* extract one CI-V frame (FE FE ... FD) from buffer; returns length or 0 */
static size_t take_frame(uint8_t *b, size_t *len, uint8_t *out) {
    size_t s = 0;
    while (s + 1 < *len && !(b[s] == 0xFE && b[s + 1] == 0xFE)) s++;
    if (s) {                                   /* drop garbage */
        memmove(b, b + s, *len - s);
        *len -= s;
    }
    if (*len < 5) return 0;
    size_t st = 2;
    while (st < *len && b[st] == 0xFE) st++;   /* extra preambles */
    for (size_t i = st; i < *len; i++) {
        if (b[i] == 0xFD) {
            size_t fl = i + 1 - (st - 2);
            memcpy(out, b + st - 2, fl);
            memmove(b, b + i + 1, *len - i - 1);
            *len -= i + 1;
            return fl;
        }
    }
    if (*len == BUF_SZ) *len = 0;              /* overflow: reset */
    return 0;
}

static void from_gui(void) {
    uint8_t f[BUF_SZ];
    size_t n;
    while ((n = take_frame(gbuf, &glen, f))) {
        uint8_t dst = f[2], src = f[3];
        if (dst == 0x00) {                     /* transceive broadcast */
            for (int i = 0; i < MAX_EP; i++)
                if (ep[i].fd >= 0) write_all(ep[i].fd, f, n);
            continue;
        }
        int to = pending >= 0 ? pending : 0;
        if (ep[to].fd >= 0) write_all(ep[to].fd, f, n);
        if (src == RADIO_ADDR) pending = -1;   /* reply done */
    }
}

static void dispatch(void) {
    if (pending >= 0 && now_ms() - pend_ts > REQ_TMO_MS) pending = -1;
    if (pending >= 0) return;
    for (int k = 0; k < MAX_EP; k++) {
        int i = (rr + k) % MAX_EP;
        if (ep[i].fd < 0) continue;
        uint8_t f[BUF_SZ];
        size_t n = take_frame(ep[i].buf, &ep[i].len, f);
        if (n) {
            write_all(gui_fd, f, n);
            pending = i; pend_ts = now_ms();
            rr = (i + 1) % MAX_EP;
            return;
        }
    }
}

static int rf_listen(void) {
    int s = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if (s < 0) return -1;
    struct sockaddr_rc a;
    memset(&a, 0, sizeof(a));
    a.rc_family = AF_BLUETOOTH;
    a.rc_channel = RF_CHANNEL;
    /* Paired (bonded) devices only: CAT can key the transmitter, and the
     * kernel default lets anyone in range connect without pairing.
     * From SP2KMO's fix in openx6100 (#292, PR #297). No listen without it. */
    struct bt_security sec = { .level = BT_SECURITY_MEDIUM };
    if (setsockopt(s, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0 ||
        bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 1) < 0) {
        close(s);
        return -1;
    }
    fprintf(stderr, "btcat: RFCOMM listening on channel %d\n", RF_CHANNEL);
    return s;
}

static int setup_pty(void) {
    /* undo stale bind mounts from a previous run */
    while (umount2(UART_PATH, MNT_DETACH) == 0) {}

    int u = open(UART_PATH, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (u < 0) { perror("btcat: open uart"); return -1; }
    set_raw(u, B19200);
    ep[0].fd = u; ep[0].is_bt = 0;

    gui_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (gui_fd < 0 || grantpt(gui_fd) < 0 || unlockpt(gui_fd) < 0) {
        perror("btcat: pty"); return -1;
    }
    const char *sn = ptsname(gui_fd);
    slave_fd = open(sn, O_RDWR | O_NOCTTY | O_CLOEXEC);  /* keep open: no EIO */
    set_raw(slave_fd, B19200);
    set_raw(gui_fd, 0);
    if (mount(sn, UART_PATH, NULL, MS_BIND, NULL) < 0) {
        perror("btcat: bind mount"); return -1;
    }
    fprintf(stderr, "btcat: %s -> %s\n", sn, UART_PATH);
    return 0;
}

int main(void) {
    if (getenv("BTCAT_UART")) UART_PATH = getenv("BTCAT_UART");
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_EP; i++) { ep[i].fd = -1; ep[i].len = 0; }
    if (setup_pty() < 0) return 1;
    write_clients();

    long rf_retry = 0;
    for (;;) {
        if (rf_fd < 0 && now_ms() >= rf_retry) {
            rf_fd = rf_listen();
            if (rf_fd < 0) rf_retry = now_ms() + 5000;
        }

        struct pollfd pf[MAX_EP + 2];
        int map[MAX_EP + 2], n = 0;
        pf[n] = (struct pollfd){ gui_fd, POLLIN, 0 }; map[n++] = -1;
        if (rf_fd >= 0) { pf[n] = (struct pollfd){ rf_fd, POLLIN, 0 }; map[n++] = -2; }
        for (int i = 0; i < MAX_EP; i++)
            if (ep[i].fd >= 0) { pf[n] = (struct pollfd){ ep[i].fd, POLLIN, 0 }; map[n++] = i; }

        poll(pf, n, pending >= 0 ? 20 : 1000);

        for (int k = 0; k < n; k++) {
            if (!pf[k].revents) continue;
            if (map[k] == -1) {
                ssize_t r = read(gui_fd, gbuf + glen, BUF_SZ - glen);
                if (r > 0) { glen += r; from_gui(); }
                else if (r < 0 && errno == EIO) usleep(100000);  /* GUI restarting */
            } else if (map[k] == -2) {
                int c = accept4(rf_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (c >= 0) {
                    int slot = -1;
                    for (int i = 1; i < MAX_EP; i++) if (ep[i].fd < 0) { slot = i; break; }
                    if (slot < 0) close(c);
                    else {
                        ep[slot].fd = c; ep[slot].is_bt = 1; ep[slot].len = 0;
                        fprintf(stderr, "btcat: BT client %d connected\n", slot);
                        write_clients();
                    }
                } else if (errno != EAGAIN) {
                    close(rf_fd); rf_fd = -1; rf_retry = now_ms() + 5000;
                }
            } else {
                int i = map[k];
                if (ep[i].len == BUF_SZ) ep[i].len = 0;
                ssize_t r = read(ep[i].fd, ep[i].buf + ep[i].len, BUF_SZ - ep[i].len);
                if (r > 0) ep[i].len += r;
                else if (ep[i].is_bt && (r == 0 || (errno != EAGAIN && errno != EINTR))) ep_close(i);
            }
        }
        dispatch();
    }
}
