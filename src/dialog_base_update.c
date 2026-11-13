/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The X6200's base firmware from the radio's own screen: Settings > Info >
 *  Base firmware. What tools/base_update does from a computer, here:
 *    - the list: the releases known (/usr/share/x6100/base_releases.txt), each "here" when its file is in
 *      /mnt/base_update (Xiegu's own system's /usr/firmware on the internal memory is copied there first), "on
 *      xiegu.eu" when it can be fetched; any other .xgf found there as an unknown file (base_flash --identify
 *      --unknown-ok: a multiple of 16 bytes, 16 to 512 KiB, not a plain ARM image), and releases xiegu.eu has that the
 *      list does not (the last check). The MFK chooses one;
 *    - Get: x6x00-base-fetch streams Xiegu's update zip from xiegu.eu and keeps only the base file, checked;
 *    - Check: xiegu.eu's downloads page against the list and this base (the daily check's own, S99basecheck);
 *    - Flash: base_flash (/usr/sbin/x6x00-base-flash: Xiegu's own app 1.0.8, step for step) as a child, the base lent
 *      to it meanwhile (radio_base_hold: the radio thread off /dev/ttyS1, every write off the base), as Xiegu's app
 *      runs its worker with its own reading of the port paused. An unknown file asks twice.
 *  The log: the children's lines, every kernel message from the moment a write starts (/dev/kmsg), and the base's
 *  serial port: the new firmware's stream starting (it runs once 9 to 12 kB/s come in) and stopping (the base then
 *  switches the radio off, about three minutes after: switch it on again). All of it also in /mnt/base_update.log,
 *  flushed to the card each second, so a power-off leaves it there; the window shows its end when opened again.
 */

#define _GNU_SOURCE

#include "dialog_base_update.h"

#include "button_bar.h"
#include "clock.h"
#include "gui_events.h"
#include "key_input.h"
#include "msg_box.h"
#include "radio.h"
#include "screen_light.h"
#include "theme.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FLASHER     "/usr/sbin/x6x00-base-flash"
#define FETCHER     "/usr/sbin/x6x00-base-fetch"
#define RELEASES    "/usr/share/x6100/base_releases.txt"
#define BASE_DIR    "/mnt/base_update"
#define CHECK_JSON  BASE_DIR "/check.json"
#define ANNOUNCED   BASE_DIR "/announced"
#define LOG_PATH    "/mnt/base_update.log"
#define EMMC_DEV    "/dev/mmcblk2p2"                /* Xiegu's own system, on the internal memory */
#define EMMC_AT     "/tmp/x6x00-emmc-ro"
#define TEXT_SIZE   32768                           /* the log kept on screen; whole lines go from the top */
#define TICK_MS     100
#define PAD         4
#define MAX_ITEMS   16
#define LIST_ROWS   6
#define STREAM_BPS  2000                            /* more than this on /dev/ttyS1: the base's firmware is running */

extern const lv_font_t codec_18;

typedef enum { ST_SCAN, ST_IDLE, ST_BUSY, ST_ARMED_UNKNOWN, ST_ARMED, ST_FLASHING, ST_WRITTEN, ST_FAILED } state_t;
typedef enum { CH_NONE, CH_GET, CH_CHECK, CH_FLASH } child_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void page_cb(button_data_t *b);
static void get_cb(button_data_t *b);
static void check_cb(button_data_t *b);
static void flash_cb(button_data_t *b);
static void close_cb(button_data_t *b);

static button_data_t btn_page  = {.type = BTN_TEXT, .label = "(BASE FW)", .press = page_cb};
static button_data_t btn_get   = {.type = BTN_TEXT, .label = "Get", .press = get_cb};
static button_data_t btn_check = {.type = BTN_TEXT, .label = "Check", .press = check_cb};
static button_data_t btn_flash = {.type = BTN_TEXT, .label = "Flash", .press = flash_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static buttons_page_t page = {.items = {&btn_page, &btn_get, &btn_check, &btn_flash, &btn_close}};

static dialog_t dialog = {.construct_cb = construct_cb, .destruct_cb = destruct_cb, .btn_page = &page};

dialog_t *dialog_base_update = &dialog;

/* A row of the list */
typedef struct {
    char ver[16], built[16], name[64], path[200], link[160], sha[65];
    bool on_site, own, unknown, bad;    /* fetchable; a factory build, not on xiegu.eu; not in the list; not an .xgf */
} item_t;

static lv_obj_t   *body, *head, *list, *scroll, *label;
static lv_timer_t *timer;
static state_t     st;
static char        text[TEXT_SIZE];
static size_t      len;
static FILE       *logf;
static time_t      log_synced;

static item_t          items[MAX_ITEMS];
static int             n_items, sel;
static pthread_t       scan_thread;
static bool            scan_started;
static pthread_mutex_t lines_lock = PTHREAD_MUTEX_INITIALIZER;
static char            lines[4096];             /* the scan thread's lines, for the LVGL thread */
static volatile bool   scan_done;

static item_t           armed;                  /* the file armed or being written */
static child_t          ch;
static pid_t            child = -1;
static int              child_out = -1;
static char             child_line[512];
static size_t           child_len;
static int              kmsg = -1;
static struct timespec  t_start, t_written;
static unsigned long    rx_last;
static bool             rx_have, streaming, stream_seen;
static int              last_rc = -1;
static bool             recover, lent;          /* lent: the base lent (radio_base_hold) from a write to the power-off */
static char             check_line[200];

/* ---- the log: on screen and on the card -------------------------------------------------------------------------- */

static void put_text(const char *s) {
    size_t n = strlen(s);

    if (n >= TEXT_SIZE / 2) {
        s += n - TEXT_SIZE / 2;
        n = TEXT_SIZE / 2;
    }
    if (len + n + 1 >= TEXT_SIZE) {             /* full: the oldest lines go */
        char  *nl  = memchr(text + TEXT_SIZE / 4, '\n', len - TEXT_SIZE / 4);
        size_t cut = nl ? (size_t)(nl - text) + 1 : TEXT_SIZE / 4;

        memmove(text, text + cut, len - cut);
        len -= cut;
    }
    memcpy(text + len, s, n);
    len += n;
    text[len] = '\0';
}

static void show(void) {
    if (!label) return;
    lv_label_set_text(label, text);
    lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static double since(const struct timespec *t) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) + (now.tv_nsec - t->tv_nsec) / 1e9;
}

/* A line: on screen (with the seconds since the write began, while it runs and after) and on the card (UTC) */
static void say(const char *fmt, ...) {
    char    msg[600], line[700];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (st >= ST_FLASHING || lent) {
        snprintf(line, sizeof(line), "%6.1f %s\n", since(&t_start), msg);
    } else {
        snprintf(line, sizeof(line), "%s\n", msg);
    }
    put_text(line);
    if (logf) {
        time_t    t = time(NULL);
        struct tm tm;
        char      ts[32];

        gmtime_r(&t, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(logf, "%s UTC %s\n", ts, msg);
        fflush(logf);
    }
}

static void log_open(void) {
    if (!logf) logf = fopen(LOG_PATH, "a");
}

/* Each second while the base is written or awaited: what was said is on the card before a power-off can take it */
static void log_sync(void) {
    time_t t = time(NULL);

    if (logf && t != log_synced) {
        fflush(logf);
        fsync(fileno(logf));
        log_synced = t;
    }
}

/* ---- the daily check's result (check.json, x6x00-base-fetch check) -------------------------------------------------- */

/* A string value of check.json's ("key": "value"), "" when there is none */
static void json_str(const char *json, const char *key, char *out, size_t n) {
    char        pat[64];
    const char *p;

    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    if ((p = strstr(json, pat))) {
        p += strlen(pat);
        size_t l = strcspn(p, "\"");
        snprintf(out, n, "%.*s", (int)(l < n ? l : n - 1), p);
    }
}

static bool json_true(const char *json, const char *key) {
    char        pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\": ", key);
    return (p = strstr(json, pat)) && !strncmp(p + strlen(pat), "true", 4);
}

static bool read_check(char *json, size_t n) {
    FILE  *f = fopen(CHECK_JSON, "r");
    size_t r;

    if (!f) return false;
    r = fread(json, 1, n - 1, f);
    fclose(f);
    json[r] = '\0';
    return r > 0;
}

static void check_summary(void) {
    char json[4096], when[40], latest[16];

    check_line[0] = '\0';
    if (!read_check(json, sizeof(json))) {
        snprintf(check_line, sizeof(check_line), "xiegu.eu: not checked yet (Check, or the daily check once online)");
        return;
    }
    json_str(json, "checked", when, sizeof(when));
    json_str(json, "latest", latest, sizeof(latest));
    snprintf(check_line, sizeof(check_line), "xiegu.eu, %s: latest %s%s", when, latest,
             json_true(json, "newer") ? " - NEWER than this base" : "");
}

void base_update_status(char *buf, size_t n) {
    char json[4096], latest[16];

    buf[0] = '\0';
    if (!read_check(json, sizeof(json))) return;
    json_str(json, "latest", latest, sizeof(latest));
    if (json_true(json, "newer")) {
        snprintf(buf, n, "%s out", latest);
    } else if (latest[0]) {
        snprintf(buf, n, "latest");
    }
}

/* Every ten minutes, the first a minute after the start: a newer release the daily check found, said once */
static void watch_cb(lv_timer_t *t) {
    char  json[4096], latest[16], seen[16] = "";
    FILE *f;

    lv_timer_set_period(t, 600000);
    if (!read_check(json, sizeof(json)) || !json_true(json, "newer")) return;
    json_str(json, "latest", latest, sizeof(latest));
    if ((f = fopen(ANNOUNCED, "r"))) {
        if (fgets(seen, sizeof(seen), f)) seen[strcspn(seen, "\r\n")] = '\0';
        fclose(f);
    }
    if (!latest[0] || !strcmp(seen, latest)) return;
    msg_schedule_long_text_fmt("Base firmware %s is out: Settings > Info > Base firmware", latest);
    if ((f = fopen(ANNOUNCED, "w"))) {
        fprintf(f, "%s\n", latest);
        fclose(f);
    }
}

void base_update_watch_init(void) {
    lv_timer_create(watch_cb, 60000, NULL);
}

/* ---- the header and the list ------------------------------------------------------------------------------------- */

static void power_text(char *s, size_t n) {
    float   ext, bat;
    uint8_t cap;
    bool    chg;

    clock_power_get(&ext, &bat, &cap, &chg);
    if (ext < 1.0f && bat < 3.0f) {             /* the GUI's first seconds: nothing read from the base yet */
        snprintf(s, n, "not read yet");
        return;
    }
    snprintf(s, n, "ext %.1f V, battery %s", ext, bat >= 3.0f ? (chg ? "in, charging" : "in") : "out");
    if (bat >= 3.0f) {
        size_t l = strlen(s);
        snprintf(s + l, n - l, " (%.1f V %d%%)", bat, cap);
    }
}

static void head_show(const char *status) {
    char pw[96], s[600];

    if (!head) return;
    power_text(pw, sizeof(pw));
    snprintf(s, sizeof(s), "BASE FIRMWARE   this base: %s%s%s   power: %s\n%s", radio_base_ver_str(),
             st >= ST_FLASHING ? ", writing " : "", st >= ST_FLASHING ? armed.ver : "", pw,
             status && status[0] ? status : check_line);
    lv_label_set_text(head, s);
}

static void list_show(void) {
    char s[2048] = "";
    int  first = sel >= LIST_ROWS ? sel - LIST_ROWS + 1 : 0;

    if (!list) return;
    if (!n_items) {
        snprintf(s, sizeof(s), st == ST_SCAN ? "  (looking for the files)" : "  (no base files)");
    }
    for (int i = first; i < n_items && i < first + LIST_ROWS; i++) {
        const item_t *it = &items[i];
        const char   *where = it->bad       ? "NOT AN .XGF "
                              : it->path[0] ? "HERE        "
                              : it->on_site ? "ON XIEGU.EU "
                                            : "            ";
        size_t        l     = strlen(s);

        snprintf(s + l, sizeof(s) - l, "%s %-7s %-10s %s %s%s%s%s\n", i == sel ? ">" : " ",
                 it->ver[0] ? it->ver : "?", it->built, where, it->name[0] ? it->name : "-",
                 it->own ? "  (factory build)" : "", it->unknown ? "  UNKNOWN" : "",
                 !strcmp(it->ver, radio_base_ver_str()) && !it->unknown ? "  <- NOW" : "");
    }
    if (n_items > LIST_ROWS) {
        size_t l = strlen(s);
        snprintf(s + l, sizeof(s) - l, "  (%d of %d: the MFK goes through them)", sel + 1, n_items);
    }
    lv_label_set_text(list, s);
}

/* ---- the files (a thread of its own: a mount, a copy, a few program runs) ------------------------------------------ */

static void scan_say(const char *fmt, ...) {
    char    msg[400];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&lines_lock);
    size_t l = strlen(lines);
    snprintf(lines + l, sizeof(lines) - l, "%s\n", msg);
    pthread_mutex_unlock(&lines_lock);
}

/* base_flash --identify --unknown-ok: its version ("1.0.8", "unknown") and SHA-256; ver "" for a file it would not
 * write (not an .xgf) */
static void identify(const char *path, char *ver, size_t n, char *sha) {
    char  cmd[320], out[160] = "", v[16] = "";
    FILE *p;

    ver[0] = sha[0] = '\0';
    snprintf(cmd, sizeof(cmd), FLASHER " --identify '%s' --unknown-ok 2>/dev/null | grep '^xiegu-base '", path);
    if ((p = popen(cmd, "r"))) {
        if (fgets(out, sizeof(out), p) && sscanf(out, "xiegu-base %15s %64s", v, sha) == 2) {
            snprintf(ver, n, "%s", v);
        }
        pclose(p);
    }
}

static bool copy_file(const char *from, const char *to) {
    char    tmp[260], buf[16384];
    int     in = open(from, O_RDONLY), out;
    ssize_t r;
    bool    ok = true;

    if (in < 0) return false;
    snprintf(tmp, sizeof(tmp), "%s.part", to);
    if ((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
        close(in);
        return false;
    }
    while ((r = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, r) != r) ok = false;
    ok = ok && r == 0 && fsync(out) == 0;
    close(in);
    close(out);
    if (ok) ok = rename(tmp, to) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

/* A file with this SHA-256 in BASE_DIR already, whatever its name */
static bool here_already(const char *sha) {
    DIR           *d = opendir(BASE_DIR);
    struct dirent *e;
    char           path[300], ver[16], s[65];
    bool           found = false;

    if (!d) return false;
    while (!found && (e = readdir(d))) {
        size_t l = strlen(e->d_name);

        if (l < 5 || strcmp(e->d_name + l - 4, ".xgf")) continue;
        snprintf(path, sizeof(path), BASE_DIR "/%s", e->d_name);
        identify(path, ver, sizeof(ver), s);
        found = s[0] && !strcmp(s, sha);
    }
    closedir(d);
    return found;
}

static item_t *add_item(void) {
    if (n_items >= MAX_ITEMS) return NULL;
    memset(&items[n_items], 0, sizeof(items[0]));
    return &items[n_items++];
}

static void *scan_main(void *arg) {
    DIR           *d;
    struct dirent *e;
    char           path[300], to[320], line[512], json[4096];
    FILE          *f;

    (void)arg;
    if (access(FLASHER, X_OK)) {
        scan_say("%s is not on this image: the base can't be written from here.", FLASHER);
        scan_done = true;
        return NULL;
    }
    mkdir(BASE_DIR, 0755);
    /* Xiegu's own system keeps its base files for its UPGRADE screen: copied here once, read-only meanwhile. Once by
     * its SHA-256: a file already here under any name is not copied again, nor is a file of the same name overwritten */
    mkdir(EMMC_AT, 0755);
    if (system("grep -q '^" EMMC_DEV " ' /proc/mounts || mount -o ro,noload " EMMC_DEV " " EMMC_AT " 2>/dev/null") == 0 &&
        (d = opendir(EMMC_AT "/usr/firmware"))) {
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);

            if (l < 5 || strcmp(e->d_name + l - 4, ".xgf")) continue;
            char ver[16], sha[65];

            snprintf(path, sizeof(path), EMMC_AT "/usr/firmware/%s", e->d_name);
            snprintf(to, sizeof(to), BASE_DIR "/%s", e->d_name);
            identify(path, ver, sizeof(ver), sha);
            if (sha[0] ? here_already(sha) : !access(to, F_OK)) continue;
            if (!access(to, F_OK)) snprintf(to, sizeof(to), BASE_DIR "/%.*s-%.8s.xgf", (int)(l - 4), e->d_name, sha);
            if (copy_file(path, to)) scan_say("copied from Xiegu's system: %s", strrchr(to, '/') + 1);
        }
        closedir(d);
        if (system("umount " EMMC_AT " 2>/dev/null")) scan_say("(the internal memory could not be unmounted)");
    }
    /* Every .xgf here, whatever its name: its version and SHA-256. Files are told apart by their sum, not their name or
     * date: a copy under another name is the same file, and the two 1.0.7 builds are two */
    static struct {
        char name[64], ver[16], sha[65];
        bool used;
    } here[MAX_ITEMS];
    int n_here = 0;

    if ((d = opendir(BASE_DIR))) {
        while ((e = readdir(d)) && n_here < MAX_ITEMS) {
            size_t l = strlen(e->d_name);

            if (l < 5 || strcmp(e->d_name + l - 4, ".xgf") || l >= sizeof(here[0].name)) continue;
            snprintf(here[n_here].name, sizeof(here[0].name), "%s", e->d_name);
            snprintf(path, sizeof(path), BASE_DIR "/%s", e->d_name);
            identify(path, here[n_here].ver, sizeof(here[0].ver), here[n_here].sha);
            here[n_here].used = false;
            if (!here[n_here].ver[0]) {
                scan_say("%s: not an .xgf base_flash would write (size, AES blocks, or a plain image)", e->d_name);
            }
            n_here++;
        }
        closedir(d);
    }
    /* The releases known, in the list's order (newest first), each here when a file's sum is its */
    if ((f = fopen(RELEASES, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            char    ver[16], built[16], name[64], size[16], sha[80], zsize[16], zsha[80], url[300];
            item_t *it;

            if (line[0] == '#' || sscanf(line, "%15s %15s %63s %15s %79s %15s %79s %299s", ver, built, name, size, sha,
                                         zsize, zsha, url) != 8)
                continue;
            if (!(it = add_item())) break;
            snprintf(it->ver, sizeof(it->ver), "%s", ver);
            snprintf(it->built, sizeof(it->built), "%s", built);
            snprintf(it->name, sizeof(it->name), "%s", name);
            snprintf(it->sha, sizeof(it->sha), "%s", sha);
            it->on_site = strcmp(url, "-") != 0;
            it->own     = !it->on_site;
            for (int i = 0; i < n_here; i++) {
                if (strcmp(here[i].sha, sha)) continue;
                if (!it->path[0]) snprintf(it->path, sizeof(it->path), BASE_DIR "/%s", here[i].name);
                here[i].used = true;            /* another copy of it: the same file, not listed again */
            }
        }
        fclose(f);
    } else {
        scan_say(RELEASES " is not on this image.");
    }
    /* The rest: unknown files (or ones fetched from xiegu.eu that the list does not have yet), each sum once */
    for (int i = 0; i < n_here; i++) {
        item_t *it;
        bool    seen = false;

        if (here[i].used) continue;
        for (int k = 0; k < i; k++)
            if (!strcmp(here[k].sha, here[i].sha) && here[i].sha[0]) seen = true;
        if (seen || !(it = add_item())) continue;
        snprintf(it->name, sizeof(it->name), "%s", here[i].name);
        snprintf(it->sha, sizeof(it->sha), "%s", here[i].sha);
        it->unknown = true;
        if (!here[i].ver[0]) {
            it->bad = true;
            continue;
        }
        snprintf(path, sizeof(path), BASE_DIR "/%s", here[i].name);
        snprintf(it->path, sizeof(it->path), "%s", path);
        snprintf(to, sizeof(to), "%s.json", path);      /* fetched from xiegu.eu: its version as it was kept */
        if ((f = fopen(to, "r"))) {
            size_t r = fread(json, 1, sizeof(json) - 1, f);
            json[r]  = '\0';
            fclose(f);
            json_str(json, "version", it->ver, sizeof(it->ver));
            json_str(json, "built", it->built, sizeof(it->built));
        }
    }
    /* Releases xiegu.eu has that the list does not (the last check): fetchable, unverified */
    if (read_check(json, sizeof(json))) {
        char *p = strstr(json, "\"not_in_list\": [");

        for (p = p ? p + 16 : NULL; p && *p && *p != ']';) {
            char    v[16], key[40], link[160];
            item_t *it;

            if (*p != '"') {
                p++;
                continue;
            }
            size_t l = strcspn(p + 1, "\"");
            snprintf(v, sizeof(v), "%.*s", (int)(l < sizeof(v) ? l : sizeof(v) - 1), p + 1);
            p += l + 2;
            snprintf(key, sizeof(key), "\"%s\": \"", v);
            char *q = strstr(json, key);
            link[0] = '\0';
            if (q) {
                q += strlen(key);
                size_t k = strcspn(q, "\"");
                snprintf(link, sizeof(link), "%.*s", (int)(k < sizeof(link) ? k : sizeof(link) - 1), q);
            }
            bool here = false;
            for (int i = 0; i < n_items; i++)
                if (!strcmp(items[i].ver, v)) here = true;
            if (here || !link[0] || !(it = add_item())) continue;
            snprintf(it->ver, sizeof(it->ver), "%s", v);
            snprintf(it->built, sizeof(it->built), "new");
            snprintf(it->link, sizeof(it->link), "%s", link);
            it->on_site = it->unknown = true;
        }
    }
    scan_done = true;
    return NULL;
}

static void scan_start(void) {
    n_items   = 0;
    sel       = 0;
    lines[0]  = '\0';
    scan_done = false;
    st        = ST_SCAN;
    if (pthread_create(&scan_thread, NULL, scan_main, NULL) == 0) {
        scan_started = true;
    } else {
        st = ST_IDLE;
        say("(no thread for the look)");
    }
}

/* ---- the buttons as the state is --------------------------------------------------------------------------------- */

static bool idle(void) {
    return st == ST_IDLE || st == ST_ARMED || st == ST_ARMED_UNKNOWN || (st == ST_FAILED && last_rc != 3 && !lent);
}

static void buttons_as_state(void) {
    const item_t *it = n_items ? &items[sel] : NULL;

    buttons_disabled(&btn_get, !idle() || !it || !it->on_site || it->path[0]);
    buttons_disabled(&btn_check, !idle());
    btn_flash.label = st == ST_ARMED_UNKNOWN              ? "UNKNOWN: OK?"
                      : st == ST_ARMED                    ? "FLASH NOW"
                      : (st == ST_FAILED && last_rc == 3) ? "Recover"
                                                          : "Flash";
    buttons_disabled(&btn_flash, !((idle() && it && it->path[0] && !it->bad) || (st == ST_FAILED && last_rc == 3)));
    buttons_refresh(&btn_flash);
    buttons_disabled(&btn_close, st == ST_FLASHING);
}

/* ---- children: x6x00-base-fetch (Get, Check) and x6x00-base-flash (Flash) ------------------------------------------ */

static bool run(child_t kind, char *const argv[]) {
    int fds[2];

    if (pipe2(fds, O_CLOEXEC)) {
        say("no pipe: %s", strerror(errno));
        return false;
    }
    child = fork();
    if (child == 0) {                           /* only what is safe between fork and exec in a threaded program */
        setsid();                               /* it goes on whatever happens to the GUI */
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) {
        close(fds[0]);
        say("%s could not be started: %s", argv[0], strerror(errno));
        return false;
    }
    child_out = fds[0];
    fcntl(child_out, F_SETFL, fcntl(child_out, F_GETFL) | O_NONBLOCK);
    child_len = 0;
    ch        = kind;
    return true;
}

static void child_read(void) {
    char    buf[1024];
    ssize_t n;

    while (child_out >= 0 && (n = read(child_out, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || child_len >= sizeof(child_line) - 1) {
                child_line[child_len] = '\0';
                say("%s", child_line);
                child_len = 0;
            } else {
                child_line[child_len++] = buf[i];
            }
        }
    }
}

/* ---- the write ------------------------------------------------------------------------------------------------- */

static void kmsg_open(void) {
    if (kmsg < 0) kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kmsg >= 0) lseek(kmsg, 0, SEEK_END);    /* from now on */
}

/* Every kernel message since the write began: "kernel [seconds since boot] text" */
static void kmsg_read(void) {
    char buf[8192];                             /* a record whole: /dev/kmsg gives one a read, EINVAL if it does not fit */

    while (kmsg >= 0) {
        ssize_t n = read(kmsg, buf, sizeof(buf) - 1);

        if (n < 0 && errno == EPIPE) continue;  /* some were overwritten: on with the next */
        if (n <= 0) break;
        buf[n] = '\0';
        char              *msg = strchr(buf, ';');
        unsigned long long ts  = 0;
        unsigned           pri = 0, seq = 0;

        sscanf(buf, "%u,%u,%llu", &pri, &seq, &ts);
        if (!msg) continue;
        msg++;
        msg[strcspn(msg, "\n")] = '\0';
        say("kernel [%llu.%06llu] %s", ts / 1000000, ts % 1000000, msg);
    }
}

/* The base's serial port (/proc/tty/driver/serial, line 1): a second's bytes and the error counters. The base's
 * firmware streams 9 to 12 kB/s; its bootloader sends single bytes; nothing at all, before a power-off */
static unsigned long serial_rate(char *s, size_t n) {
    FILE         *f = fopen("/proc/tty/driver/serial", "r");
    char          line[256];
    unsigned long rx = 0, fe = 0, oe = 0, bo = 0, rate = 0;
    bool          got = false;

    s[0] = '\0';
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *p;

        if (strncmp(line, "1:", 2)) continue;
        if ((p = strstr(line, " rx:"))) rx = strtoul(p + 4, NULL, 10), got = true;
        if ((p = strstr(line, " fe:"))) fe = strtoul(p + 4, NULL, 10);
        if ((p = strstr(line, " oe:"))) oe = strtoul(p + 4, NULL, 10);
        if ((p = strstr(line, " bo:"))) bo = strtoul(p + 4, NULL, 10);
    }
    fclose(f);
    if (!got) return 0;
    rate = rx_have ? rx - rx_last : 0;
    snprintf(s, n, "ttyS1 %lu B/s (framing %lu, overrun %lu, buffer overrun %lu)", rate, fe, oe, bo);
    rx_last = rx;
    rx_have = true;
    return rate;
}

static void start_flash(void) {
    char held[16];

    if (!lent) {                                /* a Recover finds it lent still: never given back in between */
        if (!radio_base_hold(true)) {
            st = ST_IDLE;
            say("The base could not be lent: transmitting, or the radio thread did not let go of it. Nothing was "
                "sent.");
            buttons_as_state();
            show();
            return;
        }
        lent = true;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    st = ST_FLASHING;
    log_open();
    say("--- base %s%s, from %s%s; the GUI keeps off the base meanwhile", armed.ver[0] ? armed.ver : "?",
        armed.unknown ? " (UNKNOWN FILE)" : "", armed.path, recover ? " (--recover)" : "");
    kmsg_open();
    rx_have = streaming = stream_seen = false;
    snprintf(held, sizeof(held), "%d", getpid());
    char *argv[9] = {FLASHER, "--flash", armed.path, "--yes", "--held-by", held};
    int   n       = 6;
    if (armed.unknown) argv[n++] = "--unknown-ok";
    if (recover) argv[n++] = "--recover";
    argv[n] = NULL;
    if (!run(CH_FLASH, argv)) {
        if (!recover) radio_base_hold(false), lent = false;
        st = ST_IDLE;
    }
    buttons_as_state();
    show();
}

static void flash_ended(int rc) {
    last_rc = rc;
    switch (rc) {
        case 0:
            st = ST_WRITTEN;
            clock_gettime(CLOCK_MONOTONIC, &t_written);
            say("--- written. The base starts the new firmware (its stream on ttyS1), but the GUI's I2C to it stays "
                "locked, and about three minutes on the base switches the radio off. Then switch it on again. The "
                "base stays lent until then; this log goes on to the card.");
            break;
        case 1:
            st = ST_IDLE;
            if (!recover) radio_base_hold(false), lent = false;
            say("--- refused: nothing was sent to the base. It is the GUI's again.");
            break;
        case 2:
            st = ST_FAILED;
            say("--- failed before the erase: nothing was erased. Switch the radio off and on.");
            break;
        case 3:
            st = ST_FAILED;
            say("--- failed at or after the erase: the base has no firmware now. Do not switch the radio off: "
                "Recover writes it again (its bootloader is not erased).");
            break;
        default:
            st = ST_FAILED;
            say("--- the writer ended with %d. Switch the radio off and on.", rc);
            break;
    }
}

static void child_ended(int rc) {
    child_t kind = ch;

    close(child_out);
    child_out = -1;
    child     = -1;
    ch        = CH_NONE;
    switch (kind) {
        case CH_FLASH: flash_ended(rc); break;
        case CH_GET:
            say(rc == 0 ? "--- fetched." : "--- the fetch did not finish (%d).", rc);
            scan_start();                       /* the list again, with the file */
            break;
        case CH_CHECK:
            check_summary();
            say("--- %s", check_line);
            scan_start();                       /* a release the list does not have shows up in it */
            break;
        default: break;
    }
    buttons_as_state();
}

/* After a write: the base's stream watched, said when it starts and when it stops, and every 30 s; while writing,
 * every 5 s */
static void watch_serial(void) {
    static int    quiet;
    char          s[200], status[320];
    unsigned long rate = serial_rate(s, sizeof(s));
    bool          now  = rate > STREAM_BPS;

    if (st == ST_WRITTEN || st == ST_FAILED) {
        if (now && !streaming) {
            say("the base's firmware is running: its stream is back, %s", s);
            stream_seen = true;
        } else if (!now && streaming) {
            say("the base stopped its stream (%s): it is switching the radio off. Switch it on again.", s);
            log_sync();
        } else if (++quiet >= 30) {
            say("%s", s);
            quiet = 0;
        }
    } else if (++quiet >= 5) {
        say("%s", s);
        quiet = 0;
    }
    streaming = now;
    if (st == ST_FLASHING) {
        snprintf(status, sizeof(status), "WRITING base %s: %.0f s   %s", armed.ver, since(&t_start), s);
    } else if (st == ST_WRITTEN) {
        int w = (int)since(&t_written);
        snprintf(status, sizeof(status), "WRITTEN %d:%02d ago, %s   %s", w / 60, w % 60,
                 streaming     ? "firmware running, the power-off ahead"
                 : stream_seen ? "stream stopped: the power-off"
                               : "waiting for the firmware's stream",
                 s);
    } else {
        snprintf(status, sizeof(status), "FAILED (%d), the base kept lent   %s", last_rc, s);
    }
    head_show(status);
}

static void timer_cb(lv_timer_t *t) {
    static int tick;

    (void)t;
    tick++;
    if (st == ST_SCAN) {
        pthread_mutex_lock(&lines_lock);
        if (lines[0]) {
            put_text(lines);
            lines[0] = '\0';
            show();
        }
        pthread_mutex_unlock(&lines_lock);
        if (scan_done) {
            pthread_join(scan_thread, NULL);
            scan_started = false;
            st = ST_IDLE;
            check_summary();
            list_show();
            head_show(NULL);
            buttons_as_state();
            show();
        }
        return;
    }
    size_t before = len;

    child_read();
    if (child > 0) {
        int   ws;
        pid_t r = waitpid(child, &ws, WNOHANG);

        if (r == child) {
            child_read();
            child_ended(WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + WTERMSIG(ws));
        }
    }
    if (st == ST_FLASHING || lent) {
        kmsg_read();
        if (tick % (1000 / TICK_MS) == 0) {
            watch_serial();
            log_sync();
            backlight_tick();                   /* the screen kept on: this is being watched */
        }
    }
    if (len != before) show();
}

/* ---- keys and buttons ---------------------------------------------------------------------------------------------- */

static void move(int d) {
    if (!n_items || !idle()) return;
    sel = (sel + d + n_items) % n_items;
    if (st == ST_ARMED || st == ST_ARMED_UNKNOWN) {     /* another row: what was armed is not */
        st = ST_IDLE;
        say("(disarmed)");
        show();
    }
    list_show();
    buttons_as_state();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            if (st != ST_FLASHING) dialog_destruct();
            break;
        case LV_KEY_LEFT:
        case LV_KEY_UP: move(-1); break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN: move(1); break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT: lv_obj_scroll_by(scroll, 0, 60, LV_ANIM_OFF); break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT: lv_obj_scroll_by(scroll, 0, -60, LV_ANIM_OFF); break;
        default: break;
    }
}

static void get_cb(button_data_t *b) {
    struct statvfs fs;
    item_t        *it = &items[sel];

    (void)b;
    if (!idle() || !n_items || !it->on_site || it->path[0]) return;
    if (statvfs("/mnt", &fs) == 0 && (unsigned long long)fs.f_bavail * fs.f_frsize < 4ull << 20) {
        say("Less than 4 MB free on /mnt: not fetched.");
        show();
        return;
    }
    st = ST_BUSY;
    log_open();
    say("");
    say("--- fetching base %s from xiegu.eu: Xiegu's update streamed, only the base file kept (about two thirds of "
        "its 370 MB come down: some minutes)", it->ver);
    char *argv_v[] = {FETCHER, "get", it->ver, "--out", BASE_DIR, NULL};
    char *argv_u[] = {FETCHER, "get", "--url", it->link, "--out", BASE_DIR, NULL};
    if (!run(CH_GET, it->link[0] ? argv_u : argv_v)) st = ST_IDLE;
    buttons_as_state();
    show();
}

static void check_cb(button_data_t *b) {
    (void)b;
    if (!idle()) return;
    st = ST_BUSY;
    say("");
    say("--- checking xiegu.eu's downloads page");
    char *argv[] = {FETCHER, "check", "--out", BASE_DIR, NULL};
    if (!run(CH_CHECK, argv)) st = ST_IDLE;
    buttons_as_state();
    show();
}

static void flash_cb(button_data_t *b) {
    char pw[96];

    (void)b;
    if (st == ST_ARMED) {
        start_flash();
        return;
    }
    if (st == ST_FAILED && last_rc == 3) {
        recover = true;
        start_flash();
        return;
    }
    if (st == ST_ARMED_UNKNOWN) {
        st = ST_ARMED;
        say("Understood: an unknown file. FLASH NOW writes it.");
        buttons_as_state();
        show();
        return;
    }
    if (!idle() || !n_items || !items[sel].path[0] || items[sel].bad) return;
    armed   = items[sel];
    recover = false;
    power_text(pw, sizeof(pw));
    say("");
    say("Base %s -> %s, from %s.", radio_base_ver_str(), armed.ver[0] ? armed.ver : "?", armed.path);
    say("Xiegu asks for a 13.8 V supply of 3 A or more and the battery out, for the whole write; now: %s.", pw);
    say("The GUI keeps off the base; base_flash does what Xiegu's UPGRADE screen does (about 30 s). The base then "
        "runs the new firmware and, about three minutes later, switches the radio off by itself: switch it on again. "
        "Do not switch it off while it writes.");
    if (armed.unknown) {
        st = ST_ARMED_UNKNOWN;
        say("THIS FILE IS NOT ONE OF XIEGU'S KNOWN HERE: it passes the checks an encrypted file allows, but what it "
            "holds can't be told. UNKNOWN: OK? and then FLASH NOW write it; Close or ESC leave it.");
    } else {
        st = ST_ARMED;
        say("FLASH NOW to write it; Close or ESC to leave it.");
    }
    head_show("armed");
    buttons_as_state();
    show();
}

static void close_cb(button_data_t *b) {
    (void)b;
    if (st != ST_FLASHING) dialog_destruct();
}

static void page_cb(button_data_t *b) {
    (void)b;
}

/* ---- the window ---------------------------------------------------------------------------------------------- */

/* The end of the last run's log, so that a write that ended in a power-off can be read after the power-on */
static void last_run(void) {
    FILE *f = fopen(LOG_PATH, "r");
    char  buf[4096];
    long  size;

    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, size > (long)sizeof(buf) - 1 ? size - (long)sizeof(buf) + 1 : 0, SEEK_SET);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *start = size > (long)sizeof(buf) - 1 ? strchr(buf, '\n') : buf;
    if (!start) return;
    put_text("The end of the last run (" LOG_PATH "):\n");
    put_text(start + (start != buf));
    put_text("\n");
}

static void construct_cb(lv_obj_t *parent) {
    lv_coord_t h = 480 - BUTTONS_H, lh = lv_font_get_line_height(&codec_18);
    lv_coord_t list_y = 2 * lh + 6, log_y = list_y + (LIST_ROWS + 1) * lh + 6;

    body = lv_obj_create(parent);
    dialog.obj = body;
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, 0);
    lv_obj_set_size(body, 800, h);
    lv_obj_set_style_bg_color(body, TC(bg), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(body, TC(panel_border), 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_pad_all(body, PAD, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    head = lv_label_create(body);
    lv_obj_set_width(head, lv_pct(100));
    lv_label_set_long_mode(head, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(head, 2 * lh);
    lv_obj_set_style_text_font(head, &codec_18, 0);
    lv_obj_set_style_text_color(head, TC(accent), 0);

    list = lv_label_create(body);
    lv_obj_set_pos(list, 0, list_y);
    lv_obj_set_size(list, lv_pct(100), (LIST_ROWS + 1) * lh);
    lv_label_set_long_mode(list, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(list, &codec_18, 0);
    lv_obj_set_style_text_color(list, TC(text), 0);

    scroll = lv_obj_create(body);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_pos(scroll, 0, log_y);
    lv_obj_set_size(scroll, lv_pct(100), h - 2 * PAD - 2 - log_y);
    lv_obj_set_style_border_color(scroll, TC(panel_border), 0);
    lv_obj_set_style_border_width(scroll, 1, 0);
    lv_obj_set_style_border_side(scroll, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);

    label = lv_label_create(scroll);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &codec_18, 0);
    lv_obj_set_style_text_color(label, TC(text), 0);

    if (st != ST_WRITTEN && st != ST_FLASHING && st != ST_FAILED && st != ST_BUSY) {   /* a fresh look */
        len     = 0;
        text[0] = '\0';
        last_run();
        scan_start();
    }
    check_summary();
    head_show(st == ST_SCAN ? "looking for the files" : NULL);
    list_show();
    show();
    buttons_as_state();

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
    timer = lv_timer_create(timer_cb, TICK_MS, NULL);
}

static void destruct_cb(void) {
    if (timer) lv_timer_del(timer);
    timer = NULL;
    if (scan_started) {                         /* a look under way finishes on its own: waited for */
        pthread_join(scan_thread, NULL);
        scan_started = false;
    }
    if (ch == CH_GET || ch == CH_CHECK) {       /* a fetch or a check closed with the window: ended */
        kill(-child, SIGTERM);
        waitpid(child, NULL, 0);
        close(child_out);
        child_out = -1;
        child     = -1;
        ch        = CH_NONE;
        say("(the window closed: the fetch or check ended)");
        st = ST_IDLE;
    }
    if (st == ST_SCAN || st == ST_ARMED || st == ST_ARMED_UNKNOWN) st = ST_IDLE;
    lv_group_remove_obj(dialog.obj);
    body = head = list = scroll = label = NULL;
    /* A write done or failed keeps the base lent (it switches the radio off, or wants Recover) */
    if ((st == ST_WRITTEN || st == ST_FAILED) && logf) {
        say("(the window closed; the base stays lent until the radio is switched off)");
        log_sync();
    }
}
