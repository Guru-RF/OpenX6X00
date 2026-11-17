/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/image_level.c on a multiboot card (Xiegu Multi-Boot): card_is_multiboot() from S01multiboot's mark
 *  or the menu's x6100mb= on the kernel's command line, and the word at the start when a newer card image is wanted:
 *  "use Multi-Boot" there, "Settings > Info" on a card of its own; none when the image is the one
 *  wanted. The files in test.d (made and removed here): the image's stamp, the version's image-required, the mark,
 *  the command line. LVGL's timer and the message box are stood in for: the timer's callback is run at once.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "image_level.h"
#include "msg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

/* ---- What image_level.c calls of LVGL and the GUI -------------------------------------------- */

static lv_timer_cb_t timer_cb;
static lv_timer_t   *timer_obj = (lv_timer_t *)&timer_cb;   /* any address: it is only handed back */
static char          said[256];

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data) {
    (void)period;
    (void)user_data;
    timer_cb = cb;
    return timer_obj;
}

void lv_timer_set_repeat_count(lv_timer_t *timer, int32_t repeat_count) {
    (void)timer;
    (void)repeat_count;
}

void msg_schedule_long_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(said, sizeof(said), fmt, ap);
    va_end(ap);
}

/* ---- The files --------------------------------------------------------------------------------- */

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

/* A start: the files as given (NULL: none), then the word 20 s on */
static const char *start(const char *stamp, const char *cmdline, bool mark) {
    unlink(TEST_DIR "/stamp");
    unlink(TEST_DIR "/cmdline");
    unlink(TEST_DIR "/multiboot");
    unlink(TEST_DIR "/data/.image-notice");   /* said once per version: each case is a version's first start */
    if (stamp) put(TEST_DIR "/stamp", stamp);
    if (cmdline) put(TEST_DIR "/cmdline", cmdline);
    if (mark) put(TEST_DIR "/multiboot", "root on partition 5\n");
    said[0] = 0;
    timer_cb = NULL;
    image_level_notice_init();
    if (timer_cb) timer_cb(timer_obj);
    return said;
}

#define OWN     "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait panic=10 fbcon=rotate:3 video=VGA:480x800\n"
#define MENU_F3 "console=ttyS0,115200 root=/dev/mmcblk0p6 rootwait panic=10 x6100mb=3\n"

int main(void) {
    char note[160];

    mkdir(TEST_DIR, 0755);
    mkdir(TEST_DIR "/data", 0755);
    put(TEST_DIR "/image-required", "# a comment\nx6200 4 the X6200's\nx6100 3 the WiFi driver fix\n");

    /* card_is_multiboot(): the mark, else x6100mb= as a whole argument of the command line */
    start(NULL, OWN, false);
    CHECK(!card_is_multiboot(), "a card of its own taken for a multiboot one");
    start(NULL, OWN, true);
    CHECK(card_is_multiboot(), "S01multiboot's mark not seen");
    start(NULL, MENU_F3, false);
    CHECK(card_is_multiboot(), "x6100mb= on the command line not seen");
    start(NULL, "x6100mb=1 root=/dev/mmcblk0p2\n", false);
    CHECK(card_is_multiboot(), "x6100mb= as the first argument not seen");
    start(NULL, "root=/dev/mmcblk0p2 nox6100mb=4\n", false);
    CHECK(!card_is_multiboot(), "nox6100mb= taken for the menu's mark");
    start(NULL, NULL, false);
    CHECK(!card_is_multiboot(), "no command line at all taken for a multiboot card");

    /* The word at the start: the image wanted is level 3, the card's stamp says which it has */
    CHECK(!strcmp(start("level=2\n", OWN, false), "New card image out: Settings > Info"), "own card: \"%s\"", said);
    CHECK(!strcmp(start("level=2\n", OWN, true), "New card image: use Multi-Boot"),
          "multiboot card (mark): \"%s\"", said);
    CHECK(!strcmp(start(NULL, MENU_F3, false), "New card image: use Multi-Boot"),
          "multiboot card (x6100mb=, no stamp: level 1): \"%s\"", said);
    CHECK(!strcmp(start("level=3\n", OWN, true), ""), "image up to date, still said: \"%s\"", said);
    CHECK(!strcmp(start("level=4\n", MENU_F3, false), ""), "a newer image than wanted, still said: \"%s\"", said);

    /* What it brings, for Settings > Info > Update */
    start("level=2\n", OWN, true);
    CHECK(image_newer_wanted(note, sizeof(note)) && !strcmp(note, "the WiFi driver fix"), "note \"%s\"", note);

    unlink(TEST_DIR "/stamp");
    unlink(TEST_DIR "/cmdline");
    unlink(TEST_DIR "/multiboot");
    unlink(TEST_DIR "/data/.image-notice");
    unlink(TEST_DIR "/image-required");
    rmdir(TEST_DIR "/data");
    rmdir(TEST_DIR);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
