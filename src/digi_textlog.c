/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the text of the keyboard modes on the card (digi_textlog.h, #55)
 *
 *  The apps hand text in from their own threads (RTTY's is the audio thread): it is put together into lines here
 *  under a lock, and a line that has ended is formatted into a buffer at once. The writer thread takes that buffer,
 *  appends each line to the file of the day it began, keeps the folder under the size limit, and asks the card to
 *  write out at most every DIGI_TEXTLOG_SYNC_S. It also ends the lines a pause has closed, waking each second
 *  while one is open, and sleeps for good when none is.
 *
 *  Each write opens the day's file and closes it again: a file deleted on the web Logs page in the meantime is
 *  made again, rather than written on unseen.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "digi_textlog.h"

#include "cfg/cfg_api.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLOTS   16                  /* the stream's line and the JTTY messages open at once */
#define OUT_MAX (16 * 1024)         /* lines ended, not written yet: past it (a card that hangs) they are dropped */
#define MB      (1024 * 1024ULL)
#define PATH_N  (sizeof(DIGI_TEXTLOG_DIR) + 16)   /* the folder, '/' and a day's file name */

typedef struct {
    bool     open;
    bool     msg;                   /* a message (digi_textlog_msg) rather than the stream */
    bool     tx;                    /* sent */
    uint32_t id;
    char     mode[16];
    uint32_t freq;
    time_t   start, last;
    unsigned seq;                   /* when it last grew, in updates: the order of the messages */
    long     sig_sum;
    int      sig_n;
    int      kind;
    char     text[DIGI_TEXTLOG_TEXT_MAX + 1];
    int      len;
} line_t;

static pthread_mutex_t mux     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake;            /* for the writer: a line ended, or a test waits */
static pthread_cond_t  idle    = PTHREAD_COND_INITIALIZER;   /* for digi_textlog_drain */
static pthread_once_t  once    = PTHREAD_ONCE_INIT;
static bool            running = false;

static line_t   lines[SLOTS];           /* lines[0] the stream's (digi_textlog_rx), the rest messages */
static unsigned updates;
static char     out[OUT_MAX];
static size_t   out_len;
static unsigned kicks, passes;          /* digi_textlog_drain's turns asked and done */

/* The writer's own */
static char     sync_path[PATH_N];      /* written since the last fsync */
static time_t   last_sync;
static char     last_path[PATH_N];      /* where the total was counted */
static int64_t  total = -1;             /* bytes in the folder's day files, -1 not counted */

static bool enabled(void) {
    return param_i_get(cfg_textlog) != 0;
}

static uint64_t cap_bytes(void) {
    int32_t mb = param_i_get(cfg_textlog_mb);

    return (uint64_t)(mb < 1 ? 1 : mb) * MB;
}

/* ---- Lines ------------------------------------------------------------------------------------------------- */

/* Under mux: the line formatted into out, the slot free again. Nothing for a line of spaces */
static void line_end(line_t *l) {
    char      sig[16] = "-";
    char      when[32];
    struct tm tm;

    if (!l->open)
        return;
    l->open = false;
    while (l->len > 0 && l->text[l->len - 1] == ' ')
        l->len--;
    l->text[l->len] = '\0';
    if (!l->len)
        return;
    if (l->sig_n && l->kind == DIGI_TEXTLOG_SIG_Q)
        snprintf(sig, sizeof(sig), "Q%ld%%", lround((double)l->sig_sum / l->sig_n));
    else if (l->sig_n && l->kind == DIGI_TEXTLOG_SIG_DB)
        snprintf(sig, sizeof(sig), "%+lddB", lround((double)l->sig_sum / l->sig_n));
    gmtime_r(&l->start, &tm);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%SZ", &tm);

    char line[DIGI_TEXTLOG_TEXT_MAX + 96];
    int  n = snprintf(line, sizeof(line), "%s %u.%06u %s %s %s %s\n", when, (unsigned)(l->freq / 1000000),
                      (unsigned)(l->freq % 1000000), l->mode, l->tx ? "TX" : "RX", sig, l->text);

    if (n > 0 && (size_t)n < sizeof(line) && out_len + (size_t)n <= sizeof(out)) {
        memcpy(out + out_len, line, (size_t)n);
        out_len += (size_t)n;
    }
    pthread_cond_signal(&wake);
}

static void line_begin(line_t *l, bool msg, uint32_t id, const char *mode, uint32_t freq, int kind, time_t now) {
    memset(l, 0, sizeof(*l));
    l->open  = true;
    l->msg   = msg;
    l->id    = id;
    l->freq  = freq;
    l->kind  = kind;
    l->start = l->last = now;
    snprintf(l->mode, sizeof(l->mode), "%s", mode);
}

/* A character of text: printable, a tab as a space, bytes past ASCII as they came (UTF-8 a sender typed) */
static bool text_char(char c) {
    return (unsigned char)c >= ' ' && c != 127;
}

static void line_add(line_t *l, char c) {
    if (c == '\t')
        c = ' ';
    if (!text_char(c) || (c == ' ' && (!l->len || l->text[l->len - 1] == ' ')))
        return;     /* no space at the start of a line, nor two in a row */
    l->text[l->len++] = c;
}

/* The app's figure as the text came, onto the line's average: a stream's per character */
static void line_sig(line_t *l, int sig) {
    l->sig_sum += sig;
    l->sig_n++;
}

static void start_writer(void);

void digi_textlog_rx(const char *mode, uint32_t freq_hz, digi_textlog_sig_t kind, int sig, const char *text) {
    line_t *l   = &lines[0];
    time_t  now = time(NULL);

    if (!text || !enabled())
        return;
    pthread_once(&once, start_writer);
    pthread_mutex_lock(&mux);
    /* Another app, or the decoder moved to another signal: the line so far is that one's */
    if (l->open && (strcmp(l->mode, mode) != 0 || labs((long)freq_hz - (long)l->freq) > DIGI_TEXTLOG_FREQ_HZ))
        line_end(l);
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            line_end(l);
            continue;
        }
        if (!l->open) {
            if (*p == ' ' || *p == '\t' || !text_char(*p))
                continue;
            line_begin(l, false, 0, mode, freq_hz, kind, now);
        }
        line_add(l, *p);
        line_sig(l, sig);
        l->last = now;
        if (l->len >= DIGI_TEXTLOG_TEXT_MAX)
            line_end(l);
    }
    pthread_mutex_unlock(&mux);
}

void digi_textlog_msg(const char *mode, uint32_t id, uint32_t freq_hz, digi_textlog_sig_t kind, int sig,
                      const char *text, bool complete) {
    time_t  now = time(NULL);
    line_t *l   = NULL, *oldest = NULL;

    if (!text || !enabled())
        return;
    pthread_once(&once, start_writer);
    pthread_mutex_lock(&mux);
    for (int i = 1; i < SLOTS && !l; i++)
        if (lines[i].open && lines[i].msg && lines[i].id == id && !strcmp(lines[i].mode, mode))
            l = &lines[i];
    if (!l) {
        for (int i = 1; i < SLOTS && !l; i++) {
            if (!lines[i].open)
                l = &lines[i];
            else if (!oldest || (int)(lines[i].seq - oldest->seq) < 0)
                oldest = &lines[i];
        }
        if (!l) {       /* more messages at once than slots: the one that grew last longest ago goes as it is */
            l = oldest;
            line_end(l);
        }
        line_begin(l, true, id, mode, freq_hz, kind, now);
    }
    /* The whole text again, a frame longer */
    l->len = 0;
    for (const char *p = text; *p && l->len < DIGI_TEXTLOG_TEXT_MAX; p++)
        if (*p != '\n' && *p != '\r')
            line_add(l, *p);
    l->last = now;
    l->seq  = ++updates;
    line_sig(l, sig);       /* a message's per frame */
    if (complete)
        line_end(l);
    pthread_mutex_unlock(&mux);
}

void digi_textlog_tx(const char *mode, uint32_t freq_hz, const char *text) {
    time_t now = time(NULL);
    line_t tx  = {0};

    if (!text || !enabled())
        return;
    pthread_once(&once, start_writer);
    pthread_mutex_lock(&mux);
    line_end(&lines[0]);    /* what was received before it, first */
    for (const char *p = text;; p++) {
        if (!*p || *p == '\n' || *p == '\r' || (tx.open && tx.len >= DIGI_TEXTLOG_TEXT_MAX)) {
            line_end(&tx);
            if (!*p)
                break;
            if (*p == '\n' || *p == '\r')
                continue;
        }
        if (!tx.open) {
            if (*p == ' ' || *p == '\t' || !text_char(*p))
                continue;
            line_begin(&tx, false, 0, mode, freq_hz, DIGI_TEXTLOG_SIG_NONE, now);
            tx.tx = true;
        }
        line_add(&tx, *p);
    }
    pthread_mutex_unlock(&mux);
}

void digi_textlog_end(void) {
    pthread_mutex_lock(&mux);
    for (int i = 0; i < SLOTS; i++)
        line_end(&lines[i]);
    pthread_mutex_unlock(&mux);
}

/* Under mux: the lines a pause has ended (or a clock set back, which no pause would end until it caught up) */
static void lines_expire(time_t now) {
    for (int i = 0; i < SLOTS; i++)
        if (lines[i].open && (now - lines[i].last >= DIGI_TEXTLOG_PAUSE_S || now < lines[i].last))
            line_end(&lines[i]);
}

static bool lines_open(void) {
    for (int i = 0; i < SLOTS; i++)
        if (lines[i].open)
            return true;
    return false;
}

/* ---- The folder -------------------------------------------------------------------------------------------- */

/* BOARD_DATA is a mount point: another filesystem than its parent's (as start_trace.c). On a PC just a folder */
static bool data_mounted(void) {
#ifdef HOST_BUILD
    return true;
#else
    struct stat data, up;

    return stat(BOARD_DATA, &data) == 0 && stat(BOARD_DATA "/..", &up) == 0 && data.st_dev != up.st_dev;
#endif
}

/* YYYY-MM-DD.txt: a day's file, the only ones counted and deleted */
static bool day_file(const char *name) {
    static const char form[] = "dddd-dd-dd.txt";

    if (strlen(name) != sizeof(form) - 1)
        return false;
    for (size_t i = 0; form[i]; i++)
        if (form[i] == 'd' ? name[i] < '0' || name[i] > '9' : name[i] != form[i])
            return false;
    return true;
}

typedef struct {
    char  name[15];
    off_t size;
} day_t;

static int day_cmp(const void *a, const void *b) {
    return strcmp(((const day_t *)a)->name, ((const day_t *)b)->name);
}

/* The bytes in the day files, the oldest days deleted (never keep's) while those and need are over cap */
static int64_t trim(const char *keep, uint64_t need, uint64_t cap) {
    DIR    *d = opendir(DIGI_TEXTLOG_DIR);
    day_t  *days = NULL;
    size_t  n = 0, room = 0;
    int64_t sum = 0;

    if (!d)
        return 0;
    for (struct dirent *e; (e = readdir(d));) {
        struct stat st;

        if (!day_file(e->d_name) || fstatat(dirfd(d), e->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (n == room) {
            day_t *more = realloc(days, (room ? room * 2 : 64) * sizeof(*days));
            if (!more)
                break;
            days = more;
            room = room ? room * 2 : 64;
        }
        memcpy(days[n].name, e->d_name, sizeof(days[n].name));     /* day_file: 14 characters and the NUL */
        days[n++].size = st.st_size;
        sum += st.st_size;
    }
    qsort(days, n, sizeof(*days), day_cmp);
    for (size_t i = 0; i < n && (uint64_t)sum + need > cap; i++)
        if (strcmp(days[i].name, keep) != 0 && unlinkat(dirfd(d), days[i].name, 0) == 0)
            sum -= days[i].size;
    closedir(d);
    free(days);
    return sum;
}

static void card_sync(const char *path) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

/* Lines of one day onto its file, within the limit; false when they could not go (the day's file is at it) */
static bool day_write(const char *date, const char *buf, size_t n, time_t now) {
    char     name[16], path[PATH_N];
    uint64_t cap = cap_bytes();
    int      fd;

    snprintf(name, sizeof(name), "%.10s.txt", date);
    snprintf(path, sizeof(path), DIGI_TEXTLOG_DIR "/%s", name);
    /* Counted again for another day's file, and when the count says the limit is near: files may have gone on
     * the web page meanwhile */
    if (total < 0 || strcmp(path, last_path) != 0 || (uint64_t)total + n > cap) {
        total = trim(name, n, cap);
        snprintf(last_path, sizeof(last_path), "%s", path);
    }
    if ((uint64_t)total + n > cap)
        return false;
    mkdir(DIGI_TEXTLOG_DIR, 0755);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    for (size_t done = 0; done < n;) {
        ssize_t w = write(fd, buf + done, n - done);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        done += (size_t)w;
        total += w;
    }
    if (sync_path[0] && strcmp(sync_path, path) != 0)
        card_sync(sync_path);   /* the day before's last lines, out before this day's */
    if (now - last_sync >= DIGI_TEXTLOG_SYNC_S || now < last_sync) {
        fsync(fd);
        last_sync    = now;
        sync_path[0] = '\0';
    } else {
        snprintf(sync_path, sizeof(sync_path), "%s", path);
    }
    close(fd);
    return true;
}

/* The lines taken from out, each onto the file of the day it began (its first 10 characters) */
static void store(const char *buf, size_t n, time_t now) {
    if (!data_mounted())
        return;
    for (size_t i = 0; i < n;) {
        size_t j = i;

        /* The lines of the same day, together */
        while (j < n && !strncmp(buf + i, buf + j, 10)) {
            const char *nl = memchr(buf + j, '\n', n - j);
            j = nl ? (size_t)(nl - buf) + 1 : n;
        }
        day_write(buf + i, buf + i, j - i, now);
        i = j;
    }
}

/* ---- The writer -------------------------------------------------------------------------------------------- */

static void *writer(void *arg) {
    static char chunk[OUT_MAX];
    (void)arg;

    pthread_mutex_lock(&mux);
    for (;;) {
        unsigned asked = kicks;
        time_t   now   = time(NULL);

        lines_expire(now);
        if (!enabled()) {       /* turned off: what was open or waiting is not kept */
            for (int i = 0; i < SLOTS; i++)
                lines[i].open = false;
            out_len = 0;
        }
        if (out_len) {
            size_t n = out_len;

            memcpy(chunk, out, n);
            out_len = 0;
            pthread_mutex_unlock(&mux);
            store(chunk, n, now);
            pthread_mutex_lock(&mux);
            continue;
        }
        if (sync_path[0] && (now - last_sync >= DIGI_TEXTLOG_SYNC_S || now < last_sync)) {
            char path[sizeof(sync_path)];

            snprintf(path, sizeof(path), "%s", sync_path);
            sync_path[0] = '\0';
            last_sync    = now;
            pthread_mutex_unlock(&mux);
            card_sync(path);
            pthread_mutex_lock(&mux);
            continue;
        }
        passes = asked;
        pthread_cond_broadcast(&idle);
        if (kicks != asked)
            continue;
        if (lines_open() || sync_path[0]) {
            struct timespec t;

            clock_gettime(CLOCK_MONOTONIC, &t);
            t.tv_sec++;
            pthread_cond_timedwait(&wake, &mux, &t);
        } else {
            pthread_cond_wait(&wake, &mux);
        }
    }
    return NULL;
}

static void start_writer(void) {
    pthread_condattr_t ca;
    pthread_t          th;

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&wake, &ca);
    pthread_condattr_destroy(&ca);
    if (pthread_create(&th, NULL, writer, NULL) == 0) {
        pthread_detach(th);
        running = true;
    }
}

void digi_textlog_drain(void) {
    pthread_mutex_lock(&mux);
    if (running) {
        unsigned me = ++kicks;

        pthread_cond_signal(&wake);
        while ((int)(passes - me) < 0)
            pthread_cond_wait(&idle, &mux);
    }
    pthread_mutex_unlock(&mux);
}
