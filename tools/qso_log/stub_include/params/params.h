/* tools/qso_log: the callsign setting, the one setting qso_log.c reads */
#pragma once
typedef struct {
    struct {
        char x[16];
    } callsign;
} params_t;
extern params_t params;
