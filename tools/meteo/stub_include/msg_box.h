/* tools/meteo: the message box as meteo_alerts_test.c has it */
#pragma once
#include <stdint.h>
void msg_schedule_text_fmt(const char *fmt, ...);
void msg_schedule_text_ms(uint16_t ms, const char *fmt, ...);
