/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the start's word on a card that is almost full (see card_space.h, #341)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "card_space.h"

#include "board.h"
#include "cfg/cfg_api.h"
#include "msg.h"
#include "scheduler.h"

#include "lvgl/lvgl.h"
#include <pthread.h>

/* GUI thread: the setting as it is now, the message after the ones waiting (a rollback's, the settings') */
static void notice(void *arg) {
    uint64_t *room = (uint64_t *)arg;   /* free, total */
    char      left[24];

    if (!card_space_low(room[0], room[1], param_i_get(cfg_card_space_warn))) return;
    card_space_text(room[0], left, sizeof(left));
    /* What is wrong, and what to do about it. Three short lines: the message box is some 30 characters wide, and
     * a longer line makes it scroll sideways, where it could not be read in its time */
    msg_schedule_text_ms(8000, "Card almost full: %s free\nDelete recordings or pictures\non the web Gallery page", left);
}

/* Not on the GUI thread: the first count of a big FAT partition's free clusters takes a moment */
static void *read_thread(void *arg) {
    uint64_t room[2];

    (void)arg;
    if (card_space_read(BOARD_DATA, &room[0], &room[1])) scheduler_put(notice, room, sizeof(room));
    return NULL;
}

static void start_cb(lv_timer_t *t) {
    pthread_t thread;

    (void)t;
    if (pthread_create(&thread, NULL, read_thread, NULL) == 0) pthread_detach(thread);
}

void card_space_notice_init(void) {
    lv_timer_set_repeat_count(lv_timer_create(start_cb, 15000, NULL), 1);
}
