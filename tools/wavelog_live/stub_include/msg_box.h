/* Host test shim of src/msg_box.h: the note on the screen (the test keeps it) */
#pragma once
void msg_update_text_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
