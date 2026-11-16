/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Simplex repeater of the internet voice apps (Zello, SVXConnect): the network linked to the frequency
 *  the radio is on. Its rules, apart from the GUI so a PC can test them (tools/zello, tools/svx); voice_net.c carries
 *  out what they say, for whichever app has the session. One set of rules for both: what keys for one keys for the
 *  other.
 *
 *  - Network to RF: a message from the network (a Zello channel, a reflector's talkgroup) is transmitted (voice_net.c
 *    keys through tx_player's claim and the modem, as a voice message is sent) when the frequency is free (the squelch
 *    closed, nobody else on the air or about to be, our pause after the last one over), and unkeyed at its end, at the
 *    time limit, or the moment the operator keys. A message cut is not keyed again; one the radio stopped (refused
 *    where it may not transmit, SWR, a band stop) stops network to RF until it is switched on again.
 *  - Only inside the amateur bands (ham_bands.h) or a transverter's range, whatever Settings > TX Cal > TX outside ham
 *    bands says and whatever the TX ranges allow outside them: the CB and HF Air Band apps tune outside the bands, and
 *    the link must never relay the internet onto those unattended. Out of them nothing keys, said once ("Link: amateur
 *    bands only"), and a transmission still on when the frequency leaves them is unkeyed at once.
 *  - RF to the network: what the receiver hears with the squelch open goes to the network as a message, ended when
 *    the squelch has closed (its hang time), at the time limit (then not again until the squelch has closed), or the
 *    moment the radio transmits, ours or anyone's: no loop. What the receiver hears inside the amateur bands or a
 *    transverter's range goes; outside them only while Settings > TX Cal > TX outside ham bands is on (MARS, CAP),
 *    so no airband, CB or broadcast audio reaches a channel or a talkgroup unless that licence is the operator's.
 *    With it off nothing goes, said once ("Link: amateur bands only, nothing to Zello"), and a message still going
 *    when the receiver leaves the bands ends at once. With it on it goes, and the link says so once when RF to the
 *    network starts out there or the receiver tunes out of the bands while it is on ("RF>Zello: outside the amateur
 *    bands, check your licence"), not at every message.
 *  - The CW ID: at the end of a transmission when the last one is a period ago (the first transmission's too), and on
 *    its own once a period has gone by since the last one if the link transmitted since.
 *  - One thing at a time: never RF to the network while the radio transmits, never a key while the squelch is open.
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

#define VLINK_HOLD_MS     500       /* after any transmission: nothing new (the TX's tail, the receiver back) */
#define VLINK_HANG_MS     1500      /* the squelch closed this long: the message to the network ends */

typedef struct {
    bool        to_rf;          /* the network to RF */
    bool        to_net;         /* RF to the network */
    bool        id_on;          /* the CW ID */
    uint32_t    id_period_ms;   /* the ID at least this often while the link transmits */
    uint32_t    limit_ms;       /* a transmission's longest, and a message's to the network */
    uint32_t    pause_ms;       /* between the end of a transmission and the next relayed one */
    const char *net;            /* the network's name in what the link says ("Zello", "SVX"); NULL "Net" */
    /* What the link says in the screen's language (i18n_tr: the format before the name goes in); NULL English */
    const char *(*tr)(const char *text);
} vlink_cfg_t;

typedef enum {
    VLINK_TX_DONE,              /* our transmission went and ended */
    VLINK_TX_NOT_KEYED,         /* it did not start: the radio or the player taken meanwhile; nothing sent */
    VLINK_TX_STOPPED,           /* the radio stopped it: refused there, the SWR protection, a band stop */
} vlink_tx_result_t;

typedef struct {
    uint64_t          now_ms;
    bool              connected;    /* on the network (a channel, a reflector) */
    bool              net_msg;      /* a message from the network to transmit: its talker on, or its audio not all out */
    bool              squelch_open; /* something on the frequency (vlink_sql) */
    bool              radio_busy;   /* someone else on the air or about to be: the radio in TX not by us, the GUI's
                                     * PTT, a transmission holding the player, the user talking on the network */
    bool              can_key;      /* network to RF may key: a voice mode, a callsign for the ID, the squelch set */
    bool              ham_band;     /* the radio would transmit inside the amateur bands or a transverter's range */
    bool              rx_ham_band;  /* the receiver listens inside them (vlink_rx_band): RF to the network may go */
    bool              rx_outside_ok;    /* TX outside ham bands on (MARS, CAP): RF to the network goes outside them
                                         * too */
    bool              operator_key; /* the operator keyed (a PTT press, the radio's PTT on while we transmit) */
    bool              keyed;        /* our transmission's thread runs (keying, on the air, unkeying) */
    vlink_tx_result_t result;       /* how it ended, read once keyed has gone false */
    bool              rx_ended;     /* the helper ended or refused the message to the network */
} vlink_in_t;

typedef enum {
    VLINK_IDLE,
    VLINK_RELAY,                /* on the air with the network's message */
    VLINK_ID,                   /* on the air with the ID alone */
    VLINK_ENDING,               /* told to unkey (an ID first, maybe): waiting for the end */
    VLINK_TO_NET,               /* the receiver to the network */
} vlink_state_t;

typedef struct {
    bool        key;            /* start a transmission: the network's audio, or with id the ID alone */
    bool        unkey;          /* end it: the ID before unkeying with id */
    bool        now;            /* ... with what is queued of the message dropped (the operator, the time limit, the
                                 * link switched off), not played out to its end */
    bool        id;
    bool        talk_start;     /* a message to the network from the receiver */
    bool        talk_stop;
    const char *why;            /* words for the screen, or NULL; valid until the next step */
} vlink_out_t;

typedef struct {
    vlink_state_t state;
    uint64_t      since_ms;         /* the state's start */
    uint64_t      air_end_ms;       /* our last transmission's end, 0 none */
    uint64_t      id_ms;            /* our last ID, 0 none */
    bool          tx_since_id;      /* transmitted since */
    bool          id_pending;       /* the transmission ending carries the ID */
    bool          cut;              /* the network's message now was cut: not keyed again until it ends */
    bool          rx_cut;           /* the message to the network was cut: not again until the squelch closes */
    bool          to_rf_stopped;    /* the radio stopped a transmission: network to RF off until switched on */
    bool          band_told;        /* "amateur bands only" said: not again until back inside them */
    bool          rx_band_told;     /* the same for RF to the network, the receiver's frequency */
    bool          rx_outside_told;  /* "outside the amateur bands, check your licence" said: not again until RF to
                                     * the network is off or back inside them */
    char          why[96];
} vlink_t;

void vlink_init(vlink_t *l);
/* Network to RF on again after the radio stopped it (the user switched it on) */
void vlink_rearm(vlink_t *l);
vlink_out_t vlink_step(vlink_t *l, const vlink_cfg_t *c, const vlink_in_t *in);

/* The receiver's frequency hz is inside the amateur bands (ham_bands.h) or a transverter's range (xvtr: a transverter
 * that is on covers hz): RF to the network goes. Outside them (the CB, HF Air Band and Flights' frequencies) it goes
 * only with TX outside ham bands on (vlink_in_t.rx_outside_ok), whatever the TX ranges allow */
bool vlink_rx_band(int32_t hz, bool xvtr);

/* The squelch as the receiver's audio shows it: the base mutes its audio with the squelch closed, so open is audio
 * over VLINK_SQL_OPEN for VLINK_SQL_OPEN_MS, closed again under VLINK_SQL_CLOSE for hang_ms */
#define VLINK_SQL_OPEN      600     /* RMS of the 16-bit samples, about -35 dBFS */
#define VLINK_SQL_CLOSE     300
#define VLINK_SQL_OPEN_MS   60

typedef struct {
    bool     open;
    uint32_t loud_ms;
    uint32_t quiet_ms;
} vlink_sql_t;

/* A block of ms with this RMS; the state after it */
bool vlink_sql_step(vlink_sql_t *s, int rms, uint32_t ms, uint32_t hang_ms);

/* The ID in Morse (A-Z, 0-9, / - ? . , and spaces; others left out) at wpm: its length in ms, and its tone into out
 * at rate (soft 5 ms edges), the samples written (max at most) */
uint32_t vlink_morse_ms(const char *text, int wpm);
size_t   vlink_morse_render(const char *text, int wpm, int tone_hz, int rate, float amp, int16_t *out, size_t max);

#ifdef __cplusplus
}
#endif
