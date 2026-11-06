/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The GUI loop's sleep: it waits on the front keys, the knobs and an eventfd the other threads
 *  post to, so a key press, a knob turn or a queued event is handled at once instead of at the
 *  next LVGL timer (up to 10 ms for keys and knobs, a whole frame for the rest).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void gui_wake_init(void);

/* An input device the GUI reads (its LVGL indev reads it): a wake when it has data */
void gui_wake_add_fd(int fd);

/* From any thread: something for the GUI is waiting (an event queued) ... */
void gui_wake(void);
/* ... or input read by a thread of its own (the main knob) */
void gui_wake_input(void);

typedef enum { WAKE_NONE, WAKE_EVENT, WAKE_INPUT } gui_wake_t;

/* The GUI thread: sleep up to ms; what woke it */
gui_wake_t gui_wake_wait(int32_t ms);

/* After input: the keys and knobs read at the next lv_timer_handler() ... */
void gui_wake_read_input(void);
/* ... and, once that has run, the screen drawn at the next one, or min_gap_ms after the last
 * frame when that was sooner (not later at the next regular refresh) */
void gui_wake_draw_now(uint32_t min_gap_ms);

/* The GUI loop, before lv_timer_handler(): a draw put off by the gap is due now. Dropped when a
 * frame ran since it was put off: that frame showed the input */
void gui_wake_draw_due(void);
/* ... and after the frame's commit: the ms from now (get_time()) until it is due, 0 when it is,
 * -1 when none waits (the loop sleeps no longer than that) */
int32_t gui_wake_draw_left(uint32_t now);
