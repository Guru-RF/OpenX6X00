/* Host test shim of src/msg_box.h: the note the TCI server puts on the screen (stubs.c keeps it) */
#pragma once
void msg_schedule_text_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
