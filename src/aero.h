/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The HF aeronautical frequencies: the oceanic and remote air routes (MWARA), the VOLMET weather broadcasts, the two
 *  common channels and the worldwide allotments of the airlines' own operational control (LDOC), for the HF Air Band
 *  app (dialog_aero.c) and the Scanner. Carrier (reference) frequencies in kHz: the radio listens in USB with its dial
 *  on them. The VHF airband (118-137 MHz) is the X6200's, in the Scanner. None of these is in an amateur band:
 *  cfg_tx_refusal decides TX there as anywhere else (refused unless Settings > TX Cal > TX outside ham bands is on).
 *
 *  No LVGL and no radio here (host test: tools/aerocb). Where the lists come from: aero.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AERO_VOLMET,        /* weather for aircraft, broadcast */
    AERO_MWARA,         /* air traffic on a major world air route area */
    AERO_COMMON,        /* 3023 and 5680 kHz, worldwide: search and rescue too */
    AERO_LDOC,          /* the airlines' operational control, worldwide */
} aero_kind_t;

/* What the app shows: the routes and weather heard from one part of the world, the LDOC list, or all */
typedef enum {
    AERO_AREA_ALL  = 0,
    AERO_AREA_EUR  = 1,     /* Europe, Africa, Middle East (IARU Region 1) */
    AERO_AREA_AMER = 2,     /* the Americas (Region 2) */
    AERO_AREA_ASIA = 3,     /* Asia and the Pacific (Region 3) */
    AERO_AREA_LDOC = 4,     /* the worldwide LDOC allotments */
    AERO_AREAS,
} aero_area_t;

#define AERO_IN(area) (1u << (area))

typedef struct {
    const char     *code;   /* "NAT", "VOLMET EUR" */
    const char     *name;   /* "North Atlantic", English (the screen translates it) */
    uint8_t         kind;   /* aero_kind_t */
    uint8_t         areas;  /* AERO_IN() of each area it is heard in */
    const uint16_t *khz;    /* ascending, ended by 0 */
} aero_family_t;

int                  aero_families(void);
const aero_family_t *aero_family(int i);

/* Family f is in the list of area */
bool aero_shown(const aero_family_t *f, int area);

/* "Europe, Africa, Middle East" (English) */
const char *aero_area_name(int area);

/* The area a radio starts with, by its IARU region */
int aero_area_for_region(int region);

/* The rows the list of area has, in their order: family index and kHz into fam[] and khz[], at most max; the count */
int aero_rows(int area, int16_t *fam, uint16_t *khz, int max);

#ifdef __cplusplus
}
#endif
