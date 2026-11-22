/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - speech that is off, for the host tests of the files that speak (src/voice.h, voice_text.h,
 *  voice_focus.h): each call does nothing, as on a radio with Settings > Voice > Voice mode Off. A test adds this
 *  file to what it links; one that wants to see what is said defines the call itself (these are weak).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include <stdbool.h>

#define SILENT __attribute__((weak))

SILENT bool voice_is_on(void) {
    return false;
}

SILENT bool voice_auto_active(void) {
    return false;
}

SILENT void voice_stop(void) {
}

SILENT void voice_msg_hold(bool on) {
    (void)on;
}

SILENT void voice_say_text_fmt(const char *fmt, ...) {
    (void)fmt;
}

SILENT void voice_delay_say_text_fmt(const char *fmt, ...) {
    (void)fmt;
}

SILENT void voice_say_prompted_fmt(const char *prompt, const char *fmt, ...) {
    (void)prompt;
    (void)fmt;
}

SILENT void voice_text_add(const char *text) {
    (void)text;
}

SILENT void voice_focus_init(void *group) {
    (void)group;
}
