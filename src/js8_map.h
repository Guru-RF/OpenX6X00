/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  JS8 map: the world with a path from the station to each QSO of the log
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define JS8_MAP_NO_SNR INT16_MIN

/* A station for the map, from the app */
typedef struct {
    char    call[16];
    char    grid[8];     /* "" none: placed at the centre of its country (cty.dat), hollow */
    int16_t snr;         /* JS8_MAP_NO_SNR: unknown */
    time_t  t;           /* when heard, 0 never (from the log) */
    bool    heard_me;    /* they've sent us something */
    uint8_t data[64];    /* the app's own, back to it on the press */
} js8_map_station_t;

/* The stations to show now: up to max in out, the count back. Called again every few seconds */
typedef int (*js8_map_fill_t)(js8_map_station_t *out, int max);
/* MFK press on the selected station */
typedef void (*js8_map_pick_t)(const js8_map_station_t *st);

/* A w x h map on parent, centred on the station's longitude: coastlines, the station (my_grid),
 * the stations of fill (NULL none, else redrawn every few seconds) and those of the ADIF log at
 * log_path when log_spots, with a great-circle path to each one worked. The MFK selects a
 * station (west to east), its press calls pick (NULL nothing); js8_map_zoom() zooms */
lv_obj_t *js8_map_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, const char *my_grid, const char *log_path,
                         js8_map_fill_t fill, bool log_spots, js8_map_pick_t pick);

/* The open map, from the dialog: the main knob zooms (Auto, World, x2 ... x16), the MFK steps and presses */
void js8_map_zoom(int32_t diff);

/* Stations heard (FT8/FT4 decodes), from any thread; forgotten after 15 minutes. grid "" or a
 * report keeps the one heard before. data (up to 64 bytes) comes back to the pick */
void js8_map_heard(const char *call, const char *grid, int snr, bool to_me, const void *data, size_t size);
void js8_map_heard_clear(void);
/* The fill for them */
int  js8_map_heard_fill(js8_map_station_t *out, int max);
