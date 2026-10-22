/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops
 *
 *  Screens: home (SOTA, POTA, WWBOTA, HOTA, WWFF, Bands, Log tiles), a program (the reference where you
 *  are and the nearest ones; during an activation the activation screen: frequency,
 *  S-meter, CQ, QSO count and time), Bands (activity and who hears you, from RBN) and
 *  the activation log. The dialog covers the whole screen above the buttons; the main
 *  knob still tunes. The network jobs (reference list update, POTA/WWBOTA/HOTA/WWFF spot, QRZ upload) run
 *  x6100-field in a thread; its one-line answer comes back through the scheduler.
 *  The companion app follows the screen through the remote state (dialog_field_remote_state)
 *  and logs, spots and starts the activation.
 */

#include "dialog_field.h"
#include "board.h"

#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cluster/client.h"
#include "cluster/spots.h"
#include "cw_encoder.h"
#include "dialog.h"
#include "dialog_msg_voice.h"
#include "events.h"
#include "field/bands.h"
#include "field/flog.h"
#include "field/qsy.h"
#include "field/rbn.h"
#include "field/refs.h"
#include "gps.h"
#include "keyboard.h"
#include "knobs.h"
#include "main_screen.h"
#include "meter.h"
#include "msg.h"
#include "params/params.h"
#include "qth/qth.h"
#include "radio.h"
#include "scheduler.h"
#include "station.h"
#include "styles.h"
#include "textarea_window.h"
#include "theme.h"
#include "vol.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "fonts/font_select.h"

/* VISION: one size over the other themes here, not VISION_FONT_STEPS (too big for these tables) */
#define VF(n) (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS - 1) : &sony_##n)

#ifndef FIELD_DIR   /* overridden by the UI harness */
#define FIELD_DIR   BOARD_DATA "/field"
#endif
#ifndef FIELD_ICONS
#define FIELD_ICONS "/usr/share/x6100/field"
#endif
#define REFS_PATH   FIELD_DIR "/refs.tsv"
#define STATE_PATH  FIELD_DIR "/state"
#define HOTA_KEY    FIELD_DIR "/hota.conf"   /* written by the web page (Field Ops) */
#define WWFF_KEY    FIELD_DIR "/wwff.conf"
#define LOGS_DIR    FIELD_DIR "/logs"
#define HELPER      "x6100-field"

#define W           800
#define HEAD_H      46
#define STATUS_H    34
#define MAX_NEAR    30
#define MAX_QSO     200
#define HOME_TILES  7       /* SOTA, POTA, WWBOTA, HOTA, WWFF, Bands, Log */
#define GPS_MAX_AGE 600     /* s */
#define PHONE_MAX_AGE 1800

typedef enum { SCR_HOME, SCR_PROG, SCR_BANDS, SCR_LOG } screen_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void show(screen_t s);

/* Kept on the SD card */
static struct {
    screen_t    screen;
    int         home_sel;
    char        prog;       /* 'S' SOTA, 'P' POTA, 'B' WWBOTA, 'H' HOTA, 'F' WWFF */
    bool        have_ref;
    field_ref_t ref;
    bool        active;
    time_t      start;
    char        log[160];
} st = {.prog = 'S'};
static bool st_loaded;

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .key_cb       = key_cb,
};

dialog_t *dialog_field = &dialog;

static lv_obj_t   *title, *head_icon, *head_right, *head_clock, *head_dot, *body, *list, *status;
static lv_coord_t  body_h;
static lv_timer_t *timer;
static int         sel, rows_n;
static bool        kbd_open;

/* Reference screen */
static lv_obj_t *info_ref, *info_name, *info_lines, *info_dot, *info_state;
/* Activation screen */
static lv_obj_t *act_band, *act_freq, *act_mode, *act_smeter[13], *act_sval, *act_text, *act_recent, *act_qsos, *act_time,
                *act_last;

static field_ref_t near[MAX_NEAR];
static int         near_n = -2;     /* -2: not looked up yet, -1: no list */
static double      near_lat = 999, near_lon;
static char        near_prog;

static field_qso_t qsos[MAX_QSO];
static int         qsos_n;          /* in qsos[]: the newest MAX_QSO of the log */
static int         qsos_total;      /* in the log */

/* The activation's log into qsos[]: the newest MAX_QSO, and the whole count */
static void log_load(void) {
    qsos_total = st.log[0] ? field_log_read(st.log, qsos, MAX_QSO) : 0;
    qsos_n     = qsos_total < MAX_QSO ? qsos_total : MAX_QSO;
}

static atomic_bool busy;            /* an x6100-field job runs */
static bool        updating;        /* the reference list downloads */
static lv_timer_t *update_timer;    /* shows x6100-field's progress while updating */
static char        update_text[96];
static bool        end_armed;
static bool        spotted;      /* this activation is on pota.app / wwbota.org / cqhota.app: End posts a QRT spot */
static char        wwbota_id[48]; /* wwbota.org's id of this activation's spot: QSY and QRT update it */
static time_t      end_armed_at;
static bool        spot_armed;   /* Spot asks before posting: a second press within 4 s sends */
static time_t      spot_armed_at;
/* POTA, WWBOTA, HOTA and WWFF auto spot: at the start of the activation, and again after a QSY (frequency 1 kHz away
 * or another mode) once we transmitted there and stayed QSY_SETTLE_S (field/qsy.c) */
static lv_timer_t *qsy_timer;
static int32_t     spot_hz;      /* where the last spot said we are */
static char        spot_mode[8];
static field_qsy_t qsy;

/* The remote state, written on the LVGL thread, read by the remote server */

/* Colours */

/* The home tiles: the programs, then Bands and Log */
static const struct {
    char        prog;
    const char *name, *icon, *sub;
} tiles[] = {
    {'S', "SOTA", "sota", "Summits\nOn The Air"},   {'P', "POTA", "pota", "Parks\nOn The Air"},
    {'B', "WWBOTA", "wwbota", "Bunkers\nOn The Air"}, {'H', "HOTA", "hota", "History\nOn The Air"},
    {'F', "WWFF", "wwff", "Nature\nOn The Air"},
    {0, "BANDS", "bands", "Activity\nRBN, beacon"}, {0, "LOG", "log", "Activation\nlog"},
};

static lv_color_t prog_color(char p) {
    switch (p) {
        case 'S': return TC(warn);
        case 'P': return TC(ok);
        case 'B': return TC(digi);
        case 'F': return TC(vfo);
        default: return TC(alert);      /* HOTA */
    }
}

static lv_color_t accent(int k) {
    return tiles[k].prog ? prog_color(tiles[k].prog) : k == HOME_TILES - 2 ? TC(info) : TC(text_head);
}

static lv_color_t prog_accent(void) {
    return prog_color(st.prog);
}

/* State file: key=value lines */

static void state_save(void) {
    char  tmp[] = STATE_PATH ".tmp";
    FILE *f;

    mkdir(FIELD_DIR, 0755);
    f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "screen=%d\nhome=%d\nprog=%c\nactive=%d\nstart=%lld\nlog=%s\n", st.screen, st.home_sel,
            st.prog,
            st.active, (long long)st.start, st.log);
    if (st.have_ref)
        fprintf(f, "ref=%c\t%s\t%s\t%.5f\t%.5f\t%d\t%d\n", st.ref.type, st.ref.ref, st.ref.name, st.ref.lat,
                st.ref.lon, st.ref.alt_m, st.ref.points);
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) == 0)
        rename(tmp, STATE_PATH);
}

static void state_load(void) {
    FILE *f = fopen(STATE_PATH, "r");
    char  line[300];

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');

        if (!v)
            continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "screen"))
            st.screen = (screen_t)atoi(v);
        else if (!strcmp(line, "home"))
            st.home_sel = atoi(v);
        else if (!strcmp(line, "prog"))
            st.prog = *v && strchr("PBHF", *v) ? *v : 'S';
        else if (!strcmp(line, "active"))
            st.active = atoi(v);
        else if (!strcmp(line, "start"))
            st.start = (time_t)atoll(v);
        else if (!strcmp(line, "log"))
            snprintf(st.log, sizeof(st.log), "%s", v);
        else if (!strcmp(line, "ref"))
            st.have_ref = field_ref_parse(v, &st.ref);
    }
    fclose(f);
    if ((unsigned)st.screen > SCR_LOG)      /* an edited or damaged file */
        st.screen = SCR_HOME;
    if (st.home_sel < 0 || st.home_sel >= HOME_TILES)
        st.home_sel = 0;
}

/* Helpers */

static const char *my_call(void) {
    static char call[16];
    const char *c = params.callsign.x;
    size_t      n = 0;

    /* Only what a callsign has: it goes on a command line */
    for (; c && *c && n < sizeof(call) - 1; c++)
        if (isalnum((unsigned char)*c) || *c == '/')
            call[n++] = toupper((unsigned char)*c);
    call[n] = 0;
    return call;
}

static bool ref_ok(const char *r) {
    if (!r[0])
        return false;
    for (; *r; r++)
        if (!isalnum((unsigned char)*r) && *r != '/' && *r != '-')
            return false;
    return true;
}

/* FT8/FT4 dial frequencies, kHz: in USB or DIG on one of them the mode is that, not SSB */
static const char *digi_mode(void) {
    static const int ft8[] = {1840, 3573, 5357, 7074, 10136, 14074, 18100, 21074, 24915, 28074, 50313};
    static const int ft4[] = {3575, 7047, 10140, 14080, 18104, 21140, 24919, 28180, 50318};
    int32_t          hz    = cparam_i_get(cfg_fg_freq);

    for (size_t i = 0; i < sizeof(ft8) / sizeof(ft8[0]); i++)
        if (hz >= ft8[i] * 1000 - 500 && hz <= ft8[i] * 1000 + 3500)
            return "FT8";
    for (size_t i = 0; i < sizeof(ft4) / sizeof(ft4[0]); i++)
        if (hz >= ft4[i] * 1000 - 500 && hz <= ft4[i] * 1000 + 3500)
            return "FT4";
    return NULL;
}

static const char *adif_mode(void) {
    const char *d = digi_mode();

    switch (cparam_i_get(cfg_cur_mode)) {
        case radio_mode_cw:
        case radio_mode_cwr:
            return "CW";
        case radio_mode_am:
            return "AM";
        case radio_mode_nfm:
            return "FM";
        case radio_mode_lsb_dig:
        case radio_mode_usb_dig:
            return d ? d : "FT8";
        default:
            return d ? d : "SSB";
    }
}

static bool is_cw(void) {
    int m = cparam_i_get(cfg_cur_mode);
    return m == radio_mode_cw || m == radio_mode_cwr;
}

/* The best position: radio GPS, else the QTH locator. src: "GPS", "QTH" */
static bool position(double *lat, double *lon, const char **src, int *age) {
    *age = 0;
    if (gps_last_fix(lat, lon, age) && *age < GPS_MAX_AGE) {
        *src = "GPS";
        return true;
    }
    const char *qth = station_qth();
    if (qth && strlen(qth) >= 4) {
        qth_str_to_pos(qth, lat, lon);
        *src = "QTH";
        *age = 0;
        return true;
    }
    *src = "";
    return false;
}

static void deg_text(char *buf, size_t size, double lat, double lon) {
    /* Decimal degrees: the fonts have no degree sign */
    snprintf(buf, size, "%.4f %c   %.4f %c", fabs(lat), lat >= 0 ? 'N' : 'S', fabs(lon), lon >= 0 ? 'E' : 'W');
}

static const char *prog_name_of(char p) {
    return p == 'S' ? "SOTA" : p == 'P' ? "POTA" : p == 'B' ? "WWBOTA" : p == 'F' ? "WWFF" : "HOTA";
}

static const char *prog_name(void) {
    return prog_name_of(st.prog);
}

/* "sota", "pota", "wwbota", "hota": the icons */
static const char *prog_icon(void) {
    static char s[8];
    const char *n = prog_name();
    size_t      i = 0;

    for (; n[i] && i < sizeof(s) - 1; i++)
        s[i] = tolower((unsigned char)n[i]);
    s[i] = 0;
    return s;
}

/* What the activation needs: QSOs (SOTA 4, POTA 10, WWBOTA 25 or 10 in S5 and Z3, WWFF 44) or, HOTA, 5 different calls */
static int prog_needs(void) {
    switch (st.ref.type) {
        case 'S': return 4;
        case 'P': return 10;
        case 'F': return 44;
        case 'B': return !strncmp(st.ref.ref, "B/S5-", 5) || !strncmp(st.ref.ref, "B/Z3-", 5) ? 10 : 25;
        default: return 5;
    }
}

/* The activation's count: QSOs, HOTA different calls */
static int act_count(void) {
    int n = 0;

    if (st.ref.type != 'H')
        return qsos_total;
    for (int i = 0; i < qsos_n; i++) {
        int k = 0;
        while (k < i && strcasecmp(qsos[k].call, qsos[i].call))
            k++;
        n += k == i;
    }
    return n;
}

static void freq_text(char *buf, size_t size, int32_t hz) {
    snprintf(buf, size, "%d.%03d.%03d", hz / 1000000, hz / 1000 % 1000, hz % 1000);
}

/* x6100-field jobs */

typedef void (*job_done_t)(const char *line);

typedef struct {
    job_done_t done;
    char       line[160];
} job_result_t;

typedef struct {
    job_done_t done;
    char       cmd[400];
} job_t;

static void job_result_cb(void *arg) {
    job_result_t *r = arg;
    r->done(r->line);
}

static void *job_thread(void *arg) {
    job_t       *j = arg;
    job_result_t r = {.done = j->done, .line = "error no answer"};
    FILE        *p = popen(j->cmd, "r");

    if (p) {
        if (!fgets(r.line, sizeof(r.line), p))
            snprintf(r.line, sizeof(r.line), "error no answer");
        pclose(p);
    }
    r.line[strcspn(r.line, "\r\n")] = 0;
    free(j);
    atomic_store(&busy, false);
    scheduler_put(job_result_cb, &r, sizeof(r));
    return NULL;
}

static bool job_start(job_done_t done, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool job_start(job_done_t done, const char *fmt, ...) {
    pthread_t t;
    va_list   ap;
    job_t    *j;
    bool      idle = false;

    if (!atomic_compare_exchange_strong(&busy, &idle, true)) {
        msg_update_text_fmt("Field: wait, still busy");
        return false;
    }
    j       = calloc(1, sizeof(*j));
    j->done = done;
    va_start(ap, fmt);
    vsnprintf(j->cmd, sizeof(j->cmd), fmt, ap);
    va_end(ap);
    strncat(j->cmd, " 2>/dev/null", sizeof(j->cmd) - strlen(j->cmd) - 1);
    if (pthread_create(&t, NULL, job_thread, j) != 0) {
        free(j);
        atomic_store(&busy, false);
        return false;
    }
    pthread_detach(t);
    return true;
}

/* The nearest references, looked up in a thread (the list has ~260000 lines) */

typedef struct {
    char        prog;
    double      lat, lon;
    int         n;
    field_ref_t refs[MAX_NEAR];
} near_result_t;

static void near_result_cb(void *arg) {
    near_result_t *r = arg;

    near_n    = r->n;
    near_prog = r->prog;
    near_lat  = r->lat;
    near_lon  = r->lon;
    memcpy(near, r->refs, sizeof(near));
    /* The nearest one is the reference while none is chosen */
    if (!st.active && near_n > 0 && (!st.have_ref || st.ref.type != r->prog)) {
        st.ref      = near[0];
        st.have_ref = true;
    }
    state_save();
    if (dialog.run && st.screen == SCR_PROG && !st.active) {
        sel = 0;
        show(SCR_PROG);
    }
}

static void *near_thread(void *arg) {
    near_result_t *r = arg;

    r->n = field_refs_near(REFS_PATH, r->prog, r->lat, r->lon, r->refs, MAX_NEAR);
    scheduler_put(near_result_cb, r, sizeof(*r));
    free(r);
    return NULL;
}

static void near_lookup(bool force) {
    double      lat, lon;
    const char *src;
    int         age;
    pthread_t   t;

    if (!position(&lat, &lon, &src, &age))
        return;
    /* Again when the list was replaced (the radio's update), the program changed or we moved 300 m */
    static time_t refs_mtime;
    static ino_t  refs_ino;
    struct stat   sb;
    if (stat(REFS_PATH, &sb) == 0 && (sb.st_mtime != refs_mtime || sb.st_ino != refs_ino)) {
        refs_mtime = sb.st_mtime;
        refs_ino   = sb.st_ino;
        force      = true;
    }
    if (!force && near_prog == st.prog && near_n != -2 && fabs(lat - near_lat) < 0.003 && fabs(lon - near_lon) < 0.004)
        return;
    near_result_t *r = calloc(1, sizeof(*r));
    r->prog = st.prog;
    r->lat  = lat;
    r->lon  = lon;
    near_prog = st.prog;
    near_lat  = lat;
    near_lon  = lon;
    if (pthread_create(&t, NULL, near_thread, r) == 0)
        pthread_detach(t);
    else
        free(r);
}

/* Widgets */

/* True for text with our colour markup, "#rrggbb ...#": only such labels get recolouring, so a
 * '#' in a reference's name (Bunker #12) shows instead of starting a colour command */
static bool has_markup(const char *text) {
    for (const char *p = strchr(text, '#'); p; p = strchr(p + 1, '#')) {
        int i = 1;
        while (i <= 6 && isxdigit((unsigned char)p[i])) i++;
        if (i == 7 && p[7] == ' ') return true;
    }
    return false;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_recolor(l, true);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *p = lv_obj_create(parent);

    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, TC(panel), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, TC(panel_border), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 8, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *icon(lv_obj_t *parent, const char *name, int size, lv_coord_t x, lv_coord_t y) {
    char                path[96];
    const lv_img_dsc_t *d;

    snprintf(path, sizeof(path), FIELD_ICONS "/%s_%d.png", name, size);
    d = theme_png(path);
    if (!d)
        return NULL;
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, d);
    lv_obj_set_pos(img, x, y);
    return img;
}

static lv_obj_t *dot(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t d, lv_color_t c) {
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* A list of rows, one selected; the columns of a row are separated by tabs */
static const lv_coord_t *cols;
static int               cols_n;

static void row_click_cb(lv_event_t *e);

static void list_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                        const lv_coord_t *widths, int n) {
    list = panel(parent, x, y, w, h);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    cols   = widths;
    cols_n = n;
    rows_n = 0;
}

static lv_obj_t *row_create(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *row = lv_obj_create(parent);
    char      buf[200];
    char     *cell = buf;

    lv_obj_remove_style_all(row);
    if (parent != list)
        lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, parent == list ? lv_pct(100) : W - 24, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_text_font(row, VF(22), 0);
    lv_obj_set_style_text_color(row, TC(text), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    snprintf(buf, sizeof(buf), "%s", text);
    for (int c = 0; c < cols_n && cell; c++) {
        char     *next = strchr(cell, '\t');
        lv_obj_t *l    = lv_label_create(row);

        if (next)
            *next++ = 0;
        lv_label_set_recolor(l, has_markup(cell));   /* a '#' in a name is not a colour command */
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, cols[c]);
        lv_label_set_text(l, cell);
        cell = next;
    }
    return row;
}

/* The column titles above the list */
static void list_head(const char *text, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *head = row_create(body, text, x, y);

    lv_obj_set_style_text_font(head, VF(18), 0);
    lv_obj_set_style_text_color(head, TC(text_dim), 0);
}

static lv_obj_t *list_add(const char *text) {
    lv_obj_t *row = row_create(list, text, 0, 0);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)rows_n);
    rows_n++;
    return row;
}

/* The selected row: black on the program's colour, as a highlighted line on an LCD */
static void list_select(int k) {
    if (!list || rows_n == 0)
        return;
    sel = k < 0 ? 0 : k >= rows_n ? rows_n - 1 : k;
    for (int i = 0; i < rows_n; i++) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        bool      on  = i == sel;

        lv_obj_set_style_bg_color(row, st.screen == SCR_PROG ? prog_accent() : TC(warn), 0);
        lv_obj_set_style_bg_opa(row, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(row, on ? lv_color_black() : TC(text), 0);
    }
    lv_obj_scroll_to_view(lv_obj_get_child(list, sel), LV_ANIM_OFF);
}

/* Buttons */

static void page_cb(button_data_t *b);
static void open_cb(button_data_t *b);
static void boot_cb(button_data_t *b);
static void respot_cb(button_data_t *b);
static const char *respot_label(void);
static void exit_cb(button_data_t *b);
static void back_cb(button_data_t *b);
static void activate_cb(button_data_t *b);
static void spot_cb(button_data_t *b);
static void log_cb(button_data_t *b);
static void ref_cb(button_data_t *b);
static void update_cb(button_data_t *b);
static void beacon_cb(button_data_t *b);
static void tune_cb(button_data_t *b);
static void qrz_cb(button_data_t *b);
static void qrz_prof_cb(button_data_t *b);
static const char *qrz_prof_label(void);
static void cq_cb(button_data_t *b);
static void bands_cb(button_data_t *b);
static void logbook_cb(button_data_t *b);
static void rec_cq_cb(button_data_t *b);
static const char *rec_cq_label(void);
static void cq_level_cb(button_data_t *b);
static const char *cq_level_label(void);
static const char *boot_label(void);
static const char *activate_label(void);

static button_data_t b_home   = {.type = BTN_TEXT, .label = "FIELD OPS"};
static button_data_t b_reccq  = {.type = BTN_TEXT_FN, .label_fn = rec_cq_label, .press = rec_cq_cb};
static button_data_t b_open   = {.type = BTN_TEXT, .label = "Select", .press = open_cb};
static button_data_t b_boot   = {.type = BTN_TEXT_FN, .label_fn = boot_label, .press = boot_cb};
static button_data_t b_respot = {.type = BTN_TEXT_FN, .label_fn = respot_label, .press = respot_cb};
static button_data_t b_exit   = {.type = BTN_TEXT, .label = "Radio", .press = exit_cb};
static button_data_t b_back   = {.type = BTN_TEXT, .label = "Back", .press = back_cb};
static button_data_t b_ref1   = {.type = BTN_TEXT, .label = "(1:2)", .press = page_cb};
static button_data_t b_ref2   = {.type = BTN_TEXT, .label = "(2:2)", .press = page_cb};
static button_data_t b_act1   = {.type = BTN_TEXT, .label = "(1:2)", .press = page_cb};
static button_data_t b_act2   = {.type = BTN_TEXT, .label = "(2:2)", .press = page_cb};
static button_data_t b_act    = {.type = BTN_TEXT_FN, .label_fn = activate_label, .press = activate_cb};
static const char *spot_label(void);
static button_data_t b_spot   = {.type = BTN_TEXT_FN, .label_fn = spot_label, .press = spot_cb};
static button_data_t b_log    = {.type = BTN_TEXT, .label = "Log\nQSO", .press = log_cb};
static button_data_t b_ref    = {.type = BTN_TEXT, .label = "Ref...", .press = ref_cb};
static button_data_t b_update = {.type = BTN_TEXT, .label = "Update\nlist", .press = update_cb};
static button_data_t b_cq     = {.type = BTN_TEXT, .label = "CQ", .press = cq_cb, .hold = cq_level_cb};
static button_data_t b_cqlvl  = {.type = BTN_TEXT_FN, .label_fn = cq_level_label, .press = cq_level_cb};
static button_data_t b_band   = {.type = BTN_TEXT, .label = "Bands", .press = bands_cb};
static button_data_t b_book __attribute__((unused)) = {.type = BTN_TEXT, .label = "Log\nbook", .press = logbook_cb};
static button_data_t b_bands  = {.type = BTN_TEXT, .label = "BANDS"};
static button_data_t b_beacon = {.type = BTN_TEXT, .label = "Test\nbeacon", .press = beacon_cb};
static button_data_t b_tune   = {.type = BTN_TEXT, .label = "Use\nband", .press = tune_cb};
static button_data_t b_logpg  = {.type = BTN_TEXT, .label = "LOGBOOK"};
static button_data_t b_qrz    = {.type = BTN_TEXT, .label = "Upload\nQRZ", .press = qrz_cb};
static button_data_t b_qrzprof = {.type = BTN_TEXT_FN, .label_fn = qrz_prof_label, .press = qrz_prof_cb};

static buttons_page_t page_home  = {.items = {&b_home, &b_open, &b_boot, &b_respot, &b_exit}};
static buttons_page_t page_ref1  = {.items = {&b_ref1, &b_act, &b_spot, &b_ref, &b_back}};
static buttons_page_t page_ref2  = {.items = {&b_ref2, &b_update, &b_reccq, &b_cqlvl, &b_back}};
static buttons_page_t page_act1  = {.items = {&b_act1, &b_cq, &b_log, &b_spot, &b_back}};
static buttons_page_t page_act2  = {.items = {&b_act2, &b_act, &b_band, &b_reccq, &b_back}};   /* b_book: LOG hidden */
static buttons_page_t page_bands = {.items = {&b_bands, &b_beacon, &b_tune, NULL, &b_back}};
static buttons_page_t page_log   = {.items = {&b_logpg, &b_log, &b_qrz, &b_qrzprof, &b_back}};

static const char *boot_label(void) {
    return param_i_get(cfg_field_boot) ? "Start here\nOn" : "Start here\nOff";
}

static const char *rec_cq_label(void) {
    return dialog_msg_voice_get_state() == MSG_VOICE_RECORD ? "Stop\nrec" : "Rec\nCQ";
}

/* The voice CQ's level: CQ held or CQ level, then MFK or VOL; a press ends it */
static bool cq_level_adj;

static const char *cq_level_label(void) {
    static char s[24];
    snprintf(s, sizeof(s), "CQ level\n%+d dB", (int)param_i_get(cfg_voice_msg_gain));
    return s;
}

static void cq_level_show(void) {
    msg_update_text_fmt("CQ level %+d dB: MFK or VOL to change, press to finish", (int)param_i_get(cfg_voice_msg_gain));
    buttons_refresh(&b_cqlvl);
}

static void cq_level_cb(button_data_t *b) {
    cq_level_adj = !cq_level_adj;
    if (cq_level_adj)
        cq_level_show();
    else
        msg_update_text_fmt("CQ level %+d dB", (int)param_i_get(cfg_voice_msg_gain));
}

static void cq_level_change(int d) {
    int v = param_i_get(cfg_voice_msg_gain) + d;
    param_i_set(cfg_voice_msg_gain, v < -20 ? -20 : v > 10 ? 10 : v);
    cq_level_show();
}

static const char *activate_label(void) {
    return !st.active ? "Start" : end_armed ? "End?" : "End";
}

static const char *spot_label(void) {
    return spot_armed ? "Spot?" : "Spot";
}

static void page_cb(button_data_t *b) {
    buttons_page_t *next = b == &b_ref1 ? &page_ref2 : b == &b_ref2 ? &page_ref1 : b == &b_act1 ? &page_act2 : &page_act1;

    buttons_unload_page();
    buttons_load_page(next);
}

static void boot_cb(button_data_t *b) {
    param_i_set(cfg_field_boot, !param_i_get(cfg_field_boot));
    buttons_refresh(&b_boot);
}

/* POTA, WWBOTA, HOTA, WWFF: spot again after a QSY once we transmitted there */
static const char *respot_label(void) {
    return param_i_get(cfg_field_respot) ? "Re-spot\nOn" : "Re-spot\nOff";
}

static void respot_cb(button_data_t *b) {
    param_i_set(cfg_field_respot, !param_i_get(cfg_field_respot));
    buttons_refresh(&b_respot);
}

static void exit_cb(button_data_t *b) {
    main_screen_close_app();
}

static void back_cb(button_data_t *b) {
    show(SCR_HOME);
}

static void bands_cb(button_data_t *b) {
    show(SCR_BANDS);
}

static void logbook_cb(button_data_t *b) {
    show(SCR_LOG);
}

/* Header */

static void head_set(const char *icon_name, const char *text) {
    if (head_icon) {
        lv_obj_del(head_icon);
        head_icon = NULL;
    }
    if (icon_name)
        head_icon = icon(lv_obj_get_parent(title), icon_name, 64, 84, -9);
    if (head_icon) {
        lv_img_set_zoom(head_icon, 128);   /* 32 px */
        lv_obj_set_pos(title, 154, 10);
    } else {
        lv_obj_set_pos(title, 104, 10);
    }
    lv_label_set_text(title, text);
}

/* Home */

static void home_open(int k) {
    st.home_sel = k;
    if (tiles[k].prog) {
        st.prog = tiles[k].prog;
        /* The activation keeps its program */
        if (st.active && st.have_ref)
            st.prog = st.ref.type;
        show(SCR_PROG);
    } else {
        show(k == HOME_TILES - 2 ? SCR_BANDS : SCR_LOG);
    }
}

static void open_cb(button_data_t *b) {
    home_open(sel);
}

static void home_select(int k) {
    sel = (k + HOME_TILES) % HOME_TILES;
    for (int i = 0; i < HOME_TILES; i++) {
        lv_obj_t *t  = lv_obj_get_child(body, i);
        bool      on = i == sel;

        lv_obj_set_style_border_width(t, on ? 4 : 2, 0);
        lv_obj_set_style_border_opa(t, on ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_set_style_bg_color(t, lv_color_mix(accent(i), TC(panel), on ? 80 : 30), 0);
        lv_obj_set_style_bg_grad_color(t, TC(panel), 0);
    }
}

static void tile_click_cb(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);

    home_select(k);
    home_open(k);
}

static void build_home(void) {
    lv_coord_t gap = 8, tw = (W - 16 - (HOME_TILES - 1) * gap) / HOME_TILES;
    lv_coord_t x0 = (W - HOME_TILES * tw - (HOME_TILES - 1) * gap) / 2, th = body_h - STATUS_H - 30;

    head_set(NULL, "FIELD OPERATIONS");
    list   = NULL;
    rows_n = HOME_TILES;
    for (int k = 0; k < HOME_TILES; k++) {
        lv_obj_t *t = panel(body, x0 + k * (tw + gap), 16, tw, th);

        lv_obj_set_style_border_color(t, accent(k), 0);
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);

        icon(t, tiles[k].icon, 96, (tw - 96) / 2 - 2, 24);
        const lv_font_t *nf = theme_shape.vision ? VF(28) : VF(32);   /* VISION: WWBOTA on one line */
        if (lv_txt_get_width(tiles[k].name, strlen(tiles[k].name), nf, 0, LV_TEXT_FLAG_NONE) > tw - 4)
            nf = VF(28);    /* seven tiles: WWBOTA a size or two down, the others as they are */
        if (lv_txt_get_width(tiles[k].name, strlen(tiles[k].name), nf, 0, LV_TEXT_FLAG_NONE) > tw - 4)
            nf = VF(24);
        lv_obj_t *n = label(t, nf, accent(k), 0, 140, tw - 4);
        lv_obj_t *s = label(t, VF(20), lv_color_mix(accent(k), TC(text), 160), 0, 186, tw - 4);

        lv_label_set_recolor(n, false);
        lv_label_set_text(n, tiles[k].name);
        lv_label_set_text(s, tiles[k].sub);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(status, "MFK: choose, press to open.   GEN: back to the radio");
    buttons_load_page(&page_home);
}

/* A program: the reference */

static void prog_info_update(void) {
    char        buf[200], deg[48], grid[16] = "";
    double      lat, lon;
    const char *src;
    int         age;

    if (!info_ref)
        return;
    bool have_pos = position(&lat, &lon, &src, &age);
    if (st.have_ref && st.ref.type == st.prog) {
        lv_label_set_text(info_ref, st.ref.ref);
        lv_label_set_recolor(info_name, false);
        lv_label_set_text(info_name, st.ref.name);
    } else {
        lv_label_set_text(info_ref, "------");
        lv_label_set_text(info_name, near_n == -1                         ? "No reference list"
                                     : !have_pos                          ? "No position"
                                     : near_n == 0 && near_prog == st.prog ? "None in the list: Update list"
                                                                          : "Looking...");
    }
    if (have_pos) {
        qth_pos_to_str(lat, lon, grid);
        grid[6] = 0;
        deg_text(deg, sizeof(deg), lat, lon);
    }
    char alt[48] = "";
    if (st.have_ref && st.ref.type == 'S' && st.prog == 'S')
        snprintf(alt, sizeof(alt), "#%06x ALT#  %d m     #%06x PTS#  %d\n", theme_hex(TC_text_dim), st.ref.alt_m,
                 theme_hex(TC_text_dim), st.ref.points);
    snprintf(buf, sizeof(buf), "%s%s\n%s\n#%06x Position from %s%s#", alt, have_pos ? grid : "-",
             have_pos ? deg : "GPS, phone or locator needed", theme_hex(TC_text_dim), have_pos ? src : "-",
             have_pos && age > 60 ? " (old)" : "");
    lv_label_set_text(info_lines, buf);
    lv_obj_set_style_bg_color(info_dot, TC(err), 0);
    lv_label_set_text(info_state, "NOT ACTIVE");
}

static void build_ref(void) {
    lv_coord_t h = body_h - STATUS_H - 10;
    char       row[120];
    char       t[32];

    snprintf(t, sizeof(t), "%s MODE", prog_name());
    head_set(prog_icon(), t);
    lv_obj_t *p = panel(body, 8, 6, 384, h);
    icon(p, prog_icon(), 64, 12, 12);
    info_ref   = label(p, VF(36), prog_accent(), 88, 10, 284);
    info_name  = label(p, VF(24), TC(text), 88, 54, 284);
    info_lines = label(p, VF(22), TC(text), 16, 100, 352);
    lv_label_set_long_mode(info_lines, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(info_lines, 6, 0);
    lv_obj_t *s = label(p, VF(18), TC(text_dim), 16, h - 70, 0);
    lv_label_set_text(s, "STATUS");
    info_dot   = dot(p, 18, h - 40, 16, TC(err));
    info_state = label(p, VF(26), TC(err), 44, h - 46, 0);

    static const lv_coord_t widths[] = {130, 88, 150};
    list_create(body, 400, 38, W - 408, h - 32, widths, 3);
    list_head(st.prog == 'S' ? "SUMMIT\tKM\tNAME" : st.prog == 'P' ? "PARK\tKM\tNAME" : st.prog == 'B' ? "BUNKER\tKM\tNAME"
                                 : st.prog == 'F' ? "AREA\tKM\tNAME" : "SITE\tKM\tNAME", 404, 8);
    if (near_n > 0 && near_prog == st.prog) {
        for (int k = 0; k < near_n; k++) {
            snprintf(row, sizeof(row), "%s\t%.*f\t%s", near[k].ref, near[k].km < 10 ? 1 : 0, near[k].km, near[k].name);
            list_add(row);
            if (st.have_ref && !strcmp(near[k].ref, st.ref.ref))
                sel = k;
        }
        list_select(sel);
    }
    prog_info_update();
    lv_label_set_text(status, updating ? (update_text[0] ? update_text : "Downloading the reference lists...")
                              : near_n == -1 || (near_n == 0 && near_prog == st.prog)
                                  ? "No reference list yet: Update list (page 2), with WiFi"
                                  : "MFK: choose the reference, press to use it.  Start: begin the activation");
    buttons_load_page(&page_ref1);
    near_lookup(false);
}

/* A program: the activation */

static void act_update(void) {
    char buf[64];
    int  hz = cparam_i_get(cfg_fg_freq);

    if (!act_freq)
        return;
    const char *band = field_band(hz);
    lv_label_set_text(act_band, band[0] ? band : "--");
    freq_text(buf, sizeof(buf), hz);
    lv_label_set_text(act_freq, buf);
    lv_label_set_text(act_mode, adif_mode());

    /* S-meter: 9 S units of 6 dB, then +10 .. +40 */
    int db = meter_get_raw_db();
    int n  = db <= S1 ? 0 : db <= S9 ? 1 + (db - S1) / 6 : 9 + (db - S9) / 10;
    for (int i = 0; i < 13; i++)
        lv_obj_set_style_bg_color(act_smeter[i], i >= n ? TC(chip) : i < 9 ? TC(ok) : TC(err), 0);
    if (db <= S9)
        snprintf(buf, sizeof(buf), "S%d", db <= S1 ? 0 : 1 + (db - S1) / 6);
    else
        snprintf(buf, sizeof(buf), "S9+%d", db - S9);
    lv_label_set_text(act_sval, buf);

    int s = (int)(time(NULL) - st.start);
    lv_label_set_text_fmt(act_qsos, "%d", act_count());
    lv_label_set_text_fmt(act_time, "%d:%02d", s / 3600, s / 60 % 60);
    lv_label_set_text(act_last, qsos_n ? qsos[qsos_n - 1].call : "-");

    /* The last three QSOs, the newest at the bottom */
    char      recent[200] = "", line[64];
    struct tm tm;
    for (int k = qsos_n > 3 ? qsos_n - 3 : 0; k < qsos_n; k++) {
        localtime_r(&qsos[k].t, &tm);
        snprintf(line, sizeof(line), "%s%02d:%02d  %-10s  %s / %s  %s %s", recent[0] ? "\n" : "", tm.tm_hour, tm.tm_min,
                 qsos[k].call, qsos[k].rst_s, qsos[k].rst_r, qsos[k].mode, field_qso_tag(&qsos[k]));
        strncat(recent, line, sizeof(recent) - strlen(recent) - 1);
    }
    lv_label_set_text(act_recent, recent);
}

static void build_act(void) {
    lv_coord_t h = body_h - STATUS_H - 10;
    char       t[48];

    snprintf(t, sizeof(t), "%s - ACTIVATION (%s)", prog_name(), adif_mode());
    head_set(prog_icon(), t);

    lv_obj_t *p = panel(body, 8, 6, 584, h);
    act_band    = label(p, VF(28), lv_color_black(), 14, 8, 0);
    lv_obj_set_style_bg_color(act_band, prog_accent(), 0);
    lv_obj_set_style_bg_opa(act_band, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(act_band, 10, 0);
    lv_obj_set_style_pad_ver(act_band, 4, 0);
    lv_obj_set_style_radius(act_band, 6, 0);
    act_freq = label(p, VF(60), TC(text), 120, 4, 0);
    act_mode = label(p, VF(36), TC(text), 480, 16, 0);

    /* S-meter segments */
    for (int i = 0; i < 13; i++) {
        act_smeter[i] = lv_obj_create(p);
        lv_obj_remove_style_all(act_smeter[i]);
        lv_obj_set_pos(act_smeter[i], 124 + i * 28, 70);
        lv_obj_set_size(act_smeter[i], 24, 24);
        lv_obj_set_style_radius(act_smeter[i], 2, 0);
        lv_obj_set_style_bg_opa(act_smeter[i], LV_OPA_COVER, 0);
    }
    lv_obj_t *s = label(p, VF(28), TC(text_dim), 102, 67, 0);   /* right before the first segment */
    lv_label_set_text(s, "S");
    act_sval = label(p, VF(28), TC(text), 494, 67, 0);

    lv_obj_t *box = panel(p, 12, 104, 558, h - 120);
    lv_obj_set_style_border_color(box, TC(text_dim), 0);
    act_text = label(box, VF(28), TC(text), 14, 10, 530);
    lv_label_set_long_mode(act_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(act_text, "CQ CQ CQ DE %s\n%s %s", my_call(), prog_name(), st.ref.ref);
    lv_obj_t *name = label(box, VF(34), TC(text_dim), 14, 72, 530);   /* the summit or park, big */
    lv_label_set_recolor(name, false);
    lv_label_set_text(name, st.ref.name);
    act_recent = label(box, VF(22), TC(text_dim), 14, h - 120 - 3 * 24 - 12, 530);

    lv_obj_t *r = panel(body, 600, 6, W - 608, h);
    char               need[24];
    const char        *names[] = {need, "TIME", "LAST"};
    lv_obj_t         **vals[]  = {&act_qsos, &act_time, &act_last};
    const lv_font_t   *fonts[] = {VF(60), VF(44), VF(32)};
    lv_coord_t         ys[]    = {8, 124, 222};
    snprintf(need, sizeof(need), "%s / %d", st.ref.type == 'H' ? "CALLS" : "QSOs", prog_needs());
    for (int k = 0; k < 3; k++) {
        lv_obj_t *n = label(r, VF(22), TC(text_dim), 0, ys[k], W - 610);
        lv_label_set_text(n, names[k]);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        *vals[k] = label(r, fonts[k], k == 0 ? prog_accent() : TC(text), 0, ys[k] + 28, W - 610);
        lv_obj_set_style_text_align(*vals[k], LV_TEXT_ALIGN_CENTER, 0);
    }
    act_update();
    lv_label_set_text(status, is_cw() ? "CQ calls in CW.  Log QSO: call [rst rcvd [rst sent]]"
                                      : "CQ sends your voice CQ (Rec CQ: page 2).  Log QSO: call [rst...]");
    buttons_load_page(&page_act1);
}

static void build_prog(void) {
    if (st.active && st.log[0])
        log_load();
    if (st.active && st.have_ref && st.ref.type == st.prog)
        build_act();
    else
        build_ref();
}

/* Bands: field/bands.c */

static void bands_fill(void) {
    field_bands_t fb;

    field_bands(&fb);
    lv_obj_clean(list);
    rows_n = 0;
    for (int b = 0; b < FIELD_RBN_BANDS; b++) {
        int         level = fb.level[b];
        const char *word  = field_band_word(level);
        uint32_t    color = level >= 4 ? theme_hex(TC_ok) : level >= 2 ? theme_hex(TC_warn) : theme_hex(TC_err);
        char        row[160], heard_txt[40] = "";

        if (fb.heard[b])
            snprintf(heard_txt, sizeof(heard_txt), "#%06x %d  %+d dB#", theme_hex(TC_ok), fb.heard[b], fb.best_snr[b]);
        snprintf(row, sizeof(row), "%s\t\t#%06x %s#\t%d\t%d\t%s\t%s", field_band_name(b), color, word, fb.spots[b],
                 fb.rbn[b], heard_txt, b == fb.best ? "BEST" : "");
        lv_obj_t *r = list_add(row);

        /* The activity bar, in the empty second column */
        lv_obj_t *cell = lv_obj_get_child(r, 1);
        lv_obj_t *bar  = lv_obj_create(cell);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 20 + level * 18, 14);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, (level ? lv_color_hex(color) : TC(chip)), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_color(bar, lv_color_mix((level ? lv_color_hex(color) : TC(chip)), lv_color_black(), 140), 0);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, 0);
    }
    list_select(sel);

    field_rbn_hit_t hits[6];
    int             n = field_rbn_heard(FIELD_HEARD_S, hits, 6), p = 0;
    char            line[200];

    p = snprintf(line, sizeof(line), "RBN %s.  ", field_rbn_state());
    if (!n)
        p += snprintf(line + p, sizeof(line) - p, "Nobody heard you in the last 10 min: Test beacon (CW)");
    else
        p += snprintf(line + p, sizeof(line) - p, "Heard by ");
    for (int k = 0; k < n && p < (int)sizeof(line); k++)
        p += snprintf(line + p, sizeof(line) - p, "%s %+d dB  ", hits[k].skimmer, hits[k].snr);
    lv_label_set_text(status, line);
}

static void build_bands(void) {
    static const lv_coord_t widths[] = {72, 150, 100, 80, 90, 160, 90};

    head_set("bands", "BAND OVERVIEW");
    list_create(body, 8, 36, W - 16, body_h - STATUS_H - 64, widths, 7);
    lv_label_set_text(label(body, VF(18), TC(text_dim), 14, body_h - STATUS_H - 24, W - 28),
                      "Skimmer data: Reverse Beacon Network (reversebeacon.net)");
    list_head("BAND\tACTIVITY\t\tSPOTS\tRBN\tHEARD YOU\t", 13, 6);
    lv_obj_set_style_text_font(list, VF(22), 0);
    sel = field_band_of(cparam_i_get(cfg_fg_freq));
    bands_fill();
    buttons_load_page(&page_bands);
}

/* Logbook */

static void build_log(void) {
    static const lv_coord_t widths[] = {52, 90, 200, 80, 80, 140};
    char                    row[120], t[80];   /* room for the reference and the red TEST */
    struct tm               tm;

    /* TEST in red in the title: the page and its QRZ upload are experimental (0.127) */
    snprintf(t, sizeof(t), "%s LOGBOOK  %s  #FF2020 TEST#", st.have_ref ? prog_name_of(st.ref.type) : "",
             st.have_ref ? st.ref.ref : "");
    lv_label_set_recolor(title, true);
    head_set("log", t);
    list_create(body, 8, 36, W - 16, body_h - STATUS_H - 40, widths, 6);
    list_head("N\tTIME\tCALL\tBAND\tMODE\tRST S / R", 13, 6);
    log_load();
    for (int k = 0; k < qsos_n; k++) {
        localtime_r(&qsos[k].t, &tm);
        snprintf(row, sizeof(row), "%d\t%02d:%02d\t%s\t%s\t%s %s\t%s / %s", qsos_total - qsos_n + k + 1, tm.tm_hour, tm.tm_min, qsos[k].call,
                 field_band(qsos[k].freq_hz), qsos[k].mode, field_qso_tag(&qsos[k]), qsos[k].rst_s, qsos[k].rst_r);
        list_add(row);
    }
    list_select(qsos_n - 1);
    lv_label_set_text_fmt(status, "%d QSO%s   %s", qsos_total, qsos_total == 1 ? "" : "s",
                          st.log[0] ? strrchr(st.log, '/') + 1 : "No activation yet");
    buttons_load_page(&page_log);
}

/* VOL and MFK in a bar over the buttons: what each controls, its level and value. A press (choosing
 * what it controls) turns it amber with < >. The activation shows both; the other screens keep their
 * hint line and show VOL beside it (MFK chooses in their lists). */
typedef struct {
    lv_obj_t *box, *tag, *name, *bar, *fill, *val;
    lv_coord_t w;
} knob_chip_t;

static knob_chip_t chips[2];   /* VOL, MFK */

static void chip_create(knob_chip_t *c, const char *tag, lv_coord_t y) {
    c->box = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(c->box);
    lv_obj_set_y(c->box, y);
    lv_obj_set_height(c->box, 30);
    lv_obj_set_style_bg_color(c->box, TC(bg), 0);
    lv_obj_set_style_bg_opa(c->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c->box, 1, 0);
    lv_obj_set_style_radius(c->box, 6, 0);
    lv_obj_clear_flag(c->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    c->tag = label(c->box, VF(20), lv_color_black(), 4, 4, 0);
    lv_label_set_text(c->tag, tag);
    lv_obj_set_style_bg_opa(c->tag, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(c->tag, 5, 0);
    lv_obj_set_style_pad_ver(c->tag, 2, 0);
    lv_obj_set_style_radius(c->tag, 4, 0);
    c->name = label(c->box, VF(24), TC(text_dim), 64, 4, 0);
    c->bar  = lv_obj_create(c->box);
    lv_obj_remove_style_all(c->bar);
    lv_obj_set_style_bg_color(c->bar, TC(chip), 0);
    lv_obj_set_style_bg_opa(c->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c->bar, 3, 0);
    c->fill = lv_obj_create(c->bar);
    lv_obj_remove_style_all(c->fill);
    lv_obj_set_style_bg_color(c->fill, TC(btn_mark), 0);
    lv_obj_set_style_bg_opa(c->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c->fill, 3, 0);
    c->val = label(c->box, VF(24), TC(text), 0, 4, 90);
    lv_obj_set_style_text_align(c->val, LV_TEXT_ALIGN_RIGHT, 0);
}

static void chip_place(knob_chip_t *c, lv_coord_t x, lv_coord_t w) {
    c->w = w;
    lv_obj_set_x(c->box, x);
    lv_obj_set_width(c->box, w);
    lv_obj_set_pos(c->bar, 190, 12);
    lv_obj_set_size(c->bar, w - 190 - 70, 6);
    lv_obj_set_x(c->val, w - 100);
    lv_obj_clear_flag(c->box, LV_OBJ_FLAG_HIDDEN);
}

static void chip_update(knob_chip_t *c, bool vol) {
    const char *name;
    char        val[32];
    float       level;
    bool        edit;

    if (!c->box || lv_obj_has_flag(c->box, LV_OBJ_FLAG_HIDDEN) || !knobs_info(vol, &name, val, sizeof(val), &level, &edit))
        return;
    lv_obj_set_style_bg_color(c->tag, edit ? TC(btn_mark) : TC(warn), 0);
    lv_obj_set_style_border_color(c->box, edit ? TC(panel_border) : TC(warn), 0);
    lv_obj_set_style_text_color(c->name, edit ? TC(text_dim) : TC(text), 0);
    if (edit) {
        lv_label_set_text(c->name, name);
        lv_label_set_text(c->val, val);
    } else {
        lv_label_set_text_fmt(c->name, "<  %s  >", name);
        lv_label_set_text(c->val, "");
    }
    if (edit && level >= 0) {
        lv_coord_t bw = lv_obj_get_width(c->bar);
        lv_obj_set_size(c->fill, LV_MAX(6, (lv_coord_t)(bw * LV_MIN(level, 1.0f))), 6);
        lv_obj_clear_flag(c->bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void chips_update(void) {
    chip_update(&chips[0], true);
    chip_update(&chips[1], false);
}

/* After a screen is built: both knobs on the activation, VOL beside the hint line elsewhere */
static void chips_layout(void) {
    if (act_freq) {
        lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
        chip_place(&chips[0], 8, 388);
        chip_place(&chips[1], 404, 388);
    } else {
        lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(status, W - 28 - 310);
        chip_place(&chips[0], W - 308, 300);
        lv_obj_add_flag(chips[1].box, LV_OBJ_FLAG_HIDDEN);
    }
    chips_update();
}

static void show(screen_t s) {
    if (st.screen != s)
        sel = 0;
    st.screen = s;
    state_save();
    end_armed = false;
    spot_armed = false;
    info_ref  = NULL;
    act_freq  = NULL;
    list      = NULL;
    lv_obj_clean(body);
    buttons_unload_page();
    switch (s) {
        case SCR_HOME:
            build_home();
            home_select(st.home_sel);
            break;
        case SCR_PROG:
            build_prog();
            break;
        case SCR_BANDS:
            build_bands();
            break;
        case SCR_LOG:
            build_log();
            break;
    }
    chips_layout();
}

static void refresh(void) {
    if (dialog.run)
        show(st.screen);
}

/* Actions (also from the companion app) */

static void spot_done(const char *line);

static void spot_to(bool cluster);

static bool hota_key(void) {
    return access(HOTA_KEY, R_OK) == 0;
}

static bool wwff_key(void) {
    return access(WWFF_KEY, R_OK) == 0;
}

/* The spot on the program's site (POTA, WWBOTA, HOTA, WWFF); qrt: the activation ended */
static bool spot_job(bool qrt) {
    double      khz = cparam_i_get(cfg_fg_freq) / 1000.0;
    const char *m   = adif_mode();

    if (!ref_ok(st.ref.ref))   /* it goes on a command line */
        return false;
    if (st.ref.type == 'B')
        return job_start(spot_done, HELPER " wwbota-spot --call %s --freq-khz %.1f --ref %s --mode %s --id '%s'%s",
                         my_call(), khz, st.ref.ref, m, wwbota_id, qrt ? " --qrt" : "");
    return job_start(spot_done, HELPER " %s --call %s --freq-khz %.1f --ref %s --mode %s%s",
                     st.ref.type == 'H' ? "hota-spot" : st.ref.type == 'F' ? "wwff-spot" : "pota-spot", my_call(), khz, st.ref.ref, m,
                     qrt ? " --comments QRT" : "");
}

static void qsy_cb(lv_timer_t *t) {
    bool moved;

    if (!st.active || st.ref.type == 'S' || !qsy.spot_at) {
        return;
    }
    moved = abs(cparam_i_get(cfg_fg_freq) - spot_hz) >= 1000 || strcmp(adif_mode(), spot_mode);
    /* ponytail: TX polled every 500 ms, a shorter transmission is missed */
    if (field_qsy_step(&qsy, param_i_get(cfg_field_respot), moved, radio_get_state() == RADIO_TX, time(NULL))
        && !atomic_load(&busy)) {
        spot_to(false);         /* pota.app only: a cluster spot can't be withdrawn, the old one would stay */
    }
}

static bool activation_start(void) {
    char      name[40], *p;
    struct tm tm;

    if (st.active)
        return true;
    if (!st.have_ref || st.ref.type != st.prog) {
        msg_update_text_fmt("Choose the %s reference first", prog_name());
        return false;
    }
    if (!my_call()[0]) {
        msg_update_text_fmt("Set your callsign in Settings");
        return false;
    }
    st.active = true;
    st.start  = time(NULL);
    spotted   = false;
    wwbota_id[0] = 0;
    gmtime_r(&st.start, &tm);
    snprintf(name, sizeof(name), "%s", st.ref.ref);
    for (p = name; *p; p++)
        if (*p == '/')
            *p = '_';
    mkdir(FIELD_DIR, 0755);
    mkdir(LOGS_DIR, 0755);
    snprintf(st.log, sizeof(st.log), "%s/%04d%02d%02d-%02d%02d_%s.adi", LOGS_DIR, tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, name);
    qsos_n = qsos_total = 0;
    st.screen = SCR_PROG;
    state_save();
    msg_update_text_fmt("Activation started: %s", st.ref.ref);
    refresh();
    qsy.spot_at = 0;
    if (st.ref.type != 'S') {
        spot_to(true);               /* auto spot; QSYs spot again (qsy_cb) */
        if (!qsy_timer) qsy_timer = lv_timer_create(qsy_cb, 500, NULL);
    }
    return true;
}

static void activation_end(void) {
    int s = (int)(time(NULL) - st.start);

    if (!st.active)
        return;
    st.active = false;
    state_save();
    if (qsy_timer) {
        lv_timer_del(qsy_timer);
        qsy_timer = NULL;
    }
    msg_update_text_fmt("Activation complete: %s, %d QSO%s, %d min", st.ref.ref, qsos_total, qsos_total == 1 ? "" : "s", s / 60);
    /* ponytail: spotted is not saved, a restart mid-activation sends no QRT */
    if (spotted && st.ref.type != 'S' && spot_job(true))
        spotted = false;
    if (dialog.run)
        show(SCR_PROG);
}

static void activate_cb(button_data_t *b) {
    if (!st.active) {
        activation_start();
        return;
    }
    if (!end_armed || time(NULL) - end_armed_at > 4) {
        end_armed    = true;
        end_armed_at = time(NULL);
        buttons_refresh(&b_act);
        msg_update_text_fmt("Press End again to end the activation");
        return;
    }
    activation_end();
}

static void spot_done(const char *line) {
    if (!strncmp(line, "ok", 2) && (!line[2] || line[2] == ' ')) {
        spotted = st.active;
        /* wwbota.org: "ok <id>", the spot the next QSY or the QRT updates */
        if (st.ref.type == 'B')
            snprintf(wwbota_id, sizeof(wwbota_id), "%.*s", st.active && line[2] ? (int)strspn(line + 3, "0123456789abcdefABCDEF-") : 0,
                     line + 3);   /* it goes on a command line */
        line = "ok";
    }
    msg_update_text_fmt("%s spot: %s", prog_name_of(st.ref.type), line);
}

static void spot_to(bool cluster) {
    if (!st.have_ref || !ref_ok(st.ref.ref)) {
        msg_update_text_fmt("Choose the %s reference first", prog_name());
        return;
    }
    if (!my_call()[0]) {
        msg_update_text_fmt("Set your callsign in Settings");
        return;
    }
    if (st.ref.type == 'S') {
        /* On the DX cluster: SOTAwatch's API needs a client approved by SOTA, APRS2SOTA a
         * registration (RBNHole still spots a CW CQ after an alert on SOTAwatch) */
        char comment[48];
        snprintf(comment, sizeof(comment), "SOTA %s %s", st.ref.ref, adif_mode());
        if (cluster_client_send_spot(cparam_i_get(cfg_fg_freq) / 1000.0, my_call(), comment))
            msg_update_text_fmt("SOTA spot on the DX clusters: %s %.1f %s", st.ref.ref,
                                cparam_i_get(cfg_fg_freq) / 1000.0, adif_mode());
        else
            msg_update_text_fmt("DX cluster not running: no SOTA spot");
        return;
    }
    char        comment[48];
    const char *p = prog_name_of(st.ref.type);
    spot_hz = cparam_i_get(cfg_fg_freq);
    snprintf(spot_mode, sizeof(spot_mode), "%s", adif_mode());
    qsy.spot_at = time(NULL);
    snprintf(comment, sizeof(comment), "%s %s %s", p, st.ref.ref, adif_mode());
    if (cluster)
        cluster_client_send_spot(cparam_i_get(cfg_fg_freq) / 1000.0, my_call(), comment);   /* the DX clusters too */
    if ((st.ref.type == 'H' && !hota_key()) || (st.ref.type == 'F' && !wwff_key())) {
        /* cqhota.app and the WWFF Spotline take spots with the activator's own key only */
        msg_update_text_fmt(cluster ? "%s spot on the DX clusters only: no %s key (web page, Field Ops)"
                                    : "No %s spot: no %s key (web page, Field Ops)", p, p);
        return;
    }
    if (spot_job(false))
        msg_update_text_fmt("%s spot: %s %.1f %s...", p, st.ref.ref, cparam_i_get(cfg_fg_freq) / 1000.0, adif_mode());
}

static void spot(void) {
    spot_to(true);
}

static void spot_cb(button_data_t *b) {
    if (!spot_armed || time(NULL) - spot_armed_at > 4) {
        spot_armed    = true;
        spot_armed_at = time(NULL);
        buttons_refresh(&b_spot);
        msg_update_text_fmt("Press Spot again to post %s %.1f %s", st.have_ref ? st.ref.ref : "",
                            cparam_i_get(cfg_fg_freq) / 1000.0, adif_mode());
        return;
    }
    spot_armed = false;
    buttons_refresh(&b_spot);
    spot();
}

static void rec_cq_cb(button_data_t *b) {
    char name[16];

    snprintf(name, sizeof(name), "CQ %s", prog_name());
    if (msg_voice_rec_toggle(name))
        msg_update_text_fmt("Recording \"%s\": speak, then Stop rec", name);
    else if (dialog_msg_voice_get_state() == MSG_VOICE_OFF && msg_voice_exists(name))
        msg_update_text_fmt("Saved \"%s\": CQ sends it in SSB", name);
    buttons_refresh(b);
}

static void cq_cb(button_data_t *b) {
    char text[80];

    if (!is_cw()) {
        /* The voice CQ recorded with Rec CQ ("CQ SOTA", "CQ POTA"... in /mnt/msg, also in MSG > Voice) */
        snprintf(text, sizeof(text), "CQ %s", prog_name());
        if (msg_voice_send_toggle(text))
            msg_update_text_fmt("CQ: sending \"%s\" (CQ again: stop, hold CQ: level)", text);
        else if (!msg_voice_exists(text))
            msg_update_text_fmt("No voice CQ yet: Rec CQ on page 2, speak, Stop rec");
        return;
    }
    snprintf(text, sizeof(text), "CQ CQ %s DE %s %s K", prog_name(), my_call(), my_call());
    cw_encoder_send(text, false);
}

/* The phone's "CALL|rst sent|rst received[|name|qth|grid|country]", any field may be empty */
static bool activation_log_add(const field_qso_t *q);

static void parse_log_text(const char *text, char *call, size_t call_size, char *rs, char *rr, field_qso_t *more) {
    char buf[256], *p = buf, *f;
    struct { char *dst; size_t size; } out[] = {
        {call, call_size}, {rs, 8}, {rr, 8}, {more->name, sizeof(more->name)},
        {more->qth, sizeof(more->qth)}, {more->grid, sizeof(more->grid)}, {more->country, sizeof(more->country)},
    };

    snprintf(buf, sizeof(buf), "%s", text);
    for (size_t i = 0; i < sizeof(out) / sizeof(out[0]) && (f = strsep(&p, "|")); i++)
        snprintf(out[i].dst, out[i].size, "%s", f);
}

/* The day's log, LOGS_DIR/YYYYMMDD.adi (UTC date of the QSO): every QSO outside Field Ops */
static bool day_log_add(const field_qso_t *q, const char *me) {
    field_station_t sta = {0};
    char            path[64];
    struct tm       tm;

    snprintf(sta.call, sizeof(sta.call), "%s", me);
    snprintf(sta.grid, sizeof(sta.grid), "%s", station_qth() ? station_qth() : "");
    gmtime_r(&q->t, &tm);
    snprintf(path, sizeof(path), "%s/%04d%02d%02d.adi", LOGS_DIR, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    if (!field_log_add(path, q, &sta)) {
        msg_schedule_text_ms(9000, "QSO not saved: %s", field_log_error(errno));   /* long: it scrolls */
        return false;
    }
    return true;
}

/* The phone's logger outside Field Ops (QSOs in voice and so on): the day's log, on the radio's frequency and mode */
bool dialog_field_qso_log(const char *text) {
    field_qso_t     q   = {.t = time(NULL), .freq_hz = cparam_i_get(cfg_fg_freq)};
    const char     *rst = is_cw() ? "599" : "59";
    char            call[32] = "", rs[8] = "", rr[8] = "";

    parse_log_text(text, call, sizeof(call), rs, rr, &q);
    if (!ref_ok(call)) {
        msg_update_text_fmt("Log from the phone: \"%s\" not logged", call);
        return false;
    }
    for (char *p = call; *p; p++)
        *p = toupper((unsigned char)*p);
    snprintf(q.call, sizeof(q.call), "%s", call);
    snprintf(q.mode, sizeof(q.mode), "%s", adif_mode());
    snprintf(q.rst_s, sizeof(q.rst_s), "%s", rs[0] ? rs : rst);
    snprintf(q.rst_r, sizeof(q.rst_r), "%s", rr[0] ? rr : rst);
    if (!day_log_add(&q, my_call()))
        return false;
    msg_update_text_fmt("QSO logged: %s %s %s", q.call, q.rst_s, q.rst_r);
    return true;
}

/* FT8, FT4 and JS8 QSOs into the day's log too, next to the phone's */
void dialog_field_day_log(const qso_log_record_t *r) {
    field_qso_t q = {.t = r->time, .freq_hz = (int32_t)(r->freq_mhz * 1e6f + 0.5f)};

    snprintf(q.call, sizeof(q.call), "%s", r->remote_call);
    snprintf(q.mode, sizeof(q.mode), "%s", r->mode == MODE_FT4 ? "FT4" : r->mode == MODE_JS8 ? "JS8" : r->mode == MODE_FT2 ? "FT2" : "FT8");
    /* The digital reports signed in two digits ("+01", "-05"), as in every logbook */
    bool digi = r->mode == MODE_FT8 || r->mode == MODE_FT4 || r->mode == MODE_FT2 || r->mode == MODE_JS8;
    snprintf(q.rst_s, sizeof(q.rst_s), digi ? "%+03d" : "%d", r->rsts);
    snprintf(q.rst_r, sizeof(q.rst_r), digi ? "%+03d" : "%d", r->rstr);
    snprintf(q.name, sizeof(q.name), "%s", r->name);
    snprintf(q.grid, sizeof(q.grid), "%s", r->remote_grid);
    /* An activation going on: the QSO counts for it and goes in its log, as the voice QSOs
     * logged here do; otherwise in the day's log. Not both: the web page lists every log */
    if (st.active)
        activation_log_add(&q);
    else
        day_log_add(&q, r->local_call);
}

/* A QSO in the log: false when the call is not a callsign */
/* more: name, QTH, grid and country of the other station (the phone's QRZ lookup), or NULL */
static bool log_add(const char *call_in, const char *rst_s, const char *rst_r, const field_qso_t *more) {
    field_qso_t     q   = {.t = time(NULL), .freq_hz = cparam_i_get(cfg_fg_freq)};
    field_station_t sta;
    const char     *rst = is_cw() ? "599" : "59";
    char            call[32];

    snprintf(call, sizeof(call), "%s", call_in);
    if (!st.active || !ref_ok(call))
        return false;
    for (char *p = call; *p; p++)
        *p = toupper((unsigned char)*p);
    snprintf(q.call, sizeof(q.call), "%s", call);
    snprintf(q.mode, sizeof(q.mode), "%s", adif_mode());
    snprintf(q.rst_s, sizeof(q.rst_s), "%s", rst_s && rst_s[0] ? rst_s : rst);
    snprintf(q.rst_r, sizeof(q.rst_r), "%s", rst_r && rst_r[0] ? rst_r : rst);
    if (more) {
        memcpy(q.name, more->name, sizeof(q.name));
        memcpy(q.qth, more->qth, sizeof(q.qth));
        memcpy(q.grid, more->grid, sizeof(q.grid));
        memcpy(q.country, more->country, sizeof(q.country));
        memcpy(q.their_ref, more->their_ref, sizeof(q.their_ref));
    }
    return activation_log_add(&q);
}

/* Into the activation's log, with the reference of the activation; the screen follows */
static bool activation_log_add(const field_qso_t *q) {
    field_station_t sta;

    snprintf(sta.call, sizeof(sta.call), "%s", my_call());
    snprintf(sta.grid, sizeof(sta.grid), "%s", station_qth() ? station_qth() : "");
    sta.prog = st.ref.type;
    snprintf(sta.ref, sizeof(sta.ref), "%s", st.ref.ref);
    if (!field_log_add(st.log, q, &sta)) {
        msg_schedule_text_ms(9000, "QSO not saved: %s", field_log_error(errno));   /* long: it scrolls */
        return false;
    }
    log_load();
    msg_update_text_fmt("QSO %d: %s %s %s %s", qsos_total, q->call, q->rst_s, q->rst_r, field_qso_tag(q));
    if (dialog.run && (st.screen == SCR_LOG || act_freq))
        refresh();
    return true;
}

/* Log QSO on the radio: "CALL [rst received [rst sent]]" */

static void kbd_end(void) {
    textarea_window_close();
    kbd_open = false;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static bool log_ok(void) {
    const char *v = textarea_window_get();
    char        call[32] = "", w[3][16] = {"", "", ""}, r1[8] = "", r2[8] = "";
    field_qso_t more = {0};
    int         n_rst = 0;

    /* "CALL [rst rcvd [rst sent]] [their reference]": a word shaped as a reference is theirs */
    if (v && sscanf(v, "%31s %15s %15s %15s", call, w[0], w[1], w[2]) >= 1) {
        for (int i = 0; i < 3 && w[i][0]; i++) {
            for (char *p = w[i]; *p; p++) *p = toupper((unsigned char)*p);
            if (field_ref_prog(w[i]))
                snprintf(more.their_ref, sizeof(more.their_ref), "%s", w[i]);
            else if (n_rst == 0)
                snprintf(r1, sizeof(r1), "%s", w[i]), n_rst++;
            else if (n_rst == 1)
                snprintf(r2, sizeof(r2), "%s", w[i]), n_rst++;
        }
        if (!log_add(call, r2, r1, &more))
            msg_update_text_fmt("Log: \"%s\" is not a callsign", call);
    }
    kbd_end();
    return true;
}

static bool kbd_cancel(void) {
    kbd_end();
    return true;
}

static void log_cb(button_data_t *b) {
    if (!st.active) {
        msg_update_text_fmt("Start the activation first");
        return;
    }
    kbd_open = true;
    lv_group_remove_obj(dialog.obj);
    textarea_window_open_w_label(log_ok, kbd_cancel, "Call [rst rcvd [sent]] [their ref]: ");
}

static bool set_ref(const char *in) {
    field_ref_t r;
    char        ref[16];

    snprintf(ref, sizeof(ref), "%s", in ? in : "");
    for (char *p = ref; *p; p++)
        *p = toupper((unsigned char)*p);
    if (st.active || !ref[0])
        return false;
    if (!field_refs_find(REFS_PATH, ref, &r)) {
        msg_update_text_fmt("%s: not in the reference list", ref);
        return false;
    }
    st.ref      = r;
    st.have_ref = true;
    st.prog     = r.type;
    state_save();
    return true;
}

static bool ref_ok_cb(void) {
    set_ref(textarea_window_get());
    kbd_end();
    refresh();
    return true;
}

static void ref_cb(button_data_t *b) {
    if (st.active) {
        msg_update_text_fmt("End the activation to change the reference");
        return;
    }
    kbd_open = true;
    lv_group_remove_obj(dialog.obj);
    textarea_window_open_w_label(ref_ok_cb, kbd_cancel, "Reference: ");
}

/* "<list n> <lists> <bytes> <total> <retry> <name>" in .update.progress (x6100-field) */
static void update_timer_cb(lv_timer_t *t) {
    FILE     *f = fopen(FIELD_DIR "/.update.progress", "r");
    int       n, lists, retry, len;
    long long done, total;
    char      name[16], text[sizeof(update_text)];
    bool      ok;

    if (!f)
        return;
    ok = fscanf(f, "%d %d %lld %lld %d %15s", &n, &lists, &done, &total, &retry, name) == 6;
    fclose(f);
    if (!ok)
        return;
    len = snprintf(text, sizeof(text), "Updating %s (%d/%d)", name, n, lists);
    if (done > 0 && done == total)   /* downloaded: x6100-field reads it (SOTA: about 2 minutes) */
        len += snprintf(text + len, sizeof(text) - len, ": %.1f MB, reading it...", done / 1e6);
    else if (done > 0 && total > 0)
        len += snprintf(text + len, sizeof(text) - len, ": %.1f / %.1f MB", done / 1e6, total / 1e6);
    else if (done > 0)
        len += snprintf(text + len, sizeof(text) - len, ": %.1f MB", done / 1e6);
    if (retry)
        snprintf(text + len, sizeof(text) - len, ", retry %d of 5", retry);
    if (!strcmp(text, update_text))
        return;
    strcpy(update_text, text);
    if (dialog.run && st.screen == SCR_PROG && !st.active)
        lv_label_set_text(status, text);
    else
        msg_update_text_fmt("%s", text);
}

static void update_done(const char *line) {
    updating = false;
    if (update_timer) {
        lv_timer_del(update_timer);
        update_timer = NULL;
    }
    update_text[0] = 0;
    if (!strncmp(line, "ok", 2)) {
        int         p = 0, s = 0, b = 0, h = 0, w = 0;
        const char *partial = strstr(line, " partial ");

        sscanf(line, "ok %d %d %d %d %d", &p, &s, &b, &h, &w);
        msg_update_text_fmt("Reference list: %d parks, %d summits, %d bunkers, %d HOTA, %d WWFF", p, s, b, h, w);
        if (partial)    /* a list that could not be fetched kept its previous entries */
            msg_schedule_text_ms(6000, "Not updated, kept as it was: %s", partial + 9);
        near_lookup(true);
    } else {
        msg_update_text_fmt("Reference list: %s", line);
    }
    if (dialog.run && st.screen == SCR_PROG && !st.active)
        lv_label_set_text(status, line);
}

static void list_update(void) {
    if (job_start(update_done, HELPER " update --call %s", my_call()[0] ? my_call() : "unknown")) {
        updating = true;
        msg_update_text_fmt("Downloading the reference lists: up to 10 minutes on a slow WiFi");
        if (dialog.run && st.screen == SCR_PROG && !st.active)
            lv_label_set_text(status, "Downloading the reference lists...");
        if (!update_timer)
            update_timer = lv_timer_create(update_timer_cb, 1000, NULL);
    }
}

static void update_cb(button_data_t *b) {
    list_update();
}

void dialog_field_test_beacon(void) {
    char text[64];

    if (!my_call()[0]) {
        msg_update_text_fmt("Set your callsign in Settings");
        return;
    }
    if (!is_cw()) {
        msg_update_text_fmt("Test beacon: switch to CW first");
        return;
    }
    snprintf(text, sizeof(text), "TEST DE %s %s %s", my_call(), my_call(), my_call());
    cw_encoder_send(text, false);
    msg_update_text_fmt("Test beacon on the air: watch who hears you");
}

static void beacon_cb(button_data_t *b) {
    dialog_field_test_beacon();
}

void dialog_field_tune_band(int band) {
    if (band < 0 || band >= FIELD_RBN_BANDS)
        return;
    if (main_screen_freq_locked()) {
        msg_update_text_fmt("The frequency is locked");
        return;
    }
    int32_t f = field_band_freq(band, is_cw());
    main_screen_set_freq(f);
    if (!is_cw() && !main_screen_mode_locked())
        cparam_i_set(cfg_cur_mode, f < 10000000 ? radio_mode_lsb : radio_mode_usb);
    msg_update_text_fmt("%s: %.3f MHz", field_band_name(band), f / 1e6);
}

static void tune_cb(button_data_t *b) {
    dialog_field_tune_band(sel);
}

static void qrz_done(const char *line) {
    int  sent = 0, dup = 0, bad = 0;
    char reason[80] = "";

    if (sscanf(line, "ok %d %d %d %79[^\n]", &sent, &dup, &bad, reason) >= 2) {
        msg_update_text_fmt("QRZ: %d sent, %d already there%s", sent, dup, bad ? ", some refused" : "");
        if (bad)    /* QRZ took the others; the first reason it gave */
            msg_schedule_text_ms(6000, "QRZ refused %d: %s", bad, reason);
    }
    else if (!strcmp(line, "error no QRZ key")) {
        /* the message holds 127 characters: the notice goes in two steps */
        msg_update_text_fmt("No QRZ profile: add it on the web page, Field Ops");
        msg_schedule_text_ms(8000, "Uploading needs a QRZ.com Logbook API key, which QRZ.com only gives with a paid "
                                   "QRZ.com subscription (XML level or higher).");
    } else
        msg_update_text_fmt("QRZ: %s", line);
}

static void qrz_cb(button_data_t *b) {
    if (!st.log[0] || access(st.log, R_OK) != 0) {
        msg_update_text_fmt("No log to upload");
        return;
    }
    if (job_start(qrz_done, HELPER " qrz-upload '%s'", st.log))
        msg_update_text_fmt("Uploading the log to QRZ...");
}

/* The QRZ Logbook profiles (qrz.conf, written on the web page: call1/key1 .. call3/key3, active=N;
 * an old single key= is profile 1). The button shows the active one and turns to the next with a key. */
#define QRZ_PROFILES 3
#define QRZ_CONF     FIELD_DIR "/qrz.conf"

typedef struct {
    int  active;
    char call[QRZ_PROFILES][20];
    bool key[QRZ_PROFILES];
} qrz_conf_t;

static void qrz_conf_read(qrz_conf_t *c) {
    char  line[256];
    bool  legacy = false;
    FILE *f = fopen(QRZ_CONF, "r");

    memset(c, 0, sizeof(*c));
    c->active = 1;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        int   n;

        if (!v) continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "active"))
            c->active = atoi(v);
        else if (!strcmp(line, "key"))
            legacy = *v != 0;
        else if (sscanf(line, "call%d", &n) == 1 && n >= 1 && n <= QRZ_PROFILES)
            snprintf(c->call[n - 1], sizeof(c->call[0]), "%s", v);
        else if (sscanf(line, "key%d", &n) == 1 && n >= 1 && n <= QRZ_PROFILES)
            c->key[n - 1] = *v != 0;
    }
    fclose(f);
    if (legacy && !c->key[0] && !c->key[1] && !c->key[2]) c->key[0] = true;
    if (c->active < 1 || c->active > QRZ_PROFILES || !c->key[c->active - 1]) {
        c->active = 0;
        for (int i = 0; i < QRZ_PROFILES && !c->active; i++)
            if (c->key[i]) c->active = i + 1;
    }
}

/* active=N replaced (or added), the rest of the file as it is */
static bool qrz_conf_set_active(int active) {
    char  lines[16][256];
    int   n = 0;
    bool  seen = false;
    FILE *f = fopen(QRZ_CONF, "r");

    if (!f) return false;
    while (n < 16 && fgets(lines[n], sizeof(lines[0]), f)) {
        if (!strncmp(lines[n], "active=", 7)) {
            if (seen) continue;
            snprintf(lines[n], sizeof(lines[0]), "active=%d\n", active);
            seen = true;
        }
        n++;
    }
    fclose(f);
    f = fopen(QRZ_CONF ".tmp", "w");
    if (!f) return false;
    if (!seen) fprintf(f, "active=%d\n", active);
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fclose(f);
    return rename(QRZ_CONF ".tmp", QRZ_CONF) == 0;
}

static const char *qrz_prof_label(void) {
    static char s[32];
    qrz_conf_t  c;

    qrz_conf_read(&c);
    if (!c.active)
        snprintf(s, sizeof(s), "QRZ:\nnone");
    else if (c.call[c.active - 1][0])
        snprintf(s, sizeof(s), "QRZ:\n%s", c.call[c.active - 1]);
    else
        snprintf(s, sizeof(s), "QRZ:\nprofile %d", c.active);
    return s;
}

static void qrz_prof_cb(button_data_t *b) {
    qrz_conf_t c;
    int        n = 0, next = 0;

    qrz_conf_read(&c);
    for (int i = 0; i < QRZ_PROFILES; i++) n += c.key[i];
    if (n == 0) {
        msg_update_text_fmt("No QRZ profile: add it on the web page, Field Ops");
        return;
    }
    if (n == 1) {
        msg_update_text_fmt("Only one QRZ profile");
        return;
    }
    for (int i = 1; i <= QRZ_PROFILES && !next; i++) {
        int k = (c.active - 1 + i) % QRZ_PROFILES;
        if (c.key[k]) next = k + 1;
    }
    if (!qrz_conf_set_active(next)) {
        msg_update_text_fmt("QRZ profile not saved");
        return;
    }
    msg_update_text_fmt("QRZ profile: %s", c.call[next - 1][0] ? c.call[next - 1] : "no callsign");
    buttons_refresh(b);
}

/* Keys, touch and timer */

/* MFK press or Enter: open the tile, take the reference, tune the band */
static void press(void) {
    if (kbd_open)
        return;
    if (st.screen == SCR_HOME) {
        home_open(sel);
    } else if (st.screen == SCR_PROG && !st.active && sel < near_n && near_prog == st.prog) {
        st.ref      = near[sel];
        st.have_ref = true;
        state_save();
        prog_info_update();
    } else if (st.screen == SCR_BANDS) {
        tune_cb(NULL);
    }
}

static void click_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    bool        mfk   = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;   /* the MFK press, not a tap */

    if (mfk && cq_level_adj) {
        cq_level_cb(NULL);
    } else if (mfk && act_freq) {
        main_screen_mfk(0);
        chips_update();
    } else {
        press();
    }
}

static void row_click_cb(lv_event_t *e) {
    list_select((int)(intptr_t)lv_event_get_user_data(e));
    press();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (kbd_open)
        return;
    if (cq_level_adj) {
        switch (key) {
            case LV_KEY_UP:
            case LV_KEY_LEFT:
            case KEY_VOL_LEFT_EDIT:
            case KEY_VOL_LEFT_SELECT:
                cq_level_change(-1);
                return;
            case LV_KEY_DOWN:
            case LV_KEY_RIGHT:
            case KEY_VOL_RIGHT_EDIT:
            case KEY_VOL_RIGHT_SELECT:
                cq_level_change(1);
                return;
            case LV_KEY_ENTER:
            case LV_KEY_ESC:
                cq_level_cb(NULL);
                return;
        }
    }
    switch (key) {
        case KEY_VOL_PRESS:
        case LV_KEY_ESC:   /* as on the main screen (Back and Radio leave) */
            main_screen_vol_press();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            if (act_freq)   /* the activation has no list: MFK as on the main screen */
                main_screen_mfk(-1);
            else if (st.screen == SCR_HOME)
                home_select(sel - 1);
            else
                list_select(sel - 1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            if (act_freq)
                main_screen_mfk(1);
            else if (st.screen == SCR_HOME)
                home_select(sel + 1);
            else
                list_select(sel + 1);
            break;
        case LV_KEY_ENTER:
            if (act_freq)
                main_screen_mfk(0);
            else
                press();
            break;
        /* VOL as on the main screen: volume, squelch, RF gain... with its label over the screen */
        case KEY_VOL_LEFT_EDIT:
            vol_update(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
            vol_update(1);
            break;
        case KEY_VOL_LEFT_SELECT:
            vol_change_ctrl(-1);
            break;
        case KEY_VOL_RIGHT_SELECT:
            vol_change_ctrl(1);
            break;
    }
    chips_update();
}

/* A QSO edited or deleted from the phone rewrites the log: read it again, so the count, the last call
 * and the logbook follow. */
static void log_watch(void) {
    static char        seen_log[sizeof(st.log)];
    static struct stat seen;
    struct stat        sb;

    if (!st.log[0])
        return;
    if (stat(st.log, &sb) != 0)
        memset(&sb, 0, sizeof(sb));   /* deleted: no QSOs */
    if (!strcmp(seen_log, st.log) && sb.st_size == seen.st_size && sb.st_mtim.tv_sec == seen.st_mtim.tv_sec &&
        sb.st_mtim.tv_nsec == seen.st_mtim.tv_nsec)
        return;
    bool first = strcmp(seen_log, st.log) != 0;
    snprintf(seen_log, sizeof(seen_log), "%s", st.log);
    seen = sb;
    if (first)
        return;   /* read where the log is opened or written */
    log_load();
    if (dialog.run && (st.screen == SCR_LOG || act_freq))
        refresh();
}

static void timer_cb(lv_timer_t *t) {
    static int  ticks;
    double      lat, lon;
    const char *src;
    int         age;
    time_t      now = time(NULL);
    struct tm   tm;

    localtime_r(&now, &tm);
    position(&lat, &lon, &src, &age);
    lv_label_set_text(head_right, src[0] ? src : "NO POS");
    lv_label_set_text_fmt(head_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    /* Green: a real position (GPS, phone); yellow: the locator square */
    lv_obj_set_style_bg_color(head_dot, !src[0] ? TC(err) : !strcmp(src, "QTH") ? TC(warn) : TC(ok), 0);
    if (end_armed && now - end_armed_at > 4) {
        end_armed = false;
        buttons_refresh(&b_act);
    }
    if (spot_armed && now - spot_armed_at > 4) {
        spot_armed = false;
        buttons_refresh(&b_spot);
    }
    msg_voice_poll(false);
    log_watch();
    chips_update();   /* the app or the radio's own keys may change them too */
    buttons_refresh(&b_reccq);   /* a recording stops by itself at 60 s */
    ticks++;
    if (act_freq) {
        act_update();
    } else if (st.screen == SCR_PROG) {
        prog_info_update();
        if (ticks % 10 == 0)
            near_lookup(false);
    } else if (st.screen == SCR_BANDS && ticks % 5 == 0 && list) {
        int keep = sel;
        bands_fill();
        list_select(keep);
    }
}

static void construct_cb(lv_obj_t *parent) {
    lv_coord_t h = 480 - BUTTONS_H;

    if (!st_loaded) {
        state_load();
        st_loaded = true;
        if (st.active && st.log[0])
            log_load();
    }
    dialog.obj = dialog_init(parent);
    /* The whole screen above the buttons */
    lv_obj_set_pos(dialog.obj, 0, 0);
    lv_obj_set_size(dialog.obj, W, h);
    lv_obj_set_style_radius(dialog.obj, 0, 0);
    lv_obj_set_style_border_width(dialog.obj, 0, 0);
    lv_obj_set_style_bg_color(dialog.obj, TC(bg), 0);
    lv_obj_set_style_bg_opa(dialog.obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(dialog.obj, 0, 0);
    lv_obj_move_foreground(dialog.obj);

    lv_obj_t *head = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, W, HEAD_H);
    lv_obj_set_style_bg_color(head, TC(bar), 0);
    lv_obj_set_style_bg_grad_color(head, TC(bar_grad), 0);
    lv_obj_set_style_bg_grad_dir(head, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, TC(panel_border), 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *x = label(head, VF(26), TC(text), 14, 9, 0);
    lv_label_set_text(x, "X6100");
    head_icon  = NULL;
    title      = label(head, VF(24), TC(text), 104, 10, 440);
    head_dot   = dot(head, 556, 17, 12, TC(err));
    head_right = label(head, VF(22), TC(text), 572, 11, 100);
    head_clock = label(head, VF(38), TC(text), 670, 4, 118);   /* big: read at a glance outdoors */
    lv_obj_set_style_text_align(head_clock, LV_TEXT_ALIGN_RIGHT, 0);

    body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, HEAD_H);
    body_h = h - HEAD_H;
    lv_obj_set_size(body, W, body_h);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    status = label(dialog.obj, VF(20), TC(text_dim), 14, h - STATUS_H + 4, W - 28);
    lv_label_set_long_mode(status, LV_LABEL_LONG_SCROLL_CIRCULAR);   /* one line: a long hint scrolls, never under the buttons */
    chip_create(&chips[0], "VOL", h - STATUS_H + 1);
    chip_create(&chips[1], "MFK", h - STATUS_H + 1);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    if (my_call()[0])
        field_rbn_start(my_call());
    kbd_open = false;
    show(st.screen);
    timer_cb(NULL);
    timer = lv_timer_create(timer_cb, 1000, NULL);
}

static void destruct_cb(void) {
    cq_level_adj = false;
    msg_voice_poll(true);   /* never leaves a recording open or the radio keyed */
    if (kbd_open) {
        textarea_window_close();
        kbd_open = false;
    }
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    /* Not on the phone's data while out of Field Ops; the activity of the last minutes stays */
    field_rbn_stop();
    state_save();
    list      = NULL;
    info_ref  = NULL;
    act_freq  = NULL;
    head_icon = NULL;
}
