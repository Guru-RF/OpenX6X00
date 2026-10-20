#pragma once
#include <stdbool.h>
#include <stdint.h>
bool main_screen_freq_locked(void);
bool main_screen_mode_locked(void);
void main_screen_set_freq(uint64_t freq);
