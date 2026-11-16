/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The Citizens Band (CB, 11 m) channel plans: each plan's channels, their frequencies and the modes allowed on them,
 *  for the CB app (dialog_cb.c) and the Scanner's CB list. Nothing here keys anything: all of it is outside the
 *  amateur bands, where cfg_tx_refusal decides TX as anywhere else (refused unless Settings > TX Cal > TX outside ham
 *  bands is on). The modes are those the plan allows to transmit in, so a set that may is in the right one.
 *
 *  No LVGL and no radio here (host test: tools/aerocb). Where each plan comes from: cb.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CB_AM,
    CB_FM,
    CB_USB,
    CB_LSB,
    CB_MODES,
} cb_mode_t;

#define CB_MODE_BIT(m) (1u << (m))

typedef enum {
    CB_PLAN_CEPT,       /* Europe: ECC/DEC/(11)03, the 40 channels in FM, AM and SSB */
    CB_PLAN_DE,         /* Germany: those 40 and 41-80 below them, FM only there */
    CB_PLAN_UK,         /* United Kingdom 27/81: 40 channels of its own, FM only */
    CB_PLAN_US,         /* USA and the Americas: the FCC's 40, AM first */
    CB_PLAN_AU,         /* Australia: the same 40 in AM and SSB, no FM */
    CB_PLAN_PL,         /* Poland: the 40 a 5 kHz lower, as Polish sets have them */
    CB_PLAN_A,          /* the export grid: the 40 moved 900 kHz down */
    CB_PLAN_B,          /* 450 kHz down */
    CB_PLAN_D,          /* 450 kHz up */
    CB_PLAN_E,          /* 900 kHz up, the channels below 10 m only */
    CB_PLANS,
} cb_plan_t;

/* How many channels plan has (1 to that); 0 for no such plan */
int cb_channels(int plan);

/* Channel ch of plan, Hz; 0 for none */
int32_t cb_freq(int plan, int ch);

/* "Europe (CEPT)", English (the screen translates it) */
const char *cb_plan_name(int plan);

/* The modes allowed somewhere on the plan (CB_MODE_BIT), and its first */
unsigned  cb_plan_modes(int plan);
cb_mode_t cb_plan_mode(int plan);

/* The modes allowed on channel ch, and the mode it is listened to in: want where allowed there, else the channel's own
 * (Germany's 41-80 FM, Australia's 11 AM and 16 LSB) */
unsigned  cb_channel_modes(int plan, int ch);
cb_mode_t cb_channel_mode(int plan, int ch, cb_mode_t want);

/* What a channel is kept for, "Emergency", "" none (English) */
const char *cb_channel_note(int plan, int ch);

/* The channel of plan within 1 kHz of hz, 0 none */
int cb_channel_at(int plan, int32_t hz);

/* hz is among the plan's channels: from half a channel below the lowest to half a channel above the highest */
bool cb_in_plan(int plan, int32_t hz);

/* steps channels on from hz, by channel number as a CB set's knob goes (22, 23 at 27.255, then 24 at 27.235), stopping
 * at the first and the last. Off a channel the first step is the nearest one that way. hz again when there is none */
int32_t cb_step(int plan, int32_t hz, int steps);

/* The plan a radio starts with: by its IARU region (1 Europe, 2 the Americas, 3 Australia's), or by its country
 * (ISO 3166 alpha-2, "DE"), -1 for a country without a plan of its own here (the region's then) */
int cb_plan_for_region(int region);
int cb_plan_for_country(const char *cc);

/* "CB 19", "UK 19", "PL 19", "CB D-19": a channel's name for a memory channel or the Scanner */
const char *cb_name(int plan, int ch, char *buf, size_t size);

/* "27.185" or "27.60125" (MHz) */
const char *cb_freq_text(int32_t hz, char *buf, size_t size);

/* "AM", "FM", "USB", "LSB" */
const char *cb_mode_name(cb_mode_t mode);

/* A CB callsign, as the CB clubs number their members and WSJT-CB (the 27 MHz edition of the desktop FT8 program)
 * takes them, in either case: 1 to 3 digits (the club's or the country's number), 1 or 2 letters, 1 to 3 digits
 * (4 after a single first digit): 26AT101, 1AT106, 999ZZ999, 1AT1000; or the digits and letters, a stroke and 2
 * letters: 999ZZ/ZZ. No amateur call has these forms: theirs have letters after the digit of their prefix
 * (ON6URE, 2E0ABC, 4X1AB). A CB call has no DXCC country, and the amateur services (the DX clusters, the RBN, the
 * activation programmes, Wavelog) do not get it */
bool cb_callsign(const char *call);

#ifdef __cplusplus
}
#endif
