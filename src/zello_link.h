/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's Simplex repeater: the channel linked to the frequency the radio is on. Its rules, apart
 *  from the GUI so a PC can test them (tools/zello); zello.c carries out what they say.
 *
 *  - Zello to RF: a message from the channel is transmitted (zello.c keys through tx_player's claim and the modem, as a
 *    voice message is sent) when the frequency is free (the squelch closed, nobody else on the air or about to be, our
 *    pause after the last one over), and unkeyed at its end, at the time limit, or the moment the operator keys. A
 *    message cut is not keyed again; one the radio stopped (refused where it may not transmit, SWR, a band stop) stops
 *    Zello to RF until it is switched on again.
 *  - RF to Zello: what the receiver hears with the squelch open goes to the channel as a message, ended when the
 *    squelch has closed (its hang time), at the time limit (then not again until the squelch has closed), or the
 *    moment the radio transmits, ours or anyone's: no loop.
 *  - The CW ID: at the end of a transmission when the last one is a period ago (the first transmission's too), and on
 *    its own once a period has gone by since the last one if the link transmitted since.
 *  - One thing at a time: never RF to Zello while the radio transmits, never a key while the squelch is open.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZELLO_LINK_HOLD_MS     500      /* after any transmission: nothing new (the TX's tail, the receiver back) */
#define ZELLO_LINK_HANG_MS     1500     /* the squelch closed this long: the message to Zello ends */

typedef struct {
    bool     to_rf;             /* Zello to RF */
    bool     to_zello;          /* RF to Zello */
    bool     id_on;             /* the CW ID */
    uint32_t id_period_ms;      /* the ID at least this often while the link transmits */
    uint32_t limit_ms;          /* a transmission's longest, and a message's to Zello */
    uint32_t pause_ms;          /* between the end of a transmission and the next relayed one */
} zello_link_cfg_t;

typedef enum {
    ZELLO_TX_DONE,              /* our transmission went and ended */
    ZELLO_TX_NOT_KEYED,         /* it did not start: the radio or the player taken meanwhile; nothing sent */
    ZELLO_TX_STOPPED,           /* the radio stopped it: refused there, the SWR protection, a band stop */
} zello_tx_result_t;

typedef struct {
    uint64_t          now_ms;
    bool              connected;    /* on the channel */
    bool              zello_msg;    /* a message from the channel to transmit: its talker on, or its audio not all out */
    bool              squelch_open; /* something on the frequency (zello_sql) */
    bool              radio_busy;   /* someone else on the air or about to be: the radio in TX not by us, the GUI's
                                     * PTT, a transmission holding the player, the user talking on Zello */
    bool              can_key;      /* Zello to RF may key: a voice mode, a callsign for the ID, the squelch set */
    bool              operator_key; /* the operator keyed (a PTT press, the radio's PTT on while we transmit) */
    bool              keyed;        /* our transmission's thread runs (keying, on the air, unkeying) */
    zello_tx_result_t result;       /* how it ended, read once keyed has gone false */
    bool              rx_ended;     /* the helper ended or refused the message to Zello */
} zello_link_in_t;

typedef enum {
    ZELLO_LINK_IDLE,
    ZELLO_LINK_RELAY,           /* on the air with the channel's message */
    ZELLO_LINK_ID,              /* on the air with the ID alone */
    ZELLO_LINK_ENDING,          /* told to unkey (an ID first, maybe): waiting for the end */
    ZELLO_LINK_TO_ZELLO,        /* the receiver to the channel */
} zello_link_state_t;

typedef struct {
    bool        key;            /* start a transmission: the channel's audio, or with id the ID alone */
    bool        unkey;          /* end it: the ID before unkeying with id */
    bool        now;            /* ... with what is queued of the message dropped (the operator, the time limit, the
                                 * link switched off), not played out to its end */
    bool        id;
    bool        talk_start;     /* a message to Zello from the receiver */
    bool        talk_stop;
    const char *why;            /* a word for the screen, or NULL */
} zello_link_out_t;

typedef struct {
    zello_link_state_t state;
    uint64_t           since_ms;        /* the state's start */
    uint64_t           air_end_ms;      /* our last transmission's end, 0 none */
    uint64_t           id_ms;           /* our last ID, 0 none */
    bool               tx_since_id;     /* transmitted since */
    bool               id_pending;      /* the transmission ending carries the ID */
    bool               cut;             /* the channel's message now was cut: not keyed again until it ends */
    bool               rx_cut;          /* the message to Zello was cut: not again until the squelch closes */
    bool               to_rf_stopped;   /* the radio stopped a transmission: Zello to RF off until switched on */
} zello_link_t;

void zello_link_init(zello_link_t *l);
/* Zello to RF on again after the radio stopped it (the user switched it on) */
void zello_link_rearm(zello_link_t *l);
zello_link_out_t zello_link_step(zello_link_t *l, const zello_link_cfg_t *c, const zello_link_in_t *in);

/* The squelch as the receiver's audio shows it: the base mutes its audio with the squelch closed, so open is audio
 * over ZELLO_SQL_OPEN for ZELLO_SQL_OPEN_MS, closed again under ZELLO_SQL_CLOSE for hang_ms */
#define ZELLO_SQL_OPEN      600         /* RMS of the 16-bit samples, about -35 dBFS */
#define ZELLO_SQL_CLOSE     300
#define ZELLO_SQL_OPEN_MS   60

typedef struct {
    bool     open;
    uint32_t loud_ms;
    uint32_t quiet_ms;
} zello_sql_t;

/* A block of ms with this RMS; the state after it */
bool zello_sql_step(zello_sql_t *s, int rms, uint32_t ms, uint32_t hang_ms);

/* The ID in Morse (A-Z, 0-9, / - ? . , and spaces; others left out) at wpm: its length in ms, and its tone into out
 * at rate (soft 5 ms edges), the samples written (max at most) */
uint32_t zello_morse_ms(const char *text, int wpm);
size_t   zello_morse_render(const char *text, int wpm, int tone_hz, int rate, float amp, int16_t *out, size_t max);

#ifdef __cplusplus
}
#endif
