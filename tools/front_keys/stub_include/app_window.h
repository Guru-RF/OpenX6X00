/* tools/front_keys: no window is open (the VOL press as ESC is for speech on, in a window) */
#pragma once
#include <stdbool.h>
typedef struct {
    bool run;
} dialog_t;
static inline bool dialog_is_run(void) {
    return false;
}
