/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 app
 *
 *  Same screen plan as the FT8 app: waterfall on top, decoded messages
 *  below it. The protocol work is in src/js8, which knows nothing of LVGL;
 *  it calls back from its own threads, and every such call is handed to
 *  the UI thread through scheduler_put() before it touches a widget.
 */

#define _GNU_SOURCE     /* pthread_setname_np */
#include "js8_window.h"
#include "band_plan.h"
#include "board.h"
#include "ghostnet.h"
#include "i18n.h"
#include "js8_map.h"
#include "main.h"
#include "pskreporter.h"

#include "js8/js8_receive.h"
#include "js8/js8_transmit.h"
#include "js8/js8_logic.h"
#include "js8/js8_submode.h"
#include "qth/qth.h"
#include "station.h"
#include "qso_log.h"
#include "dxcc.h"
#include "ft8/decode_list.h"

#include "audio.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cfg/digital_modes.h"
#include "dialog.h"
#include "dialog_field.h"
#include "dsp.h"
#include "events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "params/params.h"
#include "radio.h"
#include "remote_server.h"
#include "keypad.h"
#include "scheduler.h"
#include "styles.h"
#include "textarea_window.h"
#include "tx_player.h"
#include "tx_info.h"
#include "theme.h"
#include "util.h"
#include "widgets/lv_finder.h"
#include "widgets/lv_waterfall.h"

#include <liquid/liquid.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Screen geometry */
#define FLAG_W_JS8       16                        /* flags.h FLAG_W */
#define WIDTH            771                       /* waterfall, TX line and list, px */
#define WF_HEIGHT        (DIALOG_H - 26)           /* the dialog less a 13 px border top and bottom */
#define LANE_H           26                        /* the calls heard, over their offsets */
#define WF_VISIBLE       (LANE_H + 40)             /* the lane and the newest rows of the waterfall; the list under */
#define LANE_SHOW_MS     (3 * 60 * 1000)           /* a call stays in the lane this long after its last decode */
#define TX_BAR_H         30                        /* the TX line above the list */

/* Audio and waterfall */
#define SAMPLE_RATE      (AUDIO_CAPTURE_RATE / AUDIO_DECIM) /* what audio_cb gets */
#define WF_ROWS_PER_SEC  15                        /* same scroll speed as the main screen */
#define WF_ROW_SAMPLES   (SAMPLE_RATE / WF_ROWS_PER_SEC) /* audio per row */
#define WF_QUEUE         16                        /* rows made but not drawn yet */
#define WF_TICK_MS       5                         /* period of the timer that draws them */
/* Colours are scaled from the noise floor up, so the level going into the
 * radio doesn't matter: floor + WF_MIN_DB is the darkest, + WF_MAX_DB the brightest. */
#define WF_MIN_DB        0                         /* dB over the floor */
#define WF_MAX_DB        30                        /* where the palette tops out */

/* Text limits. A message stops at 20 frames, which is 345 to 425 characters
 * depending on the words, so the compose box allows more than that and the
 * frame limit is what you run into, as on desktop JS8Call. */
#define TX_TEXT_MAX      500
#define ROW_MAX          (JS8_RX_TEXT_LEN + 64 + GHOSTNET_TR_TEXT) /* a row: the message and its translation */
#define TEXT_MAX         64                        /* INFO and STATUS answers */
#define SAVED_N          10                        /* Query > Saved messages > */
#define APRS_COMMENT_MAX 43                        /* longest comment in an APRS position report */
#define JS8_APP_VERSION  "OpenX6X00"            /* what <MYVERSION> becomes */

/* Files on the DATA partition (/mnt), all plain text a PC can edit. Tests
 * point them elsewhere. */
#if !defined(JS8_TEXTS_PATH)
#define JS8_TEXTS_PATH   BOARD_DATA "/js8_texts.txt"      /* INFO, STATUS, groups, alert words, spot form */
#endif
#if !defined(JS8_INBOX_PATH)
#define JS8_INBOX_PATH   BOARD_DATA "/js8_inbox.txt"      /* MSGs to us, a line each */
#endif
#define JS8_SAVED_PATH   BOARD_DATA "/js8_saved.txt"      /* saved messages, a line each, empty line = unused */
#if !defined(JS8_HELD_PATH)
#define JS8_HELD_PATH    BOARD_DATA "/js8_held.txt"       /* MSG TO: we keep for someone else */
#endif
#if !defined(JS8_LOG_PATH)
#define JS8_LOG_PATH     BOARD_DATA "/js8call_log.adi"    /* the file name desktop JS8Call uses */
#endif

/* Timing */
#define OFFER_MS         (5 * 60 * 1000)           /* a reply waits on Reply this long */
#define SYNC_WINDOW_MS   120000                    /* Time Sync looks at the decodes of the last 2 minutes */
#define SYNC_SAMPLES     64                        /* room for 2 minutes of a crowded band */
#define HB_ADJUST_MS     8000                      /* knob left alone this long: HB interval set */
#define READ_PAUSE_MS    30000                     /* list scrolled up to read: follows again after this */
#define HOLD_MS          500                       /* long press on this app's buttons (1 s in the rest) */
#define CQ_MIN_INTERVAL  1                         /* auto CQ minutes, counted from the end of our TX; */
#define CQ_MAX_INTERVAL  30                        /* 1 is also desktop's lowest */

/* Alerts. Rows matching an alert word are always coloured; the
 * params.js8_alerts bits pick what also beeps. */
#define ALERT_COLOR        theme_hex(TC_row_alert)
#define JS8_ALERT_BEEP     0x01                    /* master switch */
#define JS8_ALERT_TO_ME    0x02                    /* directed to our call, heartbeat acks left out */
#define JS8_ALERT_INBOX    0x04                    /* a MSG went into the inbox */
#define JS8_ALERT_CQ       0x08                    /* a CQ */
#define JS8_ALERT_NEW      0x10                    /* first time heard and never logged */

/* Receive passband while the app is open, which is also where the decoder
 * looks (desktop decodes between its waterfall filter edges). The user's
 * own filter is put back on close. Transmit passband: the radio's default;
 * it only matters when the user narrowed it for voice, as JS8 goes no lower
 * than 500 Hz and up to 3000 Hz. */
#define JS8_FILTER_LOW     200                     /* RX, Hz */
#define JS8_FILTER_HIGH    3000
#define JS8_TX_FILTER_LOW  160                     /* TX, Hz */
#define JS8_TX_FILTER_HIGH 3000                     /* the radio default too */

/* Message list */
#define HISTORY          300                       /* decodes kept so a new filter can be applied to them */
#define MAX_ROWS         200                       /* at this many rows the list is cut back ... */
#define KEEP_ROWS        150                       /* ... to this many */
#define BAND_LISTS       16                        /* Stations lists kept, one per dial frequency */

/* Custom dial frequency limits: 160 m up to the top of 6 m */
#define CUSTOM_MIN_HZ    1800000
#define CUSTOM_MAX_HZ    54000000

/* What the list shows (the Show button): every decode; all but heartbeats
 * and SNR reports; only what is to us or to a group, with no HB or CQ. */
typedef enum { SHOW_ALL, SHOW_NO_HB, SHOW_DIRECTED, SHOW_COUNT } show_t;

/* What the compose window is for (edit_target; 0 is a message to send). */
typedef enum edit_e {
    EDIT_INFO = 1, EDIT_STATUS,                    /* our INFO and STATUS answers */
    EDIT_GROUPS,                                   /* Settings: our groups */
    EDIT_SAVED,                                    /* saved message number saved_edit; back to the list afterwards */
    EDIT_LOG_GRID, EDIT_LOG_NAME, EDIT_LOG_NOTE,   /* fields of the Log popup, which comes back afterwards */
    EDIT_POTA_REF, EDIT_SOTA_REF,                  /* our park or summit, for the log */
    EDIT_ALERT_WORDS,                              /* back to Alerts afterwards */
    EDIT_FREQ,                                     /* a custom dial frequency in kHz */
    EDIT_SPOT_REF, EDIT_SPOT_FREQ, EDIT_SPOT_NOTE, /* fields of the spot form, which comes back afterwards */
    EDIT_BEACON_GRID,                              /* an optional message, then the grid is spotted */
    EDIT_BEACON_GPS,                               /* an optional message, then the GPS position is spotted */
    EDIT_COUNT                                     /* how many there are */
} edit_t;

typedef enum { LOG_SAVE, LOG_GRID, LOG_NAME, LOG_NOTE, LOG_CANCEL } log_item_t; /* the Log popup, top down */
typedef enum { SP_SEND, SP_REF, SP_FREQ, SP_MODE, SP_NOTE, SP_CLOSE, SP_COUNT } spot_item_t; /* the spot form */

/* Functions used before they are defined, many of them in the later parts
 * of the file. */

/* dialog_t hooks */
static void construct_cb(lv_obj_t *parent), destruct_cb(void);
static void audio_cb(unsigned int count, float *buf);
static void key_cb(lv_event_t *ev), rotary_cb(int32_t steps);

/* The buttons' label functions */
static const char *cq_label_getter(void), *show_label_getter(void), *hold_label_getter(void),
    *stations_label_getter(void), *inbox_label_getter(void), *auto_label_getter(void), *hb_label_getter(void),
    *hb_ack_label_getter(void), *act_label_getter(void), *prompt_label_getter(void), *speed_label_getter(void),
    *decode_label_getter(void), *freq_label_getter(void);

/* The buttons' press and hold actions, page by page */
static void js8_next_page_cb(button_data_t *btn);
static void js8_prev_page_cb(button_data_t *btn);
static void cq_cb(button_data_t *btn);
static void cq_hold_cb(button_data_t *btn);
static void heartbeat_cb(button_data_t *btn);
static void query_cb(button_data_t *btn);
static void hw_cpy_cb(button_data_t *btn);
static void show_cb(button_data_t *btn);
static void reply_cb(button_data_t *btn);
static const char *reply_label_getter(void);
static void settings_cb(button_data_t *btn);
static const char *hb_main_label(void);
static void hb_main_follow(void);
static void hb_main_hold_cb(button_data_t *btn);
static void send_cb(button_data_t *btn);
static void clear_cb(button_data_t *btn);
static void time_sync_cb(button_data_t *btn);
static void time_sync_hold_cb(button_data_t *btn);
static void hold_cb(button_data_t *btn);
static void stations_cb(button_data_t *btn);
static void inbox_cb(button_data_t *btn);
static void auto_cb(button_data_t *btn);
static void hb_cb(button_data_t *btn);
static void hb_hold_cb(button_data_t *btn);
static void hb_ack_cb(button_data_t *btn);
static void texts_cb(button_data_t *btn);
static void aprs_cb(button_data_t *btn);
static void log_cb(button_data_t *btn);
static void act_cb(button_data_t *btn);
static void act_hold_cb(button_data_t *btn);
static void prompt_cb(button_data_t *btn);
static void alerts_cb(button_data_t *btn);
static void speed_cb(button_data_t *btn);
static void speed_hold_cb(button_data_t *btn);
static void decode_cb(button_data_t *btn);
static void freq_cb(button_data_t *btn);
static void map_cb(button_data_t *btn);
static void translate_cb(button_data_t *btn);
static void stop_tx_cb(button_data_t *btn);

/* CQ, heartbeats, auto replies */
static int64_t cq_interval_ms(void);
static void cq_adjust_end(void);
static void cq_adjust_turn(int32_t diff);
static void auto_cq_stop(const char *why);
static void auto_cq_tick(void);
static void hb_tick(void);
static void hb_adjust_end(void);
static void hb_pause(const char *reason);
static void auto_send(const js8_auto_result_t *r);
static void user_touch(void);
static void qso_started(const char *their_call);

/* Sending */
static bool tx_queue_at(const char *message, int offset_hz, bool automatic);
static void tx_start(void);
static void tx_stop_all(void);
static void tx_timer_cb(lv_timer_t *timer);
static void update_tx_bar(void);
static void compose_open(const char *prefill);
static void compose_close(void);
static void msg_compose(const char *call, const char *kind);

/* Popups */
static bool popup_guard(void);
static bool any_popup(void);
static void query_open(void);
static void query_close(void);
static void texts_close(void);
static void aprs_close(void);
static void log_close(void);
static void log_list_open(log_item_t first);
static void log_edit_done(const char *text);
static void log_offer(const char *call);
static void log_refresh(void);
static void inbox_close(void);
static void inbox_refresh_button(void);
static void alerts_close(void), alerts_show(void);
static void freq_close(void);
static void freq_show(void);
static void map_close(void);
static void settings_close(void);
static void spot_show(bool sota, spot_item_t focus);
static void spot_close(void);

/* Saved texts and macros */
static void load_texts(void);
static void save_texts(void);
static void load_saved(void);
static void save_saved(void);
static void saved_open(int focus);
static void macros_fill(const char *text, bool prune, char *out, size_t out_len);

/* Held messages, inbox */
static void stored_received(const js8_stored_t *k);
static void deliver_start(int id, const char *group_call, const char *text);
static void deliver_end(const char *text, bool completed);

/* APRS and spots */
static bool aprs_prepare(const char *in, char *out, size_t size);
static void aprs_beacon(bool gps, const char *message);
static void beacon_changed_cb(lv_event_t *e);
static bool spot_parse_freq(const char *text, int32_t *out);
static bool spot_freq_empty(const char *text);

/* Frequency */
static void tune_custom(const char *khz);
static bool parse_custom(const char *text, int32_t *out);
static const char *where_label(void); /* "JS8 7107.5 kHz", or the preset's name */

/* Alerts, beeps, SWR, speed */
static void alert_check(js8_rx_msg_t *m, bool new_station);
static void alert_beep(int count);
static bool beep_start(int count);
static void beep_log_level(void);
static void swr_guard_tick(void);
static void swr_guard_reset(void);
static void speed_warn(void);

/* The list */
static void handle_incoming(const js8_rx_msg_t *msg);
static void remote_msg(const js8_rx_msg_t *msg), remote_wf_row(const float *row, unsigned n), rstat_update(void);
static bool find_station(const char *call, js8_station_t *found);
static void rebuild_station_rows(void), show_selection(void);
static int64_t now_wall_ms(void), now_mono_ms(void);

/* ---- State; the UI thread's unless a comment says otherwise ------------- */

static js8_rx_t *rx; /* the decoder, while the app is open */
static js8_tx_t *tx; /* the transmitter, likewise */

/* Transmitting */
static lv_obj_t        *tx_bar;
static lv_timer_t      *tx_timer;                    /* ticks the TX line's countdown */
static js8_tx_status_t  tx_status;                   /* latest status from the transmitter */
static char             tx_preview[JS8_RX_TEXT_LEN]; /* the message on the air, as receivers decode it */
static char             last_tx_text[JS8_RX_TEXT_LEN]; /* repeated on AGN? */
static float            base_gain_offset;
static atomic_int       tx_offset_active;            /* audio offset of the message on the air */
static atomic_bool      keyed;                       /* set by the TX thread for each frame */
static bool             composing;                   /* the compose window is up */

/* Alert beeps and our TX must never overlap: a beep holds speaker_lock and
 * tx_play waits for it right after setting `keyed`, and a beep checks
 * `keyed` between its parts. */
static pthread_mutex_t speaker_lock = PTHREAD_MUTEX_INITIALIZER; /* held while a beep plays */
/* With the speaker beeping, the base stops sending receiver audio. Rather
 * than leave a gap, the decoder is fed zeros of the same length so its slot
 * timing holds, and the waterfall stops for them. The other two add up the
 * level heard meanwhile, for the log. */
static atomic_bool     beep_guard;     /* the speaker is on */
static _Atomic int64_t beep_guard_end; /* monotonic ms: a little past speaker-off, for the capture delay */
static _Atomic int64_t beep_guard_sq, beep_guard_n;

/* Time Sync: drift of recent decodes, a ring */
static js8_sync_sample_t sync_samples[SYNC_SAMPLES];
static unsigned          sync_head;               /* where the next one goes */

/* The selected station (MFK or a tap; arriving rows never change it), so
 * Reply answers them even when the list has moved on. qso_freq is their
 * offset, the green line on the waterfall, or -1. */
static char    sel_call[JS8_RX_CALL_LEN];
static int     sel_snr;
static float   qso_freq = -1;
static int64_t cursor_user_ms;                       /* when MFK last moved the list cursor */

/* Stations heard, one list per dial frequency: 7.078 and GhostNet on 7.107
 * are not the same net. A list lives as long as the radio is on, across
 * closing and opening JS8; a station leaves it an hour after last heard.
 * `stations` is the one for the frequency in use. */
static js8_stations_t *stations;
static struct { int32_t dial_khz; js8_stations_t *list; } band_lists[BAND_LISTS];
static int band_lists_next;                          /* next slot to recycle once all are used */

/* Stations view of the list */
static bool          view_stations;
static js8_station_t st_rows[MAX_ROWS];
static int           st_count;
static bool          st_alert[MAX_ROWS];             /* st_rows[i] matches an alert word */
static bool          st_worked[MAX_ROWS];            /* st_rows[i] is in the log */

/* Popups, NULL while closed */
static lv_obj_t *query_list, *aprs_list, *log_list, *inbox_list, *alerts_list, *freq_list, *spot_list;
static lv_obj_t *map_obj;
static lv_obj_t *texts_list;
static lv_obj_t *settings_list;   /* Settings... (page 3) */
static lv_obj_t *lane;            /* the calls over the waterfall */
static lv_obj_t *conv_root;       /* the conversation with one station, over the list */
static lv_obj_t *board;           /* the stations as cards: the main view */
static bool      board_on = true; /* the board shows (else the list, of messages or stations) */
static lv_obj_t *hb_info;         /* right of the TX line: the last HB and who heard it */
static int64_t   hb_sent_ms;      /* our last heartbeat queued, wall clock; 0 none yet */
static void      board_update(void);
static void      board_move(int step);
static void      board_press(void);
static void      unread_seen(const char *call);
static void      unread_add(const char *call);
static void      board_show(bool on);
static void      table_takes_keys(void);
static void      keys_leave_main(void);
static char      board_sel_call[JS8_RX_CALL_LEN];
static void      conv_close(void);
static void      tx_target(const js8_rx_msg_t *m, char *call, size_t size);
static const char *conv_call_now(void);
static void      conv_choose(const char *call);
static void      conv_fill(void);
static int64_t   alert_opened_ms;
static void      conv_open(void);
static const char *conv_call_now(void);
static lv_obj_t *alert_box;       /* "Message from ...": a message just came into the Inbox */
static void      alert_close(void);
static void      p3_inbox_view(int id);
static void      conv_refresh(const js8_rx_msg_t *m);
static void      lane_update(void);

/* Settings kept in JS8_TEXTS_PATH */
static char alert_words[128];                        /* ALERTS=, e.g. "VE7ABC @POTA SOTA" */
static char groups_text[96];                         /* GROUPS=, e.g. "@NET @GROUP2" */
static char info_text[TEXT_MAX + 1], status_text[TEXT_MAX + 1];

/* Inbox (JS8_INBOX_PATH) and messages held for others (JS8_HELD_PATH) */
static js8_inbox_t *inbox;
static js8_held_t  *held;

/* A held message on its way to someone */
typedef struct {
    int  id;                          /* in `held`; 0 = none */
    char group_call[JS8_RX_CALL_LEN]; /* for a group message: the member it is for */
    char text[JS8_RX_TEXT_LEN];       /* the words to send */
} held_ref_t;
/* Offered on Reply; if it is sent unchanged it counts as being delivered. */
static held_ref_t deliver_pending;
/* Being sent. Only marked delivered once all of it went out (desktop marks
 * it when sending starts); if TX is stopped it stays held and is offered
 * again next time they ask. */
static held_ref_t deliver_tx;

/* The end of a transmission, passed from the TX thread to the UI thread */
typedef struct {
    bool completed;                   /* false: stopped */
    char text[JS8_RX_TEXT_LEN];       /* what was sent: capitals, no outer spaces */
} tx_done_t;

/* Log */
static js8_qsos_t     *qsos;
static js8_log_entry_t log_entry;                    /* shown in the Log popup */
static char            log_pending[JS8_RX_CALL_LEN]; /* QSO over, waiting to be logged */
static bool            log_grid_typed;               /* user typed the grid: keep it */
static lv_obj_t       *log_reports;                  /* Sent / Rcvd line of the popup */
static lv_obj_t       *log_grid_btn;                 /* Grid item of the popup */

/* Automatic sending. The switches are params (js8_auto, js8_hb, js8_hb_ack,
 * js8_hb_interval), off by default. */
static js8_auto_t *autop;                            /* what to answer, and when */
static int64_t     hb_next_ms;                       /* next heartbeat; 0 = first chance */
static bool        hb_adjusting;                     /* the main knob sets the HB interval */
static int64_t     hb_adjust_ms;                     /* its last turn */
static bool        auto_cq;                          /* CQ held on: one every js8_cq_interval minutes until answered */
static int64_t     auto_cq_next_ms;                  /* next auto CQ */
static int64_t     auto_cq_from_ms;                  /* end of our last TX, where the interval starts */
static bool        cq_adjusting;                     /* the main knob sets the auto CQ interval */
static int64_t     cq_adjust_ms;                     /* its last turn */
/* With AUTO off a reply is offered instead and waits on Reply, like the
 * outgoing box on desktop. */
static struct reply_offer {
    char    call[JS8_RX_CALL_LEN];               /* who it answers */
    char    text[JS8_RX_TEXT_LEN];               /* the reply */
    int64_t ms;                                  /* when it was offered */
    int     deliver_id;                          /* held message it carries, 0 = none */
    char    deliver_group_call[JS8_RX_CALL_LEN]; /* its group member, for a group message */
} offer;
static js8_auto_result_t pending_auto;               /* an auto reply that came while we were sending */
static bool              pending_auto_valid;         /* pending_auto is waiting */
static int64_t           pending_auto_ms;            /* when it was put aside */
#define PENDING_AUTO_MS  (2 * 60 * 1000)             /* older than this it is dropped */
static int64_t           push_next_ms;               /* next check for RETRIEVE MSG notices; 0 = in 15 min */
static int64_t           touch_ms;                   /* last time the operator did anything, for <MYIDLE> */

/* Saved messages, kept as typed with their macros (desktop's Saved
 * Messages), JS8_SAVED_PATH */
static char saved_msgs[SAVED_N][TX_TEXT_MAX + 1];
static int  saved_edit;                              /* the one being edited (EDIT_SAVED) */
static int  edit_target;                             /* an edit_t, or 0 */

/* APRS spots */
static char    last_pota[16], last_sota[24];         /* park and summit last spotted */
static bool    spot_sota;                            /* the spot form is for SOTA, not POTA */
static char    spot_mode[8] = "DATA";                /* the form's fields, also in JS8_TEXTS_PATH */
static int32_t spot_typed_hz;                        /* frequency last typed in it, 0 = none */
static bool    spot_use_typed;                       /* spot that frequency rather than the JS8 dial */
static char    spot_note[32];

/* Widgets */
static lv_obj_t *waterfall, *finder, *table, *status;

/* Filters: ours while the app is open, the user's put back on close */
static int32_t filter_low, filter_high;              /* in use, Hz */
static int32_t saved_filter_low, saved_filter_high;
static bool    filter_saved;                         /* the two above hold the user's */

static show_t show = SHOW_NO_HB;

/* Decodes in a ring so the list can be built again under another filter.
 * row_hist[r] is the ring slot shown on table row r, -1 for a text row. */
static js8_rx_msg_t history[HISTORY];                /* the ring */
static uint16_t     hist_head, hist_count;           /* slot the next decode goes to; slots in use */
static uint16_t     rows;                            /* rows in the table */
static int16_t      row_hist[MAX_ROWS + 1];          /* table row -> ring slot */
static bool         auto_selecting;                  /* the cursor moved by code, not by the user */

static unsigned cycles, cycle_decodes;               /* decode cycles so far; decodes in the last one */

/* Waterfall. The spectrum is made on the receiver's thread and only used there. */
static spgramf  sg;                                  /* averages the FFTs of one row */
static float   *psd;                                 /* its output, nfft bins */
static uint16_t nfft;                                /* FFT size */
static unsigned wf_row_fill;                         /* samples so far in the current row */
static float    wf_floor_db;                         /* noise floor, smoothed */
static bool     wf_floor_set;                        /* wf_floor_db has a value */

/* Audio comes in bursts while rows are made per amount of audio, so rows
 * are queued and a timer draws them evenly. The timer checks the clock
 * itself: an LVGL timer period starts from its last run and LVGL's tick is
 * slow, which gave ~9.7 rows/s at 100 ms and a jump of two rows every 2 s. */
static float      *wf_queue[WF_QUEUE];
static uint16_t    wf_queue_size[WF_QUEUE];
static unsigned    wf_q_head, wf_q_count;
static lv_timer_t *wf_timer;                         /* draws them */
static int64_t     wf_due_us;                        /* monotonic time the next row is due */

/* Receiver level before a beep, smoothed (for the beep log) */
static float beep_level_before_db = -200.0f;

/* ---- Buttons ----------------------------------------------------------- */

/* The first button of each page: its number, press for the next page, hold for the previous. */
#define PAGE_BUTTON(name, nxt, prv)                                                                 \
    {                                                                                               \
        .type = BTN_TEXT, .label = name, .press = js8_next_page_cb, .hold = js8_prev_page_cb,     \
        .next = nxt, .prev = prv                                                                    \
    }

static buttons_page_t page_1, page_2, page_3, page_4;

/* 1: the band: a heartbeat, a CQ, the conversation with the station chosen on the board, the Inbox */
static const char *chat_label(void);
static void        chat_cb(button_data_t *btn);
static const char *view_label(void);
static void        view_cb(button_data_t *btn);
static button_data_t btn_p1     = PAGE_BUTTON("(JS8 1:4)", &page_2, &page_4);
static button_data_t btn_hb_main = {.type = BTN_TEXT_FN, .label_fn = hb_main_label, .press = heartbeat_cb,
                                    .hold = hb_main_hold_cb};  /* a press sends one, a hold: Settings on HB */
static button_data_t btn_cq     = {.type = BTN_TEXT_FN, .label_fn = cq_label_getter,
                                   .press = cq_cb, .hold = cq_hold_cb};
static button_data_t btn_chat   = {.type = BTN_TEXT_FN, .label_fn = chat_label, .press = chat_cb};
static button_data_t btn_inbox  = {.type = BTN_TEXT_FN, .label_fn = inbox_label_getter,
                                   .press = inbox_cb};
static buttons_page_t page_1    = {{&btn_p1, &btn_hb_main, &btn_cq, &btn_chat, &btn_inbox}};

/* 2: to the station selected, to anyone, and the view: the board, the messages, the stations' table */
static button_data_t btn_p2       = PAGE_BUTTON("(JS8 2:4)", &page_3, &page_1);
static button_data_t btn_query    = {.type = BTN_TEXT, .label = "Query >", .press = query_cb};
static button_data_t btn_reply    = {.type = BTN_TEXT_FN, .label_fn = reply_label_getter, .press = reply_cb};
static button_data_t btn_send     = {.type = BTN_TEXT, .label = "Send...", .press = send_cb};
static button_data_t btn_view     = {.type = BTN_TEXT_FN, .label_fn = view_label, .press = view_cb};
static buttons_page_t page_2      = {{&btn_p2, &btn_query, &btn_reply, &btn_send, &btn_view}};
static button_data_t btn_hw_cpy   = {.type = BTN_TEXT, .label = "HW CPY?", .press = hw_cpy_cb};
static button_data_t btn_stations = {.type = BTN_TEXT_FN, .label_fn = stations_label_getter,
                                     .press = stations_cb};
static button_data_t btn_map      = {.type = BTN_TEXT, .label = "Map", .press = map_cb};

/* 3: the clock, the power and the switches set once (Settings...) */
static button_data_t btn_p3        = PAGE_BUTTON("(JS8 3:4)", &page_4, &page_2);
static button_data_t btn_time_sync = {.type = BTN_TEXT, .label = "Time\nSync",
                                      .press = time_sync_cb, .hold = time_sync_hold_cb};
static button_data_t btn_settings  = {.type = BTN_TEXT, .label = "Settings...", .press = settings_cb};
static buttons_page_t page_3       = {{&btn_p3, &btn_time_sync, &btn_dig_pwr, &btn_ext_tune, &btn_settings}};

/* 4: GhostNet's on-demand translation (the message on the list's cursor), the map of the log */
static button_data_t btn_p4     = PAGE_BUTTON("(JS8 4:4)", &page_1, &page_3);
static button_data_t btn_transl = {.type = BTN_TEXT, .label = "Trans-\nlate", .press = translate_cb};
static buttons_page_t page_4    = {{&btn_p4, &btn_transl, &btn_map}};

/* The conversation with a station (press on its row): F1 and F5 back to the board, as ESC; Heartbeat stays on the
 * board's page 1 */
static const char *conv_label_getter(void);
static void        conv_back_cb(button_data_t *btn);
static void        conv_msg_cb(button_data_t *btn);
static button_data_t btn_conv       = {.type = BTN_TEXT_FN, .label_fn = conv_label_getter, .press = conv_back_cb};
static button_data_t btn_conv_msg   = {.type = BTN_TEXT, .label = "Write...", .press = conv_msg_cb};
static button_data_t btn_conv_close = {.type = BTN_TEXT, .label = "Close\nChat", .press = conv_back_cb};
static buttons_page_t page_conv     = {{&btn_conv, &btn_reply, &btn_query, &btn_conv_msg, &btn_conv_close}};

/* Not on a page: the Settings list presses them (settings_open), their labels are its rows */
static button_data_t btn_hb     = {.type = BTN_TEXT, .label = "Heart-\nbeat", .press = heartbeat_cb};
static button_data_t btn_show   = {.type = BTN_TEXT_FN, .label_fn = show_label_getter, .press = show_cb};
static button_data_t btn_clear  = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};
static button_data_t btn_hold   = {.type = BTN_TEXT_FN, .label_fn = hold_label_getter, .press = hold_cb};
static button_data_t btn_auto   = {.type = BTN_TEXT_FN, .label_fn = auto_label_getter, .press = auto_cb};
static button_data_t btn_hbauto = {.type = BTN_TEXT_FN, .label_fn = hb_label_getter,
                                   .press = hb_cb, .hold = hb_hold_cb};
static button_data_t btn_hbackk = {.type = BTN_TEXT_FN, .label_fn = hb_ack_label_getter, .press = hb_ack_cb};
static button_data_t btn_texts  = {.type = BTN_TEXT, .label = "Settings...", .press = texts_cb};
static button_data_t btn_aprs   = {.type = BTN_TEXT, .label = "APRS >", .press = aprs_cb};
static button_data_t btn_log    = {.type = BTN_TEXT, .label = "Log QSO", .press = log_cb};
static button_data_t btn_act    = {.type = BTN_TEXT_FN, .label_fn = act_label_getter,
                                   .press = act_cb, .hold = act_hold_cb};
static button_data_t btn_prompt = {.type = BTN_TEXT_FN, .label_fn = prompt_label_getter, .press = prompt_cb};
static button_data_t btn_alerts = {.type = BTN_TEXT, .label = "Alerts >", .press = alerts_cb};
static button_data_t btn_speed  = {.type = BTN_TEXT_FN, .label_fn = speed_label_getter,
                                   .press = speed_cb, .hold = speed_hold_cb};
static button_data_t btn_decode = {.type = BTN_TEXT_FN, .label_fn = decode_label_getter, .press = decode_cb};
static button_data_t btn_freq   = {.type = BTN_TEXT_FN, .label_fn = freq_label_getter, .press = freq_cb};

static dialog_t dialog = {.run           = false,
                          .construct_cb  = construct_cb,
                          .destruct_cb   = destruct_cb,
                          .audio_cb      = audio_cb,
                          .key_cb        = key_cb,
                          .rotary_cb     = rotary_cb,
                          .btn_page      = &page_1,
                          .restore_radio = true};

dialog_t *dialog_js8 = &dialog; /* see dialog_js8.h */

/* ---- The list of messages ---------------------------------------------- */

/* Does a decode belong in the list under the current Show setting? Our own
 * transmissions always do. */
static bool passes_filter(const js8_rx_msg_t *msg)
{
    if (msg->tx || show == SHOW_ALL) return true;
    if (show == SHOW_NO_HB) {
        /* SNR reports mostly answer heartbeats, so they go too; anything to
         * us stays (a heartbeat ack, say). */
        return msg->to_me || js8_to_x6100(msg, groups_text) || !(msg->heartbeat || msg->snr_report);
    }
    if (show != SHOW_DIRECTED) return true;
    /* Directed view also keeps whatever is on the selected station's
     * offset, since a long QSO soon drops the calls. "On it" means inside
     * the rxThreshold of the decode's speed, as on desktop. */
    if (qso_freq >= 0) {
        float near_hz = js8_speed_rx_threshold_hz(js8_speed_from_submode(msg->submode));
        if (fabsf(msg->freq_hz - qso_freq) <= near_hz) return true;
    }
    if (msg->to_me || js8_to_x6100(msg, groups_text)) return true; /* @X6100 counts as ours */
    return msg->to_group && !msg->heartbeat && !msg->cq;
}

/* "hh:mm:ss  TX 1500  text" for ours, "hh:mm:ss -12 1500  text" for theirs.
 * A speed other than Normal adds its letter after the offset, the same
 * column desktop shows it in. */
/* Translations (MyMemory, through x6100-translate): of the messages the operator asked for (Translate, page 4) or,
 * with GhostNet's switch on, of those arriving in English (or Spanish). By message, the newest 48 kept while the
 * radio is on; a row shows its translation on a line of its own under the message. */
#define TRANS_N 48
static struct {
    uint32_t id;
    int32_t  utc;
    char     lang[8];                    /* the GUI's language it was made in, shown in front: "NL" */
    char     text[GHOSTNET_TR_TEXT + 1];
} trans[TRANS_N];
static int  trans_head;
static bool trans_said;                  /* a failure of the automatic translation: said once while JS8 is open */

static int trans_find(const js8_rx_msg_t *msg) {
    if (msg->tx || msg->partial || msg->msg_id == 0) return -1;
    for (int i = 0; i < TRANS_N; i++)
        if (trans[i].id == msg->msg_id && trans[i].utc == msg->utc && trans[i].text[0]) return i;
    return -1;
}

static void format_row(const js8_rx_msg_t *msg, char *out, size_t out_size)
{
    int         utc = (int)msg->utc;
    js8_speed_t sp  = js8_speed_from_submode(msg->submode);
    char        letter[4];

    letter[0] = '\0';
    if (sp != JS8_SPEED_NORMAL) snprintf(letter, sizeof(letter), " %c", js8_speed_letter(sp));

    if (!msg->tx) {
        const char *tail = "";
        if (msg->partial) tail = " ...";
        else if (msg->checksum < 0) tail = "  (bad checksum)";
        const char *open = msg->low_confidence ? "[" : "", *close = msg->low_confidence ? "]" : "";
        int n = snprintf(out, out_size, "%02d:%02d:%02d %+3d %4.0f%s  %s%s%s%s", utc / 10000, utc / 100 % 100,
                         utc % 100, msg->snr, msg->freq_hz, letter, open, msg->text, close, tail);
        int t = trans_find(msg);
        if (t >= 0 && n > 0 && (size_t)n < out_size)
            snprintf(out + n, out_size - n, "\n          %s: %s", trans[t].lang, trans[t].text);
    } else {
        snprintf(out, out_size, "%02d:%02d:%02d  TX %4.0f%s  %s", utc / 10000, utc / 100 % 100, utc % 100,
                 msg->freq_hz, letter, msg->text);
    }
}

/* Should new rows scroll the list? Yes when the cursor is on the last row
 * (or nowhere), and also once MFK has been idle for READ_PAUSE_MS after
 * the user scrolled up to read. Only the cursor moves; sel_call doesn't. */
static bool at_bottom(void)
{
    if (rows == 0) return true;
    uint16_t r = 0, c = 0;
    lv_table_get_selected_cell(table, &r, &c);
    if (r == LV_TABLE_CELL_NONE || r + 1 >= rows) return true;
    return now_wall_ms() - cursor_user_ms > READ_PAUSE_MS;
}

/* Put the cursor on row r and scroll to it. The LVGL 8.3 table can't set its
 * selection from code, so the cursor goes on the row above and the table
 * gets a Down key: its handler then also scrolls the row into view. */
static void select_row(uint16_t r)
{
    if (!rows) return;
    if (r > rows - 1) r = rows - 1;

    lv_table_t *tbl = (lv_table_t *)table;
    tbl->col_act    = 0;
    if (r > 0) {
        static uint32_t down = LV_KEY_DOWN;
        tbl->row_act         = r - 1;
        auto_selecting       = true; /* the VALUE_CHANGED this sends is not the user's */
        lv_event_send(table, LV_EVENT_KEY, &down);
        auto_selecting       = false;
        return;
    }
    tbl->row_act = 0;
    lv_obj_scroll_to_y(&tbl->obj, 0, LV_ANIM_OFF);
    lv_obj_invalidate(&tbl->obj);
}

/* Cursor on the newest row, and scrolled right to the bottom: a tall last
 * row can be more than one scroll step. */
static void follow(void)
{
    if (!rows) return;
    uint16_t last = rows - 1;
    select_row(last);
    lv_obj_t *list = table;
    lv_obj_update_layout(list);
    lv_coord_t left = lv_obj_get_scroll_bottom(list);
    if (left > 0) lv_obj_scroll_by(list, 0, -left, LV_ANIM_OFF);
}

static void append_row(const char *text, int16_t slot)
{
    row_hist[rows] = slot;
    lv_table_set_cell_value(table, rows++, 0, text);
}

/* Append a row, and keep following if the list was. */
static void push_row(const char *text, int16_t slot)
{
    bool following = at_bottom();
    append_row(text, slot);
    if (following) follow();
}

/* A line of our own (band changes, TX stopped...) in the message view. */
static void add_info_row(const char *fmt, ...)
{
    if (view_stations || rows >= MAX_ROWS) return;
    char    line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    push_row(line, -1);
}

/* Start the table over with no rows. */
static void table_empty(void)
{
    lv_obj_t *list = table;
    lv_table_set_row_cnt(list, 1);
    lv_table_set_cell_value(list, 0, 0, "");
    rows = 0; /* the next append_row fills row 0 */
}

/* The message view from the history: the newest KEEP_ROWS decodes that
 * pass the filter. */
static void rebuild_message_rows(void)
{
    int16_t picked[KEEP_ROWS]; /* newest first */
    int     count = 0;
    int     idx   = hist_head;
    for (int i = 0; i < hist_count && count < KEEP_ROWS; i++) {
        idx = idx == 0 ? HISTORY - 1 : idx - 1;
        if (passes_filter(&history[idx])) picked[count++] = (int16_t)idx;
    }

    table_empty();
    if (!count) append_row("Listening for JS8...", -1);
    char line[ROW_MAX];
    while (count--) {
        format_row(&history[picked[count]], line, sizeof(line));
        append_row(line, picked[count]);
    }
    follow(); /* the newest in view */
}

/* Fill the list again, in whichever view it is. */
static void rebuild_rows(void)
{
    if (!view_stations) rebuild_message_rows();
    else rebuild_station_rows();
}

/* Our TX speed (Settings...). */
static js8_speed_t cur_speed(void)
{
    js8_speed_t sp = (js8_speed_t)params.js8_speed.x;
    return params.js8_speed.x < JS8_SPEED_COUNT ? sp : JS8_SPEED_NORMAL;
}

/* Speeds the decoder looks for: all of them (desktop's multi-decoder,
 * which is the default) or just our TX speed. */
static int rx_speed_mask(void)
{
    js8_speed_t ours = cur_speed();
    int         bits = 0;
    for (js8_speed_t sp = 0; sp < JS8_SPEED_COUNT; sp++)
        if (params.js8_rx_all.x || sp == ours) bits |= js8_speed_rx_mask(sp);
    return bits;
}

/* JS8 time is the radio's clock corrected by Time Sync (js8_rx.h). */
static int64_t now_wall_ms(void)
{
    int64_t t = js8_wall_ms();
    return t;
}

/* Ring slot of a message that is still coming in (desktop extends its line
 * every cycle), or -1. It can only be one of the last 100. */
static int find_partial(uint32_t id)
{
    if (id == 0) return -1;
    int idx = hist_head;
    for (int i = 0; i < hist_count && i < 100; i++) {
        idx = idx == 0 ? HISTORY - 1 : idx - 1;
        const js8_rx_msg_t *h = &history[idx];
        if (h->partial && !h->tx && h->msg_id == id) return idx;
    }
    return -1;
}

/* The selected station has a new offset or SNR. */
static void selection_move(float hz, int db)
{
    qso_freq = hz;
    sel_snr = db;
    show_selection(); /* the green line, the bars on its rows, the TX line */
}

/* A complete message from someone else: selection, alerts, spots, Time
 * Sync, auto replies, log. */
static void heard_from(js8_rx_msg_t *msg, bool first_time, int64_t now)
{
    if (sel_call[0] && !strcasecmp(msg->from, sel_call)) selection_move(msg->freq_hz, msg->snr); /* they moved */
    alert_check(msg, first_time);

    js8_station_t known_st;
    if (param_i_get(cfg_pskreporter) && !msg->low_confidence && msg->from[0]) {
        bool known = find_station(msg->from, &known_st); /* their grid, if they sent one */
        pskr_spot(params.callsign.x, station_qth(), VERSION, msg->from, known ? known_st.grid : "",
                  (uint32_t)cparam_i_get(cfg_fg_freq) + (uint32_t)(msg->freq_hz + 0.5f), msg->snr, "JS8");
    }
    if (msg->from[0] && !msg->low_confidence) {
        js8_sync_sample_t *s = &sync_samples[sync_head++ % SYNC_SAMPLES];
        sync_head %= SYNC_SAMPLES;
        snprintf(s->call, sizeof(s->call), "%s", msg->from);
        s->when_ms   = now;
        s->drift_ms  = msg->drift_ms;
        s->period_ms = js8_speed_period_s(js8_speed_from_submode(msg->submode)) * 1000;
    }
    handle_incoming(msg);

    char done_call[JS8_RX_CALL_LEN];
    if (js8_qsos_received(qsos, msg, params.callsign.x, now, done_call, sizeof(done_call))) log_offer(done_call);
    if (log_list && msg->to_me) log_refresh();
}

/* A complete message (never one still arriving), ours included. */
static void process_message(js8_rx_msg_t *msg)
{
    int64_t       now        = now_wall_ms();
    bool          first_time = false;
    js8_station_t before;
    if (!msg->tx && msg->from[0]) first_time = !find_station(msg->from, &before);
    js8_stations_add(stations, msg, params.callsign.x, now);
    if (!msg->tx) heard_from(msg, first_time, now);
}

/* The Stations view only changes when a message is complete. True when
 * the list shows stations, so the message view has nothing to do. */
static bool stations_view_update(const js8_rx_msg_t *msg)
{
    if (!view_stations) return false;
    if (!msg->partial) rebuild_station_rows();
    return true;
}

/* New text for a message already shown while arriving: into its slot and
 * onto its row, or onto a new row if it had none and now passes the filter. */
static void update_slot(int slot, const js8_rx_msg_t *msg)
{
    history[slot] = *msg;
    if (stations_view_update(msg)) return;

    char line[ROW_MAX];
    format_row(msg, line, sizeof(line));
    uint16_t r = 0;
    while (r < rows && row_hist[r] != slot) r++;
    if (r < rows) {
        /* The row grows onto more lines as words arrive: keep the newest in sight. */
        bool following = at_bottom();
        lv_table_set_cell_value(table, r, 0, line);
        if (following) follow();
    } else if (passes_filter(msg) && rows < MAX_ROWS) {
        push_row(line, (int16_t)slot);
    }
}

static void trans_auto(const js8_rx_msg_t *m);

/* For the web page GhostNet's messages: to GhostNet's groups, or anything on its frequencies (the GhostNet list in
 * use, not a typed frequency); no heartbeats, SNR reports or doubtful decodes */
static bool ghostnet_traffic(const js8_rx_msg_t *m) {
    if (m->heartbeat || m->snr_report || m->low_confidence) return false;
    return ghostnet_is_group(m->to) || (params.js8_ghostnet.x && !params.js8_custom_on.x);
}

static void add_message(const js8_rx_msg_t *in)
{
    js8_rx_msg_t m = *in;

    int growing = -1;
    if (!m.tx) growing = find_partial(m.msg_id);
    if (!m.partial) process_message(&m);
    if (!m.partial) trans_auto(&m);
    if (!m.partial && ghostnet_traffic(&m))
        ghostnet_log(m.tx ? 'X' : 'R', m.msg_id, m.tx ? params.callsign.x : m.from, m.to, m.snr,
                     cparam_i_get(cfg_fg_freq), m.text);
    if (!m.partial && !m.tx && m.to_me && m.from[0] &&
        !(conv_root && !strcasecmp(m.from, conv_call_now()))) unread_add(m.from); /* the card says "1 new" */
    if (growing >= 0) {
        update_slot(growing, &m);
        conv_refresh(&m);
        board_update();
        return;
    }
    conv_refresh(&m);
    board_update();

    int slot           = hist_head;
    history[hist_head] = m;
    hist_head          = (uint16_t)((hist_head + 1) % HISTORY);
    hist_count         = hist_count < HISTORY ? hist_count + 1 : HISTORY;
    if (stations_view_update(&m)) return;

    /* After a long session the slot just reused may still be on a row, which
     * would now show the new message there; build the list again. (The ring
     * is bigger than MAX_ROWS, so this is rare.) */
    bool reused = false;
    for (uint16_t r = 0; r < rows && !reused; r++) reused = row_hist[r] == slot;
    if (!reused && !passes_filter(&m)) return;
    if (reused || rows >= MAX_ROWS) {
        rebuild_rows(); /* also cuts a full list back to KEEP_ROWS */
        return;
    }
    char line[ROW_MAX];
    format_row(&m, line, sizeof(line));
    push_row(line, (int16_t)slot);
}

/* Why a translation did not come, in the message box */
static void trans_fail_say(ghostnet_tr_result_t result, const char *text) {
    switch (result) {
    case GHOSTNET_TR_SKIP:  msg_update_text_fmt("In your language already"); break;
    case GHOSTNET_TR_NET:   msg_update_text_fmt("No answer from MyMemory"); break;
    case GHOSTNET_TR_LIMIT: /* the free amount of the day (about 5000 characters): longer, to be read */
        msg_schedule_long_text_fmt("MyMemory's free translations of today are used up: more in %s h", text);
        break;
    default:                msg_update_text_fmt("MyMemory gave no translation"); break;
    }
}

/* A translation came: kept, and onto its message's row */
static void trans_store(uint32_t id, int32_t utc, const char *text) {
    int idx = hist_head;
    for (int i = 0; i < hist_count; i++) {
        idx = idx == 0 ? HISTORY - 1 : idx - 1;
        const js8_rx_msg_t *h = &history[idx];
        if (h->msg_id != id || h->utc != utc || h->tx || h->partial) continue;
        char lang[8];
        ghostnet_tr_lang(i18n_code(), lang, sizeof(lang));
        for (char *p = lang; *p; p++) *p = (char)toupper((unsigned char)*p);
        trans[trans_head].id  = id;
        trans[trans_head].utc = utc;
        snprintf(trans[trans_head].lang, sizeof(trans[0].lang), "%.2s", lang);
        snprintf(trans[trans_head].text, sizeof(trans[0].text), "%s", text);
        trans_head = (trans_head + 1) % TRANS_N;
        if (ghostnet_traffic(h)) ghostnet_log('T', id, h->from, h->to, h->snr, cparam_i_get(cfg_fg_freq), text);
        js8_rx_msg_t copy = *h;
        update_slot(idx, &copy); /* its row again, with the translation */
        conv_refresh(&copy);     /* and its bubble, when the conversation is open */
        return;
    }
}

static void trans_rx_done(uint32_t id, int32_t utc, ghostnet_tr_result_t result, const char *text) {
    if (!table) return; /* JS8 closed meanwhile */
    if (result == GHOSTNET_TR_OK) trans_store(id, utc, text);
    else if (result != GHOSTNET_TR_SKIP && !trans_said) {
        trans_said = true; /* once: the next messages would say the same */
        trans_fail_say(result, text);
    }
}

static void trans_ask_done(uint32_t id, int32_t utc, ghostnet_tr_result_t result, const char *text) {
    if (!table) return;
    if (result == GHOSTNET_TR_OK) trans_store(id, utc, text);
    else trans_fail_say(result, text);
}

/* GhostNet's switch: a whole message from someone else, worth translating, to the helper, which leaves the ones
 * already in the operator's language alone without asking MyMemory */
static void trans_auto(const js8_rx_msg_t *m) {
    if (m->tx || m->partial || m->heartbeat || m->snr_report || m->low_confidence || m->msg_id == 0) return;
    if (!ghostnet_conf()->translate_rx || strlen(m->text) < 12 || trans_find(m) >= 0) return;
    ghostnet_translate(GHOSTNET_TR_RX, m->text, m->msg_id, m->utc, trans_rx_done); /* a full queue: not this one */
}

/* Age as "now", "12m" or "3h". */
static void format_age(int64_t age_ms, char *out, size_t out_size)
{
    int64_t minutes = age_ms / 60000;
    if (minutes >= 60) snprintf(out, out_size, "%dh", (int)(minutes / 60));
    else if (minutes > 0) snprintf(out, out_size, "%dm", (int)minutes);
    else snprintf(out, out_size, "now");
}

/* Columns of a row in the Stations view. The font is proportional, so
 * spaces can't line them up: table_draw_end_cb draws each at its own x. */
typedef struct {
    char star[2];                /* '*': they heard us */
    char call[JS8_RX_CALL_LEN];  /* as heard */
    char speed[2];               /* F, T or S; empty for Normal */
    char age[8];                 /* since last heard */
    char snr[8];
    char heard[40];              /* when they heard us, and how well */
    char grid[8];
    char dist[16];               /* from their locator, else "~" from their country's middle */
    char country[32];
    int  flag;                   /* flags.h, -1 none */
} station_fields_t;

/* ---- Where a station is: its flag, country and distance, as the FT8 list shows them. From cty.dat by the call;
 * the distance from the locator it sent, else from its country's middle ("~"). Kept 30 s a call: the list is
 * drawn often and a locator may come later. */

typedef struct {
    char    call[JS8_RX_CALL_LEN];
    int64_t at;
    int     flag;
    char    country[32], prefix[8];
    int     km;         /* -1 unknown */
    bool    approx;     /* from the country */
} where_t;

#define WHERE_N     48
#define WHERE_MS    30000

static where_t where_cache[WHERE_N];
static int     where_next;

static const where_t *station_where(const char *call)
{
    int64_t now = now_mono_ms();

    if (!call || !call[0]) return NULL;
    for (int i = 0; i < WHERE_N; i++)
        if (where_cache[i].call[0] && !strcmp(where_cache[i].call, call) && now - where_cache[i].at < WHERE_MS)
            return &where_cache[i];

    where_t    *w = &where_cache[where_next];
    const char *name, *prefix;

    where_next = (where_next + 1) % WHERE_N;
    memset(w, 0, sizeof(*w));
    snprintf(w->call, sizeof(w->call), "%s", call);
    w->at   = now;
    w->flag = -1;
    w->km   = -1;
    if (dxcc_lookup(call, &name, &prefix)) {
        snprintf(w->country, sizeof(w->country), "%s", name);
        snprintf(w->prefix, sizeof(w->prefix), "%s", prefix);
        w->flag = table_view_flag_of(prefix);
    }

    const char *mine = station_qth();
    double      my_lat, my_lon, lat, lon;

    if (!mine[0]) return w;
    qth_str_to_pos(mine, &my_lat, &my_lon);

    js8_station_t st;

    if (find_station(call, &st) && st.grid[0]) {
        qth_str_to_pos(st.grid, &lat, &lon);
        w->km = (int)(qth_pos_dist(lat, lon, my_lat, my_lon) + 0.5);
    } else if (dxcc_pos(call, &lat, &lon)) {
        w->km     = (int)(qth_pos_dist(lat, lon, my_lat, my_lon) + 0.5);
        w->approx = true;
    }
    return w;
}

static lv_coord_t text_w(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static void draw_flag(lv_draw_ctx_t *ctx, int flag, lv_coord_t x, const lv_area_t *row)
{
    const lv_img_dsc_t *img = table_view_flag(flag);
    if (!img) return;

    lv_draw_img_dsc_t id;
    lv_area_t         a;
    lv_draw_img_dsc_init(&id);
    a.x1 = x;
    a.x2 = x + img->header.w - 1;
    a.y1 = (row->y1 + row->y2 - img->header.h) / 2;
    a.y2 = a.y1 + img->header.h - 1;
    lv_draw_img(ctx, &id, &a, img);
}

/* A message's sender, right-aligned after its text where it fits: flag, country and km; else the prefix; else
 * the flag and km; else nothing */
static void draw_message_where(lv_obj_draw_part_dsc_t *d, lv_obj_t *tbl, uint32_t row, const js8_rx_msg_t *msg)
{
    const where_t *w = station_where(msg->from);
    if (!w) return;

    lv_draw_label_dsc_t ld = *d->label_dsc;
    ld.font                = &sony_20;
    ld.opa                 = LV_OPA_80;

    /* After the first line: a translation (GhostNet) is a line of its own under it */
    const char *cell = lv_table_get_cell_value(tbl, row, 0);
    char        first[ROW_MAX];
    snprintf(first, sizeof(first), "%.*s", (int)strcspn(cell, "\n"), cell);

    lv_coord_t left     = d->draw_area->x1 + lv_obj_get_style_pad_left(tbl, LV_PART_ITEMS);
    lv_coord_t text_end = left + text_w(first, d->label_dsc->font);
    lv_coord_t right    = d->draw_area->x2 - 8;
    lv_area_t  line     = *d->draw_area; /* the first line, where the flag goes */
    char       km[16]   = "", with[56];

    line.y1 += lv_obj_get_style_pad_top(tbl, LV_PART_ITEMS);
    line.y2 = line.y1 + lv_font_get_line_height(d->label_dsc->font) - 1;

    if (w->km >= 0) snprintf(km, sizeof(km), "%s%d km", w->approx ? "~" : "", w->km);

    const char *names[3] = {w->country, w->prefix, ""};
    for (int i = 0; i < 3; i++) {
        if (i < 2 && !names[i][0]) continue;
        snprintf(with, sizeof(with), "%s%s%s", names[i], names[i][0] && km[0] ? "  " : "", km);
        lv_coord_t tw = text_w(with, ld.font), fw = w->flag >= 0 ? FLAG_W_JS8 + 6 : 0;
        if (right - tw - fw < text_end + 16) continue;

        lv_area_t a = *d->draw_area;
        a.x2 = right;
        a.x1 = right - tw;
        a.y1 += lv_obj_get_style_pad_top(tbl, LV_PART_ITEMS) + 2;
        lv_draw_label(d->draw_ctx, &ld, &a, with, NULL);
        if (fw) draw_flag(d->draw_ctx, w->flag, a.x1 - fw, &line);
        return;
    }
}

static void station_fields(const js8_station_t *stn, int64_t now, station_fields_t *out)
{
    memset(out, 0, sizeof(*out));
    js8_speed_t sp = js8_speed_from_submode(stn->submode);

    out->star[0] = stn->heard_me ? '*' : ' ';
    if (sp != JS8_SPEED_NORMAL) out->speed[0] = js8_speed_letter(sp);
    snprintf(out->call, sizeof(out->call), "%s", stn->call);
    snprintf(out->snr, sizeof(out->snr), "%+d", stn->snr);
    snprintf(out->grid, sizeof(out->grid), "%s", stn->grid);
    format_age(now - stn->heard_ms, out->age, sizeof(out->age));

    if (stn->heard_me) {
        char ago[8];
        format_age(now - stn->heard_me_ms, ago, sizeof(ago));
        if (!stn->has_reported_snr) snprintf(out->heard, sizeof(out->heard), "heard you (%s)", ago);
        else snprintf(out->heard, sizeof(out->heard), "heard you %+03d (%s)", stn->reported_snr, ago);
    }

    const where_t *w = station_where(stn->call);
    out->flag = w ? w->flag : -1;
    if (!w) return;
    snprintf(out->country, sizeof(out->country), "%s", w->country);
    if (w->km >= 0) snprintf(out->dist, sizeof(out->dist), "%s%d km", w->approx ? "~" : "", w->km);
}

/* Who is on the cursor's row (either view). False on text rows and on rows
 * of ours. */
static bool row_station(char *call, size_t call_len, float *hz, int *db)
{
    uint16_t r, c;
    lv_table_get_selected_cell(table, &r, &c);
    if (r >= rows || row_hist[r] < 0) return false;

    const char *who;
    if (!view_stations) {
        const js8_rx_msg_t *msg = &history[row_hist[r]];
        if (msg->tx || msg->from[0] == '\0') return false;
        who = msg->from;
        *hz = msg->freq_hz;
        *db = msg->snr;
    } else {
        const js8_station_t *stn = &st_rows[row_hist[r]];
        who                      = stn->call;
        *hz                      = stn->freq_hz;
        *db                      = stn->snr;
    }
    snprintf(call, call_len, "%s", who);
    return true;
}

/* The selected station with its most recent offset and SNR. */
static bool selected_station(char *out, size_t out_len, float *hz, int *db)
{
    if (sel_call[0] == '\0') return false;
    *hz = qso_freq;
    *db = sel_snr;
    snprintf(out, out_len, "%s", sel_call);
    return true;
}

static void rebuild_station_rows(void)
{
    /* The list is sorted again: keep the cursor on the selected station. */
    char  keep[JS8_RX_CALL_LEN];
    float unused_hz;
    int   unused_db;
    if (!selected_station(keep, sizeof(keep), &unused_hz, &unused_db)) keep[0] = '\0';

    st_count = js8_stations_list(stations, now_wall_ms(), st_rows, MAX_ROWS);
    int band = qso_log_freq_to_band(cparam_i_get(cfg_fg_freq));
    int at   = 0;

    table_empty();
    if (!st_count) append_row("No stations heard yet", -1);
    for (int k = 0; k < st_count; k++) {
        char word[16];
        st_alert[k]  = js8_alert_hit("", st_rows[k].call, alert_words, word, sizeof(word));
        st_worked[k] = qso_log_search_worked(st_rows[k].call, MODE_JS8, band) > 0;
        if (keep[0] && !strcmp(keep, st_rows[k].call)) at = rows;
        append_row(" ", (int16_t)k); /* the text is drawn by table_draw_end_cb */
    }
    select_row(at);
}

/* Background of a message row by kind, in order of importance. */
static lv_color_t message_bg(const js8_rx_msg_t *msg)
{
    if (msg->tx) return TC(row_tx);
    if (msg->alert) return lv_color_hex(ALERT_COLOR);
    if (msg->to_me || js8_to_x6100(msg, groups_text)) return TC(row_to_me);
    if (msg->cq) return TC(row_cq);
    if (msg->heartbeat) return TC(panel);
    if (msg->to_group) return TC(row_group);
    return TC(panel);
}

static void table_draw_cb(lv_event_t *ev)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(ev);
    if (d->part != LV_PART_ITEMS) return;

    lv_obj_t *tbl  = lv_event_get_target(ev);
    uint32_t  row  = d->id / lv_table_get_col_cnt(tbl);
    int16_t   slot = -1;
    if (row < rows) slot = row_hist[row];

    d->rect_dsc->bg_opa = LV_OPA_50;
    if (slot >= 0 && view_stations) {
        /* Heard us: highlighted, as desktop stars them. */
        if (st_alert[slot]) d->rect_dsc->bg_color = lv_color_hex(ALERT_COLOR);
        else d->rect_dsc->bg_color = st_rows[slot].heard_me ? TC(row_heard) : TC(panel);
    } else if (slot >= 0) {
        const js8_rx_msg_t *msg = &history[slot];
        d->rect_dsc->bg_color   = message_bg(msg);
        bool ours = msg->to_me || js8_to_x6100(msg, groups_text);
        if (!msg->tx && !msg->alert && !ours && !msg->cq && msg->heartbeat) d->label_dsc->color = TC(text_mute);
        if (msg->low_confidence) d->label_dsc->opa = LV_OPA_60;
    } else {
        d->rect_dsc->bg_color = TC(row);
        d->label_dsc->align   = LV_TEXT_ALIGN_CENTER;
    }

    uint16_t cur_row, cur_col;
    lv_table_get_selected_cell(tbl, &cur_row, &cur_col);
    if (cur_row == row && theme_shape.mono) {     /* 1bit: the cursor row framed heavy, no lighter tone */
        d->rect_dsc->border_color = TC(text);
        d->rect_dsc->border_opa = LV_OPA_COVER;
        d->rect_dsc->border_side = LV_BORDER_SIDE_FULL;
        d->rect_dsc->border_width = 2;
    } else if (cur_row == row) {
        d->rect_dsc->bg_color = lv_color_lighten(d->rect_dsc->bg_color, 30);
    }
}

/* Is the row about the selected station: sent by them, or by us to them? */
static bool row_is_selected_station(int16_t slot)
{
    if (slot < 0 || sel_call[0] == '\0') return false;
    if (view_stations) return !strcasecmp(sel_call, st_rows[slot].call);
    const js8_rx_msg_t *msg = &history[slot];
    if (!msg->tx) return !strcasecmp(sel_call, msg->from);
    size_t len = strlen(sel_call);
    if (strncasecmp(sel_call, msg->text, len)) return false;
    return msg->text[len] == '\0' || msg->text[len] == ' ';
}

/* Stations view: the columns of a row, each at its own x. */
static void draw_station_columns(lv_obj_draw_part_dsc_t *d, lv_obj_t *tbl, int16_t slot)
{
    station_fields_t cols;
    station_fields(&st_rows[slot], now_wall_ms(), &cols);
    const char *text[] = {cols.star, "", cols.call, cols.speed, cols.age, cols.snr, cols.heard, cols.country, cols.grid,
                          cols.dist};
    static const lv_coord_t col_x[] = {0, 16, 44, 158, 174, 222, 276, 476, 592, 660};
    const int               ncols   = (int)(sizeof(col_x) / sizeof(col_x[0]));

    /* Calls already logged are green, like the worked-before mark on desktop. */
    lv_draw_label_dsc_t logged = *d->label_dsc;
    logged.color               = TC(ok);

    lv_coord_t left = d->draw_area->x1 + lv_obj_get_style_pad_left(tbl, LV_PART_ITEMS);
    lv_area_t  cell = *d->draw_area;
    cell.y1 += lv_obj_get_style_pad_top(tbl, LV_PART_ITEMS);
    cell.y2 -= lv_obj_get_style_pad_bottom(tbl, LV_PART_ITEMS);
    for (int c = 0; c < ncols; c++) {
        cell.x1 = left + col_x[c];
        cell.x2 = c == ncols - 1 ? d->draw_area->x2 : left + col_x[c + 1] - 6;
        if (c == 1) {
            draw_flag(d->draw_ctx, cols.flag, cell.x1, d->draw_area);
            continue;
        }
        lv_draw_label_dsc_t style = (c == 2 && st_worked[slot]) ? logged : *d->label_dsc;
        if (c >= 7) style.font = &sony_20;  /* country, grid, km: the narrower font fits them */
        if (c >= 7) cell.y1 += 2;
        lv_draw_label(d->draw_ctx, &style, &cell, text[c], NULL);
        if (c >= 7) cell.y1 -= 2;
    }

    /* Heard sending to @X6100, while we are in it: a small tag at the end of
     * the row, right of the longest distance ("20000 km"). */
    if (st_rows[slot].x6100 && js8_x6100_joined(groups_text)) {
        lv_draw_label_dsc_t tag = *d->label_dsc;
        tag.font                = &sony_14;
        tag.align               = LV_TEXT_ALIGN_RIGHT;
        tag.align               = LV_TEXT_ALIGN_LEFT;
        cell.x1                 = left + 44 + text_w(cols.call, d->label_dsc->font) + 4;
        cell.x2                 = cell.x1 + 60;
        cell.y1 += 5;
        lv_draw_label(d->draw_ctx, &tag, &cell, "X6100", NULL);
    }
}

/* After a cell is drawn: the selected station's rows get a green bar at the
 * left, and the Stations view draws its columns. */
static void table_draw_end_cb(lv_event_t *ev)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(ev);
    if (d->part != LV_PART_ITEMS) return;

    lv_obj_t *tbl = lv_event_get_target(ev);
    uint32_t  row = d->id / lv_table_get_col_cnt(tbl);
    if (row >= rows || row_hist[row] < 0) return;
    int16_t slot = row_hist[row];

    if (row_is_selected_station(slot)) {
        lv_draw_rect_dsc_t mark;
        lv_draw_rect_dsc_init(&mark);
        mark.bg_color = TC(row_sel_bar);
        lv_area_t a   = *d->draw_area;
        a.x2          = a.x1 + 3;
        lv_draw_rect(d->draw_ctx, &mark, &a);
    }
    if (view_stations) draw_station_columns(d, tbl, slot);
    else if (!history[slot].tx && history[slot].from[0]) draw_message_where(d, tbl, row, &history[slot]);
}

/* The green line on the waterfall at the selected station's offset. */
static void show_selection(void)
{
    buttons_refresh(&btn_reply); /* "Reply\nTHEIRCALL" */
    buttons_refresh(&btn_chat);
    if (lane) lv_obj_invalidate(lane); /* the call selected stands out */
    lv_obj_t *line = finder;
    if (qso_freq < 0) lv_finder_clear_cursor(line);
    else lv_finder_set_cursor(line, (int16_t)(qso_freq + 0.5f));
    lv_obj_invalidate(line);
    lv_obj_invalidate(table); /* the green bars on its rows */
    update_tx_bar();          /* it names the selected station */
}

/* MFK or a tap moved the cursor: its station becomes the selected one. A
 * row without a station (ours, text) keeps the old selection. On a tap
 * (`announce`) a message says who it is; not on every MFK step. */
static bool select_at_cursor(bool announce)
{
    char  who[JS8_RX_CALL_LEN];
    float hz;
    int   db;
    if (!row_station(who, sizeof(who), &hz, &db)) return false;
    memcpy(sel_call, who, sizeof(sel_call));
    selection_move(hz, db);
    if (announce) msg_update_text_fmt("%s at %.0f Hz, %+d dB", who, hz, db);
    return true;
}

static void clear_selection(void)
{
    memset(sel_call, 0, sizeof(sel_call));
    board_sel_call[0] = '\0';
    buttons_refresh(&btn_reply);
    buttons_refresh(&btn_chat);
    selection_move(-1, sel_snr);
}

static void conv_open(void);

static void table_press_cb(lv_event_t *ev)
{
    LV_UNUSED(ev);
    if (board_on) { /* the list is hidden under the board, which has the press */
        board_press();
        return;
    }
    if (select_at_cursor(true) && !any_popup() && !composing) conv_open(); /* who it is, what was said */
}

static void table_select_cb(lv_event_t *ev)
{
    LV_UNUSED(ev);
    if (board_on) return; /* the hidden list's cursor selects nothing */
    if (!auto_selecting) {
        cursor_user_ms = now_wall_ms();
        select_at_cursor(false);
    }
}

/* ---- From the receiver (its threads, then the UI thread) --------------- */

static void ui_add_message(void *arg)
{
    if (!table || !dialog.run) return;
    const js8_rx_msg_t *m = arg;
    add_message(m);
    remote_msg(m); /* the browser's JS8 page */
}

static void on_message(const js8_rx_msg_t *msg, void *unused)
{
    LV_UNUSED(unused);
    scheduler_put(ui_add_message, (void *)msg, sizeof(js8_rx_msg_t));
}

/* The heartbeat part of the status line; returns its length. */
static int hb_flags(char *out, size_t size, int64_t now_ms)
{
    if (!js8_speed_heartbeats(cur_speed())) return snprintf(out, size, "HB paused (Turbo)  ");
    if (hb_next_ms <= now_ms) return snprintf(out, size, "HB %um  ", params.js8_hb_interval.x);
    time_t    when = (time_t)(hb_next_ms / 1000);
    struct tm t;
    gmtime_r(&when, &t);
    return snprintf(out, size, "HB %um next %02d:%02d  ", params.js8_hb_interval.x, t.tm_hour, t.tm_min);
}

/* The line at the top: whatever may transmit on its own comes first, then
 * where we are, the UTC time and drift, and the number of decodes. */
static void update_status(void)
{
    int64_t   now_ms = now_wall_ms();
    time_t    secs   = (time_t)(now_ms / 1000);
    struct tm utc;
    gmtime_r(&secs, &utc);

    char   flags[80];
    size_t len = 0;
    flags[0]   = '\0';
    if ((params.js8_auto.x || params.js8_hb.x) && js8_auto_idle(autop, now_ms)) {
        len += snprintf(flags + len, sizeof(flags) - len, "AUTO/HB PAUSED (idle)  ");
    } else {
        if (params.js8_auto.x) len += snprintf(flags + len, sizeof(flags) - len, "AUTO  ");
        if (params.js8_hb.x) len += hb_flags(flags + len, sizeof(flags) - len, now_ms);
        if (params.js8_auto.x && params.js8_hb.x && params.js8_hb_ack.x)
            len += snprintf(flags + len, sizeof(flags) - len, "ACK  ");
    }
    int fresh = js8_inbox_unread(inbox);
    if (fresh) snprintf(flags + len, sizeof(flags) - len, "MSG %d NEW  ", fresh);

    /* How far JS8 time is from the radio's clock (Time Sync). */
    char    drift[24];
    int64_t off = js8_drift_ms();
    drift[0]    = '\0';
    if (off != 0) snprintf(drift, sizeof(drift), " drift %+.1fs", off / 1000.0);

    const char *wav = js8_rx_wav_active(rx) ? "TEST WAV  " : "";
    lv_label_set_text_fmt(status, "%s%s%s  %02d:%02d:%02dZ%s  total %u", flags, wav, where_label(), utc.tm_hour,
                          utc.tm_min, utc.tm_sec, drift, hist_count);
}

static void ui_cycle_done(void *arg)
{
    if (!status || !dialog.run) return;
    cycle_decodes = *(const unsigned *)arg;
    ++cycles;
    update_status(); /* the clock and the decode total */
}

static void on_cycle_done(unsigned decodes, void *unused)
{
    LV_UNUSED(unused);
    scheduler_put(ui_cycle_done, &decodes, sizeof(unsigned));
}

/* A waterfall row on its way to the UI thread. */
typedef struct {
    float   *psd;  /* WIDTH values, dB over the noise floor; the UI thread frees it */
    uint16_t size; /* WIDTH */
} wf_data_t;

static void wf_queue_pop(void)
{
    unsigned h = wf_q_head;
    free(wf_queue[h]);
    wf_q_head = (h + 1) % WF_QUEUE;
    --wf_q_count;
}

static void wf_queue_clear(void)
{
    while (wf_q_count > 0) wf_queue_pop();
}

static void ui_waterfall_add(void *arg)
{
    wf_data_t *row = arg;
    if (!waterfall || !dialog.run) {
        free(row->psd);
        return;
    }
    if (wf_q_count >= WF_QUEUE) wf_queue_pop(); /* too far behind: lose the oldest row */
    unsigned at       = (wf_q_head + wf_q_count++) % WF_QUEUE;
    wf_queue[at]      = row->psd;
    wf_queue_size[at] = row->size;
}

static int64_t now_mono_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static int64_t now_mono_ms(void)
{
    return now_mono_us() / 1000;
}

/* Draws one queued row per call at most, when the clock says it's due.
 * Rows come per 735 samples at 11025 Hz on the audio's clock, which isn't
 * the CPU's: when rows pile up they are drawn a little faster until the
 * queue is short again, so the picture never jumps. */
static void wf_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (wf_q_count == 0) return;

    int64_t step = 1000000 / WF_ROWS_PER_SEC;
    if (wf_q_count > 6) step = step * 3 / 4;
    else if (wf_q_count > 3) step = step * 19 / 20;

    int64_t t_us = now_mono_us();
    if (t_us - wf_due_us > step) wf_due_us = t_us; /* audio stopped for a while (TX): restart the pace */
    if (t_us < wf_due_us) return;
    wf_due_us += step;

    unsigned h = wf_q_head;
    lv_waterfall_add_data(waterfall, wf_queue[h], wf_queue_size[h]);
    remote_wf_row(wf_queue[h], wf_queue_size[h]); /* the browser's JS8 page */
    wf_queue_pop();
    /* Paint it now. Waiting for LVGL's own refresh (every 33 ms of a slow
     * tick) put rows 66 to 134 ms apart instead of evenly. */
    lv_refr_now(NULL); /* NULL: every display */
}

static int float_order(const void *pa, const void *pb)
{
    float a = *(const float *)pa, b = *(const float *)pb;
    if (a < b) return -1;
    return a > b;
}

/* The row's 30th percentile, taken as its noise floor. False if out of memory. */
static bool percentile30(const float *row, float *out)
{
    float *tmp = malloc(WIDTH * sizeof(float));
    if (!tmp) return false;
    memcpy(tmp, row, WIDTH * sizeof(float));
    qsort(tmp, WIDTH, sizeof(float), float_order);
    *out = tmp[WIDTH * 3 / 10];
    free(tmp);
    return true;
}

/* Receiver thread: turn the spectrum gathered so far into a row of WIDTH
 * pixels over the filter's passband and send it to the UI thread. */
static void wf_emit_row(void)
{
    float   *spec = psd;
    unsigned n    = nfft;
    spgramf_get_psd(sg, spec);
    liquid_vectorf_addscalar(spec, n, -10.f * log10f(sqrtf(n)), spec);
    spgramf_reset(sg); /* the next row starts empty */

    /* The spectrum is centred: 0 Hz is bin n/2. */
    uint32_t first = n / 2u + (uint32_t)n * filter_low / SAMPLE_RATE;
    uint32_t end   = n / 2u + (uint32_t)n * filter_high / SAMPLE_RATE;
    if (end > n) end = n;
    if (end <= first) return;

    float *px = malloc(WIDTH * sizeof(float));
    if (!px) return;
    /* Each pixel takes its loudest bin, so a narrow signal falling between
     * two pixels doesn't flicker. */
    uint32_t span = end - first;
    for (uint32_t col = 0; col < WIDTH; col++) {
        uint32_t lo = first + col * span / WIDTH, hi = first + (col + 1) * span / WIDTH;
        float    top = spec[lo];
        for (uint32_t b = lo + 1; b < hi; b++)
            if (spec[b] > top) top = spec[b];
        px[col] = top;
    }

    /* Followed slowly, over about 2 s. */
    float level;
    if (percentile30(px, &level)) {
        if (wf_floor_set) {
            wf_floor_db += 0.05f * (level - wf_floor_db);
        } else {
            wf_floor_db  = level;
            wf_floor_set = true; /* from now on it only follows */
        }
    }
    for (int col = 0; col < WIDTH; col++) px[col] -= wf_floor_db;

    wf_data_t row = {.psd = px, .size = WIDTH};
    /* A full queue refuses it: the row is lost, not its memory (audit C06) */
    if (!scheduler_put(ui_waterfall_add, &row, sizeof(row))) free(px);
}

/* Receiver thread: the audio the decoder got, for the waterfall. A row is
 * made every WF_ROW_SAMPLES whatever the block sizes. Samples that are
 * exactly zero come from the beep guard (real audio never is): they are
 * left out, so the waterfall waits rather than drawing a dark stripe. */
static void on_audio(const float *samples, unsigned n, void *unused)
{
    LV_UNUSED(unused);
    if (sg == NULL) return;
    const float *p   = samples;
    const float *end = samples + n;
    for (;;) {
        while (p < end && *p == 0.0f) p++;
        if (p == end) break;
        unsigned room = WF_ROW_SAMPLES - wf_row_fill, len = 0;
        while (len < room && p + len < end && p[len] != 0.0f) len++;
        spgramf_write(sg, (float *)p, len);
        p += len;
        wf_row_fill += len;
        if (wf_row_fill < WF_ROW_SAMPLES) continue;
        wf_row_fill = 0;
        wf_emit_row();
    }
}

/* ---- Starting and stopping the receiver -------------------------------- */

static void rx_start(void)
{
    /* FFT size: a bin or more per pixel, as a power of two (FFTW does 4096
     * several times faster than 3035 = 5 x 607). Clamped to 256..8192. */
    int      width_hz = filter_high - filter_low;
    unsigned need     = 4096;
    if (width_hz > 0) need = WIDTH * SAMPLE_RATE / width_hz;
    for (nfft = 256; nfft < 8192 && nfft < need;) nfft <<= 1;
    /* Hop at most half a row so every row gets a transform. */
    unsigned hop = WF_ROW_SAMPLES / 2;
    if (nfft / 2 < hop) hop = nfft / 2;
    sg  = spgramf_create(nfft, LIQUID_WINDOW_HANN, nfft, hop);
    psd = malloc(nfft * sizeof(float));
    wf_floor_set = false; /* the floor is measured again */
    wf_row_fill = 0;

    js8_rx_cb_t hooks = {.on_message = on_message, .on_cycle_done = on_cycle_done, .on_audio = on_audio};
    js8_rx_t   *dec   = js8_rx_create(SAMPLE_RATE, rx_speed_mask(), params.callsign.x, &hooks);
    rx                = dec;
    if (dec == NULL) msg_schedule_text_fmt("JS8: cannot start decoder");
    js8_rx_set_decode_range(dec, filter_low, filter_high);
    js8_rx_set_qso_offset(dec, params.js8_tx_freq.x);
}

static void rx_stop(void)
{
    /* Destroying the receiver waits for its threads to end, so nothing uses
     * the spectrum once it returns. */
    js8_rx_t *dec = rx;
    js8_rx_destroy(dec);
    rx = NULL; /* audio_cb and the rest see it closed */
    if (sg != NULL) spgramf_destroy(sg);
    sg = NULL;
    float *old = psd;
    psd        = NULL;
    free(old);
}

static bool beep_guard_active(void)
{
    if (atomic_load(&beep_guard)) return true;
    return atomic_load(&beep_guard_end) > now_mono_ms();
}

static double sum_squares(const float *x, unsigned n)
{
    double total = 0;
    for (const float *end = x + n; x < end; x++) total += (double)*x * *x;
    return total;
}

/* Audio thread: receiver audio for the decoder. */
static void audio_cb(unsigned int count, float *buf)
{
    if (keyed) return; /* transmitting: nothing worth decoding */
    if (!beep_guard_active()) {
        if (count > 0) {
            float db = (float)(10.0 * log10(sum_squares(buf, count) / count + 1e-24));
            if (beep_level_before_db < -150.0f) beep_level_before_db = db;
            else beep_level_before_db = 0.9f * beep_level_before_db + 0.1f * db;
        }
        js8_rx_feed(rx, buf, count);
        return;
    }
    /* Beeping: note the level that came in, feed zeros instead. */
    static const float zeros[512];
    atomic_fetch_add(&beep_guard_sq, (int64_t)(sum_squares(buf, count) * 1e9));
    atomic_fetch_add(&beep_guard_n, count);
    for (unsigned left = count; left > 0;) {
        unsigned chunk = left > 512 ? 512 : left;
        js8_rx_feed(rx, zeros, chunk);
        left -= chunk;
    }
}

/* ---- Sending ----------------------------------------------------------- */

static bool tx_abort_check(void *unused)
{
    LV_UNUSED(unused);
    return js8_tx_stopping(tx); /* Stop TX was pressed */
}

/* TX thread, once per frame. tx_player keys the radio, moves the VFO so its
 * 1325 Hz tone sits at our offset, and puts PTT and VFO back afterwards. */
static bool tx_play(int16_t *samples, unsigned n, int index, int count, void *unused)
{
    LV_UNUSED(index);
    LV_UNUSED(count);
    LV_UNUSED(unused);
    keyed = true;
    /* A beep notices `keyed` and ends within one of its parts; wait for that,
     * but never more than 2 s, and if it is still on, cut the speaker. */
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_sec += 2;
    if (pthread_mutex_timedlock(&speaker_lock, &limit) != 0) radio_speaker_play(false);
    else pthread_mutex_unlock(&speaker_lock);
    /* Read each frame: the power may be changed while the app is open. */
    float gain       = tx_player_base_gain_offset();
    base_gain_offset = gain;
    tx_info_set_length(js8_speed_period_s(cur_speed()));   /* the slot, in the strip */
    bool ok          = tx_player_play(samples, n, tx_offset_active, gain, tx_abort_check, NULL);
    keyed            = false;
    return ok;
}

/* UTC in JS8 time as hhmmss. */
static int current_utc_hhmmss(void)
{
    time_t    secs = (time_t)(now_wall_ms() / 1000);
    struct tm t;
    gmtime_r(&secs, &t);
    return (t.tm_hour * 100 + t.tm_min) * 100 + t.tm_sec;
}

static void ui_tx_status(void *arg)
{
    if (!tx_bar || !dialog.run) return;
    const js8_tx_status_t *st = arg;

    /* The message goes into the list as its first frame starts. */
    bool first_frame = st->state == JS8_TX_KEYING && st->frame == 1;
    if (first_frame && tx_status.state != JS8_TX_KEYING) {
        js8_rx_msg_t ours = {0};
        ours.tx           = true;
        ours.utc          = current_utc_hhmmss();
        ours.freq_hz      = st->offset_hz;
        ours.submode      = (uint8_t)js8_speed_submode(st->speed);
        snprintf(ours.text, sizeof(ours.text), "%s", tx_preview[0] ? tx_preview : st->text);
        add_message(&ours);
        remote_msg(&ours); /* the browser's JS8 page */
        char done_call[JS8_RX_CALL_LEN];
        if (js8_qsos_sent(qsos, ours.text, params.callsign.x, now_wall_ms(), done_call, sizeof(done_call)))
            log_offer(done_call);
    }
    memcpy(&tx_status, st, sizeof(tx_status));
    update_tx_bar(); /* the new state on the TX line */
}

static void on_tx_status(const js8_tx_status_t *st, void *unused)
{
    LV_UNUSED(unused);
    scheduler_put(ui_tx_status, (void *)st, sizeof(js8_tx_status_t));
}

/* Like desktop restarting its CQ timer after any TX, the auto CQ interval
 * starts again when we stop sending. */
static void auto_cq_after_tx(void)
{
    if (!auto_cq) return; /* nothing to restart */
    auto_cq_from_ms = now_wall_ms();
    auto_cq_next_ms = cq_interval_ms() + auto_cq_from_ms;
    if (btn_cq.disp_btn) buttons_refresh(&btn_cq); /* it counts down to the next CQ */
}

static void ui_tx_done(void *arg)
{
    if (!tx_bar || !dialog.run) return;
    const tx_done_t *end = arg;
    if (!end->completed) add_info_row("TX stopped");
    deliver_end(end->text, end->completed);

    tx_status = (js8_tx_status_t){0};
    update_tx_bar(); /* back to "ready" */
    auto_cq_after_tx();
    /* Now send the auto reply that had to wait. */
    if (!pending_auto_valid) return;
    pending_auto_valid = false; /* it goes once */
    auto_send(&pending_auto);
}

static void on_tx_done(const char *text, bool completed, void *unused)
{
    LV_UNUSED(unused);
    tx_done_t end;
    memset(&end, 0, sizeof(end));
    end.completed = completed;
    if (text) snprintf(end.text, sizeof(end.text), "%s", text);
    scheduler_put(ui_tx_done, &end, sizeof(end));
}

static void tx_start(void)
{
    js8_tx_cb_t hooks = {.play = tx_play, .on_status = on_tx_status, .on_done = on_tx_done};
    keyed             = false;
    tx_status         = (js8_tx_status_t){0};
    tx                = js8_tx_create(AUDIO_PLAY_RATE, TX_PLAYER_AUDIO_HZ, &hooks);
    if (tx == NULL) msg_schedule_text_fmt("JS8: cannot start transmitter");
}

static void tx_stop_all(void)
{
    js8_tx_destroy(tx); /* stops sending, returns once the frame on the air is over */
    keyed = false;
    tx    = NULL;
}

/* Once a second from tx_timer_cb. The status line's clock also moves on
 * decode cycles, but those stop while we transmit. */
static void every_second(void)
{
    lane_update(); /* calls heard, their ages */
    board_update(); /* the cards: who heard us, the ages */
    if (conv_root) { /* chosen elsewhere (the map, the web page): the conversations follow, or close with none */
        if (!sel_call[0]) conv_close();
        else if (strcasecmp(sel_call, conv_call_now())) {
            conv_choose(sel_call);
            conv_fill();
        }
    }
    if (alert_box && now_mono_ms() - alert_opened_ms > 5 * 60 * 1000) alert_close(); /* left: it is in the Inbox */
    hb_tick();
    auto_cq_tick();
    update_status(); /* keeps the clock going */
    hb_main_follow();
}

/* Every 250 ms while the app is open. */
static void tx_timer_cb(lv_timer_t *timer)
{
    static unsigned n;
    LV_UNUSED(timer);
    swr_guard_tick();
    update_tx_bar(); /* the countdown */
    rstat_update(); /* the browser's JS8 page */
    n++;
    if (n % 4 == 0) every_second();
    if (n % 20 == 0 && view_stations) rebuild_station_rows(); /* the ages move on */
}

/* The TX line, e.g. "TX 1500 Hz Normal   ready", "... 1/3 starts in 9 s
 * K2XYZ SNR?", "... sending 2/3 ...". While a knob setting is open it says
 * how to use it instead. */
static void update_tx_bar(void)
{
    lv_obj_t *bar = tx_bar;
    if (bar == NULL) return;
    char       line[JS8_RX_TEXT_LEN + 64];
    lv_color_t bg   = TC(warn_bg);
    bool       idle = tx_status.state == JS8_TX_IDLE;

    if (idle && (cq_adjusting || hb_adjusting)) {
        if (cq_adjusting)
            snprintf(line, sizeof(line),
                     "Auto CQ %u min after each CQ: turn the knob (%d-%d), press CQ when done",
                     params.js8_cq_interval.x, CQ_MIN_INTERVAL, CQ_MAX_INTERVAL);
        else
            snprintf(line, sizeof(line), "HB every %u min: turn the knob (5-30), press HB when done",
                     params.js8_hb_interval.x);
        lv_obj_set_style_bg_color(bar, bg, 0);
        lv_label_set_text(bar, line);
        return;
    }

    const char *speed = js8_speed_name(tx_status.speed);
    if (tx_status.state == JS8_TX_KEYING) {
        snprintf(line, sizeof(line), "TX %4.0f Hz %s  sending %d/%d   %s", tx_status.offset_hz, speed,
                 tx_status.frame, tx_status.frames, tx_status.text);
        bg = TC(tx_bg);
    } else if (tx_status.state == JS8_TX_WAITING) {
        /* Slots are in JS8 time. The count comes before the text so a long
         * message only loses its tail. */
        int wait_s = (int)((tx_status.next_ms - now_wall_ms() + 999) / 1000);
        snprintf(line, sizeof(line), "TX %4.0f Hz %s  %d/%d %s %d s   %s", tx_status.offset_hz, speed,
                 tx_status.frame, tx_status.frames, tx_status.frame == 1 ? "starts in" : "next in",
                 wait_s > 0 ? wait_s : 0, tx_status.text);
    } else {
        uint16_t at = params.js8_tx_freq.x;
        snprintf(line, sizeof(line), "TX %4u Hz %s   ready%s%s%s%s", at, js8_speed_name(cur_speed()),
                 params.callsign.x[0] ? "" : "  (set your callsign: Settings > General)",
                 sel_call[0] ? "      selected: " : "", sel_call, auto_cq ? "      auto CQ" : "");
        bg = TC(list);
    }
    lv_obj_set_style_bg_color(bar, bg, 0);
    lv_label_set_text(bar, line);

    /* Red border round the waterfall while on the air. */
    lv_obj_set_style_border_width(waterfall, tx_status.state == JS8_TX_KEYING ? 3 : 0, 0);
    lv_obj_set_style_border_color(waterfall, TC(tx), 0);
}

/* Does the text start with something that looks like a call ("N0XYZ ...":
 * letters and digits, both present, maybe a /)? Then it is a directed
 * message, and the call is copied out. CQ and @groups are not. */
static bool starts_with_call(const char *text, char *call, size_t call_size)
{
    if (text[0] == '@' || !strncmp(text, "CQ", 2)) return false;
    size_t word = strcspn(text, " ");
    if (word < 3 || word >= call_size) return false;
    int letters = 0, digits = 0;
    for (const char *c = text; c < text + word; c++) {
        if (*c >= '0' && *c <= '9') digits++;
        else if (*c >= 'A' && *c <= 'Z') letters++;
        else if (*c != '/') return false;
    }
    if (letters == 0 || digits == 0) return false;
    memcpy(call, text, word);
    call[word] = '\0';
    return true;
}

/* Shows why something can't be done; false. */
#define REFUSE(...) (msg_update_text_fmt(__VA_ARGS__), false)

/* Queue `message` at `offset_hz`. On failure (a character JS8 can't send,
 * something already on its way...) a message says why and it returns false.
 * `automatic`: an auto reply, not typed by the user. */
static bool tx_queue_at(const char *message, int offset_hz, bool automatic)
{
    if (js8_tx_busy(tx)) return REFUSE("Already sending - Stop TX first");
    js8_speed_t      speed = cur_speed();
    js8_tx_preview_t plan;
    js8_tx_preview(params.callsign.x, station_qth(), message, speed, &plan);
    if (!plan.ok) return REFUSE("JS8: %s", plan.error);

    tx_offset_active = offset_hz;
    snprintf(tx_preview, sizeof(tx_preview), "%s", plan.preview);
    char why[JS8_TX_ERR_LEN];
    if (!js8_tx_send(tx, params.callsign.x, station_qth(), message, (float)offset_hz, speed, why, sizeof(why)))
        return REFUSE("JS8: %s", why);
    msg_update_text_fmt("%sQueued: %d frame%s, %.0f s", automatic ? "Auto: " : "", plan.frames,
                        plan.frames != 1 ? "s" : "", plan.seconds);
    snprintf(last_tx_text, sizeof(last_tx_text), "%s", message);

    /* Sending a directed message by hand starts a QSO with them. */
    char them[JS8_RX_CALL_LEN];
    if (automatic || !starts_with_call(message, them, sizeof(them))) return true;
    auto_cq_stop("replying");
    qso_started(them);
    return true;
}

/* Typed by the user, at our TX offset (the red band). */
static bool tx_queue(const char *message)
{
    int at = params.js8_tx_freq.x;
    return tx_queue_at(message, at, false);
}

/* Move our TX offset, kept inside what the TX speed allows. */
static void set_tx_offset(int32_t hz)
{
    int32_t top = js8_speed_max_offset_hz(cur_speed());
    if (hz < JS8_TX_MIN_OFFSET) hz = JS8_TX_MIN_OFFSET;
    if (hz > top) hz = top;
    params_uint16_set(&params.js8_tx_freq, (uint16_t)hz);
    js8_rx_set_qso_offset(rx, hz);
    lv_obj_t *band = finder;
    lv_finder_set_value(band, (int16_t)hz);
    lv_obj_invalidate(band);
    if (lane) lv_obj_invalidate(lane); /* TX moves there too */
    update_tx_bar(); /* shows the new offset */
}

/* Hold on (the default, and good JS8 manners): we answer on our own offset.
 * Hold off: we move to theirs. */
static void apply_hold(float their_freq)
{
    if (!params.js8_hold_offset.x) set_tx_offset((int32_t)(their_freq + 0.5f));
}

/* The main knob while it sets the heartbeat interval. */
static void hb_interval_turn(int32_t steps)
{
    int minutes = (int)params.js8_hb_interval.x + (steps > 0 ? 1 : -1);
    if (minutes > JS8_HB_MAX_INTERVAL) minutes = JS8_HB_MAX_INTERVAL;
    if (minutes < JS8_HB_MIN_INTERVAL) minutes = JS8_HB_MIN_INTERVAL;
    params_uint16_set(&params.js8_hb_interval, (uint16_t)minutes);
    hb_adjust_ms = now_wall_ms();
    if (btn_hbauto.disp_btn) buttons_refresh(&btn_hbauto); /* it shows the interval */
    if (hb_next_ms && params.js8_hb.x) hb_next_ms = js8_next_heartbeat_ms(now_wall_ms(), minutes);
    update_tx_bar(); /* the interval in the hint */
    update_status(); /* and the next HB time */
}

/* Main knob. Normally it moves our TX offset, as in the FT8 app (the dial
 * frequency doesn't move); it zooms the map when that is open and sets an
 * interval while one is being set. */
static void rotary_cb(int32_t steps)
{
    user_touch(); /* someone is at the radio */
    if (map_obj) {
        js8_map_zoom(steps);
    } else if (cq_adjusting) {
        cq_adjust_turn(steps);
    } else if (hb_adjusting) {
        hb_interval_turn(steps);
    } else {
        /* Turned fast, it takes bigger steps. */
        int32_t pace = steps < 0 ? -steps : steps;
        if (pace >= 6) steps *= 10;
        else if (pace >= 4) steps *= 5;
        set_tx_offset((int32_t)params.js8_tx_freq.x + steps);
    }
}

/* ---- Compose window ---------------------------------------------------- */

/* The frame count plans the whole message, which on the radio takes a few
 * tenths of a second past ~8 frames and a second or two near 20: done on
 * every key it froze the screen (up to 23 s typing a long message fast).
 * So it is counted once typing pauses (desktop's m_txTextDirtyDebounce,
 * 100 ms there; longer here for the slower processor), on a thread of its
 * own at nice 10. The newest request wins; the thread stays for the
 * program's life, idle between counts. */
#define COMPOSE_COUNT_MS      350
#define COMPOSE_COUNT_POLL_MS 50
static lv_timer_t *compose_count_timer; /* the pause */
static lv_timer_t *compose_poll_timer;  /* waits for the count */

static struct count_job {
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    bool             started;
    unsigned         want; /* the newest request */
    unsigned         done; /* the request `pv` answers */
    char             call[32], grid[16], text[TX_TEXT_MAX * 2];
    js8_speed_t      speed;
    js8_tx_preview_t pv;   /* the count for `done` */
} counter = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *count_thread(void *arg)
{
    LV_UNUSED(arg);
    pthread_setname_np(pthread_self(), "js8-count");
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10); /* behind the decoders and the GUI */
    static char      call[sizeof(counter.call)], grid[sizeof(counter.grid)], text[sizeof(counter.text)];
    js8_tx_preview_t out;
    pthread_mutex_lock(&counter.mu);
    for (;;) {
        while (counter.done == counter.want) pthread_cond_wait(&counter.cv, &counter.mu);
        unsigned    n     = counter.want;
        js8_speed_t speed = counter.speed;
        memcpy(call, counter.call, sizeof(call));
        memcpy(grid, counter.grid, sizeof(grid));
        memcpy(text, counter.text, sizeof(text));
        pthread_mutex_unlock(&counter.mu);
        js8_tx_preview(call, grid, text, speed, &out);
        pthread_mutex_lock(&counter.mu);
        if (n == counter.want) counter.pv = out; /* else typed on meanwhile: count again */
        counter.done = n;
    }
    return NULL;
}

static void compose_count_cancel(void)
{
    if (compose_count_timer) {
        lv_timer_del(compose_count_timer);
        compose_count_timer = NULL;
    }
    if (compose_poll_timer) { /* a count still running is just never shown */
        lv_timer_del(compose_poll_timer);
        compose_poll_timer = NULL;
    }
}

static void compose_poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    js8_tx_preview_t got;
    pthread_mutex_lock(&counter.mu);
    bool ready = counter.done == counter.want;
    if (ready) got = counter.pv;
    pthread_mutex_unlock(&counter.mu);
    if (!ready) return;
    lv_timer_del(compose_poll_timer);
    compose_poll_timer = NULL;
    if (!composing || !got.ok) return;
    msg_update_text_fmt("%d frame%s, %.0f s", got.frames, got.frames != 1 ? "s" : "", got.seconds);
}

static void compose_count_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    compose_count_timer = NULL; /* a one-shot: LVGL deletes it after this */
    if (!composing) return;     /* closed meanwhile */
    char typed[TX_TEXT_MAX * 2];
    macros_fill(textarea_window_get(), false, typed, sizeof(typed)); /* counted as it goes out */
    pthread_mutex_lock(&counter.mu);
    snprintf(counter.call, sizeof(counter.call), "%s", params.callsign.x);
    snprintf(counter.grid, sizeof(counter.grid), "%s", station_qth());
    snprintf(counter.text, sizeof(counter.text), "%s", typed);
    counter.speed = cur_speed();
    counter.want++;
    if (!counter.started) {
        pthread_t th;
        if (pthread_create(&th, NULL, count_thread, NULL) == 0) {
            pthread_detach(th);
            counter.started = true;
        }
    }
    pthread_cond_signal(&counter.cv);
    bool started = counter.started;
    pthread_mutex_unlock(&counter.mu);
    if (!started) return; /* no thread: no count, typing still works */
    if (!compose_poll_timer) compose_poll_timer = lv_timer_create(compose_poll_cb, COMPOSE_COUNT_POLL_MS, NULL);
}

static void compose_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (compose_count_timer) {
        lv_timer_reset(compose_count_timer);
        return;
    }
    compose_count_timer = lv_timer_create(compose_count_cb, COMPOSE_COUNT_MS, NULL);
    lv_timer_set_repeat_count(compose_count_timer, 1);
}

/* JS8 has no lower case. Small letters (a USB keyboard with Caps Lock off,
 * the "abc" page of the on-screen keyboard) are turned into capitals before
 * they are inserted, rather than being refused. */
static void compose_insert_cb(lv_event_t *ev)
{
    static char caps[8]; /* must outlive the event: the textarea reads it afterwards */
    const char *typed = lv_event_get_param(ev);
    if (strlen(typed) >= sizeof(caps)) return;
    bool changed = false;
    int  i       = 0;
    do {
        char c = typed[i];
        if (c >= 'a' && c <= 'z') {
            c       = (char)(c - 'a' + 'A');
            changed = true;
        }
        caps[i] = c;
    } while (typed[i++]);
    if (changed) lv_textarea_set_insert_replace(lv_event_get_target(ev), caps);
}

/* While typing, the text box sits at the top of the screen and the list
 * fills what the on-screen keyboard leaves free (over the waterfall), so
 * the other side's words can still be read as they come in. */
static void compose_layout(bool typing)
{
    lv_obj_t *list = table, *bar = tx_bar;
    if (list == NULL) return;
    if (typing && !keyboard_ready()) {
        lv_area_t dlg;
        lv_obj_get_coords(dialog.obj, &dlg);
        lv_coord_t kb_y = lv_obj_get_height(lv_scr_act()) / 2; /* lv_keyboard takes the lower half */
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        if (hb_info) lv_obj_add_flag(hb_info, LV_OBJ_FLAG_HIDDEN); /* it goes with the TX line */
        lv_obj_set_pos(list, 13, 13);
        lv_obj_set_size(list, WIDTH, kb_y - dlg.y1 - 15);
    } else {
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
        if (hb_info) lv_obj_clear_flag(hb_info, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(list, 13, 13 + TX_BAR_H + WF_VISIBLE);
        lv_obj_set_size(list, WIDTH, WF_HEIGHT - TX_BAR_H - WF_VISIBLE);
    }
    if (at_bottom()) follow(); /* the list got shorter or taller */
}

static void compose_close(void)
{
    if (composing) {
        compose_count_cancel();
        textarea_window_close();
        composing = false;
        compose_layout(false);
        if (table) table_takes_keys(); /* the list, or the conversations in front of it */
    }
}

/* Leaves the edit and closes the window. */
static void edit_finish(void)
{
    edit_target = 0;  /* back to composing messages */
    compose_close();  /* and the list gets its keys back */
}

/* A message closed unsent (ESC, the MFK held, its button again) comes back in the next window that starts as it
 * does (Send..., or Reply to the same station); one sent is forgotten (#280) */
static char compose_draft[TX_TEXT_MAX + 1];
static char compose_first[TX_TEXT_MAX + 1]; /* what the window opened with: closed as it was, the draft stays */

/* A message window closing unsent: what was typed in is kept (true), a text emptied forgets the draft */
static bool compose_keep(void)
{
    if (!composing || edit_target != 0 || !strcmp(textarea_window_get(), compose_first)) return false;
    snprintf(compose_draft, sizeof(compose_draft), "%s", textarea_window_get());
    if (compose_draft[strspn(compose_draft, " ")]) return true;
    compose_draft[0] = '\0';
    return false;
}

/* OK in the compose window. As in the FT8 app's keyboard we close it here
 * and return true, which makes textarea_window's own close do nothing;
 * false keeps it open. */
/* GhostNet's "Mine into English first": the first OK sends the text to MyMemory and puts the English into the box;
 * the next OK sends what is in the box then, checked or changed. Each time the window opens */
static bool     tx_tr_done;   /* the translation came, or could not: OK sends */
static bool     tx_tr_busy;   /* asked, no answer yet */
static uint32_t tx_tr_window; /* counts the windows opened: an answer for one closed since is dropped */

static void trans_tx_done(uint32_t id, int32_t utc, ghostnet_tr_result_t result, const char *text) {
    (void)utc;
    tx_tr_busy = false;
    if (id != tx_tr_window || !composing || edit_target != 0) return; /* that window was closed meanwhile */
    tx_tr_done = true;
    switch (result) {
    case GHOSTNET_TR_OK:
        textarea_window_set(text);
        msg_update_text_fmt("In English: check, OK sends");
        break;
    case GHOSTNET_TR_SKIP:  msg_update_text_fmt("English already: OK sends it"); break;
    case GHOSTNET_TR_NET:   msg_update_text_fmt("No MyMemory: OK sends as typed"); break;
    case GHOSTNET_TR_LIMIT: msg_update_text_fmt("Limit (%s h): OK sends as typed", text); break;
    default:                msg_update_text_fmt("Not translated: OK sends as is"); break;
    }
}

/* True: wait, the window stays open for the English */
static bool trans_tx_first(const char *typed) {
    char lang[16];
    ghostnet_tr_lang(i18n_code(), lang, sizeof(lang));
    if (!ghostnet_conf()->translate_tx || tx_tr_done || !typed[strspn(typed, " ")] || !strcmp(lang, "en"))
        return false; /* with the GUI in English it is English already */
    if (!tx_tr_busy) {
        if (!ghostnet_translate(GHOSTNET_TR_TX, typed, tx_tr_window, 0, trans_tx_done)) {
            tx_tr_done = true;
            msg_update_text_fmt("Busy: OK sends as typed");
            return true;
        }
        tx_tr_busy = true;
    }
    msg_update_text_fmt("Into English...");
    return true;
}

static bool compose_ok_cb(void)
{
    const char *typed = textarea_window_get();

    switch (edit_target) {
    case 0: {
        if (trans_tx_first(typed)) return false; /* window stays: the English comes into it */
        /* Macros in the message are filled in on the way out, as on
         * desktop; one that can't be (<CALL> with no station selected) is
         * sent as typed. */
        char filled[TX_TEXT_MAX * 2], out[TX_TEXT_MAX + 8];
        macros_fill(typed, false, filled, sizeof(filled));
        if (!aprs_prepare(filled, out, sizeof(out)) || !tx_queue(out)) return false; /* window stays */
        /* The held message offered on Reply went out as offered: it is on its way. */
        if (deliver_pending.id && !strcasecmp(out, deliver_pending.text))
            deliver_start(deliver_pending.id, deliver_pending.group_call, out);
        deliver_pending.id = 0;
        compose_draft[0]   = '\0'; /* sent: no draft */
        compose_close();           /* sent */
        return true;
    }
    case EDIT_INFO:
    case EDIT_STATUS:
        snprintf(edit_target == EDIT_INFO ? info_text : status_text, TEXT_MAX + 1, "%s", typed);
        save_texts(); /* JS8_TEXTS_PATH */
        msg_update_text_fmt("%s saved", edit_target == EDIT_INFO ? "INFO" : "STATUS");
        edit_finish();
        return true;
    case EDIT_GROUPS:
        js8_groups_normalise(typed, groups_text, sizeof(groups_text));
        save_texts(); /* JS8_TEXTS_PATH too */
        msg_update_text_fmt(groups_text[0] ? "Groups: %s" : "No groups", groups_text);
        edit_finish();
        return true;
    case EDIT_SAVED: {
        typed += strspn(typed, " ");
        snprintf(saved_msgs[saved_edit], sizeof(saved_msgs[saved_edit]), "%s", typed);
        for (char *end = saved_msgs[saved_edit] + strlen(saved_msgs[saved_edit]);
             end > saved_msgs[saved_edit] && end[-1] == ' ';)
            *--end = '\0';
        save_saved();
        edit_finish();
        saved_open(saved_edit); /* back to the list, on that one */
        msg_update_text_fmt(saved_msgs[saved_edit][0] ? "Saved message %d saved" : "Saved message %d cleared",
                            saved_edit + 1);
        return true;
    }
    case EDIT_FREQ:
        if (!parse_custom(typed, NULL)) return false;
        break;
    case EDIT_SPOT_FREQ:
        if (!spot_freq_empty(typed) && !spot_parse_freq(typed, NULL)) return false;
        break;
    default:
        break;
    }
    /* All the rest belong to a popup, which takes the value and comes back. */
    log_edit_done(typed);
    return true;
}

static bool compose_cancel_cb(void)
{
    deliver_pending.id = 0; /* not sent: no longer on offer */
    if (compose_keep()) msg_update_text_fmt("Not sent: kept for Send... or Reply");
    switch (edit_target) {
    case 0:
    case EDIT_INFO:
    case EDIT_STATUS:
    case EDIT_GROUPS:
        edit_finish();
        break;
    case EDIT_SAVED: /* nothing changed; back to the list */
        edit_finish();
        saved_open(saved_edit);
        break;
    default:
        log_edit_done(NULL); /* the popup comes back unchanged */
        break;
    }
    return true;
}

/* How many characters each kind of edit takes. */
static uint32_t edit_max_len(int what)
{
    switch (what) {
    case 0:
    case EDIT_SAVED:       return TX_TEXT_MAX;
    case EDIT_LOG_GRID:    return 6;
    case EDIT_POTA_REF:    return sizeof(last_pota) - 1;
    case EDIT_SOTA_REF:    return sizeof(last_sota) - 1;
    case EDIT_ALERT_WORDS: return sizeof(alert_words) - 1;
    case EDIT_GROUPS:      return sizeof(groups_text) - 1;
    case EDIT_FREQ:        return 9;
    case EDIT_SPOT_REF:    return (spot_sota ? sizeof(last_sota) : sizeof(last_pota)) - 1;
    case EDIT_SPOT_FREQ:   return 10;
    case EDIT_SPOT_NOTE:   return sizeof(spot_note) - 1;
    case EDIT_BEACON_GRID:
    case EDIT_BEACON_GPS:  return APRS_COMMENT_MAX;
    default:               return TEXT_MAX; /* INFO, STATUS, log name and comment */
    }
}

/* Grey hint in the empty box, per kind of edit. */
static const char *edit_hint(int what)
{
    switch (what) {
    case EDIT_INFO:        return " INFO, e.g. X6100 5W EFHW";
    case EDIT_STATUS:      return " STATUS, e.g. PORTABLE QRV";
    case EDIT_GROUPS:      return " Your groups, e.g. @NET @CANADA";
    case EDIT_SAVED:       return " Message, e.g. <CALL> TNX FER QSO 73 GL";
    case EDIT_LOG_GRID:    return " Their grid, e.g. DN17";
    case EDIT_LOG_NAME:    return " Their name";
    case EDIT_LOG_NOTE:    return " Comment for the log";
    case EDIT_POTA_REF:    return " Your park, e.g. CA-1234";
    case EDIT_SOTA_REF:    return " Your summit, e.g. VE7/LM-001";
    case EDIT_ALERT_WORDS: return " Calls or words, e.g. VE7ABC @POTA SOTA";
    case EDIT_FREQ:        return " Dial frequency in kHz, e.g. 7107";
    case EDIT_SPOT_REF:    return " Park CA-1234 / summit VE7/LM-001";
    case EDIT_SPOT_FREQ:   return " kHz, e.g. 7185 - empty: the JS8 dial";
    case EDIT_SPOT_NOTE:   return " Comment, e.g. QRT or CQ";
    case EDIT_BEACON_GRID:
    case EDIT_BEACON_GPS:  return " Message (optional) - Enter sends";
    default:               return " CALL MESSAGE / @ALLCALL ...";
    }
}

/* Opens the compose window for edit_target (0: a message to send). */
static void compose_open(const char *prefill)
{
    if (composing) return; /* already open */
    if (params.callsign.x[0] == '\0') {
        msg_update_text_fmt("Set your callsign first: Settings > General");
        return;
    }
    composing = true; /* from here on ESC and the keys belong to the window */
    tx_tr_done = false; /* a new message: into English first again, when GhostNet's switch says so */
    tx_tr_window++;
    keys_leave_main(); /* keys go to the text box now */
    lv_obj_t *win = textarea_window_open(compose_ok_cb, compose_cancel_cb);
    lv_obj_set_y(win, 0);
    compose_layout(true); /* list under the text box */

    bool      digits_only = edit_target == EDIT_FREQ || edit_target == EDIT_SPOT_FREQ;
    lv_obj_t *box         = textarea_window_text();
    lv_textarea_set_accepted_chars(box, digits_only ? "0123456789."
                                                    : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                                      "0123456789 .-+?!\"/@:<>{}_#&'(),=;*");
    lv_textarea_set_max_length(box, edit_max_len(edit_target));
    lv_obj_add_event_cb(box, compose_insert_cb, LV_EVENT_INSERT, NULL);
    /* Frames are only counted for a message; the APRS beacon has its own counter. */
    if (edit_target == 0) lv_obj_add_event_cb(box, compose_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    else if (edit_target >= EDIT_BEACON_GRID) lv_obj_add_event_cb(box, beacon_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    const char *start = prefill ? prefill : "";
    if (edit_target == 0 && compose_draft[0] && !strncasecmp(compose_draft, start, strlen(start))) {
        start = compose_draft;
        msg_update_text_fmt("Your message not sent yet: Enter sends it");
    }
    snprintf(compose_first, sizeof(compose_first), "%s", start);
    if (start[0]) textarea_window_set(start);
    else lv_textarea_set_placeholder_text(box, edit_hint(edit_target));
}

/* The window's button pressed again: it closes as with ESC, the words kept */
static bool compose_again(void)
{
    if (!composing) return false;
    textarea_window_cancel();
    return true;
}

/* ---- Band and frequency ------------------------------------------------ */

/* Hz as kHz with only the decimals it needs: 7107500 -> "7107.5". */
static void format_khz(int32_t hz, char *out, size_t out_size)
{
    snprintf(out, out_size, "%d.%03d", (int)hz / 1000, (int)hz % 1000);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '0') len--;
    if (len > 0 && out[len - 1] == '.') len--;
    out[len] = '\0';
}

/* Our frequency's name, for the status line and the info rows. */
static const char *where_label(void)
{
    static char name[32];
    bool        custom = params.js8_custom_on.x;
    if (custom) {
        char kilo[16];
        format_khz(params.js8_custom_hz.x, kilo, sizeof(kilo));
        snprintf(name, sizeof(name), "JS8 %s kHz", kilo);
        return name;
    }
    return cfg_digital_label_get();
}

/* JS8 is always USB-D; the mode keys are locked meanwhile. A new band
 * comes up in its own saved mode (USB after SSB), hence a call after every
 * retune. */
static void js8_usb_dig(void)
{
    const int want = radio_mode_usb_dig;
    if (cparam_i_get(cfg_cur_mode) == want) return;
    cparam_i_set(cfg_cur_mode, want);
}

/* Steps through the JS8Call or the GhostNet frequency presets (dir 0: the
 * current one). Past either end of the list nothing changes and it returns
 * false. */
static bool load_band(int8_t dir)
{
    bool found = cfg_digital_load(dir, params.js8_ghostnet.x ? CFG_DIG_TYPE_JS8_GHOSTNET : CFG_DIG_TYPE_JS8);
    js8_usb_dig(); /* found or not */
    if (found) msg_update_text_fmt("%s", cfg_digital_label_get());
    return found;
}

/* The band's JS8 frequency, or the custom one */
static void load_base(void)
{
    load_band(0); /* also sets the mode */
    int32_t hz = params.js8_custom_hz.x;
    bool    in_range = hz >= CUSTOM_MIN_HZ && hz <= CUSTOM_MAX_HZ;
    if (params.js8_custom_on.x && in_range) cparam_i_set(cfg_fg_freq, hz);
    else if (params.js8_custom_on.x) params_bool_set(&params.js8_custom_on, false); /* out of range: back to the preset */
    js8_usb_dig(); /* a custom frequency on another band loaded that band's mode */
}

/* The Stations list kept for the dial frequency in use, made on first use. */
static js8_stations_t *stations_for_band(void)
{
    int32_t khz       = cparam_i_get(cfg_fg_freq) / 1000;
    int     free_slot = -1;
    for (int s = 0; s < BAND_LISTS; s++) {
        if (band_lists[s].list == NULL) {
            if (free_slot < 0) free_slot = s;
        } else if (band_lists[s].dial_khz == khz) {
            return band_lists[s].list;
        }
    }
    int s;
    if (free_slot >= 0) {
        s                  = free_slot;
        band_lists[s].list = js8_stations_create();
    } else {
        /* Every slot used (many custom frequencies): recycle them in turn. */
        s               = band_lists_next;
        band_lists_next = (s + 1) % BAND_LISTS;
        js8_stations_clear(band_lists[s].list);
    }
    band_lists[s].dial_khz = khz;
    return band_lists[s].list;
}

/* Whenever the dial frequency changed. */
static void retuned(void)
{
    auto_cq_stop("band changed");       /* its CQ was for the old frequency */
    js8_rx_clear(rx);                     /* what it was decoding was on the old frequency */
    stations = stations_for_band();       /* as we left it, if we were here before */
    lv_waterfall_clear_data(waterfall);   /* likewise the picture */
    wf_queue_clear();                     /* and rows not drawn yet */
    clear_selection();                    /* the station is not here */
    conv_close();                         /* nor the conversations */
    if (view_stations) rebuild_rows();    /* another list */
    add_info_row("%s", where_label());    /* where we are now */
    if (btn_freq.disp_btn) buttons_refresh(&btn_freq); /* its label names the frequency */
    update_status();                      /* the name at the top */
}

/* Band keys: next or previous preset, which also ends a custom frequency. */
static void band_cb(lv_event_t *ev)
{
    int8_t dir = lv_event_get_code(ev) == EVENT_BAND_UP ? 1 : -1;
    if (load_band(dir)) {
        if (params.js8_custom_on.x) params_bool_set(&params.js8_custom_on, false);
        retuned();
        return;
    }
    msg_update_text_fmt("End of the %s list", params.js8_ghostnet.x ? "GhostNet" : "JS8");
}

/* ESC ends a knob setting first, then stops TX, and only then closes. */
static void esc_pressed(void)
{
    if (cq_adjusting) cq_adjust_end(); /* the auto CQ interval is set */
    else if (hb_adjusting) {
        hb_adjusting = false;
        update_tx_bar(); /* the hint goes */
    } else if (js8_tx_busy(tx)) stop_tx_cb(NULL);
    else dialog_destruct();
}

static void key_cb(lv_event_t *ev)
{
    uint32_t key = *(uint32_t *)lv_event_get_param(ev);
    user_touch(); /* someone is at the radio */
    bool vol_down = key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT;
    bool vol_up   = key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT;
    if (vol_down || vol_up) {
        radio_change_vol(vol_down ? -1 : 1);
    } else if (board_on && !auto_selecting && (key == LV_KEY_LEFT || key == LV_KEY_UP)) {
        board_move(-1);
    } else if (board_on && !auto_selecting && (key == LV_KEY_RIGHT || key == LV_KEY_DOWN)) {
        board_move(1); /* the user's: select_row()'s DOWN, the list following its newest row, is not */
    } else if (key == LV_KEY_ESC) { /* LVGL prints the calling function with the line below: keep it here */
        LV_LOG_USER("JS8 ESC on the list: tick %u composing %d popup %d tx %d",
                    (unsigned)lv_tick_get(), composing, any_popup(), js8_tx_busy(tx));
        esc_pressed();
    }
}

/* ---- Opening and closing ---------------------------------------------- */

/* How most bottom buttons begin: the press shows someone is at the radio, and when a list popup is
 * open the press only closes it (true: nothing more to do). */
static bool press_closes_list(void) {
    user_touch(); /* someone is at the radio */
    return popup_guard();
}

/* A switch kept in params turned over; returns its new state. */
static bool param_flip(params_bool_t *p) {
    params_bool_set(p, !p->x);
    return p->x;
}

/* A bottom button redrawn, if it's on screen at the moment. */
static void redraw(button_data_t *b) {
    if (b->disp_btn) buttons_refresh(b);
}

/* The receive filter's two edges. The top goes first because each edge is checked against the other. */
static void rx_edges_set(int32_t low, int32_t high) {
    cparam_i_set(cfg_cur_filter_high, high);
    cparam_i_set(cfg_cur_filter_low, low);
}

/* JS8's filters while the app is open (open = true), the user's again when it closes. Receive at
 * 200-3000 Hz. Transmit at 160-3000 Hz, the radio's own default, so only a filter narrowed for voice
 * changes and a signal up to 3000 Hz goes out whole. The base's noise reduction, noise blanker and
 * notches still run in DIGI modes and spoil JS8's steady tones (the auto-notch hunts exactly those),
 * so they're off. The TX filter and the DSP are sent to the radio only, the settings are not touched:
 * losing power with the app open leaves them as they were. */
static void js8_filters(bool open) {
    if (!open && !filter_saved) return;
    if (open) {
        int32_t lo = cparam_i_get(cfg_cur_filter_low), hi = cparam_i_get(cfg_cur_filter_high);

        saved_filter_low  = lo;
        saved_filter_high = hi;
    }
    filter_saved = open;
    rx_edges_set(open ? JS8_FILTER_LOW : saved_filter_low, open ? JS8_FILTER_HIGH : saved_filter_high);
    radio_set_tx_filter(open ? JS8_TX_FILTER_LOW : param_i_get(cfg_tx_filter_low),
                        open ? JS8_TX_FILTER_HIGH : param_i_get(cfg_tx_filter_high));
    radio_set_rx_dsp_off(open);
}

/* The main screen's own waterfall and spectrum: resting while the app draws its own. */
static void main_dsp(bool on) {
    dsp_set_waterfall_enabled(on);
    dsp_set_spectrum_enabled(on);
}

/* The waterfall goes in an opaque box. LVGL 8.3's lv_img never claims to cover what lies under it (its
 * cover test takes the event parameter for a clip area), so each new row also redrew the dialog's
 * background picture beneath: a megabyte read from the file row by row and alpha-blended, most of
 * what a row cost. A plain opaque object does cover, so drawing starts at the box, as it does for the
 * main screen's waterfall. */
/* ---- The lane over the waterfall: each call heard lately over its offset, on two lines where they would meet;
 * the one selected stands out, those that called us in red; TX where we send ---- */

typedef struct {
    char  call[JS8_RX_CALL_LEN];
    float hz;
    bool  me;       /* called us */
} lane_call_t;

static lane_call_t lane_calls[MAX_ROWS];
static int         lane_n;

static int lane_by_hz(const void *a, const void *b)
{
    float x = ((const lane_call_t *)a)->hz, y = ((const lane_call_t *)b)->hz;
    return (x > y) - (x < y);
}

static void lane_update(void)
{
    static js8_station_t st[MAX_ROWS];
    int64_t              now = now_wall_ms();
    int                  n   = stations ? js8_stations_list(stations, now, st, MAX_ROWS) : 0;

    lane_n = 0;
    for (int k = 0; k < n; k++) {
        if (now - st[k].heard_ms > LANE_SHOW_MS) continue;
        lane_call_t *c = &lane_calls[lane_n++];
        snprintf(c->call, sizeof(c->call), "%s", st[k].call);
        c->hz = st[k].freq_hz;
        c->me = st[k].heard_me && now - st[k].heard_me_ms < LANE_SHOW_MS;
    }
    qsort(lane_calls, lane_n, sizeof(lane_calls[0]), lane_by_hz);
    if (lane) lv_obj_invalidate(lane);
}

static lv_coord_t lane_x(float hz)
{
    if (filter_high <= filter_low) return 0;
    return (lv_coord_t)((hz - filter_low) * WIDTH / (filter_high - filter_low));
}

static void lane_draw_cb(lv_event_t *ev)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(ev);
    lv_area_t      box;
    lv_coord_t     ends[2] = {-1000, -1000};

    lv_obj_get_coords(lane, &box);

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &sony_16;

    /* TX first: the calls give way to it on its line */
    for (int k = -1; k < lane_n; k++) {
        const char *call = k < 0 ? "TX" : lane_calls[k].call;
        lv_coord_t  x    = box.x1 + lane_x(k < 0 ? params.js8_tx_freq.x : lane_calls[k].hz);
        lv_coord_t  w    = text_w(call, ld.font);

        if (x + w > box.x2) x = box.x2 - w;
        int line = x > ends[0] + 6 ? 0 : x > ends[1] + 6 ? 1 : -1;
        if (line < 0) continue; /* no room on either line */
        ends[line] = x + w;

        if (k < 0) ld.color = TC(text);
        else if (sel_call[0] && !strcasecmp(lane_calls[k].call, sel_call)) ld.color = TC(warn);
        else if (lane_calls[k].me) ld.color = TC(err);
        else ld.color = TC(text_mute);

        lv_area_t a = {x, box.y1 + 1 + line * 12, x + w, box.y1 + 1 + line * 12 + 14};
        lv_draw_label(ctx, &ld, &a, call, NULL);
    }
}

static void build_lane(void) {
    lv_obj_t *l = lane = lv_obj_create(dialog.obj);

    lv_obj_remove_style_all(l);
    lv_obj_set_style_bg_color(l, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(l, WIDTH, LANE_H);
    lv_obj_set_pos(l, 13, 13);
    lv_obj_add_event_cb(l, lane_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lane_n = 0;
}

static void build_waterfall(void) {
    lv_obj_t *box = lv_obj_create(dialog.obj);

    lv_obj_remove_style_all(box);
    lv_obj_set_style_bg_color(box, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, WIDTH, WF_HEIGHT - LANE_H);
    lv_obj_set_pos(box, 13, 13 + LANE_H);

    lv_obj_t *wf = waterfall = lv_waterfall_create(box);
    lv_obj_add_style(wf, &waterfall_style, LV_PART_MAIN);
    lv_obj_clear_flag(wf, LV_OBJ_FLAG_SCROLLABLE);
    lv_waterfall_set_palette(wf, (lv_color_t *)wf_palette, 256);
    lv_waterfall_set_size(wf, WIDTH, WF_HEIGHT - LANE_H);
    lv_waterfall_set_min(wf, WF_MIN_DB);
    lv_waterfall_set_max(wf, WF_MAX_DB);
    wf_due_us = 0; /* the first row is due at once */
    wf_timer  = lv_timer_create(wf_timer_cb, WF_TICK_MS, NULL); /* rows at an even pace */
    lv_obj_set_pos(wf, 0, 0);

    /* The finder's band is where we transmit; its cursor line is the selected message's offset. */
    lv_obj_t *f = finder = lv_finder_create(wf);
    lv_finder_set_range(f, filter_low, filter_high);
    /* An offset saved out of range (an old setting, or a damaged one) would make every send fail. */
    uint16_t at = params.js8_tx_freq.x;
    if (at < JS8_TX_MIN_OFFSET || at > js8_speed_max_offset_hz(cur_speed())) params_uint16_set(&params.js8_tx_freq, 1500);
    lv_finder_set_width(f, js8_speed_bandwidth_hz(cur_speed()));
    lv_finder_set_value(f, params.js8_tx_freq.x);
    lv_finder_clear_cursor(f);
    qso_freq       = -1; /* no green line until a station is selected */
    cursor_user_ms = 0; /* the list follows new rows */
    *sel_call      = '\0';

    const lv_style_selector_t band = LV_PART_INDICATOR;
    lv_obj_set_size(f, WIDTH, WF_HEIGHT - LANE_H);
    lv_obj_set_pos(f, 0, 0);
    lv_obj_set_style_radius(f, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(f, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(f, bg_color, band);
    lv_obj_set_style_bg_opa(f, LV_OPA_50, band);
    lv_obj_set_style_border_width(f, 1, band);
    lv_obj_set_style_border_color(f, TC(text), band);
    lv_obj_set_style_border_opa(f, LV_OPA_50, band);

    /* Speed, decodes and drift, top right over the waterfall */
    lv_obj_t *s = status = lv_label_create(wf);
    lv_obj_set_style_text_font(s, &sony_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(s, TC(text), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_50, LV_PART_MAIN);
    lv_obj_align(s, LV_ALIGN_TOP_RIGHT, -4, 4);
}

/* The line between waterfall and messages that says what's being sent */
static void build_tx_bar(void) {
    lv_obj_t *bar = tx_bar = lv_label_create(dialog.obj);

    lv_obj_set_size(bar, WIDTH, TX_BAR_H);
    lv_obj_set_pos(bar, 13, WF_VISIBLE + 13);
    lv_obj_set_style_text_font(bar, &sony_22, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_label_set_long_mode(bar, LV_LABEL_LONG_DOT);

    /* the last HB and who heard it, on the right of the line */
    hb_info = lv_label_create(dialog.obj);
    lv_obj_set_style_text_font(hb_info, &sony_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(hb_info, TC(text_mute), LV_PART_MAIN);
    lv_obj_align(hb_info, LV_ALIGN_TOP_RIGHT, -20, WF_VISIBLE + 13 + 6);
    lv_label_set_text(hb_info, "");
}


/* ---- The board: the stations of the band as cards, the main view. Those that heard us come first (a HB
 * answered, a call), then the others, newest first. A card: flag, call, signal, country and distance, and a
 * tag: messages for us not read yet, "heard you" with their report, else what they sent last (CQ, HB,
 * X6100). The MFK goes from card to card (the station selected for Reply, Query >...), its press opens the
 * conversations on that one. A card that heard us is red for 3 minutes after their answer (JS8_HEARD_FRESH_MS), then
 * plain with its tag in grey. The list stays under it, hidden, and keeps the keys. */

#define BOARD_COLS  3
#define BOARD_ROWS  3
#define BOARD_N     (BOARD_COLS * BOARD_ROWS)
#define BARS        5

typedef struct {
    lv_obj_t *box, *flag, *call, *info, *tag, *bar[BARS];
} card_t;

static card_t        cards[BOARD_N];
static lv_obj_t     *board_page;
static js8_station_t board_st[MAX_ROWS];
static int           board_n, board_sel;
static char          board_sel_call[JS8_RX_CALL_LEN];

/* Messages for us not read in the conversations yet, by call */
#define UNREAD_N 32
static struct {
    char call[JS8_RX_CALL_LEN];
    int  n;
} unread[UNREAD_N];

static int unread_of(const char *call)
{
    for (int i = 0; i < UNREAD_N; i++)
        if (unread[i].call[0] && !strcasecmp(unread[i].call, call)) return unread[i].n;
    return 0;
}

static void unread_add(const char *call)
{
    int free_at = -1;
    for (int i = 0; i < UNREAD_N; i++) {
        if (unread[i].call[0] && !strcasecmp(unread[i].call, call)) {
            unread[i].n++;
            return;
        }
        if (!unread[i].call[0] && free_at < 0) free_at = i;
    }
    if (free_at < 0) free_at = 0; /* full: the first goes */
    snprintf(unread[free_at].call, sizeof(unread[free_at].call), "%s", call);
    unread[free_at].n = 1;
}

static void unread_seen(const char *call)
{
    for (int i = 0; i < UNREAD_N; i++)
        if (unread[i].call[0] && !strcasecmp(unread[i].call, call)) unread[i].call[0] = '\0';
}

/* What a station sent last, from the messages kept: "CQ", "HB", "X6100" or "" */
static const char *last_role(const char *call)
{
    int idx = hist_head;
    for (int i = 0; i < hist_count; i++) {
        idx = (idx + HISTORY - 1) % HISTORY;
        const js8_rx_msg_t *m = &history[idx];
        if (m->tx || m->partial || strcasecmp(m->from, call)) continue;
        if (js8_to_x6100(m, groups_text)) return "X6100";
        if (m->cq) return "CQ";
        if (m->heartbeat) return "HB";
        return "";
    }
    return "";
}

static int board_order(const void *a, const void *b)
{
    const js8_station_t *x = a, *y = b;
    if (x->heard_me != y->heard_me) return x->heard_me ? -1 : 1;
    int64_t tx = x->heard_me ? x->heard_me_ms : x->heard_ms, ty = y->heard_me ? y->heard_me_ms : y->heard_ms;
    return (ty > tx) - (ty < tx);
}

static void hb_info_update(void)
{
    if (!hb_info) return;
    int heard = 0;
    for (int k = 0; k < board_n; k++)
        if (board_st[k].heard_me && hb_sent_ms && board_st[k].heard_me_ms >= hb_sent_ms) heard++;

    char text[96];
    if (hb_sent_ms) {
        time_t    t = (time_t)(hb_sent_ms / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(text, sizeof(text), "HB %02d:%02d - %d heard you", tm.tm_hour, tm.tm_min, heard);
    } else {
        snprintf(text, sizeof(text), "%d station%s - HB: say you're here", board_n, board_n == 1 ? "" : "s");
    }
    if (board_on && board_n > BOARD_N) /* the board's page */
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "   %d/%d", (board_sel > 0 ? board_sel : 0) / BOARD_N + 1,
                 (board_n + BOARD_N - 1) / BOARD_N);
    lv_label_set_text(hb_info, text);
}

static void card_show(card_t *c, const js8_station_t *st, bool sel)
{
    const where_t *w   = station_where(st->call);
    int            n     = unread_of(st->call);
    bool           you   = st->heard_me;
    bool           fresh = js8_heard_me_fresh(st, now_wall_ms());  /* red for 3 minutes, then a plain card */

    lv_obj_clear_flag(c->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(c->box, fresh ? TC(row_to_me) : TC(row), LV_PART_MAIN);
    lv_obj_set_style_border_opa(c->box, sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);

    const lv_img_dsc_t *f = w ? table_view_flag(w->flag) : NULL;
    if (f) {
        lv_img_set_src(c->flag, f);
        lv_obj_clear_flag(c->flag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->flag, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(c->call, st->call);

    char info[64] = "";
    if (w && w->country[0]) snprintf(info, sizeof(info), "%s", w->country);
    if (w && w->km >= 0)
        snprintf(info + strlen(info), sizeof(info) - strlen(info), "%s%s%d km", info[0] ? "  " : "",
                 w->approx ? "~" : "", w->km);
    lv_label_set_text(c->info, info);

    /* their signal as we hear it, -24 dB one bar to -4 dB five */
    int lit = (st->snr + 28) / 5;
    for (int b = 0; b < BARS; b++)
        lv_obj_set_style_bg_color(c->bar[b], b < lit ? TC(ok) : TC(line), LV_PART_MAIN);

    char        tag[32] = "";
    lv_color_t  col     = TC(text_mute);
    const char *role    = last_role(st->call);
    if (n) {
        snprintf(tag, sizeof(tag), "%d new", n);
        col = TC(warn);
    } else if (you && st->has_reported_snr) {
        snprintf(tag, sizeof(tag), "heard you %+d", st->reported_snr);
        col = fresh ? TC(err) : TC(text_mute);
    } else if (you) {
        snprintf(tag, sizeof(tag), "heard you");
        col = fresh ? TC(err) : TC(text_mute);
    } else if (role[0]) {
        snprintf(tag, sizeof(tag), "%s", role);
        col = !strcmp(role, "CQ") ? TC(ok) : TC(text_mute);
    }
    lv_label_set_text(c->tag, tag);
    lv_obj_set_style_text_color(c->tag, col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c->tag, tag[0] ? LV_OPA_60 : LV_OPA_TRANSP, LV_PART_MAIN);
}

static void board_update(void)
{
    if (!board) return;
    board_n = stations ? js8_stations_list(stations, now_wall_ms(), board_st, MAX_ROWS) : 0;
    qsort(board_st, board_n, sizeof(board_st[0]), board_order);

    /* the card selected follows its station when the order changes, and the list's choice */
    if (sel_call[0] && strcasecmp(sel_call, board_sel_call)) snprintf(board_sel_call, sizeof(board_sel_call), "%s", sel_call);
    board_sel = -1; /* no card framed unless its station is here */
    for (int k = 0; k < board_n; k++)
        if (board_sel_call[0] && !strcasecmp(board_st[k].call, board_sel_call)) board_sel = k;

    int first = board_sel > 0 ? board_sel / BOARD_N * BOARD_N : 0;
    for (int i = 0; i < BOARD_N; i++) {
        int k = first + i;
        if (k < board_n) card_show(&cards[i], &board_st[k], k == board_sel);
        else lv_obj_add_flag(cards[i].box, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(board_page, LV_OBJ_FLAG_HIDDEN); /* the page is on the TX line's right */
    if (!board_n) {
        lv_label_set_text(board_page, "Listening... the stations heard come here. HB tells them you are here.");
        lv_obj_clear_flag(board_page, LV_OBJ_FLAG_HIDDEN);
    }
    hb_info_update();
}

/* The MFK on the board: the next card, its station selected as a row of the list would be */
static void board_move(int step)
{
    if (!board_n) return;
    int k = board_sel >= 0 ? (board_sel + step + board_n) % board_n : 0;
    snprintf(board_sel_call, sizeof(board_sel_call), "%s", board_st[k].call);
    memcpy(sel_call, board_st[k].call, sizeof(sel_call));
    selection_move(board_st[k].freq_hz, board_st[k].snr);
    board_update();
}

static void conv_open(void);

static void board_press(void)
{
    if (!board_n || any_popup() || composing) return;
    if (board_sel < 0) board_move(0);   /* the first card, and its station selected */
    else if (strcasecmp(sel_call, board_st[board_sel].call)) board_move(0);
    conv_open();
}

static void board_show(bool on)
{
    board_on = on;
    if (!board) return;
    if (on) {
        lv_obj_clear_flag(board, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(table, LV_OBJ_FLAG_HIDDEN);
        board_update();
    } else {
        lv_obj_add_flag(board, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(table, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_board(void) {
    lv_obj_t *b = board = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, WIDTH, WF_HEIGHT - TX_BAR_H - WF_VISIBLE);
    lv_obj_set_pos(b, 13, WF_VISIBLE + TX_BAR_H + 13);
    lv_obj_set_style_bg_color(b, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t h  = WF_HEIGHT - TX_BAR_H - WF_VISIBLE;
    lv_coord_t cw = (WIDTH - (BOARD_COLS - 1) * 6) / BOARD_COLS;
    lv_coord_t ch = (h - 4 - (BOARD_ROWS - 1) * 4) / BOARD_ROWS;

    for (int i = 0; i < BOARD_N; i++) {
        card_t   *c   = &cards[i];
        lv_obj_t *box = c->box = lv_obj_create(b);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, cw, ch);
        lv_obj_set_pos(box, (i % BOARD_COLS) * (cw + 6), 2 + (i / BOARD_COLS) * (ch + 4));
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(box, TC(warn), LV_PART_MAIN);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        c->flag = lv_img_create(box);
        lv_img_set_zoom(c->flag, 384); /* 24 x 18 */
        lv_obj_set_pos(c->flag, 12, 8);

        c->call = lv_label_create(box);
        lv_obj_set_style_text_font(c->call, &sony_22, LV_PART_MAIN);
        lv_obj_set_style_text_color(c->call, TC(text), LV_PART_MAIN);
        lv_obj_set_pos(c->call, 40, 2);

        for (int k = 0; k < BARS; k++) {
            lv_obj_t *bar = c->bar[k] = lv_obj_create(box);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 4, 4 + k * 3);
            lv_obj_set_pos(bar, cw - 44 + k * 6, 22 - k * 3);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        }

        c->info = lv_label_create(box);
        lv_label_set_long_mode(c->info, LV_LABEL_LONG_DOT);
        lv_obj_set_width(c->info, cw - 110);
        lv_obj_set_style_text_font(c->info, &sony_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(c->info, TC(text_mute), LV_PART_MAIN);
        lv_obj_set_pos(c->info, 8, ch - 24);

        c->tag = lv_label_create(box);
        lv_obj_set_style_text_font(c->tag, &sony_16, LV_PART_MAIN);
        lv_obj_set_style_bg_color(c->tag, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_radius(c->tag, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(c->tag, 6, LV_PART_MAIN);
        lv_obj_align(c->tag, LV_ALIGN_BOTTOM_RIGHT, -6, -3);
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    }

    board_page = lv_label_create(b);
    lv_obj_set_style_text_font(board_page, &sony_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(board_page, TC(text_mute), LV_PART_MAIN);
    lv_obj_align(board_page, LV_ALIGN_CENTER, 0, 0);

    board_n = 0;
    board_sel_call[0] = '\0';
    board_show(board_on);
}

/* The message list: one column, over the waterfall, which shows through it. */
static void build_list(void) {
    lv_obj_t *t = table = lv_table_create(dialog.obj);

    lv_obj_remove_style(t, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_add_event_cb(t, table_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(t, table_select_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(t, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(t, table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(t, table_draw_end_cb, LV_EVENT_DRAW_PART_END, NULL);
    lv_obj_set_size(t, WIDTH, WF_HEIGHT - TX_BAR_H - WF_VISIBLE);
    lv_obj_set_pos(t, 13, WF_VISIBLE + TX_BAR_H + 13);
    lv_table_set_col_cnt(t, 1);
    lv_table_set_col_width(t, 0, WIDTH - 2);

    const lv_style_selector_t frame = LV_PART_MAIN, cell = LV_PART_ITEMS;
    lv_obj_set_style_border_width(t, 0, cell);
    lv_obj_set_style_bg_opa(t, 150, frame);
    lv_obj_set_style_bg_color(t, TC(panel), frame);
    lv_obj_set_style_border_width(t, theme_shape.border, frame);
    lv_obj_set_style_border_color(t, TC(text), frame);
    lv_obj_set_style_border_opa(t, 128, frame);
    lv_obj_set_style_text_color(t, TC(text_soft), cell);
    /* 24 px: messages run long, and the dialog's 36 px font leaves room for two rows only */
    lv_obj_set_style_text_font(t, &sony_24, cell);
    lv_obj_set_style_pad_top(t, 3, cell);
    lv_obj_set_style_pad_bottom(t, 3, cell);
    lv_obj_set_style_pad_left(t, 5, cell);
    lv_obj_set_style_pad_right(t, 0, cell);

    lv_group_add_obj(keyboard_group, t);
    lv_group_set_editing(keyboard_group, true); /* the MFK moves the selection */
}

/* The main screen's locks while the app is open; a held button counts as held after HOLD_MS here
 * (page back, auto CQ and the rest without the long wait), 0 puts the usual time back. */
static void main_screen_locks(bool on) {
    main_screen_lock_ab(on);
    keypad_set_long_time(on ? HOLD_MS : 0);
    main_screen_lock_mode(on);
    main_screen_lock_freq(on);
    main_screen_lock_band(on);
}

static void construct_cb(lv_obj_t *screen) {
    dialog.obj = dialog_init(screen);

    /* Opening the app never starts a transmission by itself: AUTO, HB, HB ACK and auto CQ begin off
     * each time. The HB interval is remembered. */
    params_bool_t *start_off[] = {&params.js8_auto, &params.js8_hb, &params.js8_hb_ack};
    for (size_t i = 0; i < sizeof(start_off) / sizeof(start_off[0]); i++) params_bool_set(start_off[i], false);
    auto_cq = false; /* not a param: off until the next hold of CQ */
    swr_guard_reset();

    main_dsp(false);
    const uint32_t band_keys[] = {EVENT_BAND_UP, EVENT_BAND_DOWN};
    for (size_t i = 0; i < 2; i++) lv_obj_add_event_cb(dialog.obj, band_cb, band_keys[i], NULL);
    load_base();

    js8_filters(true);
    filter_low  = cparam_i_get(cfg_cur_filter_low); /* the edges as the radio took them */
    filter_high = cparam_i_get(cfg_cur_filter_high); /* (each was checked against the other) */

    build_lane();
    build_waterfall();
    build_tx_bar();
    build_list();
    build_board();

    rebuild_rows(); /* empty, or the messages kept from the last time */
    const char *where = where_label();
    add_info_row("%s", where);
    main_screen_locks(true);

    cycle_decodes = 0;
    cycles        = 0;
    rx_start(); /* decoding from here on */
    update_status(); /* speed, decodes, drift */

    /* Sending: the same 7 W limit and starting gain as the FT8 app */
    const float max_w = TX_PLAYER_MAX_PWR_W;
    if (param_f_get(cfg_pwr) > max_w) {
        radio_set_pwr(max_w);
        msg_schedule_text_fmt("Power is limited to %0.0fW", max_w);
    }
    base_gain_offset = tx_player_base_gain_offset(); /* where the TX gain starts */
    tx_start(); /* the transmitter, waiting for something to send */

    /* These outlive the app, as the messages do, until the radio is switched off. Emptying them on
     * every opening brought the next decodes back all at once and without the star of those who had
     * heard us. A station still leaves an hour after it was last heard (StationList::EXPIRE_MS). */
    stations = stations_for_band(); /* this dial frequency's list */
    if (qsos == NULL) qsos = js8_qsos_create();
    if (inbox == NULL) inbox = js8_inbox_open(JS8_INBOX_PATH);
    if (held == NULL) held = js8_held_open(JS8_HELD_PATH);
    if (autop == NULL) autop = js8_auto_create();
    deliver_pending.id = deliver_tx.id = 0;
    inbox_refresh_button(); /* its count of unread messages */
    user_touch(); /* the idle hour starts now */
    load_texts(); /* INFO, STATUS, groups, alert words, the spot form */
    load_saved();

    push_next_ms = hb_next_ms = 0;
    hb_adjusting = pending_auto_valid = false;
    memset(&offer, 0, sizeof offer);
    tx_timer = lv_timer_create(tx_timer_cb, 250, NULL); /* the TX bar's countdown */
    update_tx_bar(); /* drawn once now, then by tx_timer */
}

static void destruct_cb(void) { /* the app closes: the radio goes back to how it was */
    /* The transmitter first: its thread gives the VFO back and drops PTT, and has to be gone before
     * the memory slot is restored. */
    tx_stop_all(); /* waits for the TX thread */

    lv_timer_t **timers[] = {&tx_timer, &wf_timer};
    for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
        if (!*timers[i]) continue;
        lv_timer_del(*timers[i]);
        *timers[i] = NULL;
    }
    compose_keep();  /* closed unsent: the message comes back next time */
    compose_close(); /* the keyboard, if open */

    /* Popups go now, not later as query_close() and aprs_close() would do it: dialog_destruct() frees
     * their parent, dialog.obj, as soon as this returns, and a delete still pending then would use
     * freed memory (GEN or APP pressed with the Query list open used to crash the app). */
    lv_obj_t **popups[] = {&query_list, &aprs_list,  &texts_list, &log_list,
                           &inbox_list, &alerts_list, &freq_list,  &spot_list, &settings_list, &alert_box};
    for (size_t i = 0; i < sizeof(popups) / sizeof(popups[0]); i++) {
        if (!*popups[i]) continue;
        lv_obj_del(*popups[i]);
        *popups[i] = NULL;
    }
    hb_adjusting = false; /* no knob setting outlives the app */
    radio_set_pwr(param_f_get(cfg_pwr)); /* the 7 W limit ends */

    rx_stop(); /* no more decodes */
    wf_queue_clear(); /* rows that were still waiting */
    main_dsp(true);

    /* The user's filters back before the saved band and mode return */
    js8_filters(false);
    main_screen_locks(false);

    /* dialog_destruct() deletes these with dialog.obj, right after us */
    waterfall = NULL;
    lane      = NULL;
    conv_root = NULL;
    board     = NULL;
    hb_info   = NULL;
    finder    = NULL;
    table     = NULL;
    status    = NULL;
    tx_bar    = NULL;
    rstat_update(); /* the browser's JS8 page: closed */
}

/* ---- Bottom buttons ----------------------------------------------------- */

static const char *show_label_getter(void) { /* the filter in use */
    if (show == SHOW_NO_HB) return "Show:\nNo HB";
    if (show == SHOW_DIRECTED) return "Show:\nDirected";
    return "Show:\nAll";
}

/* Each press moves to the next filter. In the Stations view the filter means nothing, so there the
 * first press only brings the messages back, filtered as before. */
static void show_cb(button_data_t *b) {
    if (press_closes_list()) return;
    bool from_stations = view_stations;
    if (from_stations) {
        view_stations = false; /* back to the messages */
        redraw(&btn_stations);
    } else {
        show = (show_t)((show + 1) % SHOW_COUNT);
        buttons_refresh(b);
    }
    rebuild_rows(); /* refiltered, or the messages again */
    if (!from_stations) return;
    const char *which = "no HB";
    if (show == SHOW_ALL) which = "all";
    else if (show == SHOW_DIRECTED) which = "directed";
    msg_update_text_fmt("Messages (%s)", which);
}

/* Everything heard so far goes: messages, stations, frames half put together, the waterfall. */
static void clear_cb(button_data_t *b) {
    (void)b;
    if (press_closes_list()) return;
    hist_count = 0;
    hist_head  = 0;
    js8_stations_clear(stations); /* this band's list */
    js8_rx_clear(rx); /* the decoder's partial messages */
    lv_waterfall_clear_data(waterfall); /* rows on screen */
    wf_queue_clear(); /* rows on their way */
    clear_selection(); /* nothing left to select */
    rebuild_rows(); /* an empty list */
    update_status(); /* no decodes */
}

/* Time Sync and its hold change JS8's timing, so neither may run while frames wait to go out. */
static bool refuse_while_sending(void) {
    bool busy = js8_tx_busy(tx);
    if (busy) msg_update_text_fmt("Not while sending - Stop TX first");
    return busy;
}

/* A drift change goes in the list and on the status line. */
static void drift_noted(const char *what) {
    add_info_row("Time Sync: %s", what);
    update_status(); /* it shows the drift */
}

/* Time Sync works like desktop JS8Call's time drift. JS8's own time (receive windows, transmit slots,
 * all the app's timing) moves to where the decodes on the band put it; the radio's clock stays as it
 * is. Every decode proposes a drift computed from the one in force when its audio came in (js8core,
 * desktop's auto-sync maths): decodes that finish just after a change can't push the next press too
 * far, and no recent decode has to be dropped after a press. A station counts once. The drift holds
 * until the radio restarts, and holding Time Sync goes back to the radio's clock. JS8's time has to be
 * within a couple of seconds already or nothing decodes at all: set the clock roughly in SETTINGS
 * first. */
static void time_sync_cb(button_data_t *b) {
    (void)b;
    if (press_closes_list() || refuse_while_sending()) return;

    int64_t  was = js8_drift_ms(), want;
    unsigned n_decodes = 0, n_stations = 0;
    bool     ok = js8_sync_drift(sync_samples, SYNC_SAMPLES, now_wall_ms(), SYNC_WINDOW_MS, was, &want, &n_decodes,
                                 &n_stations);
    if (!ok) {
        msg_update_text_fmt("Time Sync needs %d decodes in the last 2 min (have %u). Clock far off? Set it in "
                            "SETTINGS first",
                            JS8_SYNC_MIN_DECODES, n_decodes);
        return;
    }
    bool        by_station = n_stations >= JS8_SYNC_MIN_STATIONS;
    unsigned    count      = by_station ? n_stations : n_decodes;
    const char *what       = by_station ? "stations" : "decodes";
    double      step_s     = (want - was) / 1000.0;
    if (llabs(want - was) < 50) {
        msg_update_text_fmt("JS8 time is on (within 0.05 s of %u %s)", count, what);
        return;
    }
    js8_set_drift_ms(want);
    msg_update_text_fmt("JS8 time moved %+.2f s (median of %u %s); drift now %+.2f s", step_s, count, what,
                        want / 1000.0);
    char row[JS8_RX_TEXT_LEN];
    snprintf(row, sizeof(row), "JS8 time %+.2f s, drift %+.2f s", step_s, want / 1000.0);
    drift_noted(row);
}

/* Holding Time Sync drops the drift: JS8 on the radio's clock again (desktop's Reset). */
static void time_sync_hold_cb(button_data_t *b) {
    (void)b;
    if (press_closes_list() || refuse_while_sending()) return;
    bool had = js8_drift_ms() != 0;
    if (had) js8_set_drift_ms(0);
    msg_update_text_fmt(had ? "Drift reset: JS8 back on the radio's clock" : "No drift: JS8 is on the radio's clock");
    if (had) drift_noted("drift reset");
}

/* The selected station for a button that needs one; says so when there is none. */
static bool need_station(char *call, float *hz) {
    int  db;
    bool found = selected_station(call, JS8_RX_CALL_LEN, hz, &db);
    if (!found) msg_update_text_fmt("Select a station first (MFK)");
    return found;
}

/* The keyboard, with their call typed in. With AUTO off and an answer waiting for this station, the
 * answer instead (and the held message it hands over, if any). */
static void reply_cb(button_data_t *b) {
    (void)b;
    if (compose_again() || press_closes_list()) return;
    char  call[JS8_RX_CALL_LEN], start[JS8_RX_TEXT_LEN];
    float hz;
    if (!need_station(call, &hz)) return;

    bool answer = offer.text[0] && now_wall_ms() - offer.ms < OFFER_MS && !strcmp(offer.call, call);
    if (answer) {
        strcpy(start, offer.text);
        deliver_pending.id = offer.deliver_id; /* sent as offered: on its way */
        strcpy(deliver_pending.group_call, offer.deliver_group_call);
        strcpy(deliver_pending.text, offer.text);
        *offer.text = '\0'; /* offered once */
    } else {
        snprintf(start, sizeof(start), "%s ", call);
    }
    apply_hold(hz);
    compose_open(start);
    speed_warn(); /* when their speed isn't ours */
}

static void send_cb(button_data_t *b) {
    (void)b;
    if (compose_again() || press_closes_list()) return;
    compose_open(NULL); /* an empty keyboard */
}

static void stop_tx_cb(button_data_t *b) { /* also the first ESC, with b NULL */
    (void)b;
    user_touch(); /* the press counts, */
    popup_guard(); /* any list closes, and TX stops all the same */
    bool busy = js8_tx_busy(tx);
    if (busy) js8_tx_stop(tx);
    msg_update_text_fmt(busy ? "Stopping TX" : "Not sending");
    if (busy) auto_cq_stop("TX stopped");
}

/* "CALL HW CPY?" to the selected station, which is how desktop usually answers a CQ. ESC or a press
 * of the top knob stops it. */
static void hw_cpy_cb(button_data_t *b) {
    (void)b;
    if (press_closes_list()) return;
    char  call[JS8_RX_CALL_LEN], text[JS8_RX_CALL_LEN + 12];
    float hz;
    if (!need_station(call, &hz)) return;
    snprintf(text, sizeof(text), "%s HW CPY?", call); /* desktop's "how do you copy?" */
    apply_hold(hz);
    if (tx_queue(text)) speed_warn(); /* queued: their speed may not be ours */
}

/* ---- CQ ------------------------------------------------------------------ */

/* Desktop JS8Call's CQ, with the grid cut to 4 characters. A CQ starts a QSO, so heartbeats stop. */
static bool send_cq(bool by_itself) {
    char cq[32];
    snprintf(cq, sizeof(cq), "CQ CQ CQ %.4s", station_qth());
    bool queued = tx_queue_at(cq, params.js8_tx_freq.x, by_itself);
    if (queued) hb_pause("CQ");
    return queued;
}

static int64_t cq_interval_ms(void) { /* js8_cq_interval; a stored value out of range counts as the least */
    unsigned minutes = params.js8_cq_interval.x;
    bool     valid   = minutes >= CQ_MIN_INTERVAL && minutes <= CQ_MAX_INTERVAL;
    return 60000 * (int64_t)(valid ? minutes : CQ_MIN_INTERVAL);
}

static const char *cq_label_getter(void) { /* CQ, the knob setting, or auto CQ's countdown */
    static char cq_text[24];
    if (cq_adjusting) { /* the knob sets the interval */
        snprintf(cq_text, sizeof(cq_text), "CQ: knob\n< %u min >", params.js8_cq_interval.x);
    } else if (!auto_cq) {
        return "CQ";
    } else if (js8_tx_busy(tx)) { /* this CQ, or something before it */
        return "CQ auto:\nsending";
    } else {
        int left = (int)((auto_cq_next_ms - now_wall_ms() + 999) / 1000); /* whole seconds, rounded up */
        if (left <= 0) return "CQ auto:\nnow";
        if (left >= 60) snprintf(cq_text, sizeof(cq_text), "CQ auto:\n%d:%02d", left / 60, left % 60);
        else snprintf(cq_text, sizeof(cq_text), "CQ auto:\n%d s", left);
    }
    return cq_text;
}

/* The main knob sets the auto CQ interval, as it does the HB one; pressing CQ or leaving it alone
 * ends the setting. */
static void cq_knob_start(void) {
    hb_adjusting = false; /* one knob setting at a time */
    cq_adjusting = true; /* the knob sets the interval */
    cq_adjust_ms = now_wall_ms(); /* the setting times out from here */
    redraw(&btn_hbauto);
    redraw(&btn_cq);
    update_tx_bar(); /* the bar says what the knob does */
}

static void cq_adjust_end(void) { /* the knob setting ends */
    if (cq_adjusting == false) return;
    cq_adjusting = false; /* the knob is free again */
    redraw(&btn_cq);
    update_tx_bar(); /* back to what's being sent */
    if (auto_cq) { /* say how it goes on */
        unsigned minutes = params.js8_cq_interval.x;
        msg_update_text_fmt("Auto CQ %u min after each CQ (press CQ to stop)", minutes);
    }
}

/* A knob step while setting: one minute, and the next CQ moves with it. */
static void cq_adjust_turn(int32_t steps) {
    int minutes = (int)params.js8_cq_interval.x + (steps > 0 ? 1 : -1);
    minutes     = minutes < CQ_MIN_INTERVAL ? CQ_MIN_INTERVAL : minutes > CQ_MAX_INTERVAL ? CQ_MAX_INTERVAL : minutes;
    params_uint16_set(&params.js8_cq_interval, (uint16_t)minutes);
    cq_adjust_ms = now_wall_ms(); /* and the timeout starts again */
    if (auto_cq && !js8_tx_busy(tx)) auto_cq_next_ms = cq_interval_ms() + auto_cq_from_ms;
    redraw(&btn_cq);
    update_tx_bar(); /* with the new minutes */
}

/* A press sends one CQ. While the interval is being set it ends the setting, and with auto CQ running
 * it goes back to manual. */
static void cq_cb(button_data_t *b) {
    (void)b;
    if (press_closes_list()) return;
    if (cq_adjusting) cq_adjust_end(); /* done setting */
    else if (auto_cq) auto_cq_stop("manual");
    else send_cq(false);
}

/* Holding CQ starts auto CQ: one now, then another js8_cq_interval minutes after each one ends, until
 * a station answers, CQ is pressed, you reply to someone, stop TX or change band. The knob sets the
 * interval right away; a hold while auto CQ runs sets it again. */
static void cq_hold_cb(button_data_t *b) {
    if (press_closes_list()) return;
    bool starting = !auto_cq;
    if (starting) {
        /* While something is going out, the first CQ waits an interval after it; otherwise it goes
         * now, and the interval starts once it has ended (ui_tx_done). */
        int64_t t0 = now_wall_ms();
        if (!js8_tx_busy(tx) && !send_cq(false)) return; /* refused */
        auto_cq_from_ms = t0;
        auto_cq_next_ms = t0 + (js8_tx_busy(tx) ? cq_interval_ms() : 0);
        auto_cq         = true; /* until answered, stopped or idle */
        buttons_refresh(b);
        add_info_row("Auto CQ on"); /* in the list too */
    }
    cq_knob_start();
    if (starting) msg_update_text_fmt("Auto CQ on: turn the knob to set the minutes, press CQ when done");
}

static void auto_cq_stop(const char *why) { /* says why, in the list too */
    if (auto_cq == false) return;
    auto_cq = cq_adjusting = false;
    redraw(&btn_cq);
    msg_update_text_fmt("Auto CQ off: %s", why); /* on the message line */
    add_info_row("Auto CQ off: %s", why); /* and in the list */
    update_tx_bar(); /* no knob hint left */
}

/* Every second: the knob setting times out, the button counts down, and the next CQ goes when due. */
static void auto_cq_tick(void) { /* tx_timer, once a second, after hb_tick */
    if (cq_adjusting && now_wall_ms() > cq_adjust_ms + HB_ADJUST_MS) cq_adjust_end();
    if (auto_cq == false) return;
    int64_t t = now_wall_ms();
    if (js8_auto_idle(autop, t)) {
        auto_cq_stop("idle for an hour"); /* as AUTO's own watchdog */
        return;
    }
    redraw(&btn_cq);
    bool wait = t < auto_cq_next_ms || js8_tx_busy(tx) || composing || any_popup();
    if (wait) return;
    if (send_cq(true)) { /* queued */
        auto_cq_from_ms = t;
        auto_cq_next_ms = cq_interval_ms() + t; /* counted again from its end */
    } else {
        auto_cq_stop("could not send"); /* refused */
    }
}

/* ---- Heartbeat, Hold, Stations -------------------------------------------- */

/* Where a heartbeat goes: a gap in the 500-1000 Hz heartbeat sub-band, clear of everything heard in
 * the last 30 s (desktop's rule). Our own chat offset, the red band, stays put. */
static int hb_free_offset(void) {
    static js8_station_t now_heard[MAX_ROWS];
    float                hz[MAX_ROWS];
    int64_t              when[MAX_ROWS];
    int64_t              t     = now_wall_ms();
    int                  count = js8_stations_list(stations, t, now_heard, MAX_ROWS);

    for (int k = 0; k < count; k++) {
        hz[k]   = now_heard[k].freq_hz;
        when[k] = now_heard[k].heard_ms;
    }
    return js8_heartbeat_offset(hz, when, (unsigned)count, t);
}

static bool send_heartbeat(bool by_itself) {
    if (js8_speed_heartbeats(cur_speed())) {
        char hb[48];
        js8_heartbeat_text(params.callsign.x, station_qth(), hb, sizeof(hb));
        bool queued = tx_queue_at(hb, hb_free_offset(), by_itself);
        if (queued) hb_sent_ms = now_wall_ms(); /* the board counts who heard it from here */
        return queued;
    }
    if (!by_itself) msg_update_text_fmt("No heartbeats in Turbo, as in desktop JS8Call");
    return false;
}

/* A heartbeat now. With HB on, the automatic ones are counted again from this one, so one doesn't
 * follow straight after it. */
static void heartbeat_cb(button_data_t *b) {
    (void)b;
    if (press_closes_list()) return;
    if (!send_heartbeat(false)) return;
    if (!params.js8_hb.x) return; /* no timer to restart */
    unsigned minutes = params.js8_hb_interval.x;
    hb_next_ms       = js8_next_heartbeat_ms(now_wall_ms(), minutes);
    add_info_row("HB timer restarted: next in %u min", minutes);
    update_status(); /* the next heartbeat's time */
}

static const char *hold_label_getter(void) { /* js8_hold_offset */
    if (params.js8_hold_offset.x) return "Hold:\nOn";
    return "Hold:\nOff";
}

static void hold_cb(button_data_t *b) {
    if (press_closes_list()) return;
    bool stay = param_flip(&params.js8_hold_offset);
    buttons_refresh(b);
    msg_update_text_fmt(stay ? "Replies stay on your offset" : "Replies move to their offset");
}

static const char *stations_label_getter(void) { /* the view the press goes to */
    if (view_stations) return "Show\nMessages";
    return "Show\nStations";
}

static void stations_cb(button_data_t *b) {
    if (press_closes_list()) return;
    view_stations = !view_stations; /* messages <-> stations */
    buttons_refresh(b);
    rebuild_rows(); /* in the other view */
}

/* ---- List popups ---------------------------------------------------------- */

/* Nothing new while sending, typing or looking at a list; the automatic sends all wait for this. */
static bool busy_elsewhere(void) {
    bool only_alert = alert_box && !(aprs_list || spot_list || log_list || inbox_list || alerts_list || freq_list ||
                                     query_list || texts_list || settings_list || map_obj);
    return js8_tx_busy(tx) || composing || (any_popup() && !only_alert); /* the message box waits, they don't */
}

/* The keys away from the list and the conversations: a popup, the keyboard or the map takes them */
static void keys_leave_main(void) {
    if (table) lv_group_remove_obj(table);
    if (conv_root) lv_group_remove_obj(conv_root);
}

/* The message list gets the keys back once a popup has gone. */
static void table_takes_keys(void) {
    lv_group_t *keys = keyboard_group;

    if (conv_root) { /* the conversation is in front of the list */
        lv_group_add_obj(keys, conv_root);
        lv_group_focus_obj(conv_root);
        lv_group_set_editing(keys, true);
        return;
    }

    lv_group_add_obj(keys, table);
    lv_group_focus_obj(table); /* in front of the popup's items, if any are left */
    lv_group_set_editing(keys, true);
}

/* A popup goes, deleted later: the caller may be one of its own buttons. */
static void list_forget(lv_obj_t **list) {
    lv_obj_del_async(*list);
    *list = NULL;
}

/* A popup closed from one of its own buttons: its items leave the key group now, and the object itself
 * is deleted later, since the button's callback is still running. Where the keys go next is up to the
 * caller. */
static void list_drop(lv_obj_t **list) {
    uint32_t n = lv_obj_get_child_cnt(*list);
    for (uint32_t k = 0; k < n; k++) lv_group_remove_obj(lv_obj_get_child(*list, k));
    list_forget(list);
}

static void query_close(void) { /* deleted later: usually one of its own buttons is calling */
    if (query_list == NULL) return;
    list_forget(&query_list);
    if (table) table_takes_keys();
}

static void query_pick_cb(lv_event_t *ev) {
    js8_query_t which = (js8_query_t)(intptr_t)lv_event_get_user_data(ev);
    char        call[JS8_RX_CALL_LEN], text[64];
    float       hz;
    int         db;
    bool        selected = selected_station(call, sizeof(call), &hz, &db);

    query_close(); /* whatever happens next */
    if (!selected) return;
    if (js8_query_text(which, call, db, station_qth(), text, sizeof(text))) {
        apply_hold(hz);
        tx_queue(text); /* the query goes out */
    } else if (which == JS8_Q_MY_GRID) {
        msg_update_text_fmt("Set your grid first: Settings > General");
    } else {
        msg_update_text_fmt("Nothing to send");
    }
}

/* The volume keys work with a popup open too. */
static void volume_key(uint32_t key) {
    if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) radio_change_vol(1);
}

/* Up/left and down/right move through a popup's items; true if the key was one of those. */
static bool list_step_key(uint32_t key) {
    if (key == LV_KEY_UP || key == LV_KEY_LEFT) lv_group_focus_prev(keyboard_group);
    else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) lv_group_focus_next(keyboard_group);
    else return false;
    return true;
}

static void query_key_cb(lv_event_t *ev) {
    uint32_t key = *(uint32_t *)lv_event_get_param(ev);
    if (key == LV_KEY_ESC) query_close();
    else if (!list_step_key(key)) volume_key(key);
}

/* Closes whichever list popups are open. */
static void popups_close_all(void) {
    void (*const closers[])(void) = {query_close, texts_close, aprs_close, log_close, inbox_close,
                                     alerts_close, freq_close, spot_close, map_close, settings_close, alert_close};
    if (any_popup())
        for (size_t k = 0; k < sizeof(closers) / sizeof(closers[0]); k++) closers[k]();
}

/* An open list popup (Query, Settings, APRS ...) has to go before anything else happens, or it stays
 * behind with its buttons still holding the knob. So any other bottom button only closes it, and a
 * second press does the button's job. */
static bool popup_guard(void) { /* true: a list was open, and now it's closed */
    bool open = any_popup();
    if (open) {
        popups_close_all();
        msg_update_text_fmt("List closed"); /* press again for the button itself */
    }
    return open;
}

/* Turning the page closes a list as well, then turns it. */
static void js8_next_page_cb(button_data_t *b) {
    popups_close_all();
    button_next_page_cb(b);
}

/* A hold on the page button goes one page back, as on the main screen. */
static void js8_prev_page_cb(button_data_t *b) {
    popups_close_all();
    button_prev_page_cb(b);
}

/* The focused item scrolls into view in one jump: LVGL's animated scroll redraws the list on every
 * frame of the animation, and that tears on the radio's display. */
static void item_into_view_cb(lv_event_t *ev) {
    lv_obj_t *item = lv_event_get_target(ev);
    lv_obj_scroll_to_view(item, LV_ANIM_OFF);
}

static lv_obj_t *list_add_item(lv_obj_t *parent, const char *text) {
    lv_obj_t *item = lv_list_add_btn(parent, NULL, text);

    lv_obj_set_style_bg_color(item, TC(row), LV_PART_MAIN);
    lv_obj_set_style_bg_color(item, TC(focus_bg), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(item, TC(text), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(item, 6, LV_PART_MAIN);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(item, item_into_view_cb, LV_EVENT_FOCUSED, NULL);
    return item;
}

/* A list popup's frame, top right over the message list; it takes the keys from the table. */
static lv_obj_t *popup_frame(lv_coord_t w, lv_coord_t h, const char *title) {
    keys_leave_main(); /* the popup's items get the keys */
    lv_obj_t *p = lv_list_create(dialog.obj);

    lv_obj_set_size(p, w, h);
    lv_obj_align(p, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(p, &sony_24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(p, TC(list), LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_list_add_text(p, title), &sony_22, LV_PART_MAIN);
    return p;
}

/* An item of the Query list (or the saved messages, which take its place): MFK moves, a press or tap
 * runs `cb`, ESC closes. */
static lv_obj_t *query_add(const char *label, lv_event_cb_t cb, intptr_t data) {
    lv_obj_t *item = list_add_item(query_list, label);

    lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, (void *)data);
    lv_obj_add_event_cb(item, query_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, item);
    return item;
}

/* The message items of the Query list, laid out as desktop's call menu: what the keyboard opens with
 * (%s is their call) and the hint for what to type next, or the text that goes out at once. */
typedef struct {
    const char *label; /* in the list */
    const char *start;
    const char *hint;
    const char *now;
} query_msg_item_t;

static const query_msg_item_t query_msg_items[] = {
    {.label = "Message...", .start = "%s MSG ", .hint = "Message for %s's inbox: type it and press Enter"},
    {.label = "Message via them...",
     .start = "%s MSG TO:",
     .hint  = "Left at %s for someone: type their call, a space, the message"},
    {.label = "Any messages?", .now = "%s QUERY MSGS"},
    {.label = "Fetch message #...",
     .start = "%s QUERY MSG ",
     .hint  = "Type the number of the message %s holds for you"},
    {.label = "Relay via them...",
     .start = "%s>",
     .hint  = "Passed on by %s: type the call it's for, a space, the message"},
    {.label = "Can they reach...?",
     .start = "%s QUERY CALL ",
     .hint  = "Ask %s if they hear a station: type its call and ?"},
};

static void query_msg_cb(lv_event_t *ev) {
    const query_msg_item_t *it = &query_msg_items[(intptr_t)lv_event_get_user_data(ev)];
    char                    call[JS8_RX_CALL_LEN], text[JS8_RX_CALL_LEN + 16];
    float                   hz;
    int                     db;
    bool                    selected = selected_station(call, sizeof(call), &hz, &db);

    if (selected && it->start) {
        /* Into the keyboard, as texts_item_cb does it */
        list_drop(&query_list);
        apply_hold(hz);
        snprintf(text, sizeof(text), it->start, call);
        compose_open(text); /* their call and the command typed in */
        lv_group_set_editing(keyboard_group, true); /* keys to the keyboard */
        msg_update_text_fmt(it->hint, call);
        return;
    }
    query_close(); /* Any messages? goes at once */
    if (!selected) return;
    apply_hold(hz);
    snprintf(text, sizeof(text), it->now, call);
    tx_queue(text); /* QUERY MSGS */
}

static void query_hw_cpy_cb(lv_event_t *ev) {
    (void)ev;
    list_drop(&query_list); /* hw_cpy_cb would only close it */
    if (table) table_takes_keys();
    hw_cpy_cb(NULL);
}

static void query_close_item_cb(lv_event_t *ev) {
    (void)ev;
    query_close(); /* the Close item */
}

/* The frame of the Query list and of the saved messages */
static lv_obj_t *query_list_create(lv_coord_t width, const char *title) {
    lv_obj_t *p = popup_frame(width, WF_HEIGHT - 10, title);
    lv_obj_set_style_border_color(p, TC(text), LV_PART_MAIN);
    return p;
}

/* The last items, Close or < Back, in the action colour */
static lv_obj_t *query_list_end_item(const char *label, lv_event_cb_t cb) {
    lv_obj_t *item = query_add(label, cb, 0);
    lv_obj_set_style_text_color(item, TC(action), LV_PART_MAIN);
    return item;
}

static void query_saved_cb(lv_event_t *ev) {
    (void)ev;
    list_drop(&query_list);
    saved_open(0);
}

/* ---- Settings... (page 3): the switches set once, one list ---------------
 * Each row does what its button did on the old pages: the list goes first (the buttons' callbacks close an open
 * list and stop there), the button's callback runs, and a switch opens the list again on its row, its new value
 * shown. The rows that open their own list, the keyboard, the knob or send close it for good. */

typedef struct {
    button_data_t *btn;
    bool           hold;    /* its hold, not its press */
    bool           again;   /* a switch: the list again, on this row */
    const char    *text;    /* else its label, "AUTO:\nOff" read "AUTO: Off" */
} setting_t;

static const setting_t settings[] = {
    {&btn_auto, false, true, NULL},          {&btn_hbauto, false, true, NULL},
    {&btn_hbauto, true, false, "Heartbeat interval (knob)..."},
    {&btn_hbackk, false, true, NULL},        {&btn_hold, false, true, NULL},
    {&btn_show, false, true, NULL},          {&btn_speed, false, true, NULL},
    {&btn_speed, true, true, "Speed: the selected station's"},
    {&btn_decode, false, true, NULL},        {&btn_freq, false, false, NULL},
    {&btn_act, false, true, NULL},           {&btn_act, true, false, "Activation reference..."},
    {&btn_prompt, false, true, NULL},        {&btn_log, false, false, "Log QSO..."},
    {&btn_hb, false, false, "Send a heartbeat now"},
    {&btn_alerts, false, false, NULL},       {&btn_aprs, false, false, NULL},
    {&btn_texts, false, false, "Groups, INFO, STATUS, Relay..."},
    {&btn_clear, false, true, "Clear the list"},
};
#define SETTINGS_N (sizeof(settings) / sizeof(settings[0]))

static void settings_open(int focus);

static void settings_close(void) {
    if (settings_list == NULL) return;
    list_forget(&settings_list);
    if (table) table_takes_keys();
}

static void settings_key_cb2(lv_event_t *ev) {
    uint32_t key = *(uint32_t *)lv_event_get_param(ev);
    if (key == LV_KEY_ESC) settings_close();
    else if (!list_step_key(key)) volume_key(key);
}

static void settings_item_cb(lv_event_t *ev) {
    intptr_t i = (intptr_t)lv_event_get_user_data(ev);

    list_drop(&settings_list);
    if (table) table_takes_keys();
    if (i < 0 || i >= (intptr_t)SETTINGS_N) return; /* Close */

    const setting_t *st = &settings[i];
    void (*fn)(button_data_t *) = st->hold ? st->btn->hold : st->btn->press;

    if (fn) fn(st->btn);
    /* HB turned on starts its interval on the knob: here the interval has its own row, the knob stays the list's */
    if (st->btn == &btn_hbauto && !st->hold && hb_adjusting) hb_adjust_end();
    buttons_refresh(&btn_hb_main); /* "Heartbeat\nauto 10 min" when HB is on */
    if (st->again && !any_popup() && !composing) settings_open((int)i);
}

static void settings_open(int focus) {
    lv_obj_t *p = settings_list = query_list_create(380, "JS8 Settings");
    lv_obj_t *first = NULL;

    for (size_t i = 0; i < SETTINGS_N; i++) {
        const setting_t *st = &settings[i];
        char             text[64];

        if (st->text) {
            snprintf(text, sizeof(text), "%s", st->text);
        } else {
            const char *l = st->btn->type == BTN_TEXT_FN ? st->btn->label_fn() : st->btn->label;

            snprintf(text, sizeof(text), "%s", l);
            for (char *c = text; *c; c++)
                if (*c == '\n') *c = ' ';
        }

        lv_obj_t *item = list_add_item(p, text);

        lv_obj_add_event_cb(item, settings_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(item, settings_key_cb2, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, item);
        if ((int)i == focus) first = item;
    }

    lv_obj_t *close = list_add_item(p, "Close");

    lv_obj_set_style_text_color(close, TC(action), LV_PART_MAIN);
    lv_obj_add_event_cb(close, settings_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(close, settings_key_cb2, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, close);
    lv_group_focus_obj(first ? first : close);
    lv_group_set_editing(keyboard_group, false);
}

static void settings_cb(button_data_t *b) {
    (void)b;
    user_touch();
    if (settings_list) { /* a second press closes it */
        settings_close();
        return;
    }
    if (popup_guard() || composing) return;
    settings_open(-1); /* on Close: AUTO is not one press away */
}

/* ---- The conversations: on the left every station we are talking with (a message for us, one of ours to them,
 * a HB of ours they heard), newest first, their flag, last words and the messages for us not read; on the right
 * the one chosen: every message from them, to them and ours to them, theirs on the left and ours on the right in
 * blue, their flag, country, locator, distance and signal on top. The MFK goes from one station to the next, its
 * press opens Query > for it; F1, F5 or ESC back to the board. The lane and the TX line stay above it, the TX line moved
 * up over the waterfall. */

#define CONV_HEAD_H 40
#define CONV_MAX    40      /* the last messages shown */
#define CONV_LEFT_W 248
#define CONV_ROW_H  42
#define CONV_LIST_N 16

static lv_obj_t *conv_body, *conv_info, *conv_flag, *conv_title, *conv_left;
static char      conv_call[JS8_RX_CALL_LEN];
static int64_t   conv_opened_ms;    /* the press that opened it is released on it: not a press of its own */

typedef struct {
    char    call[JS8_RX_CALL_LEN];
    char    last[JS8_RX_TEXT_LEN];
    int32_t utc;
} conv_item_t;

static conv_item_t conv_items[CONV_LIST_N];
static int         conv_n;

static const char *conv_call_now(void) { return conv_root ? conv_call : ""; }

static bool conv_has(const js8_rx_msg_t *m)
{
    if (!conv_call[0] || m->partial) return false;
    if (!strcasecmp(m->from, conv_call) || !strcasecmp(m->to, conv_call)) return true;
    if (!m->tx) return false;
    char to[JS8_RX_CALL_LEN];
    tx_target(m, to, sizeof(to));
    return !strcasecmp(to, conv_call);
}

/* A callsign: letters and a digit, maybe a "/" (not CQ, HB, a group or a relay path) */
static bool looks_like_call(const char *c)
{
    size_t n = strlen(c);
    bool   digit = false, letter = false;
    if (n < 3 || n > 12 || !strcasecmp(c, "CQ") || !strcasecmp(c, "HB")) return false;
    for (const char *p = c; *p; p++) {
        if (isdigit((unsigned char)*p)) digit = true;
        else if (isalpha((unsigned char)*p)) letter = true;
        else if (*p != '/') return false;
    }
    return digit && letter;
}

/* The station a message of ours went to: its "to", else the first word of its text */
static void tx_target(const js8_rx_msg_t *m, char *call, size_t size)
{
    if (m->to[0] && m->to[0] != '@') {
        snprintf(call, size, "%s", m->to);
        return;
    }
    call[0] = '\0';
    const char *t = m->text;
    const char *colon = strstr(t, ": ");
    if (colon) t = colon + 2;
    size_t n = strcspn(t, " ");
    if (n && n < size && t[0] != '@') snprintf(call, size, "%.*s", (int)n, t);
    if (!looks_like_call(call)) call[0] = '\0'; /* our CQ, a relay A>B, a command */
}

static void conv_list_add(const char *call, const js8_rx_msg_t *m)
{
    if (!call[0] || conv_n >= CONV_LIST_N) return;
    for (int i = 0; i < conv_n; i++)
        if (!strcasecmp(conv_items[i].call, call)) return;
    conv_item_t *it = &conv_items[conv_n++];
    snprintf(it->call, sizeof(it->call), "%s", call);
    if (m) {
        const char *t = m->text, *colon = strstr(t, ": ");
        snprintf(it->last, sizeof(it->last), "%s", colon ? colon + 2 : t);
        it->utc = m->utc;
    } else {
        it->last[0] = '\0';
        it->utc     = -1;
    }
}

/* The stations we talk with, newest first; the one chosen in any case */
static void conv_list_build(void)
{
    conv_n  = 0;
    int idx = hist_head;
    for (int i = 0; i < hist_count && conv_n < CONV_LIST_N; i++) {
        idx = (idx + HISTORY - 1) % HISTORY;
        const js8_rx_msg_t *m = &history[idx];
        if (m->partial) continue;
        if (m->tx) {
            char to[JS8_RX_CALL_LEN];
            tx_target(m, to, sizeof(to));
            conv_list_add(to, m);
        } else if (m->to_me && m->from[0]) {
            conv_list_add(m->from, m);
        }
    }
    if (conv_n < CONV_LIST_N) { /* those that heard a HB of ours, nothing said yet */
        for (int k = 0; k < board_n; k++)
            if (board_st[k].heard_me) conv_list_add(board_st[k].call, NULL);
    }
    bool here = false;
    for (int i = 0; i < conv_n; i++) here |= !strcasecmp(conv_items[i].call, conv_call);
    if (!here && conv_call[0]) {
        if (conv_n == CONV_LIST_N) conv_n--;
        memmove(&conv_items[1], &conv_items[0], sizeof(conv_items[0]) * conv_n);
        conv_n++;
        memset(&conv_items[0], 0, sizeof(conv_items[0]));
        snprintf(conv_items[0].call, sizeof(conv_items[0].call), "%s", conv_call);
        conv_items[0].utc = -1;
    }
}

static void conv_left_fill(void)
{
    lv_obj_clean(conv_left);
    int sel = 0;
    for (int i = 0; i < conv_n; i++)
        if (!strcasecmp(conv_items[i].call, conv_call)) sel = i;
    int rows  = (lv_obj_get_height(conv_left)) / CONV_ROW_H;
    int first = sel >= rows ? sel - rows + 1 : 0;

    for (int i = first; i < conv_n && i < first + rows; i++) {
        conv_item_t *it  = &conv_items[i];
        lv_obj_t    *row = lv_obj_create(conv_left);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, CONV_LEFT_W, CONV_ROW_H - 1);
        lv_obj_set_pos(row, 0, (i - first) * CONV_ROW_H);
        lv_obj_set_style_bg_color(row, i == sel ? TC(sel_bg) : TC(row), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        const where_t      *w = station_where(it->call);
        const lv_img_dsc_t *f = w ? table_view_flag(w->flag) : NULL;
        if (f) {
            lv_obj_t *img = lv_img_create(row);
            lv_img_set_src(img, f);
            lv_obj_set_pos(img, 8, 6);
        }
        lv_obj_t *call = lv_label_create(row);
        lv_label_set_text(call, it->call);
        lv_obj_set_style_text_font(call, &sony_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(call, i == sel ? TC(sel_text) : TC(text), LV_PART_MAIN);
        lv_obj_set_pos(call, 30, 1);

        lv_obj_t *last = lv_label_create(row);
        lv_label_set_long_mode(last, LV_LABEL_LONG_DOT);
        lv_obj_set_size(last, CONV_LEFT_W - 40, 18); /* one line */
        js8_station_t st; /* nothing said yet: they heard us, or only chosen (the board, Chat, the map) */
        bool          heard = !it->last[0] && find_station(it->call, &st) && st.heard_me;
        lv_label_set_text(last, it->last[0] ? it->last : heard ? "heard your HB" : "nothing said yet");
        lv_obj_set_style_text_font(last, &sony_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(last, TC(text_mute), LV_PART_MAIN);
        lv_obj_set_pos(last, 8, 22);

        if (it->utc >= 0) {
            lv_obj_t *t = lv_label_create(row);
            lv_label_set_text_fmt(t, "%02d:%02d", (int)(it->utc / 10000), (int)(it->utc / 100 % 100));
            lv_obj_set_style_text_font(t, &sony_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(t, TC(text_mute), LV_PART_MAIN);
            lv_obj_align(t, LV_ALIGN_TOP_RIGHT, -6, 2);
        }
        int n = unread_of(it->call);
        if (n) {
            lv_obj_t *u = lv_label_create(row);
            lv_label_set_text_fmt(u, "%d", n);
            lv_obj_set_style_text_font(u, &sony_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(u, TC(text), LV_PART_MAIN);
            lv_obj_set_style_bg_color(u, TC(err), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(u, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(u, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(u, 6, LV_PART_MAIN);
            lv_obj_align(u, LV_ALIGN_BOTTOM_RIGHT, -6, -3);
        }
    }
}

static void conv_bubble(const js8_rx_msg_t *m)
{
    bool      ours = m->tx;
    lv_obj_t *line = lv_obj_create(conv_body);

    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, ours ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, 6, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    char      hhmm[8];
    lv_obj_t *t = lv_label_create(line);
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", (int)(m->utc / 10000), (int)(m->utc / 100 % 100));
    lv_label_set_text(t, hhmm);
    lv_obj_set_style_text_font(t, &sony_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, TC(text_mute), LV_PART_MAIN);

    /* their words without "CALL: ", ours without our call; GhostNet's translation on a line of its own under them */
    const char *text = m->text, *colon = strstr(text, ": ");
    if (colon && (size_t)(colon - text) < JS8_RX_CALL_LEN) text = colon + 2;
    char words[ROW_MAX];
    int  tr = trans_find(m);
    if (tr >= 0) snprintf(words, sizeof(words), "%s\n%s: %s", text, trans[tr].lang, trans[tr].text);
    else snprintf(words, sizeof(words), "%s", text);

    lv_obj_t *b = lv_label_create(line);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_label_set_text(b, words);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, WIDTH - CONV_LEFT_W - 90, LV_PART_MAIN);
    lv_obj_set_style_text_font(b, &sony_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(b, TC(text), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, ours ? TC(row_tx) : m->to_me ? TC(row_to_me) : TC(row), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(b, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(b, 4, LV_PART_MAIN);
    if (!ours) lv_obj_move_to_index(t, 1); /* the time outside the bubble, on its far side */
}

static void conv_fill(void)
{
    if (!conv_root) return;

    conv_list_build();
    conv_left_fill();

    /* the header: where they are, how we hear them */
    js8_station_t       st;
    const where_t      *w     = station_where(conv_call);
    const lv_img_dsc_t *f     = w ? table_view_flag(w->flag) : NULL;
    bool                known = find_station(conv_call, &st);
    char                info[160] = "", km[16] = "";

    if (f) {
        lv_img_set_src(conv_flag, f);
        lv_obj_clear_flag(conv_flag, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(conv_flag, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(conv_title, conv_call);
    if (w && w->km >= 0) snprintf(km, sizeof(km), "%s%d km", w->approx ? "~" : "", w->km);
    snprintf(info, sizeof(info), "%s  %s  %s", w ? w->country : "", known ? st.grid : "", km);
    if (known)
        snprintf(info + strlen(info), sizeof(info) - strlen(info), "   %+d dB%s", st.snr,
                 st.has_reported_snr ? "" : "");
    if (known && st.has_reported_snr)
        snprintf(info + strlen(info), sizeof(info) - strlen(info), ", they hear you %+d", st.reported_snr);
    lv_label_set_text(conv_info, info);
    lv_obj_set_x(conv_info, CONV_LEFT_W + 6 + 40 + text_w(conv_call, &sony_24) + 12);

    /* the messages, oldest first */
    lv_obj_clean(conv_body);

    int16_t picked[CONV_MAX];
    int     n   = 0;
    int     idx = hist_head;
    for (int i = 0; i < hist_count && n < CONV_MAX; i++) {
        idx = (idx + HISTORY - 1) % HISTORY;
        if (conv_has(&history[idx])) picked[n++] = (int16_t)idx;
    }
    if (!n) {
        lv_obj_t *none = lv_label_create(conv_body);
        lv_label_set_text_fmt(none, "Nothing with %s yet: Reply, Query > or Write...", conv_call);
        lv_obj_set_style_text_color(none, TC(text_mute), LV_PART_MAIN);
        lv_obj_set_style_text_font(none, &sony_18, LV_PART_MAIN);
    }
    while (n--) conv_bubble(&history[picked[n]]);
    lv_obj_update_layout(conv_body);
    lv_obj_scroll_to_y(conv_body, LV_COORD_MAX, LV_ANIM_OFF);
}

static void conv_refresh(const js8_rx_msg_t *m)
{
    if (!conv_root || m->partial) return;
    bool here = conv_has(m);
    if (here) unread_seen(conv_call); /* read as it comes in */
    if (here || m->tx || m->to_me) conv_fill(); /* this one, or the list on the left */
}

/* The station chosen: the one Reply, Query > and Write... go to */
static void conv_choose(const char *call)
{
    js8_station_t st;
    snprintf(conv_call, sizeof(conv_call), "%s", call);
    memcpy(sel_call, conv_call, sizeof(sel_call));
    snprintf(board_sel_call, sizeof(board_sel_call), "%s", conv_call);
    if (find_station(conv_call, &st)) selection_move(st.freq_hz, st.snr);
    else show_selection();
    unread_seen(conv_call);
    buttons_refresh(&btn_conv);
}

/* The TX line over the waterfall while the conversations show, back under it after */
static void tx_line_up(bool up)
{
    lv_coord_t y = up ? 13 + LANE_H : WF_VISIBLE + 13;
    if (tx_bar) lv_obj_set_y(tx_bar, y);
    if (hb_info) lv_obj_align(hb_info, LV_ALIGN_TOP_RIGHT, -20, y + 6);
}

static void conv_close(void)
{
    if (!conv_root) return;
    lv_obj_del(conv_root);
    conv_root = conv_body = conv_info = conv_flag = conv_title = conv_left = NULL;
    tx_line_up(false);
    if (table) table_takes_keys();
    buttons_load_page(&page_1);
    board_update();
}

static void conv_key_cb(lv_event_t *ev)
{
    uint32_t key = *(uint32_t *)lv_event_get_param(ev);
    int      step = 0;

    user_touch();
    if (key == LV_KEY_LEFT || key == LV_KEY_UP) step = -1;
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) step = 1;
    else if (key == LV_KEY_ESC) {
        if (js8_tx_busy(tx) || cq_adjusting || hb_adjusting) esc_pressed(); /* the first ESC stops sending */
        else conv_close();
    } else volume_key(key);
    if (!step || !conv_n) return;

    int at = 0;
    for (int i = 0; i < conv_n; i++)
        if (!strcasecmp(conv_items[i].call, conv_call)) at = i;
    conv_choose(conv_items[(at + step + conv_n) % conv_n].call);
    conv_fill();
}

static void conv_click_cb(lv_event_t *ev)
{
    LV_UNUSED(ev);
    if (now_mono_ms() - conv_opened_ms < 400) return;
    if (!any_popup() && !composing) query_open(); /* every answer and question for them */
}

static void conv_open(void)
{
    if (conv_root || !sel_call[0]) return;

    lv_coord_t top = 13 + LANE_H + TX_BAR_H, h = WF_HEIGHT - LANE_H - TX_BAR_H;
    lv_obj_t  *r   = conv_root = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, WIDTH, h);
    lv_obj_set_pos(r, 13, top);
    lv_obj_set_style_bg_color(r, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    tx_line_up(true);

    /* the way back, under the stations */
    lv_obj_t *back = lv_label_create(r);
    lv_label_set_text(back, "F1, F5 or ESC: back to the board");
    lv_obj_set_style_text_font(back, &sony_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(back, TC(text_mute), LV_PART_MAIN);
    lv_obj_set_pos(back, 6, h - 18);

    lv_obj_t *l = conv_left = lv_obj_create(r);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, CONV_LEFT_W, h - 20);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t x0 = CONV_LEFT_W + 6, cw = WIDTH - x0;
    conv_flag = lv_img_create(r);
    lv_img_set_zoom(conv_flag, 384);
    lv_obj_set_pos(conv_flag, x0 + 8, 12);
    conv_title = lv_label_create(r);
    lv_obj_set_style_text_font(conv_title, &sony_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(conv_title, TC(text), LV_PART_MAIN);
    lv_obj_set_pos(conv_title, x0 + 40, 6);
    conv_info = lv_label_create(r);
    lv_label_set_long_mode(conv_info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(conv_info, cw - 180);
    lv_obj_set_style_text_font(conv_info, &sony_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(conv_info, TC(text_mute), LV_PART_MAIN);
    lv_obj_set_pos(conv_info, x0 + 140, 13);

    lv_obj_t *b = conv_body = lv_obj_create(r);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, cw, h - CONV_HEAD_H);
    lv_obj_set_pos(b, x0, CONV_HEAD_H);
    lv_obj_set_style_bg_color(b, TC(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(b, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(b, 6, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(b, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(r, conv_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(r, conv_click_cb, LV_EVENT_CLICKED, NULL);
    keys_leave_main();
    lv_group_add_obj(keyboard_group, r);
    lv_group_focus_obj(r);
    lv_group_set_editing(keyboard_group, true);

    conv_opened_ms = now_mono_ms();
    conv_choose(sel_call);
    conv_fill();
    buttons_load_page(&page_conv);
}

static const char *conv_label_getter(void) { return "< Board"; } /* F1: back to the cards, as F5 and ESC */

static void conv_back_cb(button_data_t *b) {
    (void)b;
    if (press_closes_list()) return;
    conv_close();
}

static void conv_msg_cb(button_data_t *b) { /* Write...: words for them, through the keyboard */
    char text[JS8_RX_CALL_LEN + 8];
    (void)b;
    if (compose_again() || press_closes_list() || !conv_call[0]) return;
    snprintf(text, sizeof(text), "%s ", conv_call);
    compose_open(text);
    lv_group_set_editing(keyboard_group, true);
    msg_update_text_fmt("To %s: type it and press Enter (MSG first for their Inbox)", conv_call);
}

static const char *chat_label(void) { /* "Chat\nTHEIRCALL" once a station is chosen */
    static char text[JS8_RX_CALL_LEN + 8];
    if (!sel_call[0]) return "Chat";
    snprintf(text, sizeof(text), "Chat\n%s", sel_call);
    return text;
}

static void chat_cb(button_data_t *b) { /* the conversations, on the station chosen */
    (void)b;
    if (press_closes_list() || composing) return;
    if (!sel_call[0]) {
        msg_update_text_fmt("Choose a station first (MFK)");
        return;
    }
    conv_open();
}

static const char *view_label(void) {
    if (board_on) return "View:\nBoard";
    return view_stations ? "View:\nStations" : "View:\nMessages";
}

static void view_cb(button_data_t *b) { /* the board -> the messages -> the stations' table -> the board */
    if (press_closes_list()) return;
    if (board_on) {
        board_show(false);
        view_stations = false;
    } else if (!view_stations) {
        view_stations = true;
    } else {
        view_stations = false;
        board_show(true);
    }
    rebuild_rows();
    buttons_refresh(b);
}

/* Heartbeat's label again when HB or its interval changed elsewhere than in the Settings list (the web page), once */
static void hb_main_follow(void) {
    static int shown = -1;
    int        now   = params.js8_hb.x ? (int)params.js8_hb_interval.x : 0;
    if (now == shown || !btn_hb_main.disp_btn) return;
    shown = now;
    buttons_refresh(&btn_hb_main);
}

static const char *hb_main_label(void) { /* "Heart-\nbeat", or "Heartbeat\nauto 10 min" while they go out */
    static char text[32];
    if (!params.js8_hb.x) return "Heart-\nbeat";
    snprintf(text, sizeof(text), "Heartbeat\nauto %u min", params.js8_hb_interval.x);
    return text;
}

static void hb_main_hold_cb(button_data_t *b) { /* Settings, on the row of the automatic heartbeat */
    (void)b;
    user_touch();
    if (popup_guard() || composing) return;
    settings_open(1);
}

static const char *reply_label_getter(void) { /* "Reply\nTHEIRCALL" once a station is selected */
    static char text[JS8_RX_CALL_LEN + 8];
    if (!sel_call[0]) return "Reply";
    snprintf(text, sizeof(text), "Reply\n%s", sel_call);
    return text;
}

static void query_cb(button_data_t *b) {
    (void)b;
    user_touch(); /* even when the press only closes it */
    if (query_list) { /* a second Query > closes it */
        query_close(); /* and nothing else */
        return;
    }
    if (popup_guard() || composing || aprs_list || texts_list) return;
    query_open();
}

/* Queries and messages for the selected station, if there is one; the saved messages in any case. */
static void query_open(void) { /* (Query >) */
    char  call[JS8_RX_CALL_LEN], title[40];
    float hz;
    int   db;
    bool  selected = selected_station(call, sizeof(call), &hz, &db);

    if (selected) snprintf(title, sizeof(title), "To %s (%+d dB)", call, db);
    else snprintf(title, sizeof(title), "No station selected");
    query_list = query_list_create(300, title);

    lv_obj_t *focus_first = NULL;
    if (selected) {
        focus_first = query_add("HW CPY?", query_hw_cpy_cb, 0); /* how desktop answers a CQ */
        for (int q = 0; q < JS8_Q_COUNT; q++) query_add(js8_query_label((js8_query_t)q), query_pick_cb, q);
        for (size_t m = 0; m < sizeof(query_msg_items) / sizeof(query_msg_items[0]); m++)
            lv_obj_set_style_text_color(query_add(query_msg_items[m].label, query_msg_cb, (intptr_t)m), TC(ok),
                                        LV_PART_MAIN);
    }
    /* Next to last whether or not a station is selected */
    lv_obj_t *saved = query_add("Saved messages >", query_saved_cb, 0);
    if (focus_first == NULL) focus_first = saved;
    /* Close comes last, so it's one step back from the first item (the group wraps round). */
    query_list_end_item("Close", query_close_item_cb);
    lv_group_set_editing(keyboard_group, false); /* the MFK moves between the items */
    lv_group_focus_obj(focus_first);
    speed_warn(); /* when their speed isn't ours */
}

/* ---- Saved messages ------------------------------------------------------- */

/* As desktop JS8Call's Saved Messages: texts sent often, with macros such as <CALL>, <SNR> or
 * <MYGRID4> filled in when they go out. Query > Saved messages >: a press sends one at once (as
 * desktop does by default), holding the MFK on it edits it. The first starts as desktop's "TNX 73 GL". */

/* The time offset (DT) of the newest decode from `call` */
static bool station_dt(const char *call, float *dt) {
    for (int back = 1; back <= hist_count; back++) {
        const js8_rx_msg_t *m = &history[(hist_head + HISTORY - back) % HISTORY];
        if (m->tx || strcasecmp(m->from, call) != 0) continue;
        *dt = m->dt;
        return true;
    }
    return false;
}

/* Desktop's macros with our station's values and the selected station's. prune drops those that don't
 * apply (a saved message on its way out); otherwise they're left as typed. */
static void macros_fill(const char *text, bool prune, char *out, size_t out_len) {
    js8_macro_values_t mv = {0};
    mv.my_call            = params.callsign.x;
    mv.my_grid            = station_qth();
    mv.my_info            = info_text;
    mv.my_status          = status_text;
    mv.version            = JS8_APP_VERSION;
    mv.idle_ms            = touch_ms == 0 ? 0 : now_wall_ms() - touch_ms;
    if (sel_call[0] != '\0') {
        float dt;
        mv.call    = sel_call;
        mv.has_snr = true;
        mv.snr     = sel_snr;
        if (station_dt(sel_call, &dt)) {
            mv.has_tdelta = true;
            mv.tdelta_ms  = (int)(1000 * dt);
        }
    }
    js8_macros_expand(text, &mv, prune, out, (unsigned)out_len);
}

/* JS8_SAVED_PATH: one message per line, an empty line for an empty slot. Read back in capitals, as
 * typing here gives them. */
static void load_saved(void) { /* at every opening */
    memset(saved_msgs, 0, sizeof(saved_msgs));
    FILE *in = fopen(JS8_SAVED_PATH, "r");
    if (in == NULL) { /* nothing saved so far */
        strcpy(saved_msgs[0], "TNX 73 GL");
        return;
    }
    char buf[TX_TEXT_MAX + 8];
    for (int slot = 0; slot < SAVED_N; slot++) {
        if (!fgets(buf, sizeof(buf), in)) break;
        buf[strcspn(buf, "\r\n")] = '\0';
        for (char *p = buf; *p; p++)
            if ('a' <= *p && *p <= 'z') *p -= 'a' - 'A';
        snprintf(saved_msgs[slot], sizeof(saved_msgs[slot]), "%s", buf);
    }
    fclose(in);
}

/* Written to a temporary file first and renamed over the old one, so a power cut can't leave half a
 * file. */
static void save_saved(void) { /* after each edit */
    const char *tmp = JS8_SAVED_PATH ".tmp";
    FILE       *out = fopen(tmp, "w");
    if (out == NULL) {
        msg_update_text_fmt("Can't write %s", JS8_SAVED_PATH);
        return;
    }
    for (int slot = 0; slot < SAVED_N; slot++) fprintf(out, "%s\n", saved_msgs[slot]);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    if (rename(tmp, JS8_SAVED_PATH)) msg_update_text_fmt("Can't write %s", JS8_SAVED_PATH);
}

/* Saved message `slot` into the keyboard: Enter saves it, ESC leaves it as it was. */
static void saved_edit_open(int slot) {
    saved_edit = slot;
    list_drop(&query_list);
    edit_target = EDIT_SAVED;
    compose_open(saved_msgs[slot]);
    if (!composing) { /* the keyboard doesn't open without a callsign */
        edit_target = 0; /* nothing being edited after all */
        return;
    }
    lv_group_set_editing(keyboard_group, true); /* keys to the keyboard */
    msg_update_text_fmt("Message %d: Enter saves, empty clears. Macros: <CALL> <SNR> <TDELTA> <MYCALL> <MYGRID4> "
                        "<MYGRID12> <MYINFO> <MYSTATUS> <MYIDLE> <MYVERSION>",
                        slot + 1);
}

/* A press sends it now with its macros filled in; an empty one opens for writing. */
static void saved_item_cb(lv_event_t *ev) {
    int slot = (int)(intptr_t)lv_event_get_user_data(ev);
    user_touch(); /* a list item, not a bottom button */
    const char *msg = saved_msgs[slot];
    if (msg[0] == '\0') {
        saved_edit_open(slot);
        return;
    }
    char  who[JS8_RX_CALL_LEN];
    float hz;
    int   db;
    bool  selected = selected_station(who, sizeof(who), &hz, &db);
    /* Desktop sends it anyway with <CALL> left out; here it would go to nobody. */
    if (!selected && js8_macros_need_station(msg)) {
        msg_update_text_fmt("Select a station first: message %d has <CALL>, <SNR> or <TDELTA>", slot + 1);
        return;
    }
    char filled[2 * TX_TEXT_MAX];
    macros_fill(msg, true, filled, sizeof(filled));
    char  *start = filled + strspn(filled, " ");
    size_t len   = strlen(start);
    while (len > 0 && start[len - 1] == ' ') start[--len] = '\0';
    if (len == 0) {
        msg_update_text_fmt("Nothing to send: message %d has only macros that are empty now", slot + 1);
        return;
    }
    query_close(); /* the list goes, the message goes out */
    if (selected) apply_hold(hz);
    tx_queue(start);
}

/* Holding the MFK on one edits it. */
static void saved_hold_cb(lv_event_t *ev) {
    user_touch(); /* a list item, not a bottom button */
    /* Wait for the MFK to come up, or its release reaches the keyboard as Enter. */
    lv_indev_t *pressed = lv_indev_get_act();
    if (pressed) lv_indev_wait_release(pressed);
    saved_edit_open((int)(intptr_t)lv_event_get_user_data(ev));
}

/* The focused one: the message line shows what a press would send. */
static void saved_focus_cb(lv_event_t *ev) {
    int         slot = (int)(intptr_t)lv_event_get_user_data(ev);
    const char *msg  = saved_msgs[slot];
    char        filled[2 * TX_TEXT_MAX];

    if (msg[0] == '\0') {
        msg_update_text_fmt("Message %d is empty: press or hold to write one", slot + 1);
    } else if (sel_call[0] == '\0' && js8_macros_need_station(msg)) {
        msg_update_text_fmt("%d: %s (select a station first)", slot + 1, msg);
    } else {
        macros_fill(msg, true, filled, sizeof(filled));
        msg_update_text_fmt("%d: %s", slot + 1, filled);
    }
}

static void saved_back_cb(lv_event_t *ev) {
    (void)ev;
    list_drop(&query_list);
    query_open(); /* the Query list again */
}

/* All ten where the Query list was, wider since messages are long; `focus` gets the focus. */
static void saved_open(int focus) {
    query_list = query_list_create(560, "Saved: press sends, hold edits");
    lv_obj_t *focus_item = NULL;
    for (int slot = 0; slot < SAVED_N; slot++) {
        const char *msg = saved_msgs[slot];
        char        filled[2 * TX_TEXT_MAX], label[2 * TX_TEXT_MAX + 8];
        if (msg[0] == '\0') {
            snprintf(label, sizeof(label), "%d  (empty)", slot + 1);
        } else {
            macros_fill(msg, false, filled, sizeof(filled)); /* as desktop's menu shows them */
            snprintf(label, sizeof(label), "%d  %s", slot + 1, filled);
        }
        lv_obj_t *item = list_add_item(query_list, label);
        /* one line, never scrolling */
        lv_label_set_long_mode(lv_obj_get_child(item, 0), LV_LABEL_LONG_DOT);
        if (msg[0] == '\0') lv_obj_set_style_text_color(item, TC(text_dim), LV_PART_MAIN);
        void *data = (void *)(intptr_t)slot;
        lv_obj_add_event_cb(item, saved_item_cb, LV_EVENT_CLICKED, data);
        lv_obj_add_event_cb(item, saved_hold_cb, LV_EVENT_LONG_PRESSED, data);
        lv_obj_add_event_cb(item, saved_focus_cb, LV_EVENT_FOCUSED, data);
        lv_obj_add_event_cb(item, query_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(keyboard_group, item);
        if (slot == 0 || slot == focus) focus_item = item;
    }
    query_list_end_item("< Back", saved_back_cb);
    query_list_end_item("Close", query_close_item_cb);
    lv_group_set_editing(keyboard_group, false); /* the MFK moves between the messages */
    lv_group_focus_obj(focus_item);
}

/* tools/js8_ui_harness asks which station is selected. */
bool dialog_js8_selected_call(char *out, unsigned size) {
    float hz;
    int   db;
    if (table == NULL) return false;
    return selected_station(out, size, &hz, &db);
}

/* ---- Auto-reply and heartbeats -------------------------------------------- */

static void user_touch(void) { /* any key, button or knob: the idle watchdog starts over */
    int64_t t = now_wall_ms();
    touch_ms  = t;
    js8_auto_user_activity(autop, t);
}

/* Stations heard lately, newest first, for HEARING?, QUERY CALL and RETRIEVE MSG. */
static unsigned recent_stations(js8_heard_t *dst, unsigned room) {
    static js8_station_t by_time[MAX_ROWS];
    int                  count = js8_stations_list(stations, now_wall_ms(), by_time, MAX_ROWS);

    /* The list comes with the stations that heard us at the top: put it back in time order (a stable
     * insertion sort, so equal times keep their places). */
    for (int k = 1; k < count; k++) {
        js8_station_t moving = by_time[k];
        int           j      = k;
        for (; j > 0 && by_time[j - 1].heard_ms < moving.heard_ms; j--) by_time[j] = by_time[j - 1];
        by_time[j] = moving;
    }
    unsigned used = 0;
    for (; used < room && (int)used < count; used++) {
        dst[used] = (js8_heard_t){
            .call     = by_time[used].call,
            .snr      = by_time[used].snr,
            .heard_ms = by_time[used].heard_ms,
        };
    }
    return used;
}

static void auto_send(const js8_auto_result_t *res) {
    int64_t t = now_wall_ms();
    /* The switches may have been changed while it waited. */
    bool on = params.js8_auto.x && (!res->hb_ack || (params.js8_hb.x && params.js8_hb_ack.x));
    if (!on || js8_auto_idle(autop, t)) return;

    if (busy_elsewhere()) {
        /* Later; a newer answer replaces it, and only a different text restarts its time limit. */
        bool same = pending_auto_valid && !strcmp(pending_auto.text, res->text);
        if (!same) pending_auto_ms = t;
        pending_auto       = *res;
        pending_auto_valid = true; /* hb_tick or ui_tx_done sends it */
        return;
    }
    if (res->hb_ack && !js8_speed_heartbeats(cur_speed())) return; /* as desktop: no HB ACK in Turbo */
    int hz = res->hb_ack ? hb_free_offset() : params.js8_tx_freq.x;
    LV_LOG_USER("JS8 auto: '%s' at %d Hz", res->text, hz);
    if (!tx_queue_at(res->text, hz, true)) return;
    js8_auto_sent(autop, res, t);
    if (res->deliver_id) deliver_start(res->deliver_id, res->deliver_group_call, res->text);
    if (res->kind == JS8_REPLY_RELAY) msg_update_text_fmt("Relaying %s's message", res->to);
    add_info_row("Auto: %s", res->text);
}

/* Desktop pauses heartbeats for a QSO. Here they go off until the user turns them on again. A CQ does
 * the same, since the answers to it start a QSO. */
static void hb_pause(const char *reason) {
    if (!(params.js8_hb.x || params.js8_hb_ack.x)) return;
    params_bool_t *off[] = {&params.js8_hb, &params.js8_hb_ack};
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) params_bool_set(off[i], false);
    hb_next_ms   = 0; /* none due */
    hb_adjusting = false; /* nor its knob setting */
    redraw(&btn_hbauto);
    redraw(&btn_hbackk);
    msg_update_text_fmt("Heartbeats off: %s", reason);
    add_info_row("HB and HB ACK off: %s", reason);
    update_status(); /* no next heartbeat */
}

static void qso_started(const char *peer) {
    char reason[JS8_RX_CALL_LEN + 12];
    snprintf(reason, sizeof(reason), "QSO with %s", peer);
    hb_pause(reason);
}

/* AUTO off: what Reply will offer, and a word about it on the message line and in the list. */
static void offer_answer(const js8_auto_result_t *r) {
    strcpy(offer.call, r->to);
    strcpy(offer.text, r->text);
    offer.ms         = now_wall_ms(); /* offered for OFFER_MS */
    offer.deliver_id = r->deliver_id;
    strcpy(offer.deliver_group_call, r->deliver_group_call);

    bool is_msg = !strcmp(r->command, "MSG") || !strcmp(r->command, "MSG TO:");
    if (r->kind == JS8_REPLY_RELAY) {
        char dest[JS8_RX_CALL_LEN]; /* who the relay is for: the text up to its '>' */
        snprintf(dest, sizeof(dest), "%.*s", (int)strcspn(r->text, ">"), r->text);
        msg_update_text_fmt("%s asks you to relay to %s - select it and press Reply to pass it on", r->to, dest);
        add_info_row("%s: Reply passes on \"%s\" (AUTO does it by itself)", r->to, r->text);
    } else if (!strcmp(r->command, ">") || is_msg) {
        if (is_msg) msg_update_text_fmt("Message from %s - select it and press Reply to send ACK", r->to);
        else msg_update_text_fmt("Relayed message via %s - select it and press Reply to send ACK", r->to);
        add_info_row("%s: Reply sends \"%s\" (AUTO sends it by itself)", r->to, r->text);
    } else if (r->deliver_id) {
        msg_update_text_fmt("%s asks for the message held for them - select it and press Reply to send it", r->to);
        add_info_row("%s asks for held message %d: Reply sends it", r->to, r->deliver_id);
    } else if (!strcmp(r->command, "HW CPY?")) {
        msg_update_text_fmt("%s asks how you copy - select it and press Reply", r->to);
        add_info_row("%s asked HW CPY?: Reply has \"%s\"", r->to, r->text);
    } else if (!strcmp(r->command, "MSG ID")) {
        msg_update_text_fmt("%s holds a message for you - select it and press Reply to fetch it", r->to);
        add_info_row("%s holds a message for you: Reply sends \"%s\"", r->to, r->text);
    } else {
        msg_update_text_fmt("%s asked %s - select it and press Reply to answer", r->to, r->command);
        add_info_row("%s asked %s: Reply sends \"%s\"", r->to, r->command, r->text);
    }
}

static void handle_incoming(const js8_rx_msg_t *msg) {
    if (js8_starts_qso(msg)) { /* they answered us */
        char reason[JS8_RX_CALL_LEN + 12];
        snprintf(reason, sizeof(reason), "%s answered", msg->from);
        auto_cq_stop(reason);
        qso_started(msg->from);
    }

    js8_heard_t recent[32];
    unsigned    n_recent = recent_stations(recent, 32);

    /* INFO and STATUS go out with their macros filled in, as desktop answers INFO? and STATUS? */
    char info[TX_TEXT_MAX + 1], stat[TX_TEXT_MAX + 1];
    macros_fill(info_text, true, info, sizeof(info));
    macros_fill(status_text, true, stat, sizeof(stat));

    js8_auto_settings_t how = {0};
    how.autoreply           = params.js8_auto.x;
    how.heartbeat           = params.js8_hb.x;
    how.hb_ack              = params.js8_hb_ack.x;
    how.relay               = params.js8_relay.x;
    how.my_call             = params.callsign.x;
    how.my_grid             = station_qth();
    how.info                = info;
    how.status              = stat;
    how.groups              = groups_text;
    how.held                = held;

    js8_stored_t      stored;
    js8_auto_result_t answer;
    js8_process(autop, msg, &how, recent, n_recent, last_tx_text, now_wall_ms(), inbox, &stored, &answer);
    stored_received(&stored);

    if (answer.action == JS8_AUTO_SEND) auto_send(&answer);
    else if (answer.action == JS8_AUTO_OFFER) offer_answer(&answer);
}

/* Desktop's notices of stored messages (pushNotificationHandler): every 15 minutes, with AUTO on and
 * nothing else to send, "W1ABC RETRIEVE MSG 3" to a station we hold a message for that was heard in
 * the last 15 minutes and hasn't been told in 8 hours. One notice each time. */
#define PUSH_EVERY_MS (15 * 60 * 1000)

static void held_notice_tick(void) {
    int64_t t = now_wall_ms();
    if (push_next_ms == 0) push_next_ms = t + PUSH_EVERY_MS;
    if (t < push_next_ms) return;
    push_next_ms = t + PUSH_EVERY_MS;

    bool can = params.js8_auto.x && held && !js8_auto_idle(autop, t) && params.callsign.x[0];
    if (!can || js8_tx_busy(tx) || pending_auto_valid || composing || any_popup()) return;

    js8_heard_t recent[32];
    unsigned    n_recent = recent_stations(recent, 32);
    char        notice[64];
    if (!js8_held_push_due(held, recent, n_recent, t, notice, sizeof(notice))) return;
    LV_LOG_USER("JS8 auto: '%s'", notice);
    if (tx_queue_at(notice, params.js8_tx_freq.x, true)) add_info_row("Auto: %s", notice);
}

/* Every second: answers that had to wait, stored-message notices, and the heartbeat when it's due. */
static void hb_tick(void) { /* (tx_timer, once a second) */
    beep_log_level(); /* the alert beep's capture level, for the log */
    if (hb_adjusting && now_wall_ms() > hb_adjust_ms + HB_ADJUST_MS) hb_adjust_end();
    /* An automatic answer held back by a list or the keyboard goes once they're closed (ui_tx_done
     * does the same after a transmission), unless it's too late by then. */
    if (pending_auto_valid && now_wall_ms() - pending_auto_ms > PENDING_AUTO_MS) pending_auto_valid = false;
    if (pending_auto_valid && !busy_elsewhere()) {
        pending_auto_valid = false; /* taken */
        auto_send(&pending_auto); /* or put off again */
    }
    held_notice_tick();
    if (params.js8_hb.x == false) {
        hb_next_ms = 0; /* the first comes an interval after HB goes on */
        return;
    }
    int64_t t = now_wall_ms();
    if (js8_auto_idle(autop, t) || !js8_speed_heartbeats(cur_speed())) return;
    /* As desktop, the first comes one interval after HB is switched on (page 1's Heartbeat sends one
     * now). */
    if (!hb_next_ms) {
        hb_next_ms = js8_next_heartbeat_ms(t, params.js8_hb_interval.x);
        update_status(); /* when it's due */
    }
    if (t + 5000 < hb_next_ms) return; /* desktop gets it ready 5 s before */
    if (!params.callsign.x[0] || busy_elsewhere()) return;
    long long due = hb_next_ms;
    LV_LOG_USER("JS8 auto: heartbeat (due %lld)", due);
    if (!send_heartbeat(true)) return;
    hb_next_ms = js8_next_heartbeat_ms(t, params.js8_hb_interval.x);
    update_status(); /* the next one's time */
}

static const char *auto_label_getter(void) { /* js8_auto */
    if (params.js8_auto.x) return "AUTO:\nOn";
    return "AUTO:\nOff";
}

static void auto_cb(button_data_t *b) {
    if (press_closes_list()) return;
    bool on = param_flip(&params.js8_auto);
    buttons_refresh(b);
    redraw(&btn_hbackk); /* idle or not */
    if (on) msg_update_text_fmt("AUTO on: answers SNR? GRID? INFO? STATUS? HEARING? AGN?");
    else msg_update_text_fmt("AUTO off: answers are offered on Reply");
    update_status(); /* AUTO shows there */
}

static const char *hb_label_getter(void) { /* Off, the interval, or the knob setting it */
    static char hb_text[24];
    if (params.js8_hb.x) { /* the interval, or the knob setting it */
        const char *fmt = hb_adjusting ? "HB: knob\n< %u min >" : "HB:\n%u min";
        snprintf(hb_text, sizeof(hb_text), fmt, params.js8_hb_interval.x);
        return hb_text;
    }
    return "HB:\nOff";
}

/* Switching HB on goes straight into setting the interval with the knob. A press (or a while without
 * a turn) ends the setting, the press after that switches heartbeats off. */
static void hb_knob_start(button_data_t *b) {
    cq_adjust_end(); /* one knob setting at a time */
    hb_adjust_ms = now_wall_ms(); /* the setting times out from here */
    hb_adjusting = true; /* the knob sets the interval */
    buttons_refresh(b);
    update_tx_bar(); /* the bar says what the knob does */
}

static void hb_adjust_end(void) { /* the knob setting ends */
    if (hb_adjusting) { /* else nothing to end */
        hb_adjusting = false; /* the knob is free again */
        redraw(&btn_hbauto);
        update_tx_bar(); /* back to what's being sent */
        unsigned minutes = params.js8_hb_interval.x;
        if (params.js8_hb.x) msg_update_text_fmt("HB every %u min", minutes);
    }
}

static void hb_cb(button_data_t *b) {
    if (press_closes_list()) return;
    if (hb_adjusting) { /* this press only ends the setting */
        hb_adjust_end(); /* and nothing else */
        return;
    }
    bool on    = param_flip(&params.js8_hb);
    hb_next_ms = 0; /* counted from now */
    buttons_refresh(b);
    redraw(&btn_hbackk);
    if (on) hb_knob_start(b);
    else msg_update_text_fmt("HB off");
    update_status(); /* the next heartbeat, or none */
}

static void hb_hold_cb(button_data_t *b) {
    if (press_closes_list()) return;
    hb_knob_start(b);
}

static const char *hb_ack_label_getter(void) { /* js8_hb_ack, and whether it can act now */
    if (params.js8_hb_ack.x) return params.js8_auto.x && params.js8_hb.x ? "HB ACK:\nOn" : "HB ACK:\nOn (idle)";
    return "HB ACK:\nOff";
}

static void hb_ack_cb(button_data_t *b) {
    if (press_closes_list()) return;
    bool on = param_flip(&params.js8_hb_ack);
    buttons_refresh(b);
    if (on && !(params.js8_auto.x && params.js8_hb.x)) msg_update_text_fmt("HB ACK acts only while AUTO and HB are on");
    update_status(); /* HB ACK shows there */
}

/* ---- Settings: INFO, STATUS, relay, groups ------------------------------- */

/* @X6100 goes into Groups once, by default: X6100GROUP=1 in the file records
 * that it was done, so after the operator takes it out it stays out. Ten
 * groups is the most kept, so with ten already the last one gives way. */
static void x6100_group_default(void) {
    if (js8_x6100_joined(groups_text) || strlen(groups_text) + 8 > sizeof(groups_text)) return;
    char with[sizeof(groups_text) + 8];
    snprintf(with, sizeof(with), "@X6100 %s", groups_text);
    js8_groups_normalise(with, groups_text, sizeof(groups_text));
}

/* JS8_TEXTS_PATH holds KEY=value lines; anything else in it is skipped, and a key given twice keeps
 * the last value. */
static void load_texts(void) { /* at every opening */
    char *const cleared[] = {info_text, status_text, last_pota, last_sota, alert_words, spot_note, groups_text};
    for (size_t i = 0; i < sizeof(cleared) / sizeof(cleared[0]); i++) cleared[i][0] = '\0';

    bool  x6100_seen = false; /* X6100GROUP= in the file */
    FILE *in         = fopen(JS8_TEXTS_PATH, "r");
    if (in == NULL) {
        x6100_group_default(); /* a first start */
        return;
    }
    char buf[sizeof(alert_words) + 16];
    while (fgets(buf, sizeof(buf), in)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        char *eq = strchr(buf, '=');
        if (eq == NULL) continue;
        *eq             = '\0';
        const char *key = buf, *val = eq + 1;
        if (!strcmp(key, "INFO")) snprintf(info_text, sizeof(info_text), "%s", val);
        else if (!strcmp(key, "STATUS")) snprintf(status_text, sizeof(status_text), "%s", val);
        else if (!strcmp(key, "POTA")) snprintf(last_pota, sizeof(last_pota), "%s", val);
        else if (!strcmp(key, "SOTA")) snprintf(last_sota, sizeof(last_sota), "%s", val);
        else if (!strcmp(key, "ALERTS")) js8_alert_words_normalise(val, alert_words, sizeof(alert_words));
        else if (!strcmp(key, "GROUPS")) js8_groups_normalise(val, groups_text, sizeof(groups_text));
        else if (!strcmp(key, "X6100GROUP")) x6100_seen = true;
        else if (!strcmp(key, "SPOTMODE") && *val) snprintf(spot_mode, sizeof(spot_mode), "%s", val);
        else if (!strcmp(key, "SPOTHZ")) spot_typed_hz = atoi(val);
        else if (!strcmp(key, "SPOTTYPED")) spot_use_typed = atoi(val) != 0;
        else if (!strcmp(key, "SPOTNOTE")) snprintf(spot_note, sizeof(spot_note), "%s", val);
    }
    fclose(in);
    if (spot_typed_hz == 0) spot_use_typed = false; /* nothing typed to spot */
    if (!x6100_seen) x6100_group_default();
}

static void save_texts(void) { /* the whole file, after any change */
    const char *path = JS8_TEXTS_PATH;
    FILE       *out  = fopen(path, "w");
    if (out == NULL) {
        msg_update_text_fmt("Can't write %s", path);
        return;
    }
    fprintf(out, "INFO=%s\n", info_text);
    fprintf(out, "STATUS=%s\n", status_text);
    fprintf(out, "POTA=%s\n", last_pota);
    fprintf(out, "SOTA=%s\n", last_sota);
    fprintf(out, "ALERTS=%s\n", alert_words);
    fprintf(out, "SPOTMODE=%s\n", spot_mode);
    fprintf(out, "SPOTHZ=%d\n", (int)spot_typed_hz);
    fprintf(out, "SPOTTYPED=%d\n", spot_use_typed);
    fprintf(out, "SPOTNOTE=%s\n", spot_note);
    fprintf(out, "GROUPS=%s\n", groups_text);
    fprintf(out, "X6100GROUP=1\n");
    fclose(out);
}

static void texts_close(void) { /* the Settings list, deleted later like the Query list */
    if (texts_list == NULL) return;
    list_forget(&texts_list);
    if (table && !composing) table_takes_keys();
}

static void settings_close_cb(lv_event_t *ev) {
    (void)ev;
    texts_close(); /* the Close item */
}

/* User data of the Settings items: the texts use their edit_t, the relay switch this. */
#define SETTINGS_ITEM_RELAY 100

static const char *relay_item_text(void) {
    if (params.js8_relay.x) return "Relay: On";
    return "Relay: Off";
}

static void texts_item_cb(lv_event_t *ev) {
    int what = (int)(intptr_t)lv_event_get_user_data(ev);
    if (what != SETTINGS_ITEM_RELAY) {
        /* Straight to the keyboard. The list's buttons leave the key group now (the list itself goes
         * later, this callback is one of its own) and the table doesn't get the focus back, or the
         * keyboard would open without it and not work. */
        list_drop(&texts_list);
        edit_target         = what;
        const char *current = groups_text;
        if (what == EDIT_INFO) current = info_text;
        else if (what == EDIT_STATUS) current = status_text;
        compose_open(current);
        lv_group_set_editing(keyboard_group, true); /* as Reply and Send... leave it */
        return;
    }
    /* Desktop's "Disable message relay (>)", which also stops holding MSG TO: messages for others.
     * It switches where it is and the list stays open. */
    bool      on   = param_flip(&params.js8_relay);
    lv_obj_t *item = lv_event_get_target(ev);
    lv_label_set_text(lv_obj_get_child(item, 0), relay_item_text());
    if (on) msg_update_text_fmt("Relay on: relays (>) are passed on and MSG TO: messages held, as desktop JS8Call");
    else msg_update_text_fmt("Relay off: relays (>) and MSG TO: messages for others are ignored");
}

static void settings_key_cb(lv_event_t *ev) {
    uint32_t k = *(uint32_t *)lv_event_get_param(ev);
    if (k == LV_KEY_ESC) texts_close();
    else list_step_key(k);
}

/* A Settings item; its text stays on one line. */
static lv_obj_t *settings_item(const char *text, int what) {
    lv_obj_t *item = list_add_item(texts_list, text);

    lv_obj_add_event_cb(item, texts_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    lv_obj_add_event_cb(item, settings_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, item);
    lv_label_set_long_mode(lv_obj_get_child(item, 0), LV_LABEL_LONG_DOT);
    return item;
}

/* A Settings item showing a text, or `none` while it's empty */
static lv_obj_t *settings_text_item(const char *name, const char *value, const char *none, int what) {
    char line[TEXT_MAX + 16];
    snprintf(line, sizeof(line), "%s: %s", name, value[0] ? value : none);
    return settings_item(line, what);
}

/* Settings: INFO and STATUS (what AUTO answers to INFO? and STATUS?), relaying on or off, and the
 * groups we belong to, as in desktop's settings. */
static void texts_cb(button_data_t *b) {
    (void)b;
    user_touch(); /* even when the press only closes it */
    if (texts_list) { /* a second Settings... closes it */
        texts_close(); /* and nothing else */
        return;
    }
    if (popup_guard() || query_list || aprs_list || composing) return;

    lv_obj_t *list = texts_list = popup_frame(560, 290, "Settings");
    lv_obj_t *info_item         = settings_text_item("INFO", info_text, "(not set)", EDIT_INFO);
    settings_text_item("STATUS", status_text, "(not set)", EDIT_STATUS);
    settings_item(relay_item_text(), SETTINGS_ITEM_RELAY);
    settings_text_item("Groups", groups_text, "(none)", EDIT_GROUPS);

    lv_obj_t *done = list_add_item(list, "Close");
    lv_obj_set_style_text_color(done, TC(action), LV_PART_MAIN);
    lv_obj_add_event_cb(done, settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(done, settings_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, done);
    lv_group_focus_obj(info_item);
    lv_group_set_editing(keyboard_group, false); /* the MFK moves between the items */
}

/* ---- Popups over the waterfall: APRS, spot form, Log, Inbox, Alerts, Freq */

/* The knob back on the message list, unless the keyboard holds it. */
static void p3_knob_to_table(void) {
    if (!table || composing) return;
    table_takes_keys(); /* out of the group while a popup was up */
}

/* Close a popup. This often runs inside one of its own buttons' events, so
 * the object goes on the next LVGL pass; the pointer is cleared now. */
static void p3_popup_shut(lv_obj_t **popup) {
    if (*popup == NULL) return;
    lv_obj_del_async(*popup);
    *popup = NULL;
    p3_knob_to_table();
}

/* Leave a popup for the keyboard or another view. Its buttons quit the
 * knob's group straight away, or the keyboard opens without the focus. */
static void p3_popup_drop(lv_obj_t **popup) {
    lv_obj_t *p = *popup;
    uint32_t  n = lv_obj_get_child_cnt(p);
    for (uint32_t k = 0; k < n; k++) lv_group_remove_obj(lv_obj_get_child(p, k));
    lv_obj_del_async(p);
    *popup = NULL;
}

/* An empty popup list, top right over the waterfall. The message list
 * gives up the knob to it. */
static lv_obj_t *p3_popup_new(lv_coord_t width) {
    keys_leave_main(); /* back with p3_knob_to_table() */
    lv_obj_t *p = lv_list_create(dialog.obj);
    lv_obj_set_size(p, width, WF_HEIGHT - 10);
    lv_obj_align(p, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(p, &sony_24, 0);
    lv_obj_set_style_bg_color(p, TC(list), 0);
    lv_obj_set_style_border_color(p, TC(text), 0);
    return p;
}

/* A line of plain text in a popup (title, details), a size smaller. */
static lv_obj_t *p3_popup_note(lv_obj_t *popup, const char *text) {
    lv_obj_t *note = lv_list_add_text(popup, text);
    lv_obj_set_style_text_font(note, &sony_22, 0);
    return note;
}

/* A message body in a popup: wrapped, on the list's colours. */
static void p3_popup_body(lv_obj_t *popup, const char *text) {
    lv_obj_t *body = lv_list_add_text(popup, text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(body, TC(list), 0);
    lv_obj_set_style_text_color(body, TC(text), 0);
    lv_obj_set_style_pad_ver(body, 8, 0);
}

/* A button of a popup, in the knob's group; `id` comes back as the click
 * event's user data. */
static lv_obj_t *p3_popup_item(lv_obj_t *popup, const char *label, lv_event_cb_t on_click, lv_event_cb_t on_key,
                               intptr_t id) {
    lv_obj_t *item = list_add_item(popup, label);
    lv_obj_add_event_cb(item, on_click, LV_EVENT_CLICKED, (void *)id);
    lv_obj_add_event_cb(item, on_key, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, item);
    return item;
}

/* Too long for the popup: end in "...". A scrolling label would redraw the
 * whole list all the time. */
static void p3_popup_dots(lv_obj_t *item) {
    lv_label_set_long_mode(lv_obj_get_child(item, 0), LV_LABEL_LONG_DOT);
}

/* The knob on `item`, moving between the items (not editing). */
static void p3_popup_focus(lv_obj_t *item) {
    lv_group_set_editing(keyboard_group, false); /* the knob moves between items */
    lv_group_focus_obj(item);
}

/* Keys on a popup item: the arrows move along the list. True for ESC,
 * which each popup handles its own way. */
static bool p3_popup_key(lv_event_t *ev) {
    uint32_t key = *(uint32_t *)lv_event_get_param(ev);
    if (key == LV_KEY_UP || key == LV_KEY_LEFT) lv_group_focus_prev(keyboard_group);
    if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) lv_group_focus_next(keyboard_group);
    return key == LV_KEY_ESC;
}

/* Into the keyboard with `prefill`, the knob editing the text. */
static void p3_keyboard(const char *prefill) {
    compose_open(prefill); /* NULL: empty */
    lv_group_set_editing(keyboard_group, true); /* the knob types */
}

/* `text` in the message line as it is: no format, no arguments. */
static void p3_say(const char *text) {
    msg_update_text_fmt("%s", text);
}

/* The same for a check that failed: false. */
static bool p3_refuse(const char *why) {
    p3_say(why);
    return false;
}

/* First thing in a bottom button's handler: the screen stays awake. True
 * when a popup was open: the press only closed it. */
static bool p3_press_closes_popup(void) {
    user_touch(); /* a press counts as activity */
    return popup_guard();
}

/* The same for a button that opens popup `own`: pressed again, it closes
 * it. True when the press ends there: a popup closed, or the keyboard is
 * open. */
static bool p3_toggle_press(lv_obj_t *own, void (*shut)(void)) {
    if (own == NULL) return p3_press_closes_popup() || composing;
    user_touch(); /* the second press closes it */
    shut();
    return true;
}

/* Nothing is being sent; else the user is told to stop it first. */
static bool p3_tx_idle(void) {
    return !js8_tx_busy(tx) || p3_refuse("Not while sending - Stop TX first");
}

/* Our callsign is set; if not, the user is told where. */
static bool p3_have_callsign(void) {
    return params.callsign.x[0] != '\0' || p3_refuse("Set your callsign first: Settings > General");
}

/* The selected station's call and audio frequency. */
static bool p3_selected(char call[JS8_RX_CALL_LEN], float *freq_hz) {
    int snr_db;
    return selected_station(call, JS8_RX_CALL_LEN, freq_hz, &snr_db);
}

static const char *p3_skip_spaces(const char *s) {
    return s + strspn(s, " ");
}

/* Park or summit reference, kept in JS8_TEXTS_PATH. */
static void p3_remember_ref(bool sota, const char *ref) {
    if (sota) snprintf(last_sota, sizeof(last_sota), "%s", ref);
    else snprintf(last_pota, sizeof(last_pota), "%s", ref);
    save_texts(); /* the reference is kept with the texts */
}

/* ---- APRS through an @APRSIS gateway ------------------------------------ */

/* A JS8Call station that spots to APRS passes "@APRSIS" traffic on to
 * APRS-IS, in the forms desktop JS8Call and JS8Spotter use. A message is
 * "@APRSIS CMD :", the addressee padded to 9 columns, ':' and at most 67
 * characters; it goes out from your bare callsign (no SSID). */
#define APRS_CMD      "@APRSIS CMD :" /* + addressee in 9 columns + ':' + text */
#define APRS_CMD_RAW  "@APRSIS CMD "  /* + any APRS packet body: a position report here */
#define APRS_TEXT_MAX 67              /* APRS message text, after the addressee */

enum aprs_choice { AP_GRID, AP_GPS, AP_POTA, AP_SOTA, AP_SMS, AP_EMAIL, AP_WL_START, AP_WL_TEXT, AP_WL_SEND, AP_COUNT };

/* The APRS list, in order. The message items open the keyboard on `head`
 * with `hint` shown. */
static const struct aprs_entry {
    const char *label, *head, *hint;
} aprs_menu[AP_COUNT] = {
    [AP_GRID]     = {"Spot my grid", NULL, NULL},
    [AP_GPS]      = {"Spot GPS position", NULL, NULL},
    [AP_POTA]     = {"POTA spot", NULL, NULL},
    [AP_SOTA]     = {"SOTA spot", NULL, NULL},
    [AP_SMS]      = {"SMS text", APRS_CMD "SMS      :@", "SMS (NA7Q gateway): 10-digit number, space, message"},
    [AP_EMAIL]    = {"Email", APRS_CMD "EMAIL-2  :", "Email: address, space, message"},
    [AP_WL_START] = {"Winlink: start", APRS_CMD "WLNK-1   :SP ",
                     "Winlink 1/3: address (or call), space, subject. Spot your grid first"},
    [AP_WL_TEXT]  = {"Winlink: text", APRS_CMD "WLNK-1   :", "Winlink 2/3: one line of the message; repeat for more lines"},
    [AP_WL_SEND]  = {"Winlink: send", NULL, NULL},
};

static unsigned aprs_seq; /* the last "{NN}" message ID given, 1 to 99; 0: none yet */

/* Check and complete text before it's sent. Anything that isn't an APRS
 * CMD message passes as it is. An APRS one needs its addressee field and
 * at most APRS_TEXT_MAX characters; SMS, email and Winlink get a message
 * ID ("{01}", as JS8Spotter adds), and a POTA / SOTA spot's reference is
 * kept for the next one and for the log. */
static bool aprs_prepare(const char *typed, char *ready, size_t room) {
    const size_t cmd = strlen(APRS_CMD);
    snprintf(ready, room, "%s", typed);
    if (strncmp(typed, APRS_CMD, cmd) != 0) return true;

    const char *addr = typed + cmd;
    if (strlen(addr) < 10 || addr[9] != ':') return p3_refuse("APRS: the addressee is 9 characters, then ':'");
    int to_len = 9;
    while (to_len > 0 && addr[to_len - 1] == ' ') to_len--;
    char to[9 + 1];
    snprintf(to, sizeof(to), "%.*s", to_len, addr);

    const char *text = p3_skip_spaces(addr + 10);
    if (*text == '\0') {
        msg_update_text_fmt("APRS: nothing to send to %s", to); /* addressee without the padding */
        return false;
    }
    bool gateway_acks = strcmp(to, "SMS") == 0 || strcmp(to, "EMAIL-2") == 0 || strcmp(to, "WLNK-1") == 0;
    if (gateway_acks && strchr(text, '{') == NULL) {
        /* Start somewhere after a restart, so the gateway doesn't take a new
         * message for a repeat of an old one. */
        aprs_seq = (aprs_seq ? aprs_seq : (unsigned)(time(NULL) % 90)) % 99 + 1;
        snprintf(ready, room, "%s{%02u}", typed, aprs_seq);
    }
    size_t len = strlen(ready) - cmd - 10;
    if (len > APRS_TEXT_MAX) {
        msg_update_text_fmt("APRS allows %d characters after the addressee, this is %u: shorten it", APRS_TEXT_MAX,
                            (unsigned)len);
        return false;
    }

    /* APSPOT: "! POTA PARK MHZ MODE ..." or "! SOTA SUMMIT ...";
     * POTAGW (older): "CALL PARK KHZ MODE ..."; APRS2SOTA: "SUMMIT FREQ MODE ..." */
    char a[24] = "", b[24] = "", c[24] = "";
    sscanf(text, "%23s%23s%23s", a, b, c);
    bool apspot = strcmp(to, "APSPOT") == 0 && strcmp(a, "!") == 0 && c[0];
    if (apspot && (strcmp(b, "POTA") == 0 || strcmp(b, "SOTA") == 0)) p3_remember_ref(b[0] == 'S', c);
    else if (strcmp(to, "POTAGW") == 0 && b[0]) p3_remember_ref(false, b);
    else if ((strcmp(to, "APRS2SOTA") == 0 || strcmp(to, "SOTA") == 0) && a[0]) p3_remember_ref(true, a);
    return true;
}

/* The keyboard on `head`, the cursor at its end. */
static void aprs_compose(const char *head) {
    compose_open(head);
    if (composing) lv_textarea_set_cursor_pos(textarea_window_text(), (int32_t)strlen(head)); /* after the head */
}

/* Your grid from Settings, 6 characters at most, and its centre. Without
 * one: false, and the user is told where to set it. NULL outputs are
 * skipped. */
static bool aprs_grid(char out_grid[8], double *out_lat, double *out_lon) {
    char mine[8];
    snprintf(mine, sizeof(mine), "%.6s", station_qth());
    if (strlen(mine) < 4) return p3_refuse("Set your grid first: Settings > General");
    if (out_grid) memcpy(out_grid, mine, sizeof(mine));
    if (out_lat && out_lon) qth_str_to_pos(mine, out_lat, out_lon);
    return true;
}

/* gps.c: the last fix from gpsd and how old it is. */
extern bool gps_last_fix(double *lat, double *lon, int *age_s);

/* Where the GPS puts us now, with a 10-character grid (about 20 x 35 m).
 * No fix, or one over 2 minutes old: false, and the user is told why. */
static bool aprs_gps(char out_grid[12], double *out_lat, double *out_lon) {
    double fix_lat, fix_lon;
    int    age_s;
    char   locator[12];
    if (!gps_last_fix(&fix_lat, &fix_lon, &age_s))
        return p3_refuse("No GPS fix: plug in a GPS and wait for a fix (APP > GPS shows it)");
    if (age_s > 120) {
        msg_update_text_fmt("No current GPS fix (last one %d min ago)", age_s / 60);
        return false;
    }
    if (!js8_latlon_to_grid(fix_lat, fix_lon, 10, locator, sizeof(locator)))
        return p3_refuse("GPS position out of range");
    if (out_grid) memcpy(out_grid, locator, sizeof(locator));
    if (out_lat) *out_lat = fix_lat;
    if (out_lon) *out_lon = fix_lon;
    return true;
}

/* Whole degrees and minutes; 59.95' and over prints as the next degree. */
static void p3_deg_min(double v, int *deg, double *min) {
    v    = fabs(v);
    *deg = (int)v;
    *min = (v - *deg) * 60.0;
    if (*min >= 59.95) {
        *deg += 1;
        *min = 0;
    }
}

/* An APRS position report with a comment, "=4916.9 N/12307.2 WG TEXT".
 * Minutes to one decimal and the last digit left blank (APRS position
 * ambiguity, about 185 m): JS8 sends a space in far fewer bits than a
 * digit. The space before the comment lets JS8's word table pack it
 * (5 frames instead of 6 for "MADE IT TO CAMP"). "/G" is the grid square
 * symbol, the one JS8Call's own grid spots show. */
static void aprs_position(double lat, double lon, const char *note, char *report, size_t room) {
    int    lat_deg, lon_deg;
    double lat_min, lon_min;
    p3_deg_min(lat, &lat_deg, &lat_min);
    p3_deg_min(lon, &lon_deg, &lon_min);
    snprintf(report, room, "=%02d%04.1f %c/%03d%04.1f %cG %s", lat_deg, lat_min, lat < 0 ? 'S' : 'N', lon_deg,
             lon_min, lon < 0 ? 'W' : 'E', note);
}

/* The text Spot my grid / Spot GPS position sends. On its own, desktop
 * JS8Call's "@APRSIS GRID <grid>" (the gateway adds frequency and SNR).
 * GRID has no room for a message, so with one it is a position report
 * through CMD, the message as its comment. False, with the reason shown,
 * when there's no position. */
static bool beacon_text(bool gps, const char *note, char *text, size_t room, char grid[12]) {
    double at_lat, at_lon;
    bool   located = gps ? aprs_gps(grid, &at_lat, &at_lon) : aprs_grid(grid, &at_lat, &at_lon);
    if (!located) return false;
    note = p3_skip_spaces(note);
    if (*note == '\0') {
        snprintf(text, room, "@APRSIS GRID %s", grid); /* as desktop JS8Call sends it */
    } else {
        char report[80];
        aprs_position(at_lat, at_lon, note, report, sizeof(report));
        snprintf(text, room, APRS_CMD_RAW "%s", report);
    }
    return true;
}

static void aprs_beacon(bool gps, const char *note) {
    char beacon[112], grid[12];
    if (!beacon_text(gps, note, beacon, sizeof(beacon), grid)) return;
    note = p3_skip_spaces(note);
    if (!tx_queue(beacon)) return;
    if (*note == '\0') add_info_row("APRS: spotting %s at %s%s", params.callsign.x, grid, gps ? " (GPS)" : "");
    else add_info_row("APRS: position %s (%s%s) with \"%s\"", params.callsign.x, grid, gps ? ", GPS" : "", note);
}

/* As the beacon's message is typed: its time on the air. */
static void beacon_changed_cb(lv_event_t *ev) {
    char             beacon[112], grid[12];
    js8_tx_preview_t preview;
    LV_UNUSED(ev);
    if (!beacon_text(edit_target == EDIT_BEACON_GPS, textarea_window_get(), beacon, sizeof(beacon), grid)) return;
    js8_tx_preview(params.callsign.x, station_qth(), beacon, cur_speed(), &preview);
    if (!preview.ok) return;
    if (*p3_skip_spaces(textarea_window_get()))
        msg_update_text_fmt("Position + message: %d frames, %.0f s", preview.frames, preview.seconds);
    else
        msg_update_text_fmt("Plain position: %d frames, %.0f s - type a message, or Enter", preview.frames,
                            preview.seconds);
}

static void aprs_close(void) { p3_popup_shut(&aprs_list); }

static void aprs_item_cb(lv_event_t *ev) {
    int item = (int)(intptr_t)lv_event_get_user_data(ev);
    p3_popup_drop(&aprs_list);

    if (item == AP_POTA || item == AP_SOTA) {
        bool sota = item == AP_SOTA;
        spot_show(sota, (sota ? last_sota : last_pota)[0] ? SP_SEND : SP_REF);
        return;
    }
    if (item == AP_GRID || item == AP_GPS) {
        /* An optional message, or just Enter. The position is checked
         * first, so nothing is typed for nothing. */
        bool gps = item == AP_GPS;
        if (gps ? aprs_gps(NULL, NULL, NULL) : aprs_grid(NULL, NULL, NULL)) {
            edit_target = gps ? EDIT_BEACON_GPS : EDIT_BEACON_GRID;
            compose_open(NULL); /* an empty message: Enter sends the bare beacon */
            beacon_changed_cb(NULL); /* the bare beacon's length until something is typed */
        }
    } else if (item == AP_WL_SEND) {
        if (tx_queue(APRS_CMD "WLNK-1   :/EX")) p3_say("Winlink 3/3: sending /EX");
    } else if (item < AP_COUNT && aprs_menu[item].head) {
        aprs_compose(aprs_menu[item].head);
        p3_say(aprs_menu[item].hint);
    }
    if (!composing) p3_knob_to_table();
    else lv_group_set_editing(keyboard_group, true); /* the knob types */
}

static void aprs_close_cb(lv_event_t *ev) {
    LV_UNUSED(ev);
    p3_popup_shut(&aprs_list);
}

static void aprs_key_cb(lv_event_t *ev) {
    if (p3_popup_key(ev)) p3_popup_shut(&aprs_list);
}

static void aprs_cb(button_data_t *b) {
    LV_UNUSED(b);
    if (p3_toggle_press(aprs_list, aprs_close) || !p3_have_callsign()) return;
    aprs_list = p3_popup_new(300);
    p3_popup_note(aprs_list, "APRS via @APRSIS");
    lv_obj_t *top = NULL;
    for (int k = 0; k < AP_COUNT; k++) {
        lv_obj_t *item = p3_popup_item(aprs_list, aprs_menu[k].label, aprs_item_cb, aprs_key_cb, k);
        if (top == NULL) top = item;
    }
    lv_obj_t *done = p3_popup_item(aprs_list, "Close", aprs_close_cb, aprs_key_cb, 0);
    lv_obj_set_style_text_color(done, TC(action), 0);
    p3_popup_focus(top);
}

/* ---- Spot form for POTA and SOTA ------------------------------------------ */

/* A POTA spot goes to APSPOT, which posts it on pota.app (that needs a
 * pota.app account); a SOTA spot to APRS2SOTA (registration with
 * sotaspots.co.uk). Both want MHz. The frequency is the JS8 dial or one
 * you typed (an SSB run, say); it is kept, with the mode and comment.
 * What the gateways answer goes back over APRS: the radio never hears it. */

#define SPOT_MODE_COUNT 6
static const char *const apspot_modes[SPOT_MODE_COUNT] = {"DATA", "SSB", "CW", "FM", "AM", "FT8"}; /* APSPOT's */
static const char *const aprs2sota_modes[SPOT_MODE_COUNT] = {"DATA", "SSB", "CW", "FM", "AM", "DV"};  /* APRS2SOTA's */

static lv_obj_t *spot_rows[SP_COUNT]; /* the form's items, while it's open */
static lv_obj_t *spot_line;           /* the message as it will go out */

static const char *const *p3_spot_modes(void) {
    return spot_sota ? aprs2sota_modes : apspot_modes;
}

/* The kept mode if this gateway knows it, else DATA. */
static const char *p3_spot_mode(void) {
    const char *const *modes = p3_spot_modes();
    for (int k = 0; k < SPOT_MODE_COUNT; k++)
        if (strcmp(modes[k], spot_mode) == 0) return modes[k];
    return modes[0]; /* DATA, with either gateway */
}

static bool p3_spot_typed(void) {
    return spot_use_typed && spot_typed_hz != 0;
}

static int32_t p3_spot_hz(void) {
    return p3_spot_typed() ? spot_typed_hz : cparam_i_get(cfg_fg_freq);
}

/* MHz, 3 decimals or as many more as it takes: "7.078", "7.1855". */
static void p3_mhz(int32_t hz, char *text, size_t room) {
    int frac = (int)(hz % 1000000), digits = 6;
    while (digits > 3 && frac % 10 == 0) {
        frac /= 10;
        digits--;
    }
    snprintf(text, room, "%d.%0*d", (int)(hz / 1000000), digits, frac);
}

/* The comment to send: the typed one; with none, "JS8" on the JS8 dial. */
static const char *p3_spot_note(void) {
    if (spot_note[0] != '\0') return spot_note;
    return p3_spot_typed() ? "" : "JS8";
}

/* What follows "@APRSIS CMD :<addressee>:". */
static void p3_spot_body(char *text, size_t room) {
    char        mhz[16];
    const char *note = p3_spot_note(), *mode = p3_spot_mode();
    const char *sep  = note[0] ? " " : "";
    p3_mhz(p3_spot_hz(), mhz, sizeof(mhz));
    const char *ref  = spot_sota ? last_sota : last_pota;
    if (ref[0] == '\0') ref = "?";
    if (spot_sota) snprintf(text, room, "%s %s %s %s%s%s", ref, mhz, mode, params.callsign.x, sep, note);
    else snprintf(text, room, "! POTA %s %s %s%s%s", ref, mhz, mode, sep, note);
}

static bool spot_freq_empty(const char *typed) {
    return *p3_skip_spaces(typed) == '\0';
}

/* kHz ("7185.5") or, under 1000, MHz ("144.2"); 1.8 MHz to 1.3 GHz. On a
 * bad one, false with the reason shown: the keyboard stays open. */
static bool spot_parse_freq(const char *typed, int32_t *hz_out) {
    char  *rest;
    double num = strtod(typed, &rest);
    if (rest == typed || num <= 0) return p3_refuse("Type the frequency in kHz, e.g. 7185");
    double hz = num < 1000 ? num * 1e6 : num * 1e3;
    if (hz < 1800000 || hz > 1300000000) return p3_refuse("Out of range: 1800 kHz - 1300 MHz"); /* 160 m to 23 cm */
    if (hz_out) *hz_out = (int32_t)(hz + 0.5);
    return true;
}

static void spot_close(void) { p3_popup_shut(&spot_list); }

/* What item `item` of the form says now. */
static void p3_spot_item_text(spot_item_t item, char *text, size_t room) {
    char        mhz[16];
    const char *ref = spot_sota ? last_sota : last_pota;
    text[0]         = '\0';
    if (item == SP_SEND) {
        snprintf(text, room, "Send spot");
    } else if (item == SP_REF) {
        snprintf(text, room, "%s: %s", spot_sota ? "Summit" : "Park", ref[0] ? ref : "(type it)");
    } else if (item == SP_FREQ) {
        p3_mhz(p3_spot_hz(), mhz, sizeof(mhz));
        snprintf(text, room, "Frequency: %s MHz (%s)", mhz, p3_spot_typed() ? "typed" : "JS8 dial");
    } else if (item == SP_MODE) {
        snprintf(text, room, "Mode: %s", p3_spot_mode());
    } else if (item == SP_NOTE) {
        const char *note = p3_spot_note();
        if (spot_note[0]) snprintf(text, room, "Comment: %s", note);
        else snprintf(text, room, note[0] ? "Comment: %s (auto)" : "Comment: (none)", note);
    } else if (item == SP_CLOSE) {
        snprintf(text, room, "Close");
    }
}

/* Every label and the preview line again, the focus staying put. */
static void p3_spot_update(void) {
    char text[96];
    for (int k = 0; k < SP_COUNT; k++) {
        if (spot_rows[k] == NULL) continue;
        p3_spot_item_text((spot_item_t)k, text, sizeof(text));
        lv_label_set_text(lv_obj_get_child(spot_rows[k], 0), text);
    }
    p3_spot_body(text, sizeof(text));
    lv_label_set_text_fmt(spot_line, "%s: %s", spot_sota ? "APRS2SOTA" : "APSPOT", text);
}

static void p3_spot_send(void) {
    const char *gateway = spot_sota ? "APRS2SOTA" : "APSPOT";
    if ((spot_sota ? last_sota : last_pota)[0] == '\0') {
        msg_update_text_fmt("Type the %s first", spot_sota ? _("summit") : _("park")); /* the form can't send without */
        return;
    }
    char body[96], text[TX_TEXT_MAX + 8], ready[TX_TEXT_MAX + 8];
    p3_spot_body(body, sizeof(body));
    snprintf(text, sizeof(text), APRS_CMD "%-9s:%s", gateway, body);
    if (!aprs_prepare(text, ready, sizeof(ready)) || !tx_queue(ready)) return; /* aprs_prepare says why */
    const char *kind = spot_sota ? "SOTA" : "POTA";
    add_info_row("%s spot: %s. A JS8Call APRS gateway must hear it; the reply goes to APRS, not here", kind, body);
    p3_popup_shut(&spot_list);
}

/* The next mode the gateway takes, in place. */
static void p3_spot_next_mode(void) {
    const char *const *modes = p3_spot_modes();
    const char        *now   = p3_spot_mode();
    int                at    = 0;
    while (at < SPOT_MODE_COUNT && strcmp(modes[at], now) != 0) at++;
    snprintf(spot_mode, sizeof(spot_mode), "%s", modes[(at + 1) % SPOT_MODE_COUNT]);
    save_texts(); /* the mode is kept with the texts */
    p3_spot_update();
}

/* A field of the form into the keyboard; log_edit_done brings the form
 * back. */
static void p3_spot_edit(int field, const char *prefill, const char *hint) {
    p3_popup_drop(&spot_list);
    edit_target = field;
    p3_keyboard(prefill);
    p3_say(hint);
}

static void spot_item_cb(lv_event_t *ev) {
    spot_item_t item = (spot_item_t)(intptr_t)lv_event_get_user_data(ev);
    char        khz[16] = ""; /* the last frequency typed, even while the dial is spotted */
    if (item == SP_SEND) {
        p3_spot_send();
    } else if (item == SP_CLOSE) {
        p3_popup_shut(&spot_list);
    } else if (item == SP_MODE) {
        p3_spot_next_mode();
    } else if (item == SP_REF) {
        p3_spot_edit(EDIT_SPOT_REF, spot_sota ? last_sota : last_pota,
                     spot_sota ? "Summit, e.g. VE7/LM-001"
                               : "Park, e.g. CA-1234 (POTA uses US- and CA- now, not K- or VE-)");
    } else if (item == SP_FREQ) { /* emptied, the spot is for the JS8 dial */
        if (spot_typed_hz) format_khz(spot_typed_hz, khz, sizeof(khz));
        p3_spot_edit(EDIT_SPOT_FREQ, khz,
                     "Frequency in kHz (7185) or MHz for VHF (144.2); clear it and Enter for the JS8 dial");
    } else if (item == SP_NOTE) {
        p3_spot_edit(EDIT_SPOT_NOTE, spot_note, "Comment (optional; with none, a JS8 dial spot says JS8)");
    }
}

static void spot_key_cb(lv_event_t *ev) {
    if (p3_popup_key(ev)) p3_popup_shut(&spot_list);
}

/* The form, the knob on `focus`: Send, or the field just typed. */
static void spot_show(bool sota, spot_item_t start) {
    char text[96];
    spot_sota = sota; /* the form's labels and sends follow it */
    spot_list = p3_popup_new(560);
    p3_popup_note(spot_list, sota ? "SOTA spot via APRS2SOTA (registration needed)"
                                  : "POTA spot via APSPOT (your pota.app account)");
    spot_line = p3_popup_note(spot_list, "");
    lv_obj_set_style_text_color(spot_line, TC(title_text), 0); /* it sits on the list's light title bar */
    for (int k = 0; k < SP_COUNT; k++) {
        p3_spot_item_text((spot_item_t)k, text, sizeof(text));
        spot_rows[k] = p3_popup_item(spot_list, text, spot_item_cb, spot_key_cb, k);
        p3_popup_dots(spot_rows[k]);
    }
    lv_obj_set_style_text_color(spot_rows[SP_SEND], TC(ok), 0);
    lv_obj_set_style_text_color(spot_rows[SP_CLOSE], TC(action), 0);
    p3_spot_update();
    p3_popup_focus(spot_rows[start]);
    if (!sota && (strncmp(last_pota, "K-", 2) == 0 || strncmp(last_pota, "VE-", 3) == 0))
        p3_say("POTA park numbers are US-/CA- now (was K-/VE-): check yours");
}

/* ---- Logging a QSO ----------------------------------------------------- */

/* As in desktop JS8Call's Log QSO: an entry filled in from the QSO, saved
 * to JS8_LOG_PATH (ADIF, on the SD card's DATA partition) and to the
 * radio's QSO database, which marks the stations worked. When a two-way
 * QSO ends with 73 or SK the popup opens by itself (Log prompt); nothing
 * goes in the log until Save. */

/* A popup or the map is over the waterfall. */
static bool any_popup(void) { return aprs_list || spot_list || log_list || inbox_list || alerts_list || freq_list ||
                                     query_list || texts_list || settings_list || alert_box || map_obj; }

static bool find_station(const char *wanted, js8_station_t *found) {
    static js8_station_t seen[MAX_ROWS]; /* too big for the stack */
    int                  count = js8_stations_list(stations, now_wall_ms(), seen, MAX_ROWS);
    for (const js8_station_t *s = seen; s < seen + count; s++) {
        if (strcmp(s->call, wanted) != 0) continue;
        *found = *s;
        return true;
    }
    return false;
}

static void p3_snr(char *dst, size_t size, int db) {
    snprintf(dst, size, "%+03d", db);
}

/* Sent: our report to them, or failing that how we hear them. Rcvd: their
 * report on us, from a message or a heartbeat ack. Unknown ones are left
 * as they are. */
static void p3_log_reports(const js8_qso_t *q, const js8_station_t *st) {
    const size_t n = sizeof(log_entry.rst_sent), m = sizeof(log_entry.rst_rcvd);
    if (q && q->has_sent_snr) p3_snr(log_entry.rst_sent, n, q->sent_snr);
    else if (q && q->has_heard_snr) p3_snr(log_entry.rst_sent, n, q->heard_snr);
    else if (st) p3_snr(log_entry.rst_sent, n, st->snr);
    if (q && q->has_rcvd_snr) p3_snr(log_entry.rst_rcvd, m, q->rcvd_snr);
    else if (st && st->has_reported_snr) p3_snr(log_entry.rst_rcvd, m, st->reported_snr);
}

/* A fresh entry for `call` from what the QSO and the station list know. */
static void p3_log_fill(const char *call) {
    int64_t       t_now   = now_wall_ms();
    js8_qso_t     qso;
    js8_station_t heard;
    bool          in_qso  = js8_qsos_get(qsos, call, t_now, &qso);
    bool          in_list = find_station(call, &heard);
    const char   *grid    = "";
    float         pwr_w;

    memset(&log_entry, 0, sizeof log_entry);
    log_grid_typed = false; /* a grid heard later may still fill it in */
    if (in_qso && qso.grid[0]) grid = qso.grid;
    else if (in_list) grid = heard.grid;
    snprintf(log_entry.call, sizeof(log_entry.call), "%s", in_qso ? qso.call : call);
    snprintf(log_entry.grid, sizeof(log_entry.grid), "%s", grid);
    p3_log_reports(in_qso ? &qso : NULL, in_list ? &heard : NULL);
    log_entry.on_ms   = in_qso ? qso.start_ms : t_now;
    log_entry.off_ms  = t_now;
    log_entry.freq_hz = (uint64_t)cparam_i_get(cfg_fg_freq) + params.js8_tx_freq.x; /* dial + audio offset */
    snprintf(log_entry.my_call, sizeof log_entry.my_call, "%s", params.callsign.x);
    snprintf(log_entry.my_grid, sizeof(log_entry.my_grid), "%s", station_qth());
    pwr_w              = param_f_get(cfg_pwr);
    log_entry.tx_pwr_w = pwr_w > TX_PLAYER_MAX_PWR_W ? TX_PLAYER_MAX_PWR_W : pwr_w;
    if (params.js8_log_activation.x == 1)
        snprintf(log_entry.pota_ref, sizeof(log_entry.pota_ref), "%s", last_pota); /* MY_SIG POTA */
    else if (params.js8_log_activation.x == 2)
        snprintf(log_entry.sota_ref, sizeof(log_entry.sota_ref), "%s", last_sota); /* MY_SOTA_REF */
}

static void log_close(void) { p3_popup_shut(&log_list); }

static void p3_log_save(void) {
    char why[96];
    if (log_entry.call[0] == '\0' || log_entry.my_call[0] == '\0') {
        p3_say("Nothing to log");
        return;
    }
    log_entry.off_ms = now_wall_ms(); /* the QSO ends when it's saved */
    if (!js8_log_append(JS8_LOG_PATH, &log_entry, why, sizeof(why))) {
        msg_update_text_fmt("Not logged: %s", why);
        return;
    }
    /* Also in the radio's QSO database, for the worked-before marks here
     * and in FT8, and for Field Day. */
    char            *call = util_canonize_callsign(log_entry.call, false);
    qso_log_record_t rec  = qso_log_record_create(log_entry.my_call, call ? call : log_entry.call,
                                                  (time_t)(log_entry.on_ms / 1000), MODE_JS8, atoi(log_entry.rst_sent),
                                                  atoi(log_entry.rst_rcvd), log_entry.freq_hz,
                                                  log_entry.name[0] ? log_entry.name : NULL, NULL, log_entry.my_grid,
                                                  log_entry.grid);
    free(call);
    qso_log_record_save(rec); /* the radio's own log */
    dialog_field_day_log(&rec);

    js8_qsos_logged(qsos, log_entry.call); /* no second Log prompt for it */
    if (!strcmp(log_pending, log_entry.call)) log_pending[0] = '\0';
    const char *band_name = js8_log_band(log_entry.freq_hz);
    const char *ref       = log_entry.pota_ref[0] ? log_entry.pota_ref : log_entry.sota_ref;
    const char *sent      = log_entry.rst_sent[0] ? log_entry.rst_sent : "-";
    const char *rcvd      = log_entry.rst_rcvd[0] ? log_entry.rst_rcvd : "-";
    msg_update_text_fmt("Logged %s%s%s", log_entry.call, band_name[0] ? " on " : "", band_name);
    add_info_row("Logged %s %s %s/%s%s%s", log_entry.call, band_name, sent, rcvd, log_entry.pota_ref[0] ? " POTA " : "",
                 ref);
    if (view_stations) rebuild_station_rows(); /* the worked mark */
}

static void log_item_cb(lv_event_t *ev) {
    int item = (int)(intptr_t)lv_event_get_user_data(ev);
    if (item == LOG_SAVE) {
        p3_log_save();
        p3_popup_shut(&log_list);
        return;
    }
    if (item == LOG_CANCEL) {
        log_pending[0] = '\0'; /* no more offers for that QSO */
        p3_popup_shut(&log_list);
        return;
    }
    /* A field: into the keyboard; log_edit_done reopens the popup. */
    p3_popup_drop(&log_list);
    if (item == LOG_GRID) {
        edit_target = EDIT_LOG_GRID;
        p3_keyboard(log_entry.grid);
    } else if (item == LOG_NAME) {
        edit_target = EDIT_LOG_NAME;
        p3_keyboard(log_entry.name);
    } else {
        edit_target = EDIT_LOG_NOTE;
        p3_keyboard(log_entry.comment);
    }
}

static void log_key_cb(lv_event_t *ev) {
    if (p3_popup_key(ev)) p3_popup_shut(&log_list);
}

static lv_obj_t *p3_log_item(log_item_t item, const char *text) {
    return p3_popup_item(log_list, text, log_item_cb, log_key_cb, item);
}

static void p3_log_reports_text(char *text, size_t room) {
    const char *sent = log_entry.rst_sent[0] ? log_entry.rst_sent : "-";
    const char *rcvd = log_entry.rst_rcvd[0] ? log_entry.rst_rcvd : "-";
    snprintf(text, room, "Sent %s  Rcvd %s  %.3f MHz  %.0f W", sent, rcvd, log_entry.freq_hz / 1e6, log_entry.tx_pwr_w);
}

/* Something came from them while the popup is open, often their 73 after
 * ours opened it: a new report or grid goes in, the focus stays. */
static void log_refresh(void) { /* LVGL thread, after each message heard */
    js8_qso_t qso;
    char      text[160];
    if (log_list == NULL || !js8_qsos_get(qsos, log_entry.call, now_wall_ms(), &qso)) return;
    p3_log_reports(&qso, NULL);
    p3_log_reports_text(text, sizeof(text));
    lv_label_set_text(log_reports, text);
    if (log_grid_typed || !qso.grid[0] || !strcmp(qso.grid, log_entry.grid)) return;
    snprintf(log_entry.grid, sizeof(log_entry.grid), "%s", qso.grid);
    snprintf(text, sizeof(text), "Grid: %s", log_entry.grid);
    lv_label_set_text(lv_obj_get_child(log_grid_btn, 0), text);
}

/* "HH:MM" UTC of a wall clock time in ms. */
static void p3_hhmm(int64_t ms, char out[8]) {
    time_t    secs = (time_t)(ms / 1000);
    struct tm utc;
    strftime(out, 8, "%H:%M", gmtime_r(&secs, &utc));
}

/* The popup, the knob on `focus`: Save, or the field just typed. */
static void log_list_open(log_item_t start) {
    char      text[160], t_on[8], t_off[8];
    lv_obj_t *item[LOG_CANCEL + 1];

    log_list = p3_popup_new(560);
    p3_hhmm(log_entry.on_ms, t_on);
    p3_hhmm(log_entry.off_ms, t_off);
    snprintf(text, sizeof(text), "Log %s  %s  %s-%s UTC", log_entry.call, js8_log_band(log_entry.freq_hz), t_on, t_off);
    p3_popup_note(log_list, text);
    p3_log_reports_text(text, sizeof(text));
    log_reports = p3_popup_note(log_list, text);

    item[LOG_SAVE] = p3_log_item(LOG_SAVE, "Save to log");
    lv_obj_set_style_text_color(item[LOG_SAVE], TC(ok), 0);
    snprintf(text, sizeof(text), "Grid: %s", log_entry.grid[0] ? log_entry.grid : "(none)");
    item[LOG_GRID] = log_grid_btn = p3_log_item(LOG_GRID, text);
    snprintf(text, sizeof(text), "Name: %s", log_entry.name[0] ? log_entry.name : "(none)");
    item[LOG_NAME] = p3_log_item(LOG_NAME, text);
    snprintf(text, sizeof(text), "Comment: %s", log_entry.comment[0] ? log_entry.comment : "(none)");
    item[LOG_NOTE] = p3_log_item(LOG_NOTE, text);
    if (log_entry.pota_ref[0]) {
        snprintf(text, sizeof(text), "Activating POTA %s", log_entry.pota_ref);
        p3_popup_note(log_list, text);
    } else if (log_entry.sota_ref[0]) {
        snprintf(text, sizeof(text), "Activating SOTA %s", log_entry.sota_ref);
        p3_popup_note(log_list, text);
    }
    item[LOG_CANCEL] = p3_log_item(LOG_CANCEL, "Cancel");
    lv_obj_set_style_text_color(item[LOG_CANCEL], TC(action), 0);
    p3_popup_focus(item[start]);
}

/* Back from the keyboard for any edit from EDIT_LOG_GRID on: store the
 * value (NULL: cancelled) and go back where the edit came from. */
static void log_edit_done(const char *typed) {
    /* A copy: closing the keyboard frees `typed`. */
    char        copy[sizeof(alert_words)];
    const char *value = NULL;
    int         what  = edit_target;
    if (typed) {
        copy[sizeof(copy) - 1] = '\0';
        value                  = strncpy(copy, typed, sizeof(copy) - 1);
    }
    edit_target = 0; /* the keyboard is no longer for a field */
    compose_close(); /* frees `typed` */

    if (what == EDIT_FREQ) { /* the dial goes there; cancelled, nothing changes */
        if (value) tune_custom(value);
    } else if (what == EDIT_BEACON_GRID || what == EDIT_BEACON_GPS) {
        if (value) aprs_beacon(what == EDIT_BEACON_GPS, value);
        else p3_say("Position not sent");
    } else if (what == EDIT_POTA_REF || what == EDIT_SOTA_REF) {
        if (value) p3_remember_ref(what == EDIT_SOTA_REF, value);
        if (btn_act.disp_btn) buttons_refresh(&btn_act); /* it shows the reference */
    } else if (what == EDIT_ALERT_WORDS) {
        if (value) {
            js8_alert_words_normalise(value, alert_words, sizeof alert_words);
            save_texts(); /* ALERTS= */
            if (view_stations) rebuild_station_rows(); /* rows matching a word turn purple */
        }
        alerts_show(); /* back to the list */
    } else if (what == EDIT_SPOT_REF || what == EDIT_SPOT_FREQ || what == EDIT_SPOT_NOTE) {
        if (value && what == EDIT_SPOT_REF) {
            p3_remember_ref(spot_sota, value);
            if (btn_act.disp_btn) buttons_refresh(&btn_act); /* it shows the reference */
        } else if (value && what == EDIT_SPOT_FREQ) {
            /* A frequency is spotted from now on (and kept); empty: the JS8 dial. */
            spot_use_typed = spot_freq_empty(value) ? false : spot_parse_freq(value, &spot_typed_hz);
            save_texts(); /* SPOTHZ=, SPOTTYPED= */
        } else if (value) {
            snprintf(spot_note, sizeof spot_note, "%s", value);
            save_texts(); /* SPOTNOTE= */
        }
        spot_show(spot_sota, what == EDIT_SPOT_REF ? SP_REF : what == EDIT_SPOT_FREQ ? SP_FREQ : SP_NOTE);
    } else {
        /* A field of the Log popup: back on it; saving stays a step of its own. */
        log_item_t back = LOG_NOTE;
        if (what == EDIT_LOG_GRID) {
            back = LOG_GRID;
            if (value) {
                snprintf(log_entry.grid, sizeof log_entry.grid, "%s", value);
                log_grid_typed = true; /* a grid heard later doesn't replace it */
            }
        } else if (what == EDIT_LOG_NAME) {
            back = LOG_NAME;
            if (value) snprintf(log_entry.name, sizeof(log_entry.name), "%s", value);
        } else if (value) {
            snprintf(log_entry.comment, sizeof log_entry.comment, "%s", value);
        }
        log_list_open(back);
    }
}

static void log_cb(button_data_t *b) {
    char  who[JS8_RX_CALL_LEN];
    float audio_hz;
    LV_UNUSED(b);
    if (p3_toggle_press(log_list, log_close) || !p3_have_callsign()) return;
    /* The QSO that just ended, else the station selected. */
    if (log_pending[0] != '\0') {
        snprintf(who, sizeof(who), "%s", log_pending);
    } else if (!p3_selected(who, &audio_hz)) {
        p3_say("Select a station first (MFK)");
        return;
    }
    p3_log_fill(who);
    log_list_open(LOG_SAVE); /* the knob on Save */
}

/* A two-way QSO has ended with 73 or SK. */
static void log_offer(const char *with) {
    bool busy = composing || any_popup();
    snprintf(log_pending, sizeof(log_pending), "%s", with); /* Log QSO takes it from here */
    if (params.js8_log_prompt.x && !busy) {
        p3_log_fill(with);
        log_list_open(LOG_SAVE); /* the knob on Save */
        msg_update_text_fmt("QSO with %s ended - Save to log?", with);
        return;
    }
    if (params.js8_log_prompt.x) msg_update_text_fmt("QSO with %s ended: Settings... (page 3), Log QSO, to log it", with);
    add_info_row("QSO with %s ended: Settings... (page 3), Log QSO, to log it", with);
}

static const char *prompt_label_getter(void) { return params.js8_log_prompt.x ? "Log\nprompt: On" : "Log\nprompt: Off"; }

static void prompt_cb(button_data_t *b) {
    if (p3_press_closes_popup()) return;
    bool on = !params.js8_log_prompt.x;
    params_bool_set(&params.js8_log_prompt, on);
    buttons_refresh(b);
    p3_say(on ? "Offer to log when a QSO ends with 73 or SK" : "No log prompt: use Log QSO");
}

/* Activating a park or summit puts MY_SIG / MY_SOTA_REF in the log. The
 * reference is the last one spotted through APRS, or set by holding the
 * button. */
static const char *act_label_getter(void) { /* "POTA\nCA-1234", "SOTA\n...", or off */
    static char text[40];
    uint8_t     mode = params.js8_log_activation.x;
    if (mode == 1) snprintf(text, sizeof(text), "POTA\n%s", last_pota[0] ? last_pota : "(hold: set)");
    else if (mode == 2) snprintf(text, sizeof(text), "SOTA\n%s", last_sota[0] ? last_sota : "(hold: set)");
    else snprintf(text, sizeof(text), "Activ.:\nOff");
    return text;
}

static void act_cb(button_data_t *b) {
    if (p3_press_closes_popup()) return;
    uint8_t mode = (uint8_t)((params.js8_log_activation.x + 1) % 3); /* off, POTA, SOTA */
    params_uint8_set(&params.js8_log_activation, mode);
    buttons_refresh(b);
    if (mode == 0) {
        p3_say("Not activating: no park or summit in the log");
        return;
    }
    const char *ref = mode == 1 ? last_pota : last_sota;
    if (ref[0]) msg_update_text_fmt("Logging as %s %s (hold to change)", mode == 1 ? "POTA" : "SOTA", ref);
    else msg_update_text_fmt("Hold this button to set your %s", mode == 1 ? _("park") : _("summit"));
}

static void act_hold_cb(button_data_t *b) {
    LV_UNUSED(b);
    if (p3_press_closes_popup() || composing) return;
    uint8_t mode = params.js8_log_activation.x;
    if (mode == 0) {
        p3_say("Press to choose POTA or SOTA first");
        return;
    }
    edit_target = mode == 1 ? EDIT_POTA_REF : EDIT_SOTA_REF;
    p3_keyboard(mode == 1 ? last_pota : last_sota);
}

/* ---- Inbox: messages for us, and the ones held for others --------------- */

/* As in desktop JS8Call: a "CALL MSG text" for us with a good checksum is
 * kept in the inbox and answered "CALL ACK" (by AUTO, otherwise offered on
 * Reply). From the Query list we send them: MSG for their inbox, MSG TO:
 * to leave one at their station for someone else, QUERY MSGS to ask what
 * they hold. */

#define INBOX_ROWS 50     /* messages listed, of each kind */
#define INBOX_HELD 100000 /* item ids from here up are held messages: id - INBOX_HELD */

/* Item ids of the inbox popup's buttons; zero and up are messages. */
enum inbox_choice {
    IB_NEW        = -1,
    IB_CLOSE      = -2,
    IB_BACK       = -3,
    IB_REPLY      = -4, /* desktop's "Reply to <path>": PATH MSG ... */
    IB_DELETE     = -5,
    IB_REPLY_VIA  = -6, /* "Reply to <sender> via <path>": PATH MSG TO:SENDER ... */
    IB_REPLY_ORIG = -7, /* "Reply to <sender>": SENDER MSG ... */
    IB_FETCH_NEXT = -8, /* PATH QUERY MSG n, for the NEXT MSG ID n it ended with */
    IB_REPLY_APRS = -9, /* passed on by an APRS gateway: an APRS message back to the DE call */
};

static int inbox_open_id; /* the message on show; 0: the list */

/* The button turns green, like a selected Settings tab, while something
 * is unread. */
static void inbox_refresh_button(void) { buttons_mark(&btn_inbox, inbox && js8_inbox_unread(inbox) > 0); }

static const char *inbox_label_getter(void) { /* "Inbox", or "Inbox\n2 new" */
    static char text[24];
    int         fresh = js8_inbox_unread(inbox);
    if (fresh == 0) return strcpy(text, "Inbox");
    snprintf(text, sizeof(text), "Inbox\n%d new", fresh);
    return text;
}

/* ---- "Message from ...": a message for us in the Inbox, shown at once. The MFK's press opens it there (Reply
 * there), ESC or any button leaves it in the Inbox. Not over the keyboard or a list: the status line and the
 * Inbox button say it then. */

static int     alert_id;

static void alert_close(void)
{
    if (!alert_box) return;
    lv_group_remove_obj(alert_box);
    lv_obj_del_async(alert_box);
    alert_box = NULL;
    if (table) table_takes_keys();
}

static void alert_key_cb(lv_event_t *ev)
{
    uint32_t key = *(uint32_t *)lv_event_get_param(ev);
    if (key == LV_KEY_ESC) alert_close();
    else volume_key(key);
}

static void alert_click_cb(lv_event_t *ev)
{
    LV_UNUSED(ev);
    if (now_mono_ms() - alert_opened_ms < 400) return; /* the press that came before it */
    int id = alert_id;
    alert_close();
    if (inbox && !any_popup() && !composing) p3_inbox_view(id); /* the message, Reply under it */
}

static void alert_show(const js8_stored_t *s, const char *by)
{
    if (alert_box || any_popup() || composing || !dialog.obj) return;

    lv_obj_t *b = alert_box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 620, 180);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(b, TC(popup), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, TC(warn), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 3, LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    const where_t *w = station_where(s->from);
    lv_coord_t     x = 20;
    if (w && table_view_flag(w->flag)) {
        lv_obj_t *f = lv_img_create(b);
        lv_img_set_src(f, table_view_flag(w->flag));
        lv_img_set_zoom(f, 512);
        lv_obj_set_pos(f, x + 8, 22);
        x += 44;
    }
    lv_obj_t *head = lv_label_create(b);
    if (s->to[0] == '@') lv_label_set_text_fmt(head, "Message to %s from %s", s->to, by);
    else lv_label_set_text_fmt(head, "Message from %s", by);
    lv_obj_set_style_text_font(head, &sony_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(head, TC(warn), LV_PART_MAIN);
    lv_obj_set_pos(head, x, 12);

    lv_obj_t *text = lv_label_create(b);
    lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
    lv_obj_set_size(text, 580, 56);
    lv_label_set_text(text, s->text);
    lv_obj_set_style_text_font(text, &sony_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(text, TC(text), LV_PART_MAIN);
    lv_obj_set_pos(text, 20, 54);

    char where[64] = "";
    if (w && w->country[0]) snprintf(where, sizeof(where), "%s", w->country);
    if (w && w->km >= 0)
        snprintf(where + strlen(where), sizeof(where) - strlen(where), "%s%s%d km", where[0] ? ", " : "",
                 w->approx ? "~" : "", w->km);
    lv_obj_t *info = lv_label_create(b);
    lv_label_set_text(info, where);
    lv_obj_set_style_text_font(info, &sony_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(info, TC(text_mute), LV_PART_MAIN);
    lv_obj_set_pos(info, 20, 112);

    lv_obj_t *hint = lv_label_create(b);
    lv_label_set_text(hint, "MFK press: read it and reply      ESC or a button: later (Inbox)");
    lv_obj_set_style_text_font(hint, &sony_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, TC(text_mute), LV_PART_MAIN);
    lv_obj_set_pos(hint, 20, 142);

    alert_id        = s->id;
    alert_opened_ms = now_mono_ms();
    lv_obj_add_event_cb(b, alert_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(b, alert_click_cb, LV_EVENT_CLICKED, NULL);
    keys_leave_main();
    lv_group_add_obj(keyboard_group, b);
    lv_group_focus_obj(b);
    lv_group_set_editing(keyboard_group, true);
}

/* js8_process() stored something: a MSG for us or one of our groups (the
 * inbox), or a "MSG TO:W1ABC ..." held until W1ABC asks for it (QUERY
 * MSGS, QUERY MSG n, the "MSG ID n" of our heartbeat ack, or our RETRIEVE
 * MSG), as desktop does. */
static void stored_received(const js8_stored_t *s) {
    char by[JS8_PATH_LEN + 16];
    js8_path_display(s->path[0] ? s->path : s->from, by, sizeof by);
    if (s->kind == JS8_STORED_HELD) {
        if (s->resend) return;
        if (s->id < 0) msg_update_text_fmt("Can't save %s", JS8_HELD_PATH);
        else msg_update_text_fmt("Holding a message from %s for %s (Inbox)", by, s->to);
        add_info_row("Holding message %d from %s for %s: %s", s->id, by, s->to, s->text);
        return;
    }
    if (s->kind != JS8_STORED_INBOX) return;
    if (s->id < 0) msg_update_text_fmt("Message from %s - can't save %s", by, JS8_INBOX_PATH);
    if (!s->resend) {
        if (s->id >= 0 && s->to[0] == '@')
            msg_update_text_fmt("New message to %s from %s - Inbox on page 1", s->to, by);
        else if (s->id >= 0)
            msg_update_text_fmt("New message from %s - Inbox on page 1", by);
        add_info_row("Message from %s in the Inbox: %s", by, s->text);
        if (params.js8_alerts.x & JS8_ALERT_INBOX) alert_beep(2);
        if (s->id >= 0) alert_show(s, by);
    }
    inbox_refresh_button(); /* green while unread */
    update_status();        /* the unread count */
}

/* `s` without its leading and trailing spaces: the start, and the length
 * in `len`. */
static const char *p3_trim(const char *s, size_t *len) {
    s        = p3_skip_spaces(s);
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') n--;
    *len = n;
    return s;
}

/* The same message as the transmitter reports it, which is in upper case
 * and trimmed. */
static bool p3_same_sent(const char *x, const char *y) {
    size_t nx, ny;
    x = p3_trim(x, &nx);
    y = p3_trim(y, &ny);
    return nx == ny && strncasecmp(x, y, nx) == 0;
}

/* A held message went into the queue: delivered once all of it is sent. */
static void deliver_start(int held_id, const char *group, const char *sent) {
    snprintf(deliver_tx.text, sizeof deliver_tx.text, "%s", sent);
    snprintf(deliver_tx.group_call, sizeof deliver_tx.group_call, "%s", group ? group : "");
    deliver_tx.id = held_id;
}

/* A transmission ended. When it was the held message: sent in full, it's
 * delivered; stopped halfway, it stays held and is offered again at their
 * next QUERY MSGS or heartbeat. */
static void deliver_end(const char *sent, bool whole) {
    int id = deliver_tx.id;
    if (id == 0 || !p3_same_sent(sent, deliver_tx.text)) return;
    deliver_tx.id = 0; /* nothing on its way any more */
    if (!whole) {
        add_info_row("Held message %d stopped before the end: still held", id);
        return;
    }
    if (deliver_tx.group_call[0]) js8_held_group_delivered(held, id, deliver_tx.group_call);
    else js8_held_delivered(held, id);
    add_info_row("Held message %d delivered", id);
}

static void inbox_close(void) { p3_popup_shut(&inbox_list); }

/* "CALL MSG " or "CALL MSG TO:" (`kind`) in the keyboard. `call` can be a
 * relay path, "K2XYZ>N0XYZ". */
static void msg_compose(const char *to, const char *kind) {
    char text[JS8_PATH_LEN + 12];
    bool leave = strcmp(kind, "MSG TO:") == 0;
    snprintf(text, sizeof(text), "%s %s", to, kind);
    p3_keyboard(text);
    if (leave) msg_update_text_fmt("Left at %s for someone: type their call, a space, the message", to);
    else msg_update_text_fmt("Message for %s's inbox: type it and press Enter", to);
}

static void p3_inbox_view(int id);

/* Out of the popup's buttons, then onto view `id` (0: the list). */
static void p3_inbox_goto(int id) {
    p3_popup_drop(&inbox_list);
    p3_inbox_view(id);
}

static void p3_inbox_key_cb(lv_event_t *ev) {
    if (!p3_popup_key(ev)) return;
    if (inbox_open_id) p3_inbox_goto(0); /* ESC in a message: back to the list */
    else p3_popup_shut(&inbox_list);
}

/* Who sent a message an APRS gateway passed on, from its last " DE <call>",
 * which must end the text. */
static bool p3_aprs_sender(const char *text, char *call, size_t room) {
    const char *last = NULL;
    for (const char *p = text; (p = strstr(p, " DE ")) != NULL; p++) last = p;
    if (last == NULL) return false;
    const char *word = p3_skip_spaces(last + 4);
    size_t      len  = strcspn(word, " ");
    if (len == 0 || len >= room || *p3_skip_spaces(word + len) != '\0') return false;
    snprintf(call, room, "%.*s", (int)len, word);
    return true;
}

/* Reply, Reply via, Fetch next or Reply by APRS on the message shown. */
static void p3_inbox_answer(int action) {
    js8_inbox_msg_t m;
    if (!js8_inbox_get(inbox, inbox_open_id, &m)) return;
    const char *path = m.path[0] ? m.path : m.from;
    char        sender[JS8_RX_CALL_LEN] = "", aprs_to[JS8_RX_CALL_LEN], text[JS8_PATH_LEN + JS8_RX_CALL_LEN + 24];
    int         next                    = 0;
    js8_delivered_signature(m.text, sender, sizeof(sender), &next);
    if (action == IB_REPLY_APRS && !p3_aprs_sender(m.text, aprs_to, sizeof(aprs_to))) return;

    if (action == IB_FETCH_NEXT) { /* goes at once; back to the message list */
        snprintf(text, sizeof(text), "%s QUERY MSG %d", path, next);
        p3_popup_shut(&inbox_list);
        tx_queue(text); /* says why if it can't */
        return;
    }
    p3_popup_drop(&inbox_list);
    if (action == IB_REPLY) {
        msg_compose(path, "MSG ");
    } else if (action == IB_REPLY_ORIG) {
        msg_compose(sender, "MSG ");
    } else if (action == IB_REPLY_VIA) {
        snprintf(text, sizeof(text), "%s MSG TO:%s ", path, sender);
        p3_keyboard(text);
        msg_update_text_fmt("Left at %s for %s: type the message and press Enter", path, sender);
    } else {
        snprintf(text, sizeof(text), APRS_CMD "%-9.9s:", aprs_to);
        aprs_compose(text);
        lv_group_set_editing(keyboard_group, true); /* the knob types */
        msg_update_text_fmt("APRS message to %s: type it and press Enter", aprs_to);
    }
}

/* New message: to the station selected, else from scratch. */
static void p3_inbox_new(void) {
    char  to[JS8_RX_CALL_LEN];
    float audio_hz;
    p3_popup_drop(&inbox_list);
    if (p3_selected(to, &audio_hz)) {
        apply_hold(audio_hz); /* answer on their frequency, as Hold does */
        msg_compose(to, "MSG ");
        return;
    }
    p3_keyboard(NULL);
    p3_say("Type their call, then MSG and the message");
}

static void p3_inbox_item_cb(lv_event_t *ev) {
    int action = (int)(intptr_t)lv_event_get_user_data(ev);
    int shown  = inbox_open_id;
    if (action >= 0) { /* a message, or a held one */
        p3_inbox_goto(action);
    } else if (action == IB_CLOSE) {
        p3_popup_shut(&inbox_list);
    } else if (action == IB_BACK) {
        p3_inbox_goto(0);
    } else if (action == IB_DELETE) {
        if (shown >= INBOX_HELD) js8_held_delete(held, shown - INBOX_HELD);
        else js8_inbox_delete(inbox, shown);
        p3_say("Message deleted");
        p3_inbox_goto(0);
        inbox_refresh_button(); /* green while unread */
    } else if (action == IB_NEW) {
        p3_inbox_new();
    } else {
        p3_inbox_answer(action);
    }
}

static lv_obj_t *p3_inbox_item(const char *text, int action) {
    lv_obj_t *item = p3_popup_item(inbox_list, text, p3_inbox_item_cb, p3_inbox_key_cb, action);
    p3_popup_dots(item);
    return item;
}

static void p3_utc(int64_t ms, const char *fmt, char *out, size_t room) {
    time_t    secs = (time_t)(ms / 1000);
    struct tm utc;
    strftime(out, room, fmt, gmtime_r(&secs, &utc));
}

/* "Back", in the colour of the popups' way-out items. */
static void p3_inbox_back(void) {
    lv_obj_set_style_text_color(p3_inbox_item("Back", IB_BACK), TC(action), 0);
}

/* A message held here for another station, or NULL if it's gone. */
static lv_obj_t *p3_inbox_held_view(int id) {
    js8_held_msg_t m;
    char           text[JS8_RX_TEXT_LEN + 48], when[32], by[JS8_PATH_LEN + 16];
    if (!js8_held_get(held, id, &m)) return NULL;
    p3_utc(m.utc_ms, "%d %b %H:%M UTC", when, sizeof(when));
    js8_path_display(m.path, by, sizeof by);
    snprintf(text, sizeof(text), "For %s from %s  %s", m.to, by, when);
    p3_popup_note(inbox_list, text);
    p3_popup_body(inbox_list, m.text);
    if (m.group) snprintf(text, sizeof(text), "For any %s member to fetch for 2 days; fetched by %d", m.to, m.got);
    else if (m.delivered) snprintf(text, sizeof(text), "Delivered to %s", m.to);
    else snprintf(text, sizeof(text), "Waiting for %s to ask (QUERY MSGS)", m.to);
    p3_popup_note(inbox_list, text);
    lv_obj_t *del = p3_inbox_item("Delete", IB_DELETE);
    p3_inbox_back();
    return del;
}

/* One inbox message, now read, with desktop's Reply choices: to the path
 * it came by; and for a delivered one ("... FROM N0XYZ"), to its sender
 * through the station that held it (MSG TO:) or direct. NULL if it's gone. */
static lv_obj_t *p3_inbox_msg_view(int id) {
    js8_inbox_msg_t m;
    char            text[JS8_RX_TEXT_LEN + 48], when[32], by[JS8_PATH_LEN + 16];
    char            sender[JS8_RX_CALL_LEN] = "", aprs_from[JS8_RX_CALL_LEN];
    int             next                    = 0;
    lv_obj_t       *top                     = NULL;
    if (!js8_inbox_get(inbox, id, &m)) return NULL;
    js8_inbox_mark_read(inbox, id);
    inbox_refresh_button(); /* green while unread */
    update_status();        /* the unread count */

    const char *path = m.path[0] ? m.path : m.from;
    p3_utc(m.utc_ms, "%d %b %H:%M UTC", when, sizeof(when));
    js8_path_display(path, by, sizeof by);
    if (m.to[0] == '@') snprintf(text, sizeof(text), "From %s to %s  %s", by, m.to, when);
    else snprintf(text, sizeof(text), "From %s  %s", by, when);
    p3_popup_note(inbox_list, text);
    p3_popup_body(inbox_list, m.text);

    bool is_signed = js8_delivered_signature(m.text, sender, sizeof(sender), &next) && sender[0] != '@';
    if (strcmp(path, "APRS") == 0) {
        if (p3_aprs_sender(m.text, aprs_from, sizeof(aprs_from))) {
            snprintf(text, sizeof(text), "Reply by APRS to %s", aprs_from);
            top = p3_inbox_item(text, IB_REPLY_APRS);
        }
    } else {
        snprintf(text, sizeof(text), "Reply: MSG to %s", by);
        top = p3_inbox_item(text, IB_REPLY);
        if (is_signed && strcmp(sender, path) != 0) {
            snprintf(text, sizeof(text), "Reply to %s via %s (MSG TO:)", sender, by);
            p3_inbox_item(text, IB_REPLY_VIA);
            snprintf(text, sizeof(text), "Reply to %s", sender);
            p3_inbox_item(text, IB_REPLY_ORIG);
        }
        if (next > 0) {
            snprintf(text, sizeof(text), "Fetch the next: QUERY MSG %d", next);
            p3_inbox_item(text, IB_FETCH_NEXT);
        }
    }
    lv_obj_t *del = p3_inbox_item("Delete", IB_DELETE);
    p3_inbox_back();
    return top ? top : del;
}

/* The list: New message, the inbox (newest last), then what's held here
 * for others. The knob starts on the oldest unread message, if any. */
static lv_obj_t *p3_inbox_list_view(void) {
    static js8_inbox_msg_t mine[INBOX_ROWS];
    static js8_held_msg_t  theirs[INBOX_ROWS];
    char                   text[JS8_RX_TEXT_LEN + 48], by[JS8_PATH_LEN + 16];
    char                   to[JS8_RX_CALL_LEN];
    float                  audio_hz;
    int                    total;
    lv_obj_t              *top, *unread = NULL;

    int n = js8_inbox_list(inbox, mine, INBOX_ROWS);
    total = js8_inbox_count(inbox);
    snprintf(text, sizeof(text), _("Inbox: %d message%s, %d new"), total, total == 1 ? "" : "s", js8_inbox_unread(inbox));
    p3_popup_note(inbox_list, text);

    if (p3_selected(to, &audio_hz)) snprintf(text, sizeof(text), "New message to %s...", to);
    else snprintf(text, sizeof(text), "New message...");
    top = p3_inbox_item(text, IB_NEW);
    lv_obj_set_style_text_color(top, TC(ok), 0);

    for (const js8_inbox_msg_t *r = mine; r < mine + n; r++) {
        char when[16];
        p3_utc(r->utc_ms, "%d %b %H:%M", when, sizeof(when));
        js8_path_display(r->path[0] ? r->path : r->from, by, sizeof by);
        snprintf(text, sizeof(text), "%s%s  %s  %s", r->read ? "" : "* ", when, by, r->text);
        lv_obj_t *item = p3_inbox_item(text, r->id);
        if (!r->read) {
            lv_obj_set_style_text_color(item, TC(accent_soft), 0);
            unread = item;
        }
    }
    if (n == 0) p3_popup_note(inbox_list, "No messages yet");

    /* Held here for others (MSG TO:) until they ask. */
    int nh = js8_held_list(held, theirs, INBOX_ROWS);
    if (nh > 0) {
        snprintf(text, sizeof(text), "Held for others: %d waiting", js8_held_waiting(held));
        p3_popup_note(inbox_list, text);
    }
    for (const js8_held_msg_t *h = theirs; h < theirs + nh; h++) {
        char got[16] = "";
        js8_path_display(h->path, by, sizeof by);
        if (h->group) snprintf(got, sizeof(got), " (%d got it)", h->got);
        snprintf(text, sizeof(text), "%sfor %s%s from %s  %s", h->delivered ? "(sent) " : "", h->to, got, by, h->text);
        lv_obj_t *item = p3_inbox_item(text, INBOX_HELD + h->id);
        if (h->delivered) lv_obj_set_style_text_color(item, TC(text_mute), 0);
    }
    lv_obj_set_style_text_color(p3_inbox_item("Close", IB_CLOSE), TC(action), 0);
    return unread ? unread : top;
}

/* The popup on the list (id 0), an inbox message or a held one. A message
 * that has gone meanwhile shows the list instead. */
static void p3_inbox_view(int id) {
    lv_obj_t *top;
    inbox_open_id = id;
    inbox_list    = p3_popup_new(600);
    if (id == 0) top = p3_inbox_list_view();
    else if (id >= INBOX_HELD) top = p3_inbox_held_view(id - INBOX_HELD);
    else top = p3_inbox_msg_view(id);
    if (top == NULL) {
        inbox_open_id = 0;
        lv_obj_del(inbox_list); /* nothing in it is running */
        p3_inbox_view(0);
        return;
    }
    p3_popup_focus(top);
}

static void inbox_cb(button_data_t *b) {
    LV_UNUSED(b);
    if (p3_toggle_press(inbox_list, inbox_close) || inbox == NULL) return;
    p3_inbox_view(0);
}

/* ---- Alert sounds and words --------------------------------------------- */

/* Desktop JS8Call's notifications and highlight words: a beep on the
 * speaker for whatever the Alerts list has on, and the rows that match an
 * alert word (calls or words you type) in purple. Never while sending:
 * the speaker path carries the TX audio then.
 *
 * The base unit puts this app's audio on the speaker only in its play
 * mode (radio_speaker_play, as voice prompts and the recorder use); out
 * of it the beep went nowhere. While in play mode the audio the base
 * sends us is not the receiver's, so the beep is short and beep_guard has
 * the decoder fed silence meanwhile. */

#define BEEP_HZ       1000  /* tone pitch */
#define BEEP_MS       120   /* each beep */
#define BEEP_GAP_MS   100   /* silence after each */
#define BEEP_LEAD_MS  50    /* silence first, while the base goes into play mode */
#define BEEP_TAIL_MS  300   /* guard after the speaker is off: capture latency */
#define BEEP_AMPL     13000 /* peak, about -8 dBFS */
#define BEEP_EVERY_MS 3000  /* no more than one alert sound in this time */

enum beep_samples {
    BEEP_TONE_N = AUDIO_PLAY_RATE * BEEP_MS / 1000,
    BEEP_GAP_N  = AUDIO_PLAY_RATE * BEEP_GAP_MS / 1000,
    BEEP_LEAD_N = AUDIO_PLAY_RATE * BEEP_LEAD_MS / 1000,
};

static int16_t     beep_wave[BEEP_TONE_N + BEEP_GAP_N]; /* one beep and the gap after it */
static int16_t     beep_silence[BEEP_LEAD_N];
static int         beep_times;   /* beeps the thread plays */
static atomic_bool beeping;      /* the beep thread is running */
static int64_t     beep_last_ms; /* wall clock of the last alert sound */

/* Feed `n` samples to audio_play() 2048 at a time: a bigger write never
 * finds room in the stream and hangs (tx_player does the same). Stops if
 * the transmitter keys. */
static void p3_beep_feed(const int16_t *pcm, size_t count) {
    size_t done = 0;
    while (done < count && !atomic_load(&keyed)) {
        size_t step = count - done < 2048 ? count - done : 2048;
        audio_play((int16_t *)pcm + done, step);
        done += step;
    }
}

/* audio_play() blocks until the stream has room, so the beep can't play on
 * the LVGL thread: it has this one. */
static void *p3_beep_thread(void *unused) {
    LV_UNUSED(unused);
    pthread_mutex_lock(&speaker_lock); /* one sound at a time on the speaker */
    if (!atomic_load(&keyed)) {
        /* Zero the capture level sums; the receiver's level just before is
         * in beep_level_before_db (for the log). */
        atomic_store(&beep_guard_sq, 0);
        atomic_store(&beep_guard_n, 0);
        atomic_store(&beep_guard, true);
        bool headphones = audio_play_bt_begin(); /* Bluetooth headphones instead of the speaker */
        if (!headphones) radio_speaker_play(true);
        p3_beep_feed(beep_silence, BEEP_LEAD_N);
        for (int k = 0; k < beep_times; k++) p3_beep_feed(beep_wave, BEEP_TONE_N + BEEP_GAP_N);
        audio_play_wait();
        if (headphones) audio_play_bt_end();
        else radio_speaker_play(false);
        atomic_store(&beep_guard_end, now_mono_ms() + BEEP_TAIL_MS);
        atomic_store(&beep_guard, false);
    }
    pthread_mutex_unlock(&speaker_lock); /* the TX player waits on it */
    atomic_store(&beeping, false);       /* the next beep may start */
    return NULL;
}

/* After the guard: the capture level while the speaker played, next to
 * the receiver's just before. It shows whether play mode really cuts the
 * receiver on this base. */
static void beep_log_level(void) { /* LVGL thread, once the guard is over */
    int64_t samples = atomic_load(&beep_guard_n);
    if (samples == 0 || beep_guard_active()) return;
    double mean_sq   = (double)atomic_load(&beep_guard_sq) / 1e9 / (double)samples;
    double during_db = 20.0 * log10(sqrt(mean_sq) + 1e-12);
    LV_LOG_USER("JS8 beep: capture %.1f dBFS during the beep (%lld samples), %.1f dBFS before", during_db,
                (long long)samples, beep_level_before_db);
    atomic_store(&beep_guard_n, 0); /* logged once */
}

/* An alert sound, if Beep is on and none played in the last BEEP_EVERY_MS. */
static void alert_beep(int times) {
    if ((params.js8_alerts.x & JS8_ALERT_BEEP) == 0) return;
    if (now_wall_ms() - beep_last_ms >= BEEP_EVERY_MS) beep_start(times);
}

/* The beep, made once: 5 ms fades at both ends so it doesn't click. */
static void p3_beep_make(void) {
    static bool ready;
    if (ready) return;
    const int fade = AUDIO_PLAY_RATE / 200;
    for (int i = 0; i < BEEP_TONE_N; i++) {
        float gain = 1.0f;
        if (i < fade) gain = (float)i / fade;
        if (i > BEEP_TONE_N - fade) gain = (float)(BEEP_TONE_N - i) / fade;
        beep_wave[i] = (int16_t)(BEEP_AMPL * gain * sinf(2.0f * (float)M_PI * BEEP_HZ * i / AUDIO_PLAY_RATE));
    }
    ready = true;
}

/* `count` beeps now; false while sending or while a beep is playing. */
static bool beep_start(int times) {
    pthread_t thread;
    if (atomic_load(&keyed) || js8_tx_busy(tx) || atomic_exchange(&beeping, true)) return false;
    beep_last_ms = now_wall_ms();
    p3_beep_make();
    beep_times = times;
    if (pthread_create(&thread, NULL, p3_beep_thread, NULL) != 0) atomic_store(&beeping, false);
    else pthread_detach(thread);
    return true;
}

/* ---- High-SWR guard (randal007, x6100-js8 beta 4.5) -------------------- */

/* For unattended stations: the SWR above 3:1 for half a second while we transmit turns every
 * automatic sender off (AUTO, HB, HB ACK, auto CQ). The message on the air carries on. Three
 * beeps once the transmitter is done (the speaker is the TX audio path while keyed), whatever
 * the Alerts list says. */
#define SWR_TRIP    3.0f
#define SWR_TRIP_MS 500

static uint8_t swr_msg_id;
static int64_t swr_high_since; /* 0: the last reading was at or under the limit */
static float   swr_high_max;
static bool    swr_beep_due;

static void swr_guard_trip(float swr) {
    if (!params.js8_auto.x && !params.js8_hb.x && !params.js8_hb_ack.x && !auto_cq) return;
    LV_LOG_USER("JS8 high SWR: %.1f:1 for %d ms while sending, AUTO / HB / HB ACK / auto CQ off", swr, SWR_TRIP_MS);
    params_bool_set(&params.js8_auto, false);
    params_bool_set(&params.js8_hb, false);
    params_bool_set(&params.js8_hb_ack, false);
    hb_next_ms   = 0;
    hb_adjusting = false;
    auto_cq_stop("high SWR");
    if (btn_auto.disp_btn) buttons_refresh(&btn_auto);
    if (btn_hbauto.disp_btn) buttons_refresh(&btn_hbauto);
    if (btn_hbackk.disp_btn) buttons_refresh(&btn_hbackk);
    add_info_row("High SWR %.1f:1: AUTO, HB, HB ACK and auto CQ off", swr);
    msg_update_text_fmt("High SWR %.1f:1: AUTO, HB, HB ACK and auto CQ off. Check the antenna", swr);
    update_status();
    swr_beep_due = true;
}

static void swr_guard_reset(void) {
    swr_high_since = 0;
    swr_beep_due   = false;
}

/* Every 250 ms (tx_timer). Readings over the limit for 500 ms trip it: a spike as the radio
 * keys doesn't. */
static void swr_guard_tick(void) {
    if (!atomic_load(&keyed)) {
        swr_high_since = 0;
        tx_info_refresh(&swr_msg_id, NULL, NULL, NULL); /* the next TX starts fresh */
        if (swr_beep_due && beep_start(3)) swr_beep_due = false;
        return;
    }
    float swr;
    if (!tx_info_refresh(&swr_msg_id, NULL, NULL, &swr)) return;
    if (swr <= SWR_TRIP) {
        swr_high_since = 0;
        return;
    }
    int64_t now = now_mono_ms();
    if (!swr_high_since) {
        swr_high_since = now;
        swr_high_max   = swr;
        return;
    }
    swr_high_max = LV_MAX(swr_high_max, swr);
    if (now - swr_high_since < SWR_TRIP_MS) return;
    swr_high_since = 0;
    swr_guard_trip(swr_high_max);
}

/* GhostNet's emergency group: how long its notice stays in the message box */
#define FLASH_SHOW_MS 20000

/* Every decode except our own: GhostNet's emergency group, alert words, then the sounds. */
static void alert_check(js8_rx_msg_t *msg, bool first_heard) {
    char    word[16];
    uint8_t on = params.js8_alerts.x;
    if (msg->low_confidence) return; /* likely a false decode */
    /* A message to @GSTFLASH (or naming it), for an operator in that group (GhostNet mode puts it in Groups): the
     * notice in red, the row as an alert word's, three beeps with Beep on. The station's text is only shown, its
     * '#' doubled so that it cannot colour the box; nothing else follows from it. */
    if (ghostnet_groups_have(groups_text, "@GSTFLASH") && ghostnet_is_flash(msg->to, msg->text)) {
        char shown[2 * JS8_RX_TEXT_LEN];
        ghostnet_box_text(msg->text, shown, sizeof(shown));
        msg->alert = true;
        msg_schedule_text_ms(FLASH_SHOW_MS, "#%06x EMERGENCY @GSTFLASH:# %s", (unsigned)theme_hex(TC_err), shown);
        LV_LOG_USER("JS8 @GSTFLASH from %s", msg->from);
        alert_beep(3);
        return;
    }
    if (js8_alert_hit(msg->text, msg->from, alert_words, word, sizeof(word))) {
        msg->alert = true; /* the row turns purple */
        msg_update_text_fmt("Alert %s: %s", word, msg->text);
        alert_beep(2); /* two beeps for a word */
        return;
    }
    /* A heartbeat ack names us too, but isn't a message to us. */
    bool to_me = msg->to_me && strstr(msg->text, " HEARTBEAT SNR") == NULL;
    bool beep  = false;
    if ((on & JS8_ALERT_TO_ME) && to_me) {
        beep = true;
    } else if ((on & JS8_ALERT_CQ) && msg->cq) {
        beep = true;
    } else if ((on & JS8_ALERT_NEW) && first_heard && msg->from[0]) {
        int band = qso_log_freq_to_band(cparam_i_get(cfg_fg_freq));
        if (qso_log_search_worked(msg->from, MODE_JS8, band) <= 0) {
            msg_update_text_fmt("New station: %s", msg->from);
            beep = true;
        }
    }
    if (beep) alert_beep(1);
}

enum alerts_choice { AL_BEEP, AL_TO_ME, AL_INBOX, AL_CQ, AL_NEW, AL_WORDS, AL_TEST, AL_CLOSE };

/* The on / off items, in list order, and the JS8_ALERT_ bit of each. */
static const struct alert_switch {
    const char *name;
    uint8_t     flag;
} alert_items[AL_NEW + 1] = {
    {"Beep", JS8_ALERT_BEEP},
    {"Message to me", JS8_ALERT_TO_ME},
    {"Inbox message", JS8_ALERT_INBOX},
    {"Someone calls CQ", JS8_ALERT_CQ},
    {"New station (not in log)", JS8_ALERT_NEW},
};

static void p3_alert_item_text(int item, char *out, size_t size) {
    bool on = (params.js8_alerts.x & alert_items[item].flag) != 0;
    snprintf(out, size, "%s: %s", alert_items[item].name, on ? "On" : "Off");
}

static void alerts_close(void) { p3_popup_shut(&alerts_list); }

/* Test beep: now, whatever the 3 s spacing says; it still counts toward it
 * only if it played. */
static void p3_alerts_test(void) {
    int64_t before = beep_last_ms;
    if ((params.js8_alerts.x & JS8_ALERT_BEEP) == 0) p3_say("Beep is off");
    else if (atomic_load(&keyed) || js8_tx_busy(tx)) p3_say("Not while transmitting");
    beep_last_ms = 0;
    alert_beep(2);
    if (beep_last_ms == 0) beep_last_ms = before;
}

static void p3_alerts_item_cb(lv_event_t *ev) {
    int  item = (int)(intptr_t)lv_event_get_user_data(ev);
    char text[sizeof(alert_words) + 24];
    if (item == AL_CLOSE) {
        p3_popup_shut(&alerts_list);
    } else if (item == AL_TEST) {
        p3_alerts_test();
    } else if (item == AL_WORDS) {
        /* Into the keyboard; log_edit_done brings the list back. */
        p3_popup_drop(&alerts_list);
        edit_target = EDIT_ALERT_WORDS; /* back to the list after the keyboard */
        p3_keyboard(alert_words[0] ? alert_words : NULL);
        p3_say("Calls or words to watch for, separated by spaces");
    } else { /* an on / off item: flip it and say so in place */
        params_uint8_set(&params.js8_alerts, params.js8_alerts.x ^ alert_items[item].flag);
        p3_alert_item_text(item, text, sizeof(text));
        lv_label_set_text(lv_obj_get_child(lv_event_get_target(ev), 0), text);
    }
}

static void p3_alerts_key_cb(lv_event_t *ev) {
    if (p3_popup_key(ev)) p3_popup_shut(&alerts_list);
}

static lv_obj_t *p3_alerts_add(int item, const char *text) {
    lv_obj_t *made = p3_popup_item(alerts_list, text, p3_alerts_item_cb, p3_alerts_key_cb, item);
    p3_popup_dots(made);
    return made;
}

static void alerts_show(void) { /* also back from typing the alert words */
    char      text[sizeof(alert_words) + 24];
    lv_obj_t *top = NULL;
    alerts_list   = p3_popup_new(560);
    p3_popup_note(alerts_list, "Alerts: beep, and alert words in purple");
    for (int k = AL_BEEP; k <= AL_NEW; k++) {
        p3_alert_item_text(k, text, sizeof(text));
        lv_obj_t *made = p3_alerts_add(k, text);
        if (top == NULL) top = made;
    }
    snprintf(text, sizeof(text), "Alert words: %s", alert_words[0] ? alert_words : "(none)");
    lv_obj_set_style_text_color(p3_alerts_add(AL_WORDS, text), TC(row_alert_text), 0);
    p3_alerts_add(AL_TEST, "Test beep");
    lv_obj_set_style_text_color(p3_alerts_add(AL_CLOSE, "Close"), TC(action), 0);
    p3_popup_focus(top);
}

static void alerts_cb(button_data_t *b) {
    LV_UNUSED(b);
    if (!p3_toggle_press(alerts_list, alerts_close)) alerts_show();
}

/* ---- Sending speed and decoded speeds (T6) ------------------------------ */

/* The way desktop JS8Call works: we send everything at one speed (Normal,
 * Fast, Turbo or Slow) and decode all of them at once, unless Decode is
 * set to My speed. Turbo sends no heartbeats and no HB acks. */

/* The speed of the selected row: a station, or a message we received. */
static bool p3_selected_speed(js8_speed_t *speed) {
    uint16_t at_row, at_col;
    lv_table_get_selected_cell(table, &at_row, &at_col);
    if (at_row >= rows) return false; /* also: nothing selected */
    int slot = row_hist[at_row];
    if (slot < 0) return false;        /* an info row */
    if (!view_stations && history[slot].tx) return false; /* one of ours */
    *speed = js8_speed_from_submode(view_stations ? st_rows[slot].submode : history[slot].submode);
    return true;
}

/* Answering someone heard at another speed: point it out (desktop offers
 * "Jump to Fast speed"; here it's a hold on Speed). */
static void speed_warn(void) { /* before a reply goes out */
    js8_speed_t theirs, ours = cur_speed();
    char        who[JS8_RX_CALL_LEN];
    float       audio_hz;
    if (!p3_selected_speed(&theirs) || theirs == ours || !p3_selected(who, &audio_hz)) return;
    msg_update_text_fmt("%s was heard on %s, you send %s - Settings... (page 3), Speed: theirs, to match", who, js8_speed_name(theirs),
                        js8_speed_name(ours));
}

/* Whatever we send goes at `speed` from now on. */
static void p3_speed_set(js8_speed_t speed) {
    if (!p3_tx_idle()) return;
    params_uint8_set(&params.js8_speed, (uint8_t)speed);
    /* A wider signal needs the offset further from 3000 Hz. */
    int top_hz = js8_speed_max_offset_hz(speed);
    if (params.js8_tx_freq.x > top_hz) params_uint16_set(&params.js8_tx_freq, (uint16_t)top_hz);
    uint16_t tx_offset = params.js8_tx_freq.x;
    js8_rx_set_qso_offset(rx, tx_offset);
    lv_finder_set_width(finder, js8_speed_bandwidth_hz(speed));
    lv_finder_set_value(finder, (int16_t)tx_offset);
    lv_obj_invalidate(finder); /* redraw the TX marker at its new width */
    if (!params.js8_rx_all.x) js8_rx_set_submodes(rx, rx_speed_mask()); /* My speed follows it */
    /* A heartbeat that fell due during Turbo mustn't go the moment we
     * leave it: the interval starts over. */
    hb_next_ms = 0; /* the heartbeat interval starts over */
    if (btn_speed.disp_btn) buttons_refresh(&btn_speed); /* the label shows the speed */
    update_tx_bar(); /* the bar's length follows the slot */
    const char *name = js8_speed_name(speed);
    msg_update_text_fmt("Sending %s: %d s slots, %d Hz wide%s", name, js8_speed_period_s(speed),
                        js8_speed_bandwidth_hz(speed), js8_speed_heartbeats(speed) ? "" : ", no heartbeats (as desktop)");
    add_info_row("Sending at %s speed", name);
}

static const char *speed_label_getter(void) { /* "Speed:\nNormal" */
    static char text[24];
    snprintf(text, sizeof(text), "Speed:\n%s", js8_speed_name(cur_speed()));
    return text;
}

static void speed_cb(button_data_t *b) {
    LV_UNUSED(b);
    if (!p3_press_closes_popup()) p3_speed_set((js8_speed_t)((cur_speed() + 1) % JS8_SPEED_COUNT));
}

/* Held: go to the selected station's speed. */
static void speed_hold_cb(button_data_t *b) {
    js8_speed_t theirs;
    LV_UNUSED(b);
    if (p3_press_closes_popup()) return;
    if (!p3_selected_speed(&theirs)) p3_say("Select a station first (MFK)");
    else if (theirs == cur_speed()) msg_update_text_fmt("Already sending %s", js8_speed_name(theirs));
    else p3_speed_set(theirs);
}

static const char *decode_label_getter(void) { return params.js8_rx_all.x ? "Decode:\nAll speeds" : "Decode:\nMy speed"; }

static void decode_cb(button_data_t *b) {
    if (p3_press_closes_popup()) return;
    bool all = !params.js8_rx_all.x;
    params_bool_set(&params.js8_rx_all, all);
    js8_rx_set_submodes(rx, rx_speed_mask()); /* every speed, or ours only */
    buttons_refresh(b);
    if (all) p3_say("Decoding every speed (Normal, Fast, Turbo, Slow)");
    else msg_update_text_fmt("Decoding %s only", js8_speed_name(cur_speed())); /* My speed */
}

/* ---- Dial frequencies: JS8Call's, GhostNet's or your own ---------------- */

/* The band keys step through JS8Call's usual dial frequencies or, with
 * GhostNet chosen, through GhostNet's (3.575, 7.107, 14.107 MHz). A
 * custom frequency is kept; the band keys go from it back to the list.
 * GhostNet mode starts on 7.107, the weekly nets' frequency, and puts
 * GhostNet's groups in ours; its own popup has the nets' times and the
 * reminder, and RTTY on GhostNet's RTTY window. */

enum freq_choice { FQ_JS8, FQ_GHOSTNET, FQ_GN_MORE, FQ_CUSTOM, FQ_CLOSE };

static const char *freq_label_getter(void) { /* "Freq:\nJS8", "Freq:\nGhostNet" or the custom kHz */
    static char text[32];
    char        khz[16];
    if (!params.js8_custom_on.x) return params.js8_ghostnet.x ? "Freq:\nGhostNet" : "Freq:\nJS8";
    format_khz(params.js8_custom_hz.x, khz, sizeof khz);
    snprintf(text, sizeof(text), "Freq:\n%s", khz);
    return text;
}

static void freq_close(void) { p3_popup_shut(&freq_list); }

/* Onto the list's closest frequency. */
static void p3_use_list(bool use_ghostnet) {
    if (!p3_tx_idle()) return;
    params_bool_set(&params.js8_ghostnet, use_ghostnet);
    params_bool_set(&params.js8_custom_on, false); /* the band keys use the list again */
    load_band(0); /* the closest one */
    retuned();    /* station list, waterfall, labels */
}

/* GhostNet mode: the GhostNet list from 7.107 MHz, and @GHOSTNET, @GSTFLASH
 * and the regional group (when the country is known, or one was typed on
 * the web page) added to our groups. With AUTO on, JS8 answers the queries
 * sent to our groups, as desktop JS8Call does. */
static void p3_ghostnet_mode(void) {
    char region[16], before[sizeof(groups_text)];
    if (!p3_tx_idle()) return;
    params_bool_set(&params.js8_ghostnet, true);
    params_bool_set(&params.js8_custom_on, false);
    cparam_i_set(cfg_fg_freq, 7107000); /* the weekly nets; the band keys go on to 14.107 and 3.575 */
    load_band(0);
    retuned();
    snprintf(before, sizeof(before), "%s", groups_text);
    ghostnet_region_tag(region, sizeof(region));
    ghostnet_groups_add(groups_text, sizeof(groups_text), region);
    js8_groups_normalise(groups_text, groups_text, sizeof(groups_text));
    if (strcmp(before, groups_text)) save_texts();
    add_info_row("GhostNet groups: %s", groups_text); /* the band's name is in the message box already */
}

/* ---- GhostNet's popup: the nets, the reminder, RTTY ---- */

enum gn_choice { GN_REMIND, GN_TR_RX, GN_TR_TX, GN_RTTY, GN_CLOSE, GN_ROW };
#define GN_ROWS 16                       /* the nets' windows listed */

static lv_obj_t *gn_row_obj[GN_ROWS];    /* the rows, to mark a net's reminder on all of its rows */
static char      gn_row_net[GN_ROWS][8]; /* the net of each */
static int       gn_rows;

static void gn_item_text(int item, char *out, size_t size) {
    const ghostnet_conf_t *c = ghostnet_conf();
    switch (item) {
    case GN_REMIND:
        snprintf(out, size, _("Reminder %d min before: %s"), c->remind_min, c->remind ? _("On") : _("Off"));
        break;
    case GN_TR_RX:
        snprintf(out, size, _("Translate arriving messages: %s"), c->translate_rx ? _("On") : _("Off"));
        break;
    case GN_TR_TX:
        snprintf(out, size, _("Mine into English first: %s"), c->translate_tx ? _("On") : _("Off"));
        break;
    case GN_RTTY: snprintf(out, size, "%s", _("RTTY 7.077 MHz, 45.45 baud (closes JS8)")); break;
    default: snprintf(out, size, "%s", _("Close"));
    }
}

/* A row of the schedule: "* Thu 18:00 UTC (20:00 CEST)  JS8 7.107  Europe", the star on a net reminded */
static void gn_row_text(const ghostnet_slot_t *s, bool reminded, char *out, size_t size) {
    char when[48], freq[16] = "";
    ghostnet_when(s->start, when, sizeof(when));
    if (s->khz > 0) snprintf(freq, sizeof(freq), " %d.%03d", s->khz / 1000, s->khz % 1000);
    snprintf(out, size, "%s%s  %s%s  %s", reminded ? "* " : "", when, s->mode, freq, s->name);
}

static void gn_row_mark(int k, bool reminded) {
    lv_obj_t *label = lv_obj_get_child(gn_row_obj[k], 0);
    char      text[128];
    const char *now = lv_label_get_text(label);
    bool        had = !strncmp(now, "* ", 2);
    snprintf(text, sizeof(text), "%s%s", reminded ? "* " : "", had ? now + 2 : now);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(gn_row_obj[k], reminded ? TC(ok) : TC(text), 0);
}

static void gn_item_cb(lv_event_t *ev) {
    int             item = (int)(intptr_t)lv_event_get_user_data(ev);
    ghostnet_conf_t c    = *ghostnet_conf();
    char            text[96];
    if (item >= GN_ROW) { /* a net's row: its reminder set or cleared */
        const char *net = gn_row_net[item - GN_ROW];
        ghostnet_net_toggle(&c, net);
        bool on = ghostnet_net_reminded(&c, net);
        if (!ghostnet_conf_save(&c)) {
            p3_say("Can't write the GhostNet settings");
            return;
        }
        for (int k = 0; k < gn_rows; k++)
            if (!strcmp(gn_row_net[k], net)) gn_row_mark(k, on);
        /* short: a longer text scrolls in the box and is gone before it is read */
        if (on && !c.remind) msg_update_text_fmt("%s marked, Reminder off", net);
        else msg_update_text_fmt(on ? "%s: reminder set" : "%s: no reminder", net);
        return;
    }
    switch (item) {
    case GN_REMIND:
        c.remind = !c.remind;
        if (c.remind && !c.nets[0]) {
            /* none chosen yet: the weekly net of the radio's IARU region to begin with */
            const char *net = ghostnet_region_net(band_plan_region());
            ghostnet_net_toggle(&c, net);
            for (int k = 0; k < gn_rows; k++)
                if (!strcmp(gn_row_net[k], net)) gn_row_mark(k, true);
        }
        break;
    case GN_TR_RX:
        c.translate_rx = !c.translate_rx;
        trans_said     = false;
        if (c.translate_rx) msg_update_text_fmt("Their texts go to MyMemory");
        break;
    case GN_TR_TX:
        c.translate_tx = !c.translate_tx;
        tx_tr_done     = false;
        if (c.translate_tx) msg_update_text_fmt("OK: English first, OK sends");
        break;
    case GN_RTTY:
        if (!p3_tx_idle()) return;
        ghostnet_rtty_open(); /* once this app has closed */
        /* The popup is left to destruct_cb, which deletes it at once: shut here, its delete would come after
         * dialog_destruct() has freed dialog.obj, its parent, and crash the GUI */
        dialog_destruct();
        return;
    default: p3_popup_shut(&freq_list); return;
    }
    if (!ghostnet_conf_save(&c)) {
        p3_say("Can't write the GhostNet settings");
        return;
    }
    gn_item_text(item, text, sizeof(text));
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(ev), 0), text);
}

static void gn_key_cb(lv_event_t *ev) {
    if (p3_popup_key(ev)) p3_popup_shut(&freq_list);
}

static void ghostnet_show(void) { /* where the Freq popup was */
    static ghostnet_slot_t slots[GN_ROWS];
    const ghostnet_conf_t *c = ghostnet_conf();
    char                   text[128], region[16];
    lv_obj_t              *top = NULL;

    freq_list = p3_popup_new(620);
    ghostnet_region_tag(region, sizeof(region));
    snprintf(text, sizeof(text), _("GhostNet: groups @GHOSTNET @GSTFLASH %s"), region);
    p3_popup_note(freq_list, text);
    for (int k = GN_REMIND; k <= GN_CLOSE; k++) {
        gn_item_text(k, text, sizeof(text));
        lv_obj_t *made = p3_popup_item(freq_list, text, gn_item_cb, gn_key_cb, k);
        p3_popup_dots(made);
        if (k == GN_CLOSE) lv_obj_set_style_text_color(made, TC(action), 0);
        if (top == NULL) top = made;
    }
    p3_popup_note(freq_list, "The next 7 days; * reminded: press a net to set or clear it");
    gn_rows = 0;
    int n = ghostnet_upcoming(time(NULL), slots, GN_ROWS, 7 * 24, false);
    for (int i = 0; i < n && gn_rows < GN_ROWS; i++) {
        const ghostnet_slot_t *s = &slots[i];
        /* VARA and ALE need a PC, the voice window is for emergencies: in the web page's list only */
        if (!strcmp(s->mode, "VARA") || !strcmp(s->mode, "ALE") || !strcmp(s->mode, "LSB")) continue;
        bool reminded = ghostnet_net_reminded(c, s->net);
        gn_row_text(s, reminded, text, sizeof(text));
        lv_obj_t *row = p3_popup_item(freq_list, text, gn_item_cb, gn_key_cb, GN_ROW + gn_rows);
        p3_popup_dots(row);
        lv_obj_set_style_text_font(row, &sony_22, 0);
        if (reminded) lv_obj_set_style_text_color(row, TC(ok), 0);
        snprintf(gn_row_net[gn_rows], sizeof(gn_row_net[0]), "%s", s->net);
        gn_row_obj[gn_rows++] = row;
    }
    if (n == 0) p3_popup_note(freq_list, "No schedule: ghostnet_nets.txt is missing");
    p3_popup_note(freq_list, "Own net, reminder time, regional group: web page GhostNet");
    p3_popup_focus(top);
}

/* As typed: kHz ("7107.5") or, under 100, MHz ("7.1075"). On a bad one,
 * false with the reason shown: the keyboard stays open. */
static bool parse_custom(const char *typed, int32_t *hz_out) {
    char  *rest;
    double num = strtod(typed, &rest);
    if (rest == typed || num <= 0) return p3_refuse("Type the dial frequency in kHz, e.g. 7107");
    double  scale = num < 100 ? 1e6 : 1e3;
    int32_t dial  = (int32_t)(num * scale + 0.5);
    if (dial < CUSTOM_MIN_HZ || dial > CUSTOM_MAX_HZ) return p3_refuse("Out of range: 1800 - 54000 kHz");
    if (!p3_tx_idle()) return false;
    if (hz_out) *hz_out = dial;
    return true;
}

static void tune_custom(const char *typed) {
    int32_t dial;
    if (!parse_custom(typed, &dial)) return;
    params_int32_set(&params.js8_custom_hz, dial);
    params_bool_set(&params.js8_custom_on, true); /* the Freq label shows it */
    cparam_i_set(cfg_fg_freq, dial);
    js8_usb_dig(); /* the mode JS8 needs */
    retuned();     /* station list, waterfall, labels */
    p3_say(where_label());
}

static void p3_freq_item_cb(lv_event_t *ev) {
    int  item     = (int)(intptr_t)lv_event_get_user_data(ev);
    char last[16] = "";
    if (item == FQ_CUSTOM) {
        /* Into the keyboard, the last one filled in. */
        p3_popup_drop(&freq_list);
        if (params.js8_custom_hz.x != 0) format_khz(params.js8_custom_hz.x, last, sizeof last);
        edit_target = EDIT_FREQ; /* log_edit_done tunes it */
        p3_keyboard(last);
        p3_say("Dial frequency in kHz, then Enter");
        return;
    }
    if (item == FQ_GN_MORE) { /* GhostNet's popup where this one was */
        p3_popup_drop(&freq_list);
        ghostnet_show();
        return;
    }
    p3_popup_shut(&freq_list);
    if (item == FQ_JS8) p3_use_list(false);
    if (item == FQ_GHOSTNET) p3_ghostnet_mode();
}

static void p3_freq_key_cb(lv_event_t *ev) {
    if (p3_popup_key(ev)) p3_popup_shut(&freq_list);
}

static lv_obj_t *p3_freq_add(int item, const char *text) {
    lv_obj_t *made = p3_popup_item(freq_list, text, p3_freq_item_cb, p3_freq_key_cb, item);
    p3_popup_dots(made);
    return made;
}

static void freq_show(void) { /* the Freq popup */
    char      text[64], khz[16];
    lv_obj_t *opt[FQ_CLOSE + 1];
    freq_list = p3_popup_new(560);
    snprintf(text, sizeof(text), "Now: %s", where_label());
    p3_popup_note(freq_list, text);
    opt[FQ_JS8]      = p3_freq_add(FQ_JS8, "JS8Call frequencies (7.078, 14.078...)");
    opt[FQ_GHOSTNET] = p3_freq_add(FQ_GHOSTNET, "GhostNet mode: 7.107 + its groups");
    opt[FQ_GN_MORE]  = p3_freq_add(FQ_GN_MORE, "GhostNet: nets, reminder, RTTY...");
    if (params.js8_custom_hz.x == 0) {
        snprintf(text, sizeof(text), "Custom kHz...");
    } else {
        format_khz(params.js8_custom_hz.x, khz, sizeof khz);
        snprintf(text, sizeof(text), "Custom kHz... (last %s)", khz);
    }
    opt[FQ_CUSTOM] = p3_freq_add(FQ_CUSTOM, text);
    opt[FQ_CLOSE]  = p3_freq_add(FQ_CLOSE, "Close");
    lv_obj_set_style_text_color(opt[FQ_CLOSE], TC(action), 0);
    /* The list in use stands out, and the knob starts on it. */
    int now = params.js8_custom_on.x ? FQ_CUSTOM : params.js8_ghostnet.x ? FQ_GHOSTNET : FQ_JS8;
    lv_obj_set_style_text_color(opt[FQ_GN_MORE], TC(row_alert_text), 0);
    lv_obj_set_style_text_color(opt[now], TC(ok), 0);
    p3_popup_focus(opt[now]);
}

static void freq_cb(button_data_t *b) {
    LV_UNUSED(b);
    if (!p3_toggle_press(freq_list, freq_close)) freq_show();
}

/* ---- QSO map --------------------------------------------------------------- */

static void map_close(void) {
    if (!map_obj) return;
    lv_obj_del_async(map_obj);
    map_obj = NULL;
    if (table && !composing) table_takes_keys();
}

/* VOL passed on while the map has the keys, as in FT8's (#305) */
static void map_key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (key == LV_KEY_ESC) map_close();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) radio_change_vol(1);
}

/* The stations of this band, for the map */
static int map_fill(js8_map_station_t *out, int max) {
    static js8_station_t list[MAX_ROWS];
    int64_t              now = now_wall_ms();
    int                  n   = js8_stations_list(stations, now, list, MAX_ROWS);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].call, sizeof(out[i].call), "%s", list[i].call);
        snprintf(out[i].grid, sizeof(out[i].grid), "%s", list[i].grid);
        out[i].snr      = list[i].snr;
        out[i].t        = time(NULL) - (time_t)((now - list[i].heard_ms) / 1000);
        out[i].heard_me = list[i].heard_me;
    }
    return n;
}

/* MFK press on a station of the map: back to the list with it selected, as a press on its row */
static void map_pick(const js8_map_station_t *st) {
    js8_station_t s;
    map_close();
    if (!find_station(st->call, &s)) {
        msg_update_text_fmt("%s: not heard on this band", st->call);
        return;
    }
    snprintf(sel_call, sizeof(sel_call), "%s", s.call);
    qso_freq = s.freq_hz;
    sel_snr  = s.snr;
    show_selection();
    msg_update_text_fmt("%s at %.0f Hz, %+d dB", s.call, s.freq_hz, s.snr);
}

/* Over the waterfall and the list: the world, the station, the stations heard on this band
 * and a path to each QSO of the log */
static void map_cb(button_data_t *btn) {
    (void)btn;
    user_touch();
    if (map_obj) { /* Map again closes it */
        map_close();
        return;
    }
    if (popup_guard()) return;
    if (composing) return;
    map_obj = js8_map_create(dialog.obj, WIDTH, WF_HEIGHT, station_qth(), JS8_LOG_PATH, map_fill, true, map_pick);
    if (!map_obj) {
        msg_update_text_fmt("Not enough memory for the map");
        return;
    }
    lv_obj_set_pos(map_obj, 13, 13);
    keys_leave_main();
    lv_obj_add_event_cb(map_obj, map_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, map_obj);
    lv_group_focus_obj(map_obj);
    lv_group_set_editing(keyboard_group, true); /* the MFK steps through the stations */
}

/* Translate (page 4): the message on the list's cursor into the GUI's language, from English (or Spanish), under it
 * on its row. Only on this press: the text goes to MyMemory over the internet */
static void translate_cb(button_data_t *btn) {
    uint16_t r = 0, c = 0;

    (void)btn;
    user_touch();
    if (popup_guard() || composing) return;
    if (board_on || view_stations) { /* the list of messages is not in sight */
        msg_update_text_fmt("View: Messages first");
        return;
    }
    lv_table_get_selected_cell(table, &r, &c);
    if (r == LV_TABLE_CELL_NONE || r >= rows || row_hist[r] < 0) {
        msg_update_text_fmt("Turn the MFK onto a message");
        return;
    }
    const js8_rx_msg_t *m = &history[row_hist[r]];
    if (m->tx) msg_update_text_fmt("Your own message");
    else if (m->partial) msg_update_text_fmt("Still arriving: wait");
    else if (trans_find(m) >= 0) msg_update_text_fmt("Translated already");
    else if (!ghostnet_translate(GHOSTNET_TR_ASK, m->text, m->msg_id, m->utc, trans_ask_done))
        msg_update_text_fmt("Busy: try again shortly");
    else msg_update_text_fmt("Translating...");
}


/* ---- The browser's JS8 page (remote_server.c), from OlivLoze's fork --------------------------------------------
 * Its messages and its state in remote_server.h's types, and its commands as the buttons' own handlers. Only the
 * hooks above call in here (ui_add_message, ui_tx_status, wf_timer_cb, tx_timer_cb, destruct_cb) */

static pthread_mutex_t     rstat_lock = PTHREAD_MUTEX_INITIALIZER;
static remote_js8_status_t rstat;         /* what js8_window_status() hands out, under rstat_lock */
static char                rnote[80];     /* the answer to the pages' last command, and whether it refused */
static bool                rnote_err;
static remote_js8_rc_t     rnote_cmd;     /* the command it answers */
static uint32_t            rnotes;

/* A message as it comes, ours too, with the sender's locator when the app knows it */
static void remote_msg(const js8_rx_msg_t *msg) {
    static remote_js8_msg_t r; /* the LVGL thread's alone: 700 bytes off its stack */
    js8_station_t           st;

    memset(&r, 0, sizeof(r));
    r.msg_id         = msg->msg_id;
    r.utc            = msg->utc;
    r.snr            = msg->snr;
    r.freq_hz        = (int)(msg->freq_hz + 0.5f);
    r.submode        = msg->submode;
    r.tx             = msg->tx;
    r.to_me          = msg->to_me;
    r.cq             = msg->cq;
    r.heartbeat      = msg->heartbeat;
    r.to_group       = msg->to_group;
    r.low_confidence = msg->low_confidence;
    r.partial        = msg->partial;
    r.checksum       = msg->checksum;
    snprintf(r.from, sizeof(r.from), "%s", msg->from);
    snprintf(r.to, sizeof(r.to), "%s", msg->to);
    if (!msg->tx && msg->from[0] && find_station(msg->from, &st)) snprintf(r.grid, sizeof(r.grid), "%s", st.grid);
    snprintf(r.text, sizeof(r.text), "%s", msg->text);
    remote_server_js8_msg(&r);
}

/* Every third row of the waterfall: five a second, enough for the page, a third of the bytes */
static void remote_wf_row(const float *row, unsigned n) {
    static unsigned k;
    if (k++ % 3 == 0) remote_server_app_wf(row, n, filter_low, filter_high);
}

/* LVGL thread: the state as it is now, for any thread to read */
static void rstat_update(void) {
    remote_js8_status_t s;
    int64_t             now = now_wall_ms(), next = 0;

    memset(&s, 0, sizeof(s)); /* the padding too: the panel thread compares two of them whole */
    s.open = dialog.run && tx_bar != NULL;
    if (s.open) {
        s.busy    = tx != NULL && js8_tx_busy(tx);
        s.sending = tx_status.state == JS8_TX_KEYING;
        s.auto_cq = auto_cq;
        s.hb      = params.js8_hb.x;
        s.auto_on = params.js8_auto.x;
        s.hold    = params.js8_hold_offset.x;
        s.speed   = cur_speed();
        s.frame   = tx_status.frame;
        s.frames  = tx_status.frames;
        s.txf     = params.js8_tx_freq.x;
        s.sel_hz  = sel_call[0] && qso_freq >= 0 ? (int32_t)(qso_freq + 0.5f) : -1;
        s.hb_min  = params.js8_hb_interval.x;
        snprintf(s.sel, sizeof(s.sel), "%s", sel_call);
        snprintf(s.where, sizeof(s.where), "%s", where_label());
        if (s.busy) snprintf(s.text, sizeof(s.text), "%s", tx_preview[0] ? tx_preview : tx_status.text);
        if (tx_status.state != JS8_TX_IDLE) next = tx_status.next_ms;
        else if (auto_cq) next = auto_cq_next_ms;
        else if (params.js8_hb.x) next = hb_next_ms;
    }
    s.next_s   = next > now ? (int32_t)((next - now + 999) / 1000) : -1;
    s.notes    = rnotes;
    s.note_err = rnote_err;
    s.note_cmd = rnote_cmd;
    snprintf(s.note, sizeof(s.note), "%s", rnote);
    pthread_mutex_lock(&rstat_lock);
    rstat = s;
    pthread_mutex_unlock(&rstat_lock);
}

void js8_window_status(remote_js8_status_t *s) {
    pthread_mutex_lock(&rstat_lock);
    *s = rstat;
    pthread_mutex_unlock(&rstat_lock);
}

bool js8_window_running(void) { return dialog.run; }

/* The answer to a page's command, in the state the pages get; the radio's own screen says it its way */
static bool remote_note(remote_js8_rc_t what, bool err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rnote, sizeof(rnote), fmt, ap);
    va_end(ap);
    rnote_err = err;
    rnote_cmd = what;
    rnotes++;
    rstat_update();
    return !err;
}

bool js8_window_remote(remote_js8_rc_t what, int32_t arg, const char *text) {
    if (!dialog.run || !tx_bar) return remote_note(what, true, "The JS8 app is closed on the radio");
    /* A list open on the radio would take the press for itself (popup_guard): the browser's goes through */
    popups_close_all();
    user_touch(); /* someone is working the app */
    switch (what) {
    case REMOTE_JS8_CQ: cq_cb(&btn_cq); break;
    case REMOTE_JS8_AUTOCQ: /* as the CQ hold, without handing the knob the interval */
        if (!auto_cq) cq_hold_cb(&btn_cq);
        cq_adjust_end();
        break;
    case REMOTE_JS8_HBNOW: heartbeat_cb(NULL); break;
    case REMOTE_JS8_HB: /* as the HB button, the same: the interval is not left on the knob */
        hb_adjust_end();
        hb_cb(&btn_hbauto);
        hb_adjust_end();
        break;
    case REMOTE_JS8_AUTO: auto_cb(&btn_auto); break;
    case REMOTE_JS8_HOLD: hold_cb(&btn_hold); break;
    case REMOTE_JS8_SPEED: speed_cb(NULL); break;
    case REMOTE_JS8_STOP: stop_tx_cb(NULL); break;
    case REMOTE_JS8_TXFREQ: set_tx_offset(arg); break;
    case REMOTE_JS8_SELECT: { /* as a station picked on the map */
        js8_station_t st;
        if (!text || !find_station(text, &st)) return remote_note(what, true, "%s: not heard on this band", text ? text : "");
        snprintf(sel_call, sizeof(sel_call), "%s", st.call);
        qso_freq = st.freq_hz;
        sel_snr  = st.snr;
        show_selection();
        break;
    }
    case REMOTE_JS8_SEND: { /* as OK in the compose window: the macros filled; to the station selected, as Reply */
        static char      filled[TX_TEXT_MAX * 2], out[TX_TEXT_MAX + 8];
        static js8_tx_preview_t plan;
        char             them[JS8_RX_CALL_LEN];
        if (!text || !*text) return remote_note(what, true, "Nothing to send");
        if (js8_tx_busy(tx)) return remote_note(what, true, "Already sending: Stop first");
        macros_fill(text, false, filled, sizeof(filled));
        if (!aprs_prepare(filled, out, sizeof(out))) return remote_note(what, true, "Not sent: the radio's screen says why");
        js8_tx_preview(params.callsign.x, station_qth(), out, cur_speed(), &plan);
        if (!plan.ok) return remote_note(what, true, "Not sent: %s", plan.error);
        if (sel_call[0] && qso_freq >= 0 && starts_with_call(out, them, sizeof(them)) && !strcasecmp(them, sel_call))
            apply_hold(qso_freq); /* Hold off: on their offset, as Reply does */
        if (!tx_queue(out)) return remote_note(what, true, "Not sent: the radio's screen says why");
        return remote_note(what, false, "Queued: %d frame%s, %.0f s", plan.frames, plan.frames != 1 ? "s" : "", plan.seconds);
    }
    }
    rstat_update();
    return true;
}
