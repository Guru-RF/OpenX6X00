#pragma once

#include <stdbool.h>
#include <stdint.h>

bool is_cq_modifier(const char *text);

/* The QTH's square as an FT8 message carries it, into out (5 bytes): "JO21" from "jo21cv" or "JO21CV"; empty when
 * the QTH has none ("JO", nothing set). The settings take a locator in either case, and of 2 characters too */
void ftx_qth_square(const char *qth, char *out);

/* The time sec.nsec (CLOCK_REALTIME) from the nearest slot start, slots of slot seconds counted
 * from the minute: -slot/2 .. +slot/2, in nanoseconds, so FT4's 7.5 s and FT2's 3.75 s slots
 * start on their fractions of a second */
int64_t ftx_slot_offset_ns(int64_t sec, long nsec, float slot);

/* The kernel's clock discipline says ntpd keeps the clock (from the network, or from the GPS
 * through its SHM driver), synchronised with a maximum error of 1 s at most; *err_ms gets it */
bool ftx_clock_synced(int *err_ms);
