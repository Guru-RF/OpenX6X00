#pragma once

#include <stdbool.h>
#include <stdint.h>

bool is_cq_modifier(const char *text);

/* The time sec.nsec (CLOCK_REALTIME) from the nearest slot start, slots of slot seconds counted
 * from the minute: -slot/2 .. +slot/2, in nanoseconds, so FT4's 7.5 s and FT2's 3.75 s slots
 * start on their fractions of a second */
int64_t ftx_slot_offset_ns(int64_t sec, long nsec, float slot);

/* The kernel's clock discipline says ntpd keeps the clock (from the network, or from the GPS
 * through its SHM driver), synchronised with a maximum error of 1 s at most; *err_ms gets it */
bool ftx_clock_synced(int *err_ms);
