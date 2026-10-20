/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 operating helpers for the dialog: canned
 *  messages, heartbeats, heard stations, automatic replies, QSO log,
 *  inbox and held messages
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "js8_receive.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-press messages to the selected station, in menu order. */
typedef enum {        /* values index the menu */
    JS8_Q_SNR_Q = 0,  /* "W1ABC SNR?" */
    JS8_Q_SEND_SNR,   /* "W1ABC SNR -12" */
    JS8_Q_GRID_Q,     /* "W1ABC GRID?" */
    JS8_Q_MY_GRID,    /* "W1ABC GRID JN35AB" */
    JS8_Q_INFO_Q,     /* "W1ABC INFO?" */
    JS8_Q_STATUS_Q,   /* "W1ABC STATUS?" */
    JS8_Q_HEARING_Q,  /* "W1ABC HEARING?" */
    JS8_Q_AGN_Q,      /* "W1ABC AGN?" */
    JS8_Q_RR,         /* "W1ABC RR" */
    JS8_Q_73,         /* "W1ABC 73" */
    JS8_Q_COUNT,      /* number of entries above */
} js8_query_t;

/* The menu text for one entry ("SNR?", "Send SNR", ...); "" if out of range. */
const char *js8_query_label(js8_query_t which);

/* The message for one entry, addressed to to_call. False, with "" written,
 * when a value it needs (the call, a usable SNR, our grid) is missing. */
bool js8_query_text(js8_query_t which, const char *to_call, int their_snr, const char *my_grid,
                    char *out, unsigned out_len);

/* Our heartbeat in the desktop form: "IU1ABC: HEARTBEAT JN35". */
void js8_heartbeat_text(const char *call, const char *grid, char *out, unsigned out_len);

/* An audio offset for the heartbeat between 500 and 1000 Hz that keeps 50 Hz
 * clear of every signal heard in the last 30 seconds, picked at random.
 * offsets_hz[i] was last heard at heard_ms[i]. */
int js8_heartbeat_offset(const float *offsets_hz, const int64_t *heard_ms, unsigned count, int64_t now_ms);

/* ---- Heard stations ---- */

/* One row of the station list. Copied around as plain memory, so the
 * member order is fixed. */
typedef struct {
    char    call[JS8_RX_CALL_LEN];    /* as heard, possibly compound */
    char    grid[8];                  /* last grid heard, at most 6 characters */
    int64_t heard_ms;                 /* last decode, wall clock */
    int16_t snr;                      /* our report of them */
    float   freq_hz;                  /* their audio offset */
    uint8_t submode;                  /* their last speed: 0 Normal, 1 Fast, 2 Turbo, 4 Slow */
    bool    heard_me;                 /* they have called us: shown with a star */
    int64_t heard_me_ms;              /* when they last did */
    bool    has_reported_snr;         /* reported_snr below is valid */
    int16_t reported_snr;             /* their report of us */
    bool    x6100;                    /* they have sent something to @X6100 */
} js8_station_t;

typedef struct js8_stations js8_stations_t; /* opaque */

js8_stations_t *js8_stations_create(void); /* NULL when out of memory */
void js8_stations_add(js8_stations_t *s, const js8_rx_msg_t *msg, const char *my_call, int64_t now_ms);
/* Copies at most max rows into out: stations that called us come first,
 * the rest follow by last heard. Returns how many were copied. */
int js8_stations_list(js8_stations_t *s, int64_t now_ms, js8_station_t *out, int max);
void js8_stations_clear(js8_stations_t *s);

/* ---- Automatic replies and heartbeat timing ---- */

typedef struct js8_held js8_held_t;   /* opaque, see "Held messages" */
typedef struct js8_inbox js8_inbox_t; /* opaque, see "Inbox" */

/* Room for a relay path such as "K2XYZ>N0XYZ". */
#define JS8_PATH_LEN 48

/* The operator's switches and station details, read on every message. */
typedef struct {
    bool        autoreply;            /* AUTO switch */
    bool        heartbeat;            /* HB switch */
    bool        hb_ack;               /* HB ACK switch, only honoured with AUTO and HB both on */
    bool        relay;                /* relay ">" traffic and store MSG TO: for others */
    const char *my_call, *my_grid, *info, *status; /* NULL counts as empty */
    const char *groups;               /* our groups as "@ONE @TWO", NULL for none */
    js8_held_t *held;                 /* store for messages held for others, NULL for none */
} js8_auto_settings_t;

/* A recently heard station, as HEARING?, QUERY CALL and RETRIEVE MSG need it. */
typedef struct {
    const char *call;                 /* entries with NULL here are skipped */
    int         snr;                  /* how we heard it */
    int64_t     heard_ms;             /* when */
} js8_heard_t;

typedef enum {            /* what to do with a reply */
    JS8_AUTO_IGNORE = 0,  /* nothing to do */
    JS8_AUTO_SEND,        /* transmit it now */
    JS8_AUTO_OFFER,       /* AUTO is off: put it in front of the operator instead */
} js8_auto_action_t;

typedef enum {            /* why a reply is sent */
    JS8_REPLY_QUERY = 0,  /* answers a question to us */
    JS8_REPLY_HB_ACK,     /* acknowledges a heartbeat */
    JS8_REPLY_ACK,        /* confirms a message stored here, or the end of a relay */
    JS8_REPLY_SUGGEST,    /* never sent by itself, only proposed */
    JS8_REPLY_RELAY,      /* forwards a relay to its next hop */
    JS8_REPLY_STORED,     /* about held messages (YES MSG ID / NO, the message) or QUERY CALL */
} js8_reply_kind_t;

/* What to send back. Plain memory: member order is fixed. */
typedef struct {
    js8_auto_action_t action;                              /* JS8_AUTO_IGNORE: the rest is empty */
    js8_reply_kind_t  kind;                                /* what sort of reply */
    bool              hb_ack;                              /* goes out in the heartbeat sub-band */
    bool              allcall;                             /* replies to @ALLCALL, which is rate limited */
    char              text[JS8_RX_TEXT_LEN];               /* the message to transmit */
    char              to[JS8_RX_CALL_LEN];                 /* the station it answers */
    char              command[16];                         /* the command answered, e.g. "SNR?" */
    int               deliver_id;                          /* held message this hands over, 0 for none;
                                                              call js8_held_delivered() after sending */
    char              deliver_group_call[JS8_RX_CALL_LEN]; /* for a group message, the station fetching it */
} js8_auto_result_t;

typedef enum { JS8_STORED_NONE = 0, JS8_STORED_INBOX, JS8_STORED_HELD } js8_stored_kind_t;

/* A message js8_process() put in the inbox or the held store. */
typedef struct {
    js8_stored_kind_t kind;                   /* JS8_STORED_NONE: nothing was stored */
    int               id;                     /* -1 when the file write failed; the copy in memory stays */
    bool              resend;                 /* already stored: a repeat, not a new message */
    char              from[JS8_RX_CALL_LEN];  /* sender */
    char              to[JS8_RX_CALL_LEN];    /* recipient */
    char              path[JS8_PATH_LEN];     /* route it came by */
    char              text[JS8_RX_TEXT_LEN];  /* message body */
} js8_stored_t;

typedef struct js8_auto js8_auto_t; /* opaque: the rate limits */

js8_auto_t *js8_auto_create(void); /* NULL when out of memory */
void js8_auto_destroy(js8_auto_t *a);

/* Handles one received message the way desktop JS8Call does: a MSG to us
 * lands in inbox, a MSG TO: for someone else in s->held (reported in
 * stored), and out gets the reply to queue or offer, if any. heard lists
 * recent stations, newest first; last_tx is what we sent last (for AGN?).
 * stored and out may each be NULL. */
void js8_process(js8_auto_t *a, const js8_rx_msg_t *msg, const js8_auto_settings_t *s,
                 const js8_heard_t *heard, unsigned n_heard, const char *last_tx, int64_t now_ms,
                 js8_inbox_t *inbox, js8_stored_t *stored, js8_auto_result_t *out);
/* Tells the rate limiter that reply went out. */
void js8_auto_sent(js8_auto_t *a, const js8_auto_result_t *reply, int64_t now_ms);
/* The operator did something on the radio. After an hour of nothing,
 * automatic transmissions stop. */
void js8_auto_user_activity(js8_auto_t *a, int64_t when_ms);
bool js8_auto_idle(js8_auto_t *a, int64_t when_ms);

/* True when msg opens a QSO with us. */
bool js8_starts_qso(const js8_rx_msg_t *rx);

/* When the next heartbeat is due, as desktop schedules it; the interval is
 * kept within JS8_HB_MIN_INTERVAL..JS8_HB_MAX_INTERVAL minutes. */
int64_t js8_next_heartbeat_ms(int64_t now_ms, int minutes);

/* One decode as Time Sync uses it: who sent it, when it was decoded, the
 * drift that would have put it on time (js8_rx_msg_t.drift_ms) and the
 * slot length of its speed. */
typedef struct {
    char    call[JS8_RX_CALL_LEN];    /* "" when unknown: counted, but not as a station */
    int64_t when_ms;                  /* 0 marks an unused entry */
    int32_t drift_ms;                 /* suggested drift */
    int32_t period_ms;                /* slot length; 0 or less means 15 s */
} js8_sync_sample_t;

#define JS8_SYNC_MIN_DECODES  3 /* fewer decodes than this: no answer */
#define JS8_SYNC_MIN_STATIONS 3 /* from this many stations up, one vote each */

/* The drift Time Sync should apply, as desktop JS8Call works it out. Each
 * suggestion is folded into the slot nearest current_ms, since a drift is
 * only known modulo the slot length. Samples with when_ms 0, in the future
 * or older than window_ms are ignored. With JS8_SYNC_MIN_STATIONS or more
 * stations the answer is the median of each station's newest decode, so a
 * single busy station cannot dominate; otherwise it is the median of all
 * decodes. Returns false below JS8_SYNC_MIN_DECODES decodes or when
 * drift_ms is NULL. decodes and stations, if not NULL, get the counts. */
bool js8_sync_drift(const js8_sync_sample_t *s, unsigned n, int64_t now_ms, int64_t window_ms,
                    int64_t current_ms, int64_t *drift_ms, unsigned *decodes, unsigned *stations);

/* Maidenhead locator of lat/lon with 2, 4, 6, 8 or 10 characters (10 is a
 * box of roughly 20 x 35 m), letters in capitals. False when the position
 * or the length is out of range or out is too small. */
bool js8_latlon_to_grid(double lat_deg, double lon_deg, int chars, char *out, unsigned out_size);

/* ---- QSO log ---- */

/* What we know about the QSO with one station. */
typedef struct {
    char    call[JS8_RX_CALL_LEN];                    /* the other station */
    char    grid[8];                                  /* best grid they gave, up to 6 characters, or "" */
    int64_t start_ms;                                 /* first directed message in either direction */
    bool    has_sent_snr, has_rcvd_snr, has_heard_snr; /* which of the three reports are known */
    int16_t sent_snr;                                 /* our report to them */
    int16_t rcvd_snr;                                 /* their report to us */
    int16_t heard_snr;                                /* our last decode of them */
    bool    two_way;                                  /* both ends have sent; heartbeat acks excluded */
} js8_qso_t;

typedef struct js8_qsos js8_qsos_t; /* opaque */

js8_qsos_t *js8_qsos_create(void); /* NULL when out of memory */
void js8_qsos_destroy(js8_qsos_t *q);
/* Feed a received message, or one of ours ("MYCALL: W1ABC ...") as it is
 * sent. True, with the station in ended, when it is the 73 or SK closing a
 * two-way QSO that has not been offered for logging yet. */
bool js8_qsos_received(js8_qsos_t *q, const js8_rx_msg_t *msg, const char *my_call, int64_t now_ms,
                       char *ended, unsigned ended_len);
bool js8_qsos_sent(js8_qsos_t *q, const char *text, const char *my_call, int64_t now_ms,
                   char *ended, unsigned ended_len);
bool js8_qsos_get(js8_qsos_t *q, const char *station, int64_t now_ms, js8_qso_t *out);
void js8_qsos_logged(js8_qsos_t *q, const char *station);

/* A contact to log. Empty strings and a zero tx_pwr_w are not written. */
typedef struct {
    char     call[JS8_RX_CALL_LEN];    /* CALL */
    char     grid[12];                 /* GRIDSQUARE */
    char     name[64];                 /* NAME */
    char     comment[128];             /* COMMENT */
    char     rst_sent[8], rst_rcvd[8]; /* RST_SENT, RST_RCVD */
    int64_t  on_ms, off_ms;            /* QSO start and end, UTC */
    uint64_t freq_hz;                  /* dial frequency plus audio offset */
    char     my_call[JS8_RX_CALL_LEN]; /* STATION_CALLSIGN and OPERATOR */
    char     my_grid[12];              /* MY_GRIDSQUARE */
    float    tx_pwr_w;                 /* TX_PWR */
    char     pota_ref[16];             /* written as MY_SIG POTA and MY_SIG_INFO */
    char     sota_ref[24];             /* written as MY_SOTA_REF */
} js8_log_entry_t;

/* Adds e to the ADIF file at path as desktop JS8Call writes it (MODE MFSK,
 * SUBMODE JS8), starting the file with a header when it is new. On failure
 * returns false and puts the reason in err. */
bool js8_log_append(const char *path, const js8_log_entry_t *e, char *err, unsigned err_size);
/* Band name such as "40m"; "" when the frequency is in no band. */
const char *js8_log_band(uint64_t hz);

/* ---- Inbox ---- */

typedef struct {
    int     id;                        /* stable while the message exists */
    int64_t utc_ms;                    /* when it was received */
    char    from[JS8_RX_CALL_LEN];     /* sender */
    char    to[JS8_RX_CALL_LEN];       /* our call or one of our groups */
    char    path[JS8_PATH_LEN];        /* route back: "K2XYZ>N0XYZ" when relayed, else the sender */
    char    text[JS8_RX_TEXT_LEN];     /* message body */
    bool    read;                      /* opened by the operator */
} js8_inbox_msg_t;

/* Opens the inbox kept in the file at path (no file: empty inbox). Every
 * change is written back at once. */
js8_inbox_t *js8_inbox_open(const char *file);
void js8_inbox_close(js8_inbox_t *b);
/* Stores a message and returns its id; the same text again within 30
 * minutes returns the first id. -1 when the file could not be written
 * (the message is kept in memory anyway). */
int js8_inbox_add(js8_inbox_t *b, const char *from, const char *text, int64_t utc_ms);
/* Copies at most max messages, newest first; returns how many. */
int js8_inbox_list(js8_inbox_t *b, js8_inbox_msg_t *out, int max);
bool js8_inbox_get(js8_inbox_t *b, int msg_id, js8_inbox_msg_t *out);
void js8_inbox_mark_read(js8_inbox_t *b, int msg_id);
void js8_inbox_delete(js8_inbox_t *b, int msg_id);
int js8_inbox_unread(js8_inbox_t *b);
int js8_inbox_count(js8_inbox_t *b);

/* "MSG TO:" messages kept here for other stations until they ask for them
 * with QUERY MSGS or QUERY MSG n, as desktop JS8Call does. */
typedef struct {
    int     id;                        /* stable while the message exists */
    int64_t utc_ms;                    /* when it was received */
    char    from[JS8_RX_CALL_LEN];     /* sender */
    char    to[JS8_RX_CALL_LEN];       /* base callsign, or @GROUP */
    char    path[JS8_PATH_LEN];        /* route it came by */
    char    text[JS8_RX_TEXT_LEN];     /* message body */
    bool    delivered;                 /* fetched by its recipient */
    bool    group;                     /* @GROUP message: every member can fetch it for two days */
    int     got;                       /* @GROUP message: number of stations that fetched it */
} js8_held_msg_t;

js8_held_t *js8_held_open(const char *file);
void js8_held_close(js8_held_t *h);
/* Stores a message for to and returns its id (a repeat returns the first
 * id), or -1 when the file could not be written. */
int js8_held_add(js8_held_t *h, const char *from, const char *to, const char *text, int64_t utc_ms);
/* Copies at most max messages, newest first; returns how many. */
int js8_held_list(js8_held_t *h, js8_held_msg_t *out, int max);
bool js8_held_get(js8_held_t *h, int msg_id, js8_held_msg_t *out);
void js8_held_delivered(js8_held_t *h, int msg_id);
/* Records that station fetched group message msg_id. */
void js8_held_group_delivered(js8_held_t *h, int msg_id, const char *station);
/* Desktop's reminder for stored messages: when a station we hold mail for
 * was heard in the last 15 minutes and has not been reminded for 8 hours,
 * writes "W1ABC RETRIEVE MSG 3" to text, marks it reminded and returns true. */
bool js8_held_push_due(js8_held_t *h, const js8_heard_t *heard, unsigned n_heard, int64_t now_ms,
                       char *text, unsigned text_len);
void js8_held_delete(js8_held_t *h, int msg_id);
/* Messages still waiting to be fetched. */
int js8_held_waiting(js8_held_t *h);
int js8_held_count(js8_held_t *h);
/* "FROM: MYCALL MSG TO:W1ABC text" with a good checksum: writes the
 * recipient to to and the message to text. */
bool js8_msg_to_for_me(const js8_rx_msg_t *msg, const char *my_call, char *to, unsigned to_len,
                       char *text, unsigned text_len);

/* "FROM: MYCALL MSG text" with a good checksum, meant for the inbox:
 * writes the message to out. */
bool js8_msg_for_me(const js8_rx_msg_t *msg, const char *my_call, char *out, unsigned out_size);

/* A path for display, desktop style: "K2XYZ>N0XYZ" becomes "N0XYZ via K2XYZ". */
void js8_path_display(const char *path, char *out, unsigned out_size);
/* Reads the tail of a delivered message, "... FROM N0XYZ [NEXT MSG ID 4
 * [+2]]": the first sender into from, the following id (0 for none) into
 * next_id. */
bool js8_delivered_signature(const char *text, char *from, unsigned from_size, int *next_id);
/* Cleans up the groups field: capitals, '@' in front, duplicates dropped,
 * no more than 10, joined as "@ONE @TWO". */
void js8_groups_normalise(const char *typed, char *out, unsigned out_size);
/* @X6100, the firmware's group, is one of `groups` ("@ONE @TWO"). */
bool js8_x6100_joined(const char *groups);
/* A received message to @X6100 while we are in it: it is shown like one to
 * us. js8_process() never answers the group, in it or not. */
bool js8_to_x6100(const js8_rx_msg_t *msg, const char *groups);

/* ---- Alerts ---- */

/* Cleans up the alert words field: capitals, duplicates dropped, no more
 * than 20, e.g. "VE7ABC @POTA SOTA". */
void js8_alert_words_normalise(const char *typed, char *out, unsigned out_size);
/* Which of the cleaned up words a decode contains first; it goes to hit.
 * False when none does. */
bool js8_alert_hit(const char *text, const char *from, const char *words, char *hit, unsigned hit_size);

#define JS8_HB_MIN_INTERVAL     5  /* minutes */
#define JS8_HB_MAX_INTERVAL     30 /* minutes */
#define JS8_HB_DEFAULT_INTERVAL 30 /* minutes */

/* Values for the desktop message macros: <MYCALL> <MYGRID4> <MYGRID12>
 * <MYINFO> <MYSTATUS> <MYCQ> <MYHB> <MYREPLY> <MYVERSION> <MYIDLE>, and
 * <CALL> <SNR> <TDELTA> for the selected station. */
typedef struct {
    const char *my_call, *my_grid, *my_info, *my_status, *version; /* NULL counts as empty */
    int64_t     idle_ms;      /* time since the operator last used the radio */
    const char *call;         /* selected station, NULL or "" when none */
    bool        has_snr;      /* snr below is valid */
    int         snr;          /* our report of the selected station */
    bool        has_tdelta;   /* tdelta_ms below is valid */
    int         tdelta_ms;    /* its time offset (DT) */
} js8_macro_values_t;

/* Fills the macros in text, in capitals. With prune, any <...> still left
 * (such as <CALL> with no station selected) is removed, as desktop does
 * when a saved message is used; without it, it is left as typed. */
void js8_macros_expand(const char *text, const js8_macro_values_t *v, bool prune, char *out, unsigned out_len);
/* True when text contains <CALL>, <SNR> or <TDELTA>, so a station must be
 * selected first. */
bool js8_macros_need_station(const char *text);

#ifdef __cplusplus
}
#endif
