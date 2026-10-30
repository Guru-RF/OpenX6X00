/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: the activation log, one ADIF file per activation
 *  (MY_SOTA_REF, MY_SIG POTA / MY_POTA_REF, MY_SIG WWBOTA, HOTA, WWFF, TOTA, WCA, MOTA, ARLHS or TILES / MY_SIG_INFO;
 *  a Tiles activation's tile in MY_GRIDSQUARE too)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    time_t  t;
    char    call[16];
    int32_t freq_hz;
    char    mode[8];    /* CW, SSB, AM, FM, FT8, FT4, JS8, PSK31, OLIVIA, JTTY, RTTY...; "" not known. One that
                         * ADIF has as a SUBMODE (FT4, PSK31, JTTY) goes in under its MODE (field_adif_record) */
    char    rst_s[4], rst_r[4];
    char    name[48], qth[48], grid[8], country[32];   /* the other station, from a logbook lookup; "" unknown */
    char    their_ref[16];  /* the other station's reference: park to park, summit to summit...; "" none */
} field_qso_t;

typedef struct {
    char call[16];
    char grid[8];
    char prog;          /* 'S' SOTA, 'P' POTA, 'B' WWBOTA, 'H' HOTA, 'F' WWFF, 'T' TOTA, 'C' WCA, 'M' MOTA, 'L' LH,
                         * 'G' Tiles */
    char ref[16];
} field_station_t;

/* The programme of a reference by its shape: 'S' SOTA (I/LO-123), 'P' POTA (US-1234), 'F' WWFF (DLFF-0123),
 * 'B' WWBOTA (B/G-0070), 'H' HOTA (RO-H0390), 'T' TOTA (OKR-0150), 'C' WCA (DL-01156), 'M' MOTA (X42018),
 * 'L' a lighthouse's ARLHS number (BEL 002, also BEL-002, USA 1286, AUS 079H), 'G' a Tiles tile (a 6-character
 * locator, JO20ab, in either case); 0 when it is none of them */
char field_ref_prog(const char *ref);

/* A tile as Tiles on the Air writes it, "JO20ab" (the field in capitals, the subsquare in small letters), from a
 * 6-character locator in either case; false when in is not one */
bool field_ref_tile(const char *in, char *out, size_t size);

/* A lighthouse's ARLHS number as the list writes it, "BEL 002", from "BEL-002", "bel 2", "BEL002", "ARLHS BEL 002"
 * or "LH BEL-2"; false when in is not one */
bool field_ref_lh(const char *in, char *out, size_t size);

/* The ADIF SIG of a programme ("POTA", "WWBOTA"...), NULL for SOTA (its own SOTA_REF fields) and unknown letters */
const char *field_prog_sig(char prog);

/* "S2S", "P2P", "F2F", "B2B", "H2H", "T2T", "C2C", "M2M", "L2L", "T3" (tile to tile) for a QSO with their reference,
 * "" without */
const char *field_qso_tag(const field_qso_t *q);

/* The mode a QSO logged by hand goes into the log with (field_qso_t.mode). The digital app open names it: app as
 * main_screen_digital_app() gives it ("PSK31", "Olivia", "JTTY", "RTTY", "FT4"...), NULL for none; in CW and CW-R it
 * is CW all the same. Without one, the radio's mode (radio_mode_t) does: CW, AM, FM, SSB, and FT8 or FT4 on their
 * dial frequencies (hz) in USB, LSB and the data modes. "" when it is not known: a data mode with no app open, away
 * from FT8 and FT4. ADIF has no mode for "some data mode", and the program sending through TCI or the USB audio is
 * not known */
const char *field_qso_mode(const char *app, int mode, int32_t hz);

/* ADIF band ("20m"), "" outside the amateur bands */
const char *field_band(int32_t hz);

/* One ADIF record (with <eor>) into buf; returns its length */
size_t field_adif_record(char *buf, size_t size, const field_qso_t *q, const field_station_t *st);

/* Appends the QSO (a header first when the file is new) */
bool field_log_add(const char *path, const field_qso_t *q, const field_station_t *st);

/* What went wrong with field_log_add, from its errno, in words for the operator */
const char *field_log_error(int err);

/* The QSOs of the file, oldest first, the newest max of them when there are more; returns the
 * count in the file (0 when missing), which may be more than max */
int field_log_read(const char *path, field_qso_t *out, int max);

#ifdef __cplusplus
}
#endif
