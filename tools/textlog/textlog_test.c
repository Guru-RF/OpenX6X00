/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the keyboard modes' text log (src/digi_textlog.c, #55); see the Makefile.
 *
 *  The text goes in as the apps hand it over (a character at a time as PSK does, a block as Olivia, a message that
 *  grows as JTTY, what is sent), the clock is the test's own, and after each step the writer thread is waited for
 *  (digi_textlog_drain) and the day's files are read back. The card's fsync calls are counted.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cfg/cfg_api.h"
#include "digi_textlog.h"

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- The settings and the clock: the test's ---------------------------------------------------------------- */

struct ParamInt {
    int32_t v;
};

static ParamInt on = {1}, mb = {10};
ParamInt       *cfg_textlog = &on, *cfg_textlog_mb = &mb;

int32_t param_i_get(const ParamInt *p) {
    return p->v;
}

static volatile time_t fake_now;

time_t __wrap_time(time_t *t) {
    if (t)
        *t = fake_now;
    return fake_now;
}

static int fsyncs;

int __real_fsync(int fd);
int __wrap_fsync(int fd) {
    fsyncs++;
    return __real_fsync(fd);
}

static time_t utc(int y, int mo, int d, int h, int mi, int s) {
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s};

    return timegm(&tm);
}

/* ---- The files ---------------------------------------------------------------------------------------------- */

static const char *path_of(const char *name) {
    static char p[512];

    snprintf(p, sizeof(p), DIGI_TEXTLOG_DIR "/%s", name);
    return p;
}

/* A day's file, "" when there is none */
static const char *day(const char *date) {
    static char buf[4 * 1024 * 1024];
    char        name[32];
    FILE       *f;
    size_t      n = 0;

    snprintf(name, sizeof(name), "%s.txt", date);
    buf[0] = '\0';
    if ((f = fopen(path_of(name), "r"))) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

static long size_of(const char *name) {
    struct stat st;

    return stat(path_of(name), &st) == 0 ? (long)st.st_size : -1;
}

static void make_file(const char *name, long size) {
    FILE *f = fopen(path_of(name), "w");

    for (long i = 0; f && i < size; i++)
        fputc(i % 64 == 63 ? '\n' : 'x', f);
    if (f)
        fclose(f);
}

static void clear_dir(void) {
    static const char *names[] = {"2026-10-01.txt", "2026-10-02.txt", "2026-10-03.txt", "2026-10-31.txt",
                                  "2026-11-01.txt", "2026-11-02.txt", "2026-11-03.txt", "notes.txt"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        unlink(path_of(names[i]));
}

/* Characters one at a time, as the PSK window hands them over */
static void psk(uint32_t f, int q, const char *s) {
    for (; *s; s++) {
        char c[2] = {*s, 0};
        digi_textlog_rx("PSK31", f, DIGI_TEXTLOG_SIG_Q, q, c);
    }
}

static bool has(const char *text, const char *want) {
    bool ok = strstr(text, want) != NULL;

    if (!ok)
        printf("        wanted: %s        in:\n%s", want, text);
    return ok;
}

int main(void) {
    const char *t;

    mkdir(BOARD_DATA, 0755);
    fake_now = utc(2026, 10, 31, 14, 5, 12);

    printf("Lines\n");
    psk(14071000, 87, "CQ CQ DE ON6URE ON6URE K\n");
    digi_textlog_drain();
    t = day("2026-10-31");
    check(!strcmp(t, "2026-10-31 14:05:12Z 14.071000 PSK31 RX Q87% CQ CQ DE ON6URE ON6URE K\n"),
          "the decoder's line break ends a line: time, MHz, mode, RX, quality, text");
    check(!access(DIGI_TEXTLOG_DIR, F_OK), "the folder is made: %s", DIGI_TEXTLOG_DIR);

    fake_now += 20;
    psk(14071000, 80, "PA3");
    psk(14071000, 90, "XYZ");
    digi_textlog_drain();
    fake_now += DIGI_TEXTLOG_PAUSE_S - 1;
    digi_textlog_drain();
    check(!strstr(day("2026-10-31"), "PA3XYZ"), "%d s without text: the line is still open", DIGI_TEXTLOG_PAUSE_S - 1);
    fake_now += 1;
    digi_textlog_drain();
    check(has(day("2026-10-31"), "2026-10-31 14:05:32Z 14.071000 PSK31 RX Q85% PA3XYZ\n"),
          "%d s: a pause ends it, at the time it began, the quality averaged over it", DIGI_TEXTLOG_PAUSE_S);

    fake_now += 60;
    psk(14071000, 70, "  HELLO\x01\x07   WORLD \t AGAIN   \r\n");
    digi_textlog_drain();
    check(has(day("2026-10-31"), "Q70% HELLO WORLD AGAIN\n"),
          "spaces at the start and the end go, runs of them are one, control characters are dropped");

    psk(14071000, 70, "\n\n   \n");
    digi_textlog_drain();
    check(!strstr(day("2026-10-31"), "Q70% \n") && !strstr(day("2026-10-31"), "Q70%\n"),
          "empty lines and lines of spaces are not written");

    fake_now += 60;
    psk(14071000, 75, "AAA");
    psk(14071000 + DIGI_TEXTLOG_FREQ_HZ, 75, "BBB");
    psk(14071000 + DIGI_TEXTLOG_FREQ_HZ * 2 + 1, 75, "CCC\n");
    digi_textlog_drain();
    t = day("2026-10-31");
    check(has(t, "14.071000 PSK31 RX Q75% AAABBB\n") && has(t, "14.071061 PSK31 RX Q75% CCC\n"),
          "a move of more than %d Hz from where the line began ends it; the follow loop's few Hz do not",
          DIGI_TEXTLOG_FREQ_HZ);

    fake_now += 60;
    digi_textlog_rx("OLIVIA-8/250", 14072500, DIGI_TEXTLOG_SIG_Q, 64, "RYRY ");
    psk(14071000, 88, "NEXT\n");
    digi_textlog_drain();
    t = day("2026-10-31");
    check(has(t, "14.072500 OLIVIA-8/250 RX Q64% RYRY\n") && has(t, "14.071000 PSK31 RX Q88% NEXT\n"),
          "another mode's text ends the open line");

    fake_now += 60;
    {
        char long_text[451];

        for (int i = 0; i < 450; i++)
            long_text[i] = (char)('A' + i / 200);
        long_text[450] = '\0';
        digi_textlog_rx("PSK31", 14071000, DIGI_TEXTLOG_SIG_Q, 90, long_text);
        digi_textlog_end();
        digi_textlog_drain();
        t = day("2026-10-31");
        char a[220] = "Q90% ", b[220] = "Q90% ", c[80] = "Q90% ";
        memset(a + 5, 'A', 200);
        a[205] = '\n';
        memset(b + 5, 'B', 200);
        b[205] = '\n';
        memset(c + 5, 'C', 50);
        c[55] = '\n';
        check(has(t, a) && has(t, b) && has(t, c), "a line of 450 characters: %d, %d and the 50 left (the app closed)",
              DIGI_TEXTLOG_TEXT_MAX, DIGI_TEXTLOG_TEXT_MAX);
    }

    printf("Sent text\n");
    fake_now = utc(2026, 10, 31, 14, 20, 0);
    psk(14071000, 82, "PA3XYZ DE ON6URE PSE K");
    fake_now += 3;
    digi_textlog_tx("PSK31", 14071000, "\nON6URE DE PA3XYZ\r\nTNX FER CALL\n");
    digi_textlog_drain();
    t = day("2026-10-31");
    {
        const char *rx  = strstr(t, "RX Q82% PA3XYZ DE ON6URE PSE K\n");
        const char *tx1 = strstr(t, "2026-10-31 14:20:03Z 14.071000 PSK31 TX - ON6URE DE PA3XYZ\n");
        const char *tx2 = strstr(t, "2026-10-31 14:20:03Z 14.071000 PSK31 TX - TNX FER CALL\n");
        check(rx && tx1 && tx2 && rx < tx1 && tx1 < tx2,
              "sent: a TX line for each line, '-' for the figure, after the received line that was open");
    }

    fake_now += 60;
    {
        char text[256];
        bool ascii = true;

        memset(text, 'a', 199);
        snprintf(text + 199, sizeof(text) - 199, "\xc3\xa9 de Ren\xc3\xa9\n");   /* 199 a's, then UTF-8 "é de René" */
        digi_textlog_tx("PSK31", 14071000, text);
        digi_textlog_rx("PSK31", 14071000, DIGI_TEXTLOG_SIG_Q, 50, "NOISE \xe9\x9b\xff\n");
        digi_textlog_drain();
        t = day("2026-10-31");
        for (const char *p = t; *p; p++)
            ascii = ascii && (unsigned char)*p < 128;
        memset(text, 'a', 199);
        snprintf(text + 199, sizeof(text) - 199, "?\n");
        check(ascii && has(t, text) && has(t, "TX - ? de Ren??\n") && has(t, "RX Q50% NOISE ???\n"),
              "past ASCII: '?' a byte, as PSK and Olivia send it; the file stays plain ASCII, no UTF-8 cut in two");
    }

    printf("Messages (JTTY)\n");
    fake_now = utc(2026, 10, 31, 14, 30, 0);
    digi_textlog_msg("JTTY", 1, 14090850, DIGI_TEXTLOG_SIG_DB, -10, "CQ", false);
    digi_textlog_msg("JTTY", 2, 14091400, DIGI_TEXTLOG_SIG_DB, -5, "TEST", false);
    fake_now += 2;
    digi_textlog_msg("JTTY", 1, 14090851, DIGI_TEXTLOG_SIG_DB, -14, "CQ PA3XYZ", false);
    fake_now += 2;
    digi_textlog_msg("JTTY", 1, 14090850, DIGI_TEXTLOG_SIG_DB, -12, "CQ PA3XYZ JO21", true);
    digi_textlog_drain();
    t = day("2026-10-31");
    check(has(t, "2026-10-31 14:30:00Z 14.090850 JTTY RX -12dB CQ PA3XYZ JO21\n") && !strstr(t, "JTTY RX -10dB CQ\n"),
          "a message is written once, whole, when it is complete; S/N averaged over its frames");
    check(!strstr(t, "TEST"), "the other message, still growing, is not written yet");
    fake_now += DIGI_TEXTLOG_PAUSE_S;
    digi_textlog_drain();
    check(has(day("2026-10-31"), "2026-10-31 14:30:00Z 14.091400 JTTY RX -5dB TEST\n"),
          "a message that stopped growing (no end frame) is written after the pause");

    for (int i = 0; i < 20; i++) {
        char text[16];

        snprintf(text, sizeof(text), "M%02d", i);
        digi_textlog_msg("JTTY", 100 + i, 14090000 + i * 100, DIGI_TEXTLOG_SIG_DB, -1, text, false);
    }
    digi_textlog_drain();
    t = day("2026-10-31");
    check(has(t, "RX -1dB M00\n") && has(t, "RX -1dB M04\n") && !strstr(t, "M05"),
          "20 messages at once: the five that grew longest ago are written early to make room");
    digi_textlog_end();
    digi_textlog_drain();
    check(has(day("2026-10-31"), "RX -1dB M19\n"), "the app closes: the rest are written");

    printf("The day\n");
    fake_now = utc(2026, 10, 31, 23, 59, 55);
    psk(7041000, 77, "BEFORE MIDNIGHT ");
    fake_now = utc(2026, 11, 1, 0, 0, 3);
    psk(7041000, 77, "AND AFTER\n");
    fake_now = utc(2026, 11, 1, 0, 0, 10);
    psk(7041000, 77, "THE NEW DAY\n");
    digi_textlog_drain();
    check(has(day("2026-10-31"), "2026-10-31 23:59:55Z 7.041000 PSK31 RX Q77% BEFORE MIDNIGHT AND AFTER\n"),
          "a line goes to the file of the UTC day it began");
    check(!strcmp(day("2026-11-01"), "2026-11-01 00:00:10Z 7.041000 PSK31 RX Q77% THE NEW DAY\n"),
          "the next UTC day, a file of its own");

    printf("The clock\n");
    fake_now = utc(2026, 11, 1, 1, 0, 0);
    psk(7041000, 77, "SET BACK");
    digi_textlog_drain();
    fake_now -= 3600;
    digi_textlog_drain();
    check(has(day("2026-11-01"), "01:00:00Z 7.041000 PSK31 RX Q77% SET BACK\n"),
          "the clock set back an hour (GPS, NTP): the open line is written, not held until the clock catches up");

    printf("The card\n");
    fake_now = utc(2026, 11, 1, 2, 0, 0);
    int before = fsyncs;
    psk(7041000, 77, "ONE\n");
    digi_textlog_drain();
    fake_now += 1;
    psk(7041000, 77, "TWO\n");
    digi_textlog_drain();
    fake_now += 1;
    psk(7041000, 77, "THREE\n");
    digi_textlog_drain();
    check(fsyncs - before == 1, "three lines in 2 s: written as they end, the card made to write out once (%d)",
          fsyncs - before);
    fake_now += DIGI_TEXTLOG_SYNC_S;
    digi_textlog_drain();
    check(fsyncs - before == 2, "%d s later the last two are made to go out too (%d)", DIGI_TEXTLOG_SYNC_S,
          fsyncs - before);
    fake_now += 60;
    digi_textlog_drain();
    check(fsyncs - before == 2, "nothing new, nothing more (%d)", fsyncs - before);

    printf("The size limit\n");
    clear_dir();
    mb.v = 1;
    make_file("2026-10-01.txt", 400 * 1024);
    make_file("2026-10-02.txt", 400 * 1024);
    make_file("2026-10-03.txt", 400 * 1024);
    make_file("notes.txt", 900 * 1024);
    fake_now = utc(2026, 11, 2, 9, 0, 0);
    psk(3581000, 66, "OVER THE LIMIT\n");
    digi_textlog_drain();
    check(size_of("2026-10-01.txt") < 0 && size_of("2026-10-02.txt") > 0 && size_of("2026-10-03.txt") > 0,
          "1.2 MB of days and a limit of 1 MB: the oldest day goes, the next two stay");
    check(has(day("2026-11-02"), "Q66% OVER THE LIMIT\n"), "and the line is written");
    check(size_of("notes.txt") == 900 * 1024, "a file that is not a day's is neither counted nor deleted");

    make_file("2026-11-03.txt", 1024 * 1024 - 20);
    fake_now = utc(2026, 11, 3, 9, 0, 0);
    psk(3581000, 66, "TODAY IS FULL\n");
    digi_textlog_drain();
    check(size_of("2026-11-03.txt") == 1024 * 1024 - 20 && !strstr(day("2026-11-03"), "TODAY IS FULL"),
          "the newest day alone at the limit: it is not deleted, and nothing more is kept");
    check(size_of("2026-10-02.txt") < 0 && size_of("2026-10-03.txt") < 0 && size_of("2026-11-02.txt") < 0,
          "the older days have gone for it");
    mb.v = 2;
    psk(3581000, 66, "MORE ROOM\n");
    digi_textlog_drain();
    check(has(day("2026-11-03"), "Q66% MORE ROOM\n"), "a higher limit: the day goes on");

    printf("Off\n");
    on.v = 0;
    psk(3581000, 66, "NOT KEPT\n");
    digi_textlog_tx("PSK31", 3581000, "NOR THIS\n");
    digi_textlog_msg("JTTY", 7, 3581000, DIGI_TEXTLOG_SIG_DB, -3, "NOR THAT", true);
    digi_textlog_drain();
    t = day("2026-11-03");
    check(!strstr(t, "NOT KEPT") && !strstr(t, "NOR THIS") && !strstr(t, "NOR THAT"), "off: nothing is written");
    on.v = 1;
    psk(3581000, 66, "HALF ");
    digi_textlog_drain();
    on.v = 0;
    fake_now += 1;
    digi_textlog_drain();
    on.v = 1;
    fake_now += DIGI_TEXTLOG_PAUSE_S;
    digi_textlog_drain();
    check(!strstr(day("2026-11-03"), "HALF"), "turned off with a line open: that line is not written either");
    psk(3581000, 66, "ON AGAIN\n");
    digi_textlog_drain();
    check(has(day("2026-11-03"), "Q66% ON AGAIN\n"), "on again: written again");

    printf("\n%d of %d cases right\n", good, cases);
    return good == cases ? 0 : 1;
}
