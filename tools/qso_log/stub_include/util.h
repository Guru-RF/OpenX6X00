/* tools/qso_log: the one helper of src/util.cpp qso_log.c takes, cut out of it into build/canon.c */
#pragma once
#include <stdbool.h>
char *util_canonize_callsign(const char *callsign, bool strip_slashes);
