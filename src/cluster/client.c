/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  DX cluster telnet client.
 */

#define _GNU_SOURCE /* strcasestr */

#include "client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#define CONNECT_TIMEOUT_MS 10000
#define RETRY_MIN_MS       15000
#define RETRY_MAX_MS       240000 /* backoff doubles up to this */
#define STABLE_MS          60000  /* a session this long resets the backoff */
#define IDLE_TIMEOUT_MS    600000 /* nothing received: the link may be dead */
#define SHOW_DX_COUNT      "50"
#define LOGIN_FALLBACK_MS  4000
#define SHOW_DX_DELAY_MS   2000
#define MAX_AGE_MIN        60
#define LINE_MAX_LEN       512

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool            thread_running; /* under lock */
static volatile bool   stop_requested;
static volatile bool   reconnect_requested; /* new server or callsign */

/* More sources into the same store, whose "same DX within 1 kHz" keeps one spot: EXTRA_LINKS
 * other servers of the list (one node only gets its own region's spots well) and the
 * activator spots of pota.app, wwbota.org and cqhota.app. Their threads leave the state shown, the main server's, alone. */
#define EXTRA_LINKS      4
#define POTA_SPOTS       "/usr/sbin/x6100-pota-spots"
#define POTA_POLL_MS     60000
static __thread bool   extra_link;
static volatile unsigned extras_gen;       /* +1: other servers or callsign changed */
static __thread unsigned my_gen;          /* the extra link's, at its connect */
static bool            extras_running;      /* under lock */
static char            extra_host[EXTRA_LINKS][128];
static int             extra_port[EXTRA_LINKS];

#define ABORTED() (stop_requested || (extra_link ? my_gen != extras_gen : reconnect_requested))

static cluster_store_t store;
static uint32_t        store_gen = 1;
static cluster_state_t state     = CLUSTER_STOPPED;
static char            state_detail[64];

static char host_name[128];
static char     spot_cmd[128];  /* cluster_client_send_spot(): to every link, under lock */
static unsigned spot_seq;
static int64_t  spot_at;
static __thread unsigned spot_sent; /* the link's last spot_seq sent */
#define SPOT_VALID_MS 120000        /* a link that logs in later doesn't send an old spot */
static int  host_port;
static char login_call[CLUSTER_CALL_LEN];

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static uint16_t utc_now_min(void) {
    time_t    now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
}

static void set_state(cluster_state_t s, const char *detail) {
    if (extra_link) return;
    pthread_mutex_lock(&lock);
    state = s;
    snprintf(state_detail, sizeof(state_detail), "%s", detail ? detail : "");
    pthread_mutex_unlock(&lock);
}

void cluster_config_load(char *host, size_t host_size, int *port) {
    snprintf(host, host_size, "%s", CLUSTER_DEFAULT_HOST);
    *port = CLUSTER_DEFAULT_PORT;

    FILE *f = fopen(CLUSTER_CONF, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "host=", 5) == 0 && line[5]) {
            snprintf(host, host_size, "%s", line + 5);
        } else if (strncmp(line, "port=", 5) == 0) {
            int p = atoi(line + 5);
            if (p > 0 && p < 65536) *port = p;
        }
    }
    fclose(f);
}

int cluster_servers_load(cluster_server_t *out, int max) {
    char host[128];
    int  port, n = 0;
    cluster_config_load(host, sizeof(host), &port);

    FILE *f = fopen(CLUSTER_CONF, "r");
    char  line[320];
    while (f && fgets(line, sizeof(line), f) && n < max) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "server=", 7) != 0) continue;
        char *bar1 = strchr(line + 7, '|');
        char *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
        if (!bar2) continue;
        *bar1 = *bar2 = '\0';
        int p         = atoi(bar2 + 1);
        if (!line[7] || !bar1[1] || p <= 0 || p > 65535) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", line + 7);
        snprintf(out[n].host, sizeof(out[n].host), "%s", bar1 + 1);
        out[n].port = p;
        n++;
    }
    if (f) fclose(f);

    /* No saved list: well-known public clusters */
    static const cluster_server_t defaults[] = {
        {"DXFUN", "dxfun.com", 8000},
        {"VE7CC", "dxc.ve7cc.net", 23},
        {"W3LPL", "dxc.w3lpl.net", 7373},
        {"K1TTT", "k1ttt.net", 7373},
        {"NC7J", "dxc.nc7j.com", 7373},
        {"W1NR", "dxc.w1nr.net", 23},
        {"WA9PIE", "dxc.wa9pie.net", 8000},
        {"K4ZR", "k4zr.no-ip.org", 7300},
        {"DB0SUE", "db0sue.de", 8000},
        {"GB7DJK", "gb7djk.dxcluster.net", 7300},
        {"AE5E", "dxspots.com", 23},
        {"HamQTH", "hamqth.com", 7300},
        {"IZ7AUH", "dx.iz7auh.net", 8000},
        {"ON0DXK", "on0dxk.dyndns.org", 8000},
        {"N2WQ", "dxc.n2wq.com", 7300},
    };
    if (n == 0) {
        while (n < max && n < (int)(sizeof(defaults) / sizeof(defaults[0]))) {
            out[n] = defaults[n];
            n++;
        }
    }
    bool listed = false;
    for (int i = 0; i < n; i++) listed |= strcmp(out[i].host, host) == 0 && out[i].port == port;
    if (!listed && n < max) {
        snprintf(out[n].name, sizeof(out[n].name), "%s", host);
        snprintf(out[n].host, sizeof(out[n].host), "%s", host);
        out[n].port = port;
        n++;
    }
    return n;
}

/* Sets the server in use; the saved servers, filters and alerts are kept. */
bool cluster_config_save(const char *host, int port) {
    static const char *keep[] = {"server=", "filter_modes=", "filter_bands=", "filter_activity=", "max_age=",
                                 "skip_spotters=", "alert=", "alert_off=", "alert_popup=", "alert_beep="};
    static char        servers[24 * 1024];
    servers[0] = '\0';
    FILE *in   = fopen(CLUSTER_CONF, "r");
    char  line[512];
    while (in && fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = '\0';
        for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
            if (strncmp(line, keep[i], strlen(keep[i])) == 0) {
                if (strlen(servers) + strlen(line) + 2 < sizeof(servers)) {
                    strcat(servers, line);
                    strcat(servers, "\n");
                }
                break;
            }
    }
    if (in) fclose(in);

    FILE *f = fopen(CLUSTER_CONF ".tmp", "w");
    if (!f) return false;
    fprintf(f, "host=%s\nport=%d\n%s", host, port, servers);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok      = fclose(f) == 0 && ok;
    return ok && rename(CLUSTER_CONF ".tmp", CLUSTER_CONF) == 0;
}

time_t cluster_config_mtime(void) {
    struct stat st;
    return stat(CLUSTER_CONF, &st) == 0 ? st.st_mtime : 0;
}

uint8_t cluster_spot_mode_class(const cluster_spot_t *s) {
    switch (cluster_guess_mode(s)) {
        case CLUSTER_MODE_CW:   return CLUSTER_FM_CW;
        case CLUSTER_MODE_DIGI: return CLUSTER_FM_DIG;
        case CLUSTER_MODE_AM:
        case CLUSTER_MODE_FM:   return CLUSTER_FM_AMFM;
        default:                return CLUSTER_FM_SSB;
    }
}

static uint8_t mode_bit(const char *w) {
    if (strcasecmp(w, "CW") == 0) return CLUSTER_FM_CW;
    if (strcasecmp(w, "SSB") == 0) return CLUSTER_FM_SSB;
    if (strcasecmp(w, "DIG") == 0) return CLUSTER_FM_DIG;
    if (strcasecmp(w, "AMFM") == 0) return CLUSTER_FM_AMFM;
    return 0;
}

/* Whole-string match, * is any run of characters, case-insensitive. */
static bool glob_match(const char *pat, const char *str) {
    const char *star = NULL, *mark = NULL;
    while (*str) {
        if (*pat == '*') {
            star = pat++;
            mark = str;
        } else if (*pat && toupper((unsigned char)*pat) == toupper((unsigned char)*str)) {
            pat++;
            str++;
        } else if (star) {
            pat = star + 1;
            str = ++mark;
        } else {
            return false;
        }
    }
    while (*pat == '*') pat++;
    return *pat == '\0';
}

/* Alerts: the whole call, or, when the pattern has no '/', any part of it between slashes,
 * so IK1ABC also fires for IK1ABC/P and EA8/IK1ABC/M (but not IK1ABCD) */
static bool alert_call_match(const char *pat, const char *call) {
    char part[CLUSTER_CALL_LEN];

    if (glob_match(pat, call)) return true;
    if (strchr(pat, '/') || !strchr(call, '/')) return false;
    snprintf(part, sizeof(part), "%s", call);
    for (char *save, *w = strtok_r(part, "/", &save); w; w = strtok_r(NULL, "/", &save))
        if (glob_match(pat, w)) return true;
    return false;
}

static char *trim(char *s) {
    while (*s == ' ') s++;
    for (size_t n = strlen(s); n > 0 && s[n - 1] == ' '; n--) s[n - 1] = '\0';
    return s;
}

/* "CW,SSB" -> bits; unknown words are ignored */
static uint8_t parse_modes(char *list) {
    uint8_t bits = 0;
    for (char *w = strtok(list, ","); w; w = strtok(NULL, ",")) bits |= mode_bit(trim(w));
    return bits;
}

/* "20,17" -> bits of cluster_band_of() indexes; unknown bands are ignored */
static uint16_t parse_bands(char *list) {
    uint16_t bits = 0;
    for (char *w = strtok(list, ","); w; w = strtok(NULL, ",")) {
        int m = atoi(w);
        for (int b = 0; m > 0 && b < 16; b++)
            if (atoi(cluster_band_name(b)) == m) bits |= (uint16_t)(1u << b);
    }
    return bits;
}

const char *const cluster_act_names[CLUSTER_ACT_COUNT] = {"POTA", "SOTA", "WWBOTA", "HOTA"};

/* "POTA,SOTA" -> CLUSTER_ACT_* bits; unknown words are ignored */
static uint8_t parse_activity(char *list) {
    uint8_t bits = 0;
    for (char *w = strtok(list, ","); w; w = strtok(NULL, ",")) {
        w = trim(w);
        for (int i = 0; i < CLUSTER_ACT_COUNT; i++)
            if (strcasecmp(w, cluster_act_names[i]) == 0) bits |= (uint8_t)(1u << i);
    }
    return bits;
}

static bool all_digits(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)p[i])) return false;
    return n > 0;
}

static bool prefix_ok(const char *p, size_t n) {
    bool letter = false;
    if (n < 1 || n > 4) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)p[i])) return false;
        letter |= isalpha((unsigned char)p[i]) != 0;
    }
    return letter;
}

/* US-1234, IT-0123, K-12345 (not a WWFF IFF-0123, a TOTA tower OKR-0150 or a WCA castle DL-01156, I-14619: the
 * shapes Field Ops tells apart, src/field/flog.c field_ref_prog) */
static bool park_ref(const char *w) {
    const char *dash = strchr(w, '-');
    if (!dash || strchr(w, '/')) return false;
    size_t n = (size_t)(dash - w);
    if (!prefix_ok(w, n) || (n >= 2 && strncmp(dash - 2, "FF", 2) == 0)) return false;
    size_t d = strlen(dash + 1);
    if (d < 4 || d > 5 || !all_digits(dash + 1, d)) return false;
    if (n >= 3 && dash[-1] == 'R' && d == 4) return false;                                /* a tower */
    if (n <= 3 && d == 5 && (dash[1] == '0' || (n == 1 && w[0] == 'I'))) return false;   /* a castle */
    return true;
}

/* I/LO-123, W7W/KG-001, HB/BE-150 */
static bool summit_ref(const char *w) {
    const char *slash = strchr(w, '/');
    if (!slash || !prefix_ok(w, (size_t)(slash - w))) return false;
    const char *r = slash + 1;
    return isalnum((unsigned char)r[0]) && isalnum((unsigned char)r[1]) && r[2] == '-' &&
           strlen(r + 3) == 3 && all_digits(r + 3, 3);
}

/* B/G-0070, B/ON-0733 (WWBOTA) */
static bool bunker_ref(const char *w) {
    const char *dash = strchr(w, '-');
    if (strncmp(w, "B/", 2) != 0 || !dash || !prefix_ok(w + 2, (size_t)(dash - w - 2))) return false;
    return strlen(dash + 1) == 4 && all_digits(dash + 1, 4);
}

/* RO-H0390, IT-H0001 (HOTA) */
static bool hota_ref(const char *w) {
    const char *dash = strchr(w, '-');
    if (!dash || dash[1] != 'H' || !prefix_ok(w, (size_t)(dash - w))) return false;
    size_t d = strlen(dash + 2);
    return d >= 4 && d <= 5 && all_digits(dash + 2, d);
}

uint8_t cluster_spot_activity(const cluster_spot_t *s) {
    char    text[sizeof(s->comment)];
    uint8_t bits = 0;

    for (size_t i = 0; i < sizeof(text); i++) {
        text[i] = (char)toupper((unsigned char)s->comment[i]);
        if (!text[i]) break;
    }
    text[sizeof(text) - 1] = '\0';
    if (strstr(text, "POTA")) bits |= CLUSTER_ACT_POTA;
    if (strstr(text, "SOTA")) bits |= CLUSTER_ACT_SOTA;
    if (strstr(text, "BOTA")) bits |= CLUSTER_ACT_WWBOTA;
    if (strstr(text, "HOTA")) bits |= CLUSTER_ACT_HOTA;
    for (char *w = strtok(text, " ,;:()[]!?\t"); w; w = strtok(NULL, " ,;:()[]!?\t")) {
        if (park_ref(w)) bits |= CLUSTER_ACT_POTA;
        if (summit_ref(w)) bits |= CLUSTER_ACT_SOTA;
        if (bunker_ref(w)) bits |= CLUSTER_ACT_WWBOTA;
        if (hota_ref(w)) bits |= CLUSTER_ACT_HOTA;
    }
    return bits;
}

static void filter_line(cluster_filter_t *f, char *line) {
    if (strncmp(line, "filter_modes=", 13) == 0) {
        f->modes = parse_modes(line + 13);
    } else if (strncmp(line, "filter_bands=", 13) == 0) {
        f->bands = parse_bands(line + 13);
    } else if (strncmp(line, "filter_activity=", 16) == 0) {
        f->activity = parse_activity(line + 16);
    } else if (strncmp(line, "max_age=", 8) == 0) {
        int v      = atoi(line + 8);
        f->max_age = (uint16_t)(v > 0 && v < 1440 ? v : 0);
    } else if (strncmp(line, "skip_spotters=", 14) == 0) {
        snprintf(f->skip, sizeof(f->skip), "%s", line + 14);
    } else if (strncmp(line, "alert_popup=", 12) == 0) {
        f->popup = atoi(line + 12) != 0;
    } else if (strncmp(line, "alert_beep=", 11) == 0) {
        f->beep = atoi(line + 11) != 0;
    } else if (strncmp(line, "alert=", 6) == 0 && f->alert_count < CLUSTER_MAX_ALERTS) {
        char *bar1 = strchr(line + 6, '|');
        char *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
        if (!bar2) return;
        *bar1 = *bar2 = '\0';
        char *pat     = trim(line + 6);
        if (!*pat || strlen(pat) >= sizeof(f->alerts[0].pattern)) return;
        cluster_alert_t *a = &f->alerts[f->alert_count++];
        snprintf(a->pattern, sizeof(a->pattern), "%s", pat);
        a->bands = parse_bands(bar1 + 1);
        a->mode  = mode_bit(trim(bar2 + 1));
    }
}

void cluster_filter_load(cluster_filter_t *f) {
    uint32_t gen = f->gen;
    memset(f, 0, sizeof(*f));
    f->gen   = gen + 1;
    f->modes = CLUSTER_FM_ALL;

    FILE *fp = fopen(CLUSTER_CONF, "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        filter_line(f, line);
    }
    fclose(fp);
    if (!f->modes) f->modes = CLUSTER_FM_ALL; /* a list with no known mode is not "hide everything" */
}

static time_t filter_loaded = -1;

bool cluster_filter_save(uint8_t modes, uint16_t bands, uint8_t activity) {
    static const char *drop[] = {"filter_modes=", "filter_bands=", "filter_activity="};
    static char        rest[24 * 1024];
    char               line[512];

    rest[0]  = '\0';
    FILE *in = fopen(CLUSTER_CONF, "r");
    while (in && fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = '\0';
        bool skip = !line[0];
        for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); i++)
            skip |= strncmp(line, drop[i], strlen(drop[i])) == 0;
        if (!skip && strlen(rest) + strlen(line) + 2 < sizeof(rest)) {
            strcat(rest, line);
            strcat(rest, "\n");
        }
    }
    if (in) fclose(in);

    FILE *f = fopen(CLUSTER_CONF ".tmp", "w");
    if (!f) return false;
    fputs(rest, f);
    modes &= CLUSTER_FM_ALL;
    if (modes && modes != CLUSTER_FM_ALL) {
        static const char *names[] = {"CW", "SSB", "DIG", "AMFM"};
        const char        *sep     = "";
        fputs("filter_modes=", f);
        for (int i = 0; i < 4; i++)
            if (modes & (1 << i)) {
                fprintf(f, "%s%s", sep, names[i]);
                sep = ",";
            }
        fputs("\n", f);
    }
    if (bands) {
        const char *sep = "";
        fputs("filter_bands=", f);
        for (int b = 0; b < 16; b++)
            if (bands & (1u << b) && atoi(cluster_band_name(b)) > 0) {
                fprintf(f, "%s%d", sep, atoi(cluster_band_name(b)));
                sep = ",";
            }
        fputs("\n", f);
    }
    if (activity) {
        const char *sep = "";
        fputs("filter_activity=", f);
        for (int i = 0; i < CLUSTER_ACT_COUNT; i++)
            if (activity & (1u << i)) {
                fprintf(f, "%s%s", sep, cluster_act_names[i]);
                sep = ",";
            }
        fputs("\n", f);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok      = fclose(f) == 0 && ok;
    ok      = ok && rename(CLUSTER_CONF ".tmp", CLUSTER_CONF) == 0;
    filter_loaded = -1; /* read again even within the same second */
    return ok;
}

const cluster_filter_t *cluster_filter(void) {
    static cluster_filter_t f;
    time_t                  m = cluster_config_mtime();

    if (m != filter_loaded) {
        filter_loaded = m;
        cluster_filter_load(&f);
    }
    return &f;
}

int cluster_alert_match(const cluster_filter_t *f, const cluster_spot_t *s) {
    int     band = cluster_band_of(s->freq_hz);
    uint8_t mode = 0;

    for (int i = 0; i < f->alert_count; i++) {
        const cluster_alert_t *a = &f->alerts[i];
        if (!alert_call_match(a->pattern, s->dx)) continue;
        if (a->bands && (band < 0 || !(a->bands & (1u << band)))) continue;
        if (a->mode) {
            if (!mode) mode = cluster_spot_mode_class(s);
            if (a->mode != mode) continue;
        }
        return i;
    }
    return -1;
}

bool cluster_filter_pass(const cluster_filter_t *f, const cluster_spot_t *s, uint16_t now_utc_min) {
    if (cluster_alert_match(f, s) >= 0) return true;
    if (!(f->modes & cluster_spot_mode_class(s))) return false;
    if (f->bands) {
        int band = cluster_band_of(s->freq_hz);
        if (band < 0 || !(f->bands & (1u << band))) return false;
    }
    if (f->activity && !(f->activity & cluster_spot_activity(s))) return false;
    if (f->max_age) {
        uint16_t age = cluster_age_min(s->utc_min, now_utc_min);
        if (age > f->max_age && age < 1440 - 5) return false;
    }
    if (f->skip[0]) {
        char list[sizeof(f->skip)];
        snprintf(list, sizeof(list), "%s", f->skip);
        for (char *w = strtok(list, ","); w; w = strtok(NULL, ",")) {
            w = trim(w);
            if (*w && glob_match(w, s->spotter)) return false;
        }
    }
    return true;
}

bool cluster_parse_server(const char *text, char *host, size_t host_size, int *port) {
    while (*text == ' ') text++;
    const char *colon = strrchr(text, ':');
    size_t      n     = colon ? (size_t)(colon - text) : strlen(text);
    while (n > 0 && text[n - 1] == ' ') n--;
    if (n == 0 || n >= host_size) return false;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum((unsigned char)text[i]) || text[i] == '.' || text[i] == '-')) return false;
    int p = CLUSTER_DEFAULT_PORT;
    if (colon) {
        char *end;
        long  v = strtol(colon + 1, &end, 10);
        while (*end == ' ') end++;
        if (end == colon + 1 || *end || v <= 0 || v > 65535) return false;
        p = (int)v;
    }
    memcpy(host, text, n);
    host[n] = '\0';
    *port   = p;
    return true;
}

void cluster_client_feed_line(const char *line) {
    cluster_spot_t spot;
    if (!cluster_parse_line(line, &spot)) return;
    if (spot.when && time(NULL) - spot.when > MAX_AGE_MIN * 60) return; /* old sh/dx row */
    pthread_mutex_lock(&lock);
    cluster_store_add(&store, &spot);
    cluster_store_expire(&store, utc_now_min(), MAX_AGE_MIN);
    store_gen++;
    pthread_mutex_unlock(&lock);
}

int cluster_client_snapshot(cluster_spot_t *out, int max, uint32_t *gen) {
    static uint16_t last_expire = 0xffff;
    int             n           = -1;
    uint16_t        now         = utc_now_min();

    pthread_mutex_lock(&lock);
    if (now != last_expire) {
        int before = store.count;
        cluster_store_expire(&store, now, MAX_AGE_MIN);
        if (store.count != before) store_gen++;
        last_expire = now;
    }
    if (*gen != store_gen) {
        n = store.count < max ? store.count : max;
        memcpy(out, store.spots + (store.count - n), (size_t)n * sizeof(*out));
        *gen = store_gen;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

cluster_state_t cluster_client_state(char *detail, size_t size) {
    pthread_mutex_lock(&lock);
    cluster_state_t s = state;
    if (detail) snprintf(detail, size, "%s", state_detail);
    pthread_mutex_unlock(&lock);
    return s;
}

/* Sleeps in small steps so that stop requests are served quickly. */
static void wait_ms(int ms) {
    for (int t = 0; t < ms && !ABORTED(); t += 500) usleep(500000);
}

static int open_socket(const char *host, int port_num) {
    struct addrinfo hints = {0}, *res = NULL;
    char            port[8];
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", port_num);

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        set_state(CLUSTER_FAILED, "no DNS (WiFi?)");
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0 && !ABORTED(); ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            int           so  = 1;
            socklen_t     len = sizeof(so);
            for (int t = 0; t < CONNECT_TIMEOUT_MS && !ABORTED(); t += 250) {
                if (poll(&pfd, 1, 250) != 0) break;
            }
            if ((pfd.revents & POLLOUT) && getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &len) == 0 && so == 0) break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && !ABORTED()) set_state(CLUSTER_FAILED, "no answer");
    return fd;
}

static void send_text(int fd, const char *text) {
    size_t n = strlen(text);
    while (n > 0) {
        ssize_t w = send(fd, text, n, MSG_NOSIGNAL);
        if (w > 0) {
            text += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (poll(&pfd, 1, 1000) > 0) continue;
        }
        return;
    }
}

/* Telnet commands (IAC ...) are dropped, never answered: clusters work
 * without option negotiation. */
static size_t strip_telnet(unsigned char *buf, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0xFF && i + 1 < n) {
            unsigned char cmd = buf[i + 1];
            if (cmd == 0xFF) {
                buf[o++] = 0xFF;
                i++;
            } else if (cmd >= 0xFB && cmd <= 0xFE) {
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        buf[o++] = buf[i];
    }
    return o;
}

static bool looks_like_login_prompt(const char *text) {
    return strcasestr(text, "login") || strcasestr(text, "call") || strcasestr(text, "enter your");
}

/* Returns true when the session lasted long enough to reset the backoff. */
static bool session(int fd, const char *call) {
    char          line[LINE_MAX_LEN];
    size_t        len        = 0;
    bool          logged_in  = false;
    int64_t       started    = now_ms();
    int64_t       login_at   = 0;
    bool          asked_spots = false;
    bool          spider      = false; /* DXSpider wants "sh/dx 50", the others "sh/dx/50" */
    int64_t       last_rx     = started;
    unsigned char buf[1024];

    set_state(CLUSTER_CONNECTED, "");

    while (!ABORTED()) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int           r   = poll(&pfd, 1, 1000);
        int64_t       now = now_ms();

        if (!logged_in && now - started > LOGIN_FALLBACK_MS) {
            send_text(fd, call);
            send_text(fd, "\r\n");
            logged_in = true;
            login_at  = now;
        }
        if (logged_in && !asked_spots && now - login_at > SHOW_DX_DELAY_MS) {
            /* The latest spots at once, not only the new ones from now on. */
            send_text(fd, spider ? "sh/dx " SHOW_DX_COUNT "\r\n" : "sh/dx/" SHOW_DX_COUNT "\r\n");
            asked_spots = true;
        }
        if (logged_in && asked_spots && spot_sent != spot_seq) {
            char cmd[sizeof(spot_cmd)];
            pthread_mutex_lock(&lock);
            memcpy(cmd, spot_cmd, sizeof(cmd));
            spot_sent = spot_seq;
            bool fresh = now - spot_at < SPOT_VALID_MS;
            pthread_mutex_unlock(&lock);
            if (fresh && cmd[0]) send_text(fd, cmd);
        }
        if (now - last_rx > IDLE_TIMEOUT_MS) break;
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;

        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        last_rx = now;
        n       = (ssize_t)strip_telnet(buf, (size_t)n);

        for (ssize_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    line[len] = '\0';
                    if (strcasestr(line, "DXSpider")) spider = true;
                    cluster_client_feed_line(line);
                    len = 0;
                }
                continue;
            }
            if (c == '\a') continue;
            if (len < sizeof(line) - 1) line[len++] = c;
        }

        /* The login prompt has no newline after it. */
        if (!logged_in && len > 0) {
            line[len] = '\0';
            if (looks_like_login_prompt(line)) {
                send_text(fd, call);
                send_text(fd, "\r\n");
                logged_in = true;
                login_at  = now_ms();
                len       = 0;
            }
        }
    }
    close(fd);
    if (!ABORTED()) set_state(CLUSTER_FAILED, "connection closed");
    return now_ms() - started > STABLE_MS;
}

static void *thread_cb(void *arg) {
    (void)arg;
    int delay = RETRY_MIN_MS;
    for (;;) {
        while (!stop_requested) {
            char host[sizeof(host_name)], call[sizeof(login_call)];
            int  port;
            pthread_mutex_lock(&lock);
            reconnect_requested = false; /* under the lock that start() sets it with */
            memcpy(host, host_name, sizeof(host));
            memcpy(call, login_call, sizeof(call));
            port = host_port;
            pthread_mutex_unlock(&lock);

            set_state(CLUSTER_CONNECTING, "");
            int fd = open_socket(host, port);
            if (fd >= 0 && session(fd, call)) delay = RETRY_MIN_MS;
            /* No network: retry calmly, with a longer wait each time. */
            if (!reconnect_requested) {
                wait_ms(delay);
                delay = delay * 2 < RETRY_MAX_MS ? delay * 2 : RETRY_MAX_MS;
            }
            if (reconnect_requested) delay = RETRY_MIN_MS;
        }
        /* Restarted while stopping: carry on instead of exiting. */
        pthread_mutex_lock(&lock);
        if (stop_requested) {
            thread_running = false;
            state          = CLUSTER_STOPPED;
            state_detail[0] = '\0';
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        pthread_mutex_unlock(&lock);
    }
}

/* Another server, as thread_cb() but without touching the state shown */
static void *extra_cb(void *arg) {
    int i     = (int)(intptr_t)arg;
    int delay = RETRY_MIN_MS;

    extra_link = true;
    while (!stop_requested) {
        char host[sizeof(extra_host[0])], call[sizeof(login_call)];
        int  port;
        pthread_mutex_lock(&lock);
        my_gen = extras_gen;
        memcpy(host, extra_host[i], sizeof(host));
        memcpy(call, login_call, sizeof(call));
        port = extra_port[i];
        pthread_mutex_unlock(&lock);

        int fd = host[0] ? open_socket(host, port) : -1;
        if (fd >= 0 && session(fd, call)) delay = RETRY_MIN_MS;
        for (int t = 0; t < delay && !stop_requested && my_gen == extras_gen; t += 500) usleep(500000);
        delay = my_gen != extras_gen ? RETRY_MIN_MS : (delay * 2 < RETRY_MAX_MS ? delay * 2 : RETRY_MAX_MS);
    }
    return NULL;
}

/* pota.app's, wwbota.org's and cqhota.app's activator spots, a minute apart, through POTA_SPOTS */
static void *pota_cb(void *arg) {
    (void)arg;
    extra_link = true;
    while (!stop_requested) {
        FILE *f = popen(POTA_SPOTS " 2>/dev/null", "r");
        char  line[256];
        while (f && fgets(line, sizeof(line), f)) {
            char          *fld[5];
            char          *w = line;
            cluster_spot_t sp;
            int            n;
            line[strcspn(line, "\r\n")] = 0;
            for (n = 0; n < 5 && w; n++) {
                fld[n] = w;
                if ((w = strchr(w, '|'))) *w++ = 0;
            }
            if (n < 5 || strlen(fld[4]) != 4) continue;
            memset(&sp, 0, sizeof(sp));
            sp.freq_hz = atoi(fld[0]);
            snprintf(sp.dx, sizeof(sp.dx), "%s", fld[1]);
            snprintf(sp.spotter, sizeof(sp.spotter), "%s", fld[2]);
            snprintf(sp.comment, sizeof(sp.comment), "%s", fld[3]);
            sp.utc_min = (uint16_t)(atoi(fld[4]) / 100 * 60 + atoi(fld[4]) % 100);
            if (sp.freq_hz <= 0 || !sp.dx[0] || cluster_age_min(sp.utc_min, utc_now_min()) > MAX_AGE_MIN) continue;
            pthread_mutex_lock(&lock);
            cluster_store_add(&store, &sp);
            store_gen++;
            pthread_mutex_unlock(&lock);
        }
        if (f) pclose(f);
        for (int t = 0; t < POTA_POLL_MS && !stop_requested; t += 500) usleep(500000);
    }
    return NULL;
}

static void start_thread(void *(*fn)(void *), void *arg) {
    pthread_t      t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
}

/* The other servers: the first EXTRA_LINKS of the list on another host (under lock) */
static void pick_extras(const char *main_host) {
    cluster_server_t list[CLUSTER_MAX_SERVERS];
    int              n = cluster_servers_load(list, CLUSTER_MAX_SERVERS), k = 0;
    char             host[EXTRA_LINKS][128] = {{0}};
    int              port[EXTRA_LINKS]      = {0};

    for (int i = 0; i < n && k < EXTRA_LINKS; i++) {
        if (strcasecmp(list[i].host, main_host) == 0) continue;
        snprintf(host[k], sizeof(host[k]), "%s", list[i].host);
        port[k++] = list[i].port;
    }
    if (memcmp(host, extra_host, sizeof(host)) || memcmp(port, extra_port, sizeof(port))) {
        memcpy(extra_host, host, sizeof(host));
        memcpy(extra_port, port, sizeof(port));
        extras_gen++;
    }
}

/* The thread is detached and never joined: a DNS lookup or a connect can
 * block for seconds, and closing the app must not wait for it. */
void cluster_client_start(const char *host, int port, const char *callsign) {
    pthread_mutex_lock(&lock);
    if (strcmp(host_name, host) != 0 || strcmp(login_call, callsign) != 0 || host_port != port)
        reconnect_requested = true; /* the running session picks up the change */
    if (strcmp(login_call, callsign) != 0) extras_gen++;
    pick_extras(host);
    snprintf(host_name, sizeof(host_name), "%s", host);
    snprintf(login_call, sizeof(login_call), "%s", callsign);
    host_port      = port;
    stop_requested = false;
    if (!thread_running) {
        pthread_t      t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, thread_cb, NULL) == 0) {
            thread_running = true;
        } else {
            state = CLUSTER_FAILED;
            snprintf(state_detail, sizeof(state_detail), "no thread");
        }
        pthread_attr_destroy(&attr);
    }
    if (!extras_running) {
        extras_running = true;
        for (int i = 0; i < EXTRA_LINKS; i++) start_thread(extra_cb, (void *)(intptr_t)i);
        start_thread(pota_cb, NULL);
    }
    pthread_mutex_unlock(&lock);
}

bool cluster_client_send_spot(double khz, const char *call, const char *comment) {
    pthread_mutex_lock(&lock);
    bool ok = thread_running && !stop_requested;
    if (ok) {
        snprintf(spot_cmd, sizeof(spot_cmd), "DX %.1f %s %s\r\n", khz, call, comment);
        spot_seq++;
        spot_at = now_ms();
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

void cluster_client_stop(void) {
    stop_requested = true;
}

void cluster_client_retry(void) {
    pthread_mutex_lock(&lock);
    if (thread_running && state == CLUSTER_FAILED) reconnect_requested = true;
    pthread_mutex_unlock(&lock);
}
