/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  DX cluster telnet client: a worker thread that logs in with the
 *  callsign, asks for the latest spots and keeps the spot store updated
 *  (all bands, the last hour). It runs from boot, independent of the app,
 *  and reconnects on its own, with a backoff, until cluster_client_stop().
 */

#pragma once

#include "spots.h"
#include "../board.h"

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CLUSTER_CONF /* overridden by the tests and the UI harness */
#define CLUSTER_CONF         BOARD_DATA "/cluster.conf"
#endif
#define CLUSTER_DEFAULT_HOST "dxfun.com"
#define CLUSTER_DEFAULT_PORT 8000

typedef enum {
    CLUSTER_STOPPED,
    CLUSTER_CONNECTING,
    CLUSTER_CONNECTED,
    CLUSTER_FAILED, /* waits and retries */
} cluster_state_t;

/* Host and port from CLUSTER_CONF ("host=..." and "port=..." lines),
 * or the defaults. */
void cluster_config_load(char *host, size_t host_size, int *port);

/* Saved servers: "server=name|host|port" lines (the web page writes them).
 * Without any, the default list; the one in use is added when not listed. */
#define CLUSTER_MAX_SERVERS 16
typedef struct {
    char name[32];
    char host[128];
    int  port;
} cluster_server_t;

int cluster_servers_load(cluster_server_t *out, int max);

/* Sets the server in use in CLUSTER_CONF, keeping the saved servers
 * (atomically: a temporary file, then rename). */
bool cluster_config_save(const char *host, int port);

/* "enabled=0" in CLUSTER_CONF: the client stays off (missing or 1: on). The radio's Link button and the web page
 * write it */
bool cluster_config_enabled(void);
bool cluster_enabled_save(bool on);

/* The call to log in with: "login=" in CLUSTER_CONF (e.g. ON6URE-2, so the radio and a logger with ON6URE are not
 * one login to the cluster), else `callsign` */
void cluster_config_login(char *out, size_t size, const char *callsign);

/* Modification time of CLUSTER_CONF, 0 when missing: the app reloads the
 * server when the file changes (e.g. from the web configurator). */
time_t cluster_config_mtime(void);

/* Filters and alerts, from the same file (the web page writes them):
 *   filter_modes=CW,SSB,DIG,AMFM  modes shown (missing: all)
 *   max_age=30                    minutes (0 or missing: the store's own limit)
 *   skip_spotters=K1TTT-#,*-3     spotter calls hidden, * matches any run
 *   filter_bands=40,20,6          bands shown (missing: all)
 *   filter_activity=POTA,SOTA,WWBOTA,HOTA   only these activations (missing: all spots)
 *   alert=PATTERN|bands|mode      e.g. VP8*|20,17,15|CW (bands, mode: empty = any)
 *   alert_off=PATTERN|bands|mode  an alert turned off: kept in the list, not used
 *   alert_popup=1, alert_beep=1
 * Matching is case-insensitive; an alert pattern must match the whole call. */
#define CLUSTER_MAX_ALERTS 50
#define CLUSTER_FM_CW      (1 << 0)
#define CLUSTER_FM_SSB     (1 << 1)
#define CLUSTER_FM_DIG     (1 << 2)
#define CLUSTER_FM_AMFM    (1 << 3)
#define CLUSTER_FM_ALL     0xF
#define CLUSTER_ACT_POTA   (1 << 0)
#define CLUSTER_ACT_SOTA   (1 << 1)
#define CLUSTER_ACT_WWBOTA (1 << 2)
#define CLUSTER_ACT_HOTA   (1 << 3)
#define CLUSTER_ACT_COUNT  4
extern const char *const cluster_act_names[CLUSTER_ACT_COUNT]; /* "POTA", "SOTA", "WWBOTA", "HOTA": bit order */

typedef struct {
    char     pattern[32];
    uint16_t bands; /* bit per cluster_band_of() index, 0 = all */
    uint8_t  mode;  /* one CLUSTER_FM_*, 0 = any */
} cluster_alert_t;

typedef struct {
    uint8_t         modes;
    uint16_t        bands;    /* bit per cluster_band_of() index, 0 = all */
    uint8_t         activity; /* CLUSTER_ACT_* bits, 0 = all spots */
    uint16_t        max_age;
    char            skip[128];
    cluster_alert_t alerts[CLUSTER_MAX_ALERTS];
    int             alert_count;
    bool            popup, beep;
    uint32_t        gen; /* changes when the file was read again */
} cluster_filter_t;

/* Mode class (CLUSTER_FM_*) of a spot */
uint8_t cluster_spot_mode_class(const cluster_spot_t *s);

/* CLUSTER_ACT_* of a spot, from its comment: "POTA" or a park reference
 * (US-1234, IT-0123), "SOTA" or a summit reference (I/LO-123, W7W/KG-001),
 * "WWBOTA"/"BOTA" or a bunker (B/G-0070), "HOTA" or a HOTA reference (RO-H0390). */
uint8_t cluster_spot_activity(const cluster_spot_t *s);

/* Sets the modes, bands and activity filters in CLUSTER_CONF, keeping the
 * other lines (CLUSTER_FM_ALL / 0 / 0 = no filter). LVGL thread only. */
bool cluster_filter_save(uint8_t modes, uint16_t bands, uint8_t activity);

/* Reads the filters from CLUSTER_CONF (defaults when missing). */
void cluster_filter_load(cluster_filter_t *f);

/* The filters in use, read again when CLUSTER_CONF changed (LVGL thread only). */
const cluster_filter_t *cluster_filter(void);

/* Index of the alert the spot matches, or -1. */
int cluster_alert_match(const cluster_filter_t *f, const cluster_spot_t *s);

/* The spot is shown: its mode, age and spotter pass (an alert match always does). */
bool cluster_filter_pass(const cluster_filter_t *f, const cluster_spot_t *s, uint16_t now_utc_min);

/* "host" or "host:port" -> host, port (23 when missing). */
bool cluster_parse_server(const char *text, char *host, size_t host_size, int *port);

/* Starts the client, or makes the running one reconnect when the server
 * or the callsign changed. One login; refused by the node or taken over by another login with the call
 * (DXSpider), it waits 30 minutes. cluster_client_stop() closes the link at once. */
void cluster_client_start(const char *host, int port, const char *callsign);
void cluster_client_stop(void);

/* Posts a spot on the cluster link ("DX <kHz> <call> <comment>"), sent once logged in, within 2 minutes.
 * False when the client is not running. */
bool cluster_client_send_spot(double khz, const char *call, const char *comment);

/* Retries at once when waiting after a failure (e.g. the WiFi is back). */
void cluster_client_retry(void);

/* Copies the spots (oldest first) when they changed since *gen; returns
 * the count, or -1 when nothing changed. */
int cluster_client_snapshot(cluster_spot_t *out, int max, uint32_t *gen);

cluster_state_t cluster_client_state(char *detail, size_t size);


/* Feeds a line as if it came from the cluster (tests, UI harness). */
void cluster_client_feed_line(const char *line);

#ifdef __cplusplus
}
#endif
