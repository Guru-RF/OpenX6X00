/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - speaks what the knob lands on (accessibility)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The keyboard group's focus callback when no window has one of its own: the newly focused object
 * described and spoken, and its value spoken as it changes */
void voice_focus_init(lv_group_t *group);
void voice_focus_cb(lv_group_t *group);

/* Plain-text description of obj for speech: its row's name and a list entry, a text field, a
 * switch, a value... False when there is nothing to say */
bool voice_focus_describe(lv_obj_t *obj, char *buf, size_t size);

/* What a window shows to say first: its name as it opens, an instruction. Spoken together with the
 * focus change that usually comes with it ("<hint>. <focused item>"), or alone if none comes */
void voice_focus_hint(const char *text);

/* The same, added after a hint already waiting ("SWR Scan" then "F1 Run, F2 Scale, F3 Span"), else on its own */
void voice_focus_hint_add(const char *text);

/* A status line said when its text changes (a window's "Connecting", "Talking now: K1ABC"), not at every refresh.
 * last: the caller's own buffer of what was said; prefix said before it (may be NULL) */
void voice_say_label_change(lv_obj_t *label, char *last, size_t size, const char *prefix);

/* A hint is waiting to be said (a window is opening) */
bool voice_focus_hint_waiting(void);

/* A window that keeps its own selection inside one focused object (the APPS and Modes grids): its focus says
 * nothing, the window speaks its chosen item itself */
#define VOICE_FOCUS_QUIET   LV_OBJ_FLAG_USER_4
void voice_focus_quiet(lv_obj_t *obj);

/* A window moving a selection itself (a list following its newest row): the reader says nothing of it meanwhile */
void voice_focus_mute(bool on);

/* Every label in obj read aloud, top to bottom: a window's whole text, on request (the power key) */
void voice_focus_read(lv_obj_t *obj);

/* The same with a heading said first ("Weather, 5 days. ..."); head may be NULL */
void voice_focus_read_as(const char *head, lv_obj_t *obj);

/* The same with something said after it ("F-keys: F1 Run, ...") */
void voice_focus_read_full(const char *head, lv_obj_t *obj, const char *tail);

/* The line reader: a screen of text (Weather, Settings > Info) read a line at a time. voice_lines_set() lays obj out,
 * gathers its labels in reading order (column by column, a short name joined to the value under it: "Wind, NW 15
 * km/h"), says head, how many lines, and the first; voice_lines_step() says the next (dir > 0) or the one before, with
 * its place ("Humidity, 80 %, 5 of 14"). voice_lines_clear() when the screen goes */
void voice_lines_set(lv_obj_t *obj, const char *head);
void voice_lines_step(int dir);
void voice_lines_clear(void);

/* Lines a window composes itself (a table read across its rows): begin, add each, then done says head, how many and
 * the first, as voice_lines_set(); voice_lines_step() goes through them */
void voice_lines_begin(void);
void voice_lines_add(const char *text);
void voice_lines_done(const char *head);

/* What obj says when the knob lands on it, in place of what the reader would find in it (a row drawn with
 * pictures: signal bars, a lock). Kept until obj is deleted or set again; NULL or "" forgets it */
void voice_focus_set_text(lv_obj_t *obj, const char *text);

/* The name a control is called by, in place of its row's label ("Day", "Month", "Year" for three boxes of one "Date"
 * row). Its value is said after it as usual. Kept until obj is deleted */
void voice_focus_set_name(lv_obj_t *obj, const char *name);

/* A table that draws its rows itself (its cells blank): what row says, from the window's data. Kept until the
 * table is deleted */
typedef void (*voice_row_text_cb_t)(lv_obj_t *table, uint16_t row, char *buf, size_t size);
void voice_focus_set_row_cb(lv_obj_t *table, voice_row_text_cb_t cb);

#ifdef __cplusplus
}
#endif
