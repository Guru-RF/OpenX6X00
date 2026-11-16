/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GhostNet in the JS8 app: the HF nets of GhostNet version 1.5 (s2underground), mostly in JS8, for
 *  passing news without the internet or the phone networks. The nets' times with a reminder on any screen, an own
 *  net of the operator's, the groups @GHOSTNET, @GSTFLASH and the regional @GN + country + department, the red
 *  notice of an @GSTFLASH message and the translation of messages on request (x6100-translate, MyMemory).
 *
 *  ghostnet.c is the part a PC test checks (tools/ghostnet): the settings file, the schedule, the groups, the
 *  @GSTFLASH test and the helper's answers; ghostnet_gui.c the part on the screen: the reminder, the RTTY shortcut;
 *  ghostnet_tr.c the translation's worker.
 *
 *  After the GhostNet mode of the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The operator's choices, a KEY=value line each: the GhostNet popup of the JS8 app and the web page GhostNet both
 * write it (whole, through a new file), and each reads it again when it changed */
#ifndef GHOSTNET_CONF
#define GHOSTNET_CONF BOARD_DATA "/ghostnet.conf"
#endif
/* The nets' schedule: the DATA partition's copy first, then the image's */
#ifndef GHOSTNET_NETS_USER
#define GHOSTNET_NETS_USER BOARD_DATA "/ghostnet_nets.txt"
#endif
#ifndef GHOSTNET_NETS
#define GHOSTNET_NETS BOARD_SHARE "/ghostnet_nets.txt"
#endif
/* GhostNet's messages as the JS8 app heard and sent them, for the web page GhostNet; the one before in ".1" */
#ifndef GHOSTNET_LOG
#define GHOSTNET_LOG BOARD_DATA "/ghostnet_log.txt"
#endif
#define GHOSTNET_LOG_MAX (256 * 1024)   /* then it becomes ".1", and the one before that goes */

#define GHOSTNET_OWN "OWN"      /* the id of the operator's own net in the nets list */

typedef struct {
    bool remind;            /* a reminder before each net in `nets` */
    int  remind_min;        /* how long before, 1-120 minutes */
    char nets[96];          /* the nets reminded, by id: "EU,B_NAEU,OWN" */
    bool own_on;            /* the operator's own net is in the lists */
    char own_name[32];
    int  own_day;           /* the radio's local time: 0 Sunday ... 6 Saturday */
    int  own_hh, own_mm;
    int  own_min;           /* how long it lasts, 5-240 minutes */
    int  own_khz;
    char own_mode[8];       /* JS8, RTTY, LSB or USB */
    char dept[4];           /* the department, state or province of the regional group: up to 3 letters or digits */
    char region[16];        /* the regional group typed by hand ("@GNURYMO"); "" makes it from country and dept */
    bool translate_rx;      /* messages arriving in English (or Spanish) come with their translation; off by default */
    bool translate_tx;      /* what the operator types goes into English first, to be checked before it is sent */
} ghostnet_conf_t;

void ghostnet_conf_defaults(ghostnet_conf_t *c);
/* One line of the file into c (the line is changed); a key it does not know is skipped */
void ghostnet_conf_parse_line(ghostnet_conf_t *c, char *line);
/* The file's, read again when its time or inode changed; the defaults without one. LVGL thread */
const ghostnet_conf_t *ghostnet_conf(void);
/* Written whole through a new file; false when it could not be. LVGL thread */
bool ghostnet_conf_save(const ghostnet_conf_t *c);
/* Is net (an id, GHOSTNET_OWN too) one of c's reminded nets; on and off */
bool ghostnet_net_reminded(const ghostnet_conf_t *c, const char *net);
void ghostnet_net_toggle(ghostnet_conf_t *c, const char *net);
/* The weekly net of an IARU region (1 Europe, 2 North America, 3 Australia): the one a reminder starts with */
const char *ghostnet_region_net(int iaru_region);

/* ---- The schedule ---- */

#define GHOSTNET_NETS_MAX 16
#define GHOSTNET_SEGS_MAX 48

typedef struct {
    char id[8];
    char name[40];
} ghostnet_net_t;

typedef struct {
    char net[8];
    int  wday, hh, mm;      /* UTC, 0 Sunday */
    int  minutes;
    int  khz;               /* 0: none (a PC mode) */
    char mode[8];           /* JS8, RTTY, VARA, ALE, LSB */
    char what[40];          /* "JS8 40 m" */
} ghostnet_seg_t;

typedef struct {
    int            nnets, nsegs;
    ghostnet_net_t nets[GHOSTNET_NETS_MAX];
    ghostnet_seg_t segs[GHOSTNET_SEGS_MAX];
} ghostnet_sched_t;

/* One line of the schedule file into s (the line is changed); false for one that is not a net or a window */
bool ghostnet_sched_parse_line(ghostnet_sched_t *s, char *line);
/* The file's, read again when it changed. LVGL thread */
const ghostnet_sched_t *ghostnet_sched(void);
/* A net's name, or the id when the file names none */
const char *ghostnet_net_name(const ghostnet_sched_t *s, const char *id);

/* A window of a net: the next one, or the one going on now */
typedef struct {
    time_t start;           /* UTC */
    int    minutes;
    int    khz;
    char   net[8];
    char   name[40];        /* "Europe", "Bridge North America-Europe", the own net's name */
    char   what[40];        /* "JS8 40 m" */
    char   mode[8];
} ghostnet_slot_t;

/* The next start of a weekly window on wday at hh:mm UTC, or the start of the one going on at now */
time_t ghostnet_next_utc(time_t now, int wday, int hh, int mm, int minutes);
/* The same in the radio's local time (the own net), its summer time followed */
time_t ghostnet_next_local(time_t now, int wday, int hh, int mm, int minutes);
/* The windows of s and c's own net that start within `hours` of now or go on at now, by start; only_reminded: the
 * nets c reminds. How many went into out */
int ghostnet_upcoming_from(const ghostnet_sched_t *s, const ghostnet_conf_t *c, time_t now, ghostnet_slot_t *out,
                           int max, int hours, bool only_reminded);
/* The same with the files' schedule and settings. LVGL thread */
int ghostnet_upcoming(time_t now, ghostnet_slot_t *out, int max, int hours, bool only_reminded);
/* "Thu 18:00 UTC (19:00 CET)": the day and time in UTC and, where the radio's zone is not UTC, the local time and
 * its zone. The day's name in English: the caller's _() translates it */
void ghostnet_when(time_t t, char *out, size_t size);

/* The reminders said lately, so each comes once though it is due at every look until its net starts */
#define GHOSTNET_SAID 8
typedef struct {
    char said[GHOSTNET_SAID][24];
    int  next;
} ghostnet_remind_t;

/* The slot of `slots` (as ghostnet_upcoming gives them) whose reminder is due at now and was not said yet: a net's
 * first JS8 window, or the own net, starting within remind_min minutes. Marked said; -1 for none */
int ghostnet_remind_pick(ghostnet_remind_t *r, const ghostnet_slot_t *slots, int n, time_t now, int remind_min);

/* ---- The groups ---- */

/* The alpha-3 code of an ISO 3166-1 alpha-2 country code (either case); false for one it does not know */
bool ghostnet_iso3(const char *iso2, char out[4]);
/* The regional group: `manual` when one was typed (in capitals, with its '@'), else "@GN" + the country's alpha-3
 * + up to 3 letters or digits of dept ("UY", "MO": "@GNURYMO"); "" without a country */
void ghostnet_region_group(const char *iso2, const char *dept, const char *manual, char *out, size_t size);
/* @GHOSTNET, @GSTFLASH and region (unless "") added to groups ("@ONE @TWO") where not in it yet, while they fit */
void ghostnet_groups_add(char *groups, size_t size, const char *region);
/* group is one of groups ("@ONE @TWO"), either case */
bool ghostnet_groups_have(const char *groups, const char *group);
/* to (a callsign or @GROUP) is one of GhostNet's groups: @GHOSTNET, @GSTFLASH or a regional @GN... */
bool ghostnet_is_group(const char *to);
/* A message for @GSTFLASH, GhostNet's emergency group: sent to it, or naming it as a word of its text */
bool ghostnet_is_flash(const char *to, const char *text);
/* text for the message box, where "#rrggbb text#" colours a part: each '#' doubled, which the box shows as one */
void ghostnet_box_text(const char *text, char *out, size_t size);
/* A line of GHOSTNET_LOG: kind 'R' heard, 'X' sent, 'T' the translation of message id from `from`; tabs and line
 * ends in the text as spaces. The web page reads it (ghostnet_web.py messages()). LVGL thread */
void ghostnet_log(char kind, uint32_t id, const char *from, const char *to, int snr, int32_t dial_hz, const char *text);

/* ---- The translation helper (rootfs/usr/sbin/x6100-translate) ---- */

typedef enum {
    GHOSTNET_TR_RX,         /* an arriving message, only when it is in the other language (the switch) */
    GHOSTNET_TR_ASK,        /* a message the operator asked for (the Translate button) */
    GHOSTNET_TR_TX,         /* the operator's text into English, before it is sent */
} ghostnet_tr_kind_t;

typedef enum {
    GHOSTNET_TR_OK,         /* the translation */
    GHOSTNET_TR_SKIP,       /* nothing to translate: in the operator's language already */
    GHOSTNET_TR_NET,        /* no answer from MyMemory: no connection */
    GHOSTNET_TR_BAD,        /* an answer without a translation, or no helper */
    GHOSTNET_TR_LIMIT,      /* MyMemory's free daily amount is used up; text: the hours until more */
} ghostnet_tr_result_t;

/* MyMemory's code of the GUI's language (i18n_code(): "" English, "es", "zh_CN"): "en", "es", "zh-CN" */
void ghostnet_tr_lang(const char *gui_code, char *out, size_t size);
/* The helper's argument for kind: "rx", "ask" or "tx" */
const char *ghostnet_tr_verb(ghostnet_tr_kind_t kind);
/* The helper's line ("ok <text>", "skip", "error net", "limit 9") as a result, with the text or the hours */
ghostnet_tr_result_t ghostnet_tr_parse(const char *line, char *text, size_t size);

/* The translation, done by the helper on a thread of its own, one at a time: done runs on the LVGL thread with the
 * result and the text (or the hours of GHOSTNET_TR_LIMIT). False when the queue is full. LVGL thread */
#define GHOSTNET_TR_TEXT 480    /* the longest text sent, and the longest answer kept */
typedef void (*ghostnet_tr_cb)(uint32_t id, int32_t utc, ghostnet_tr_result_t result, const char *text);
bool ghostnet_translate(ghostnet_tr_kind_t kind, const char *text, uint32_t id, int32_t utc, ghostnet_tr_cb done);

/* ---- On the screen (ghostnet_gui.c) ---- */

/* The reminder's look every 20 s, from the start. LVGL thread */
void ghostnet_init(void);
/* The radio's country, ISO 3166-1 alpha-2, for the regional group; "" while none is known */
const char *ghostnet_country_iso2(void);
/* The regional group from the settings and the country (ghostnet_region_group) */
void ghostnet_region_tag(char *out, size_t size);
/* Opens the RTTY app on GhostNet's RTTY window: 7.077 MHz USB, 45.45 baud, 170 Hz shift, not reversed. After the
 * app that asked has closed (lv_async_call); the radio goes back to where it was when RTTY closes, and RTTY to its
 * own rate, shift and reverse (rtty_open_with). LVGL thread */
void ghostnet_rtty_open(void);

#ifdef __cplusplus
}
#endif
