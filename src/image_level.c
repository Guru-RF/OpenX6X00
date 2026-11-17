/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "image_level.h"

#include "board.h"
#include "main.h"
#include "msg.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef IMAGE_STAMP                                  /* tools/multiboot: its own */
#define IMAGE_STAMP    "/etc/openx6x00-image"
#endif
#define IMAGE_REQUIRED BOARD_SHARE "/image-required"
#define NOTICE_SEEN    BOARD_DATA "/.image-notice"   /* the version it was said for */
#ifndef MULTIBOOT
#define MULTIBOOT      "/tmp/x6100-multiboot"         /* S01multiboot: there on a multiboot card */
#define CMDLINE        "/proc/cmdline"
#endif

static int stamped_level(void) {
    FILE *f = fopen(IMAGE_STAMP, "r");
    char  line[64];
    int   level = 1;

    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "level=", 6) == 0) level = atoi(line + 6);
    }
    if (f) fclose(f);
    return level > 0 ? level : 1;
}

bool card_is_multiboot(void) {
    FILE *f;
    char  args[1024];
    bool  yes = false;

    if (access(MULTIBOOT, F_OK) == 0) return true;
    /* S01multiboot did not run (started by hand, an image before it): the menu's own mark on the kernel's command
       line at least */
    if ((f = fopen(CMDLINE, "r"))) {
        args[0] = ' ';
        if (fgets(args + 1, sizeof(args) - 1, f)) yes = strstr(args, " x6100mb=") != NULL;
        fclose(f);
    }
    return yes;
}

bool image_newer_wanted(char *note, size_t size) {
    FILE *f = fopen(IMAGE_REQUIRED, "r");
    char  line[256], model[16];
    int   level, n;
    bool  wanted = false;

    while (f && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%15s %d %n", model, &level, &n) < 2 || strcmp(model, BOARD_NAME_LOWER))
            continue;
        if (level > stamped_level()) {
            line[strcspn(line, "\r\n")] = '\0';
            snprintf(note, size, "%s", line + n);
            wanted = true;
        }
        break;
    }
    if (f) fclose(f);
    return wanted;
}

static void notice_cb(lv_timer_t *t) {
    char note[160], seen[64] = "";
    FILE *f;

    (void)t;
    if (!image_newer_wanted(note, sizeof(note))) return;
    /* once per version: the version it was said for is kept on the DATA partition */
    if ((f = fopen(NOTICE_SEEN, "r"))) {
        if (!fgets(seen, sizeof(seen), f)) seen[0] = '\0';
        fclose(f);
        seen[strcspn(seen, "\r\n")] = '\0';
    }
    if (strcmp(seen, VERSION) == 0) return;
    /* the message box takes one short line (a longer one scrolls out of its 4 s): what the image brings is on
       Settings > Info > Update. On a multiboot card the image goes through Xiegu Multi-Boot: written onto the card, it
       would erase the menu and the other systems */
    if (card_is_multiboot())
        msg_schedule_long_text_fmt("New card image: use Multi-Boot");
    else
        msg_schedule_long_text_fmt("New card image out: Settings > Info");
    if ((f = fopen(NOTICE_SEEN, "w"))) {
        fprintf(f, "%s\n", VERSION);
        fclose(f);
    }
}

void image_level_notice_init(void) {
    lv_timer_set_repeat_count(lv_timer_create(notice_cb, 20000, NULL), 1);
}
