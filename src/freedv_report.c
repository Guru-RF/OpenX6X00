/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  FreeDV Reporter for the RADE app: the helper process and its socket. See freedv_report.h.
 *
 *  The socket pair is SOCK_SEQPACKET: a line is one record, sent whole or not at all, so a full buffer never leaves
 *  half a line behind. The helper has it as its stdin and stdout; its stderr goes to /dev/null. It is spawned with
 *  no signal blocked and the default actions (the GUI's threads block some), so SIGTERM ends it.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "freedv_report.h"
#include "board.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

#define MSG_FILE   BOARD_DATA "/freedv_reporter.txt"
#define STEP_US    50000        /* the reaper looks every 50 ms */
#define TERM_STEPS 20           /* SIGTERM after 1 s */
#define KILL_STEPS 40           /* SIGKILL after 2 s */
#define EOO_TICKS  5            /* UI ticks (200 ms) for the callsign of an End-of-Over */
#define EOO_NONE   UINT32_MAX

extern char **environ;

static const char *helper = X6100_SBIN "/x6100-freedv-reporter";
static int         fd = -1;
static pid_t       pid = -1;
static uint32_t    sent_freq;
static int         sent_tx = -1;
static char        sent_msg[FREEDV_REPORT_MSG_MAX + 1];
static uint32_t    rx_eoo = EOO_NONE;   /* the receiver's End-of-Over count last seen, EOO_NONE: none yet */
static int         rx_wait;             /* ticks left for the callsign of the last one */
static float       rx_snr;              /* the last SNR with sync */

void freedv_report_set_helper(const char *path) { helper = path; }

bool freedv_report_running(void) { return pid > 0; }

/* One line, whole or not at all, never waiting: a full buffer (a helper that stopped reading) drops it */
static void send_line(const char *line)
{
    if (fd < 0)
        return;
    send(fd, line, strlen(line), MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void *reaper(void *arg)
{
    pid_t p = (pid_t)(intptr_t)arg;

    for (int i = 0; i < KILL_STEPS; i++) {
        pid_t r = waitpid(p, NULL, WNOHANG);

        if (r == p || (r < 0 && errno == ECHILD))
            return NULL;
        if (i == TERM_STEPS)
            kill(p, SIGTERM);
        usleep(STEP_US);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    return NULL;
}

bool freedv_report_start(const char *call, const char *grid, bool rx_only, uint32_t freq_hz, const char *version)
{
    int                        sv[2];
    char                       freq[16];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    sigset_t                   none, dfl;

    if (pid > 0)
        return true;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0)
        return false;
    snprintf(freq, sizeof(freq), "%u", (unsigned)freq_hz);
    char *argv[] = {(char *)helper, (char *)call, (char *)grid, rx_only ? "1" : "0", freq, (char *)version, NULL};

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGINT);
    sigaddset(&dfl, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dfl);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t p;
    int   r = posix_spawn(&p, helper, &fa, &attr, argv, environ);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(sv[1]);
    if (r != 0) {
        close(sv[0]);
        return false;
    }
    fd = sv[0];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    pid = p;
    sent_freq = freq_hz;
    sent_tx = 0;
    sent_msg[0] = '\0';
    rx_eoo = EOO_NONE;      /* the overs heard before it started are not reported */
    rx_wait = 0;
    rx_snr = 0.0f;
    return true;
}

void freedv_report_stop(void)
{
    pthread_t      t;
    pthread_attr_t a;

    if (pid <= 0)
        return;
    close(fd);          /* the helper's stdin ends: it says goodbye to the server and exits */
    fd = -1;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, reaper, (void *)(intptr_t)pid) != 0) {
        kill(pid, SIGKILL);     /* no thread to wait with: at once, and reaped */
        waitpid(pid, NULL, 0);
    }
    pthread_attr_destroy(&a);
    pid = -1;
}

void freedv_report_freq(uint32_t hz)
{
    char line[32];

    if (pid <= 0 || !hz || hz == sent_freq)
        return;
    snprintf(line, sizeof(line), "freq %u\n", (unsigned)hz);
    send_line(line);
    sent_freq = hz;
}

void freedv_report_tx(bool on)
{
    if (pid <= 0 || sent_tx == (int)on)
        return;
    send_line(on ? "tx 1\n" : "tx 0\n");
    sent_tx = on;
}

void freedv_report_rx(const char *call, int snr_db)
{
    char line[48];

    if (pid <= 0 || !call || !call[0] || strlen(call) > 12)
        return;
    for (const char *c = call; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '/'))
            return;
    snprintf(line, sizeof(line), "rx %s %d\n", call, snr_db);
    send_line(line);
}

void freedv_report_rx_stats(const freedv_rx_stats_t *s)
{
    if (pid <= 0 || !s)
        return;
    if (s->sync)
        rx_snr = s->snr_db;
    if (rx_eoo == EOO_NONE || s->eoo_count < rx_eoo) {
        rx_eoo = s->eoo_count;
        rx_wait = 0;
        return;
    }
    if (s->eoo_count > rx_eoo) {
        rx_eoo = s->eoo_count;
        rx_wait = EOO_TICKS;
    }
    if (rx_wait > 0) {
        if (s->callsign[0]) {
            freedv_report_rx(s->callsign, (int)(rx_snr < 0 ? rx_snr - 0.5f : rx_snr + 0.5f));
            rx_wait = 0;
        } else {
            rx_wait--;
        }
    }
}

/* One line: no control characters, the spaces folded, whole UTF-8 characters (the popup types å ä ö ü é ñ), at most
 * FREEDV_REPORT_MSG_MAX bytes */
static void message_clean(char *out, size_t size, const char *in)
{
    size_t n = 0, max = size - 1 < FREEDV_REPORT_MSG_MAX ? size - 1 : FREEDV_REPORT_MSG_MAX;
    bool   gap = false;

    while (in && *in) {
        unsigned char c = (unsigned char)*in;
        size_t        len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        size_t        k = 1;

        while (len && k < len && ((unsigned char)in[k] & 0xc0) == 0x80)
            k++;
        if (!len || k < len || c <= ' ' || c == 0x7f) {    /* a space, a control or a broken character */
            gap = n > 0;
            in += len && k == len ? len : 1;
            continue;
        }
        if (n + (gap ? 1 : 0) + len > max)
            break;
        if (gap)
            out[n++] = ' ';
        gap = false;
        memcpy(out + n, in, len);
        n += len;
        in += len;
    }
    out[n] = '\0';
}

void freedv_report_message(const char *text)
{
    char m[FREEDV_REPORT_MSG_MAX + 1], line[FREEDV_REPORT_MSG_MAX + 8];

    message_clean(m, sizeof(m), text);
    if (pid <= 0 || !strcmp(m, sent_msg))
        return;
    snprintf(line, sizeof(line), "msg %s\n", m);
    send_line(line);
    snprintf(sent_msg, sizeof(sent_msg), "%s", m);
}

bool freedv_report_poll(char *line, size_t size)
{
    if (pid <= 0 || size < 2)
        return false;
    ssize_t n = recv(fd, line, size - 1, MSG_DONTWAIT);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return false;
    if (n <= 0) {       /* the helper closed its end: it exited or died */
        freedv_report_stop();
        snprintf(line, size, "gone");
        return true;
    }
    line[n] = '\0';
    line[strcspn(line, "\r\n")] = '\0';
    return true;
}

void freedv_report_message_load(char *buf, size_t size)
{
    char  raw[256] = "";
    FILE *f = fopen(MSG_FILE, "r");

    if (f) {
        if (!fgets(raw, sizeof(raw), f))
            raw[0] = '\0';
        fclose(f);
    }
    message_clean(buf, size, raw);
}

bool freedv_report_message_save(const char *text)
{
    char  m[FREEDV_REPORT_MSG_MAX + 1];
    FILE *f = fopen(MSG_FILE ".tmp", "w");

    message_clean(m, sizeof(m), text);
    if (!f)
        return false;
    bool ok = fprintf(f, "%s\n", m) >= 0;

    ok = fclose(f) == 0 && ok;
    if (!ok || rename(MSG_FILE ".tmp", MSG_FILE) != 0) {
        unlink(MSG_FILE ".tmp");
        return false;
    }
    return true;
}
