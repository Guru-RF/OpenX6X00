/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Flights: the aircraft within a range of the radio, as ADS-B receivers of volunteers hear them (the
 *  x6100-flights helper asks airplanes.live, else adsb.lol). A plot with north up and the radio in the middle, and the
 *  list, the nearest first, with distance and bearing; the MFK chooses one, the lines under the list say more of it.
 *  Asked when the app opens and then as the helper says (20 s, 60 s from adsb.lol), only while the app is open and
 *  the screen lit. The position: the radio's GPS, the phone's, else the QTH locator (station_position), sent rounded
 *  to 0.1 degree by the helper.
 *
 *  From condoruy's CX8ANM edition of OpenX6X00 (its Flights app, Vuelos).
 */
#include "dialog_flights.h"

#include "buttons.h"
#include "cJSON.h"
#include "events.h"
#include "keyboard.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "station.h"
#include "styles.h"
#include "theme.h"
#include "app_frame.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

#define HELPER    X6100_SBIN "/x6100-flights"
#define DATA_FILE "/tmp/x6100-flights.json"
#define RETRY_S   60        /* after an error, as the helper's own */
#define ROWS      6
#define ROW_H     22
#define PLOT      (APP_FRAME_BODY_H - 2)
#define LIST_X    (PLOT + 12)
#define LIST_W    (APP_FRAME_W - LIST_X - 8)
#define LABELS_N  15        /* more aircraft than that: only the chosen one named on the plot */

static const int ranges_km[] = {25, 50, 100, 200, 400};
#define RANGES_N (int)(sizeof(ranges_km) / sizeof(ranges_km[0]))

/* Kept while the radio is on */
static int range_i = 2;

static app_frame_t frame;
static lv_obj_t   *plot, *rows[ROWS], *cells[ROWS][5], *detail;
static lv_timer_t *timer;
static cJSON      *data;
static bool        updating;
static int         gen;             /* an answer for an earlier opening or range is dropped */
static time_t      next_at;
static char        error[120];
static char        sel_hex[12];     /* the chosen aircraft, kept across updates */
static int         sel, top;
static const char *pos_src = "";

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void range_cb(button_data_t *b);
static void update_cb(button_data_t *b);
static const char *range_label(void);

static button_data_t btn_range  = {.type = BTN_TEXT_FN, .label_fn = range_label, .press = range_cb};
static button_data_t btn_update = {.type = BTN_TEXT, .label = "Update", .press = update_cb};

static buttons_page_t page = {{&btn_range, &btn_update}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
};

dialog_t *dialog_flights = &dialog;

/* ---- the answer ---- */

static cJSON *item(cJSON *o, const char *k) {
    return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
}

static bool num(cJSON *o, const char *k, double *v) {
    cJSON *j = item(o, k);

    if (!cJSON_IsNumber(j)) return false;
    *v = j->valuedouble;
    return true;
}

static const char *str(cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(item(o, k));

    return s ? s : "";
}

static cJSON *aircraft(void) {
    cJSON *ac = item(data, "ac");

    return cJSON_IsArray(ac) ? ac : NULL;
}

static int aircraft_n(void) {
    return cJSON_GetArraySize(aircraft());
}

/* The flight number, else the registration, else the transponder's address */
static const char *ident(cJSON *a) {
    const char *s = str(a, "call");

    if (!s[0]) s = str(a, "reg");
    if (!s[0]) s = str(a, "hex");
    return s;
}

/* The fonts have no degree sign: "SW 225" */
static const char *compass(double b) {
    static const char *n[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

    return n[(int)((b + 22.5) / 45) % 8];
}

static void fmt_alt(char *b, size_t n, cJSON *a) {
    double m;

    if (cJSON_IsTrue(item(a, "ground")))
        snprintf(b, n, "%s", _("ground"));
    else if (num(a, "alt_m", &m))
        snprintf(b, n, "%d m", (int)m);
    else
        snprintf(b, n, "-");
}

/* ---- the plot: north up, the radio in the middle, rings at a third, two thirds and the range ---- */

static void plot_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t      *ctx = lv_event_get_draw_ctx(e);
    lv_obj_t           *o   = lv_event_get_target(e);
    lv_coord_t          w   = lv_obj_get_width(o);
    lv_point_t          c   = {o->coords.x1 + w / 2, o->coords.y1 + w / 2};
    lv_coord_t          r   = w / 2 - 16;
    int                 km  = ranges_km[range_i];
    lv_draw_arc_dsc_t   arc;
    lv_draw_line_dsc_t  line;
    lv_draw_label_dsc_t lbl;
    lv_draw_rect_dsc_t  dot;
    lv_area_t           a;
    char                txt[16];

    lv_draw_arc_dsc_init(&arc);
    arc.color = TC(text_faint);
    arc.width = 1;
    lv_draw_label_dsc_init(&lbl);
    lbl.font  = &sony_14;
    lbl.color = TC(text_dim);
    for (int k = 1; k <= 3; k++) {
        lv_draw_arc(ctx, &arc, &c, r * k / 3, 0, 360);
        snprintf(txt, sizeof(txt), "%d", km * k / 3);
        a = (lv_area_t){c.x + 3, c.y - r * k / 3 + 1, c.x + 43, c.y - r * k / 3 + 17};
        lv_draw_label(ctx, &lbl, &a, txt, NULL);
    }
    lv_draw_line_dsc_init(&line);
    line.color = TC(text_faint);
    line.width = 1;
    lv_point_t p1 = {c.x - r, c.y}, p2 = {c.x + r, c.y};
    lv_draw_line(ctx, &line, &p1, &p2);
    p1 = (lv_point_t){c.x, c.y - r};
    p2 = (lv_point_t){c.x, c.y + r};
    lv_draw_line(ctx, &line, &p1, &p2);
    lbl.color = TC(accent);
    lbl.font  = &sony_16;
    a = (lv_area_t){c.x - 5, c.y - r - 16, c.x + 9, c.y - r};
    lv_draw_label(ctx, &lbl, &a, "N", NULL);

    /* The radio */
    lv_draw_rect_dsc_init(&dot);
    dot.radius   = LV_RADIUS_CIRCLE;
    dot.bg_color = TC(accent);
    a = (lv_area_t){c.x - 2, c.y - 2, c.x + 2, c.y + 2};
    lv_draw_rect(ctx, &dot, &a);

    /* Each aircraft a dot, a stroke the way it flies and its name while there are not too many; the chosen one larger
     * (1bit: all ink) */
    cJSON *ac;
    int    i = 0, n = aircraft_n();

    lbl.font   = &sony_14;
    line.width = 2;
    cJSON_ArrayForEach(ac, aircraft()) {
        double d, b, trk;
        bool   is_sel = i == sel, emerg = cJSON_IsTrue(item(ac, "emerg")), ground = cJSON_IsTrue(item(ac, "ground"));

        i++;
        if (!num(ac, "dist_km", &d) || !num(ac, "brg", &b) || d > km) continue;

        lv_coord_t x   = c.x + (lv_coord_t)lround(r * d / km * sin(b * M_PI / 180));
        lv_coord_t y   = c.y - (lv_coord_t)lround(r * d / km * cos(b * M_PI / 180));
        lv_color_t col = emerg ? TC(err) : is_sel ? TC(text_head) : ground ? TC(text_dim) : TC(ok);

        if (num(ac, "trk", &trk) && !ground) {
            line.color = col;
            p1 = (lv_point_t){x, y};
            p2 = (lv_point_t){x + (lv_coord_t)lround(12 * sin(trk * M_PI / 180)),
                              y - (lv_coord_t)lround(12 * cos(trk * M_PI / 180))};
            lv_draw_line(ctx, &line, &p1, &p2);
        }
        int s = is_sel ? 5 : 3;

        dot.bg_color = col;
        a = (lv_area_t){x - s, y - s, x + s, y + s};
        lv_draw_rect(ctx, &dot, &a);
        if (is_sel || n <= LABELS_N) {
            lbl.color = col;
            a = (lv_area_t){x + 6, y - 8, x + 96, y + 8};
            lv_draw_label(ctx, &lbl, &a, ident(ac), NULL);
        }
    }
}

/* ---- the list, the chosen one, the header and the status line ---- */

static void show_rows(void) {
    int n = aircraft_n();

    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    if (top > n - ROWS) top = LV_MAX(n - ROWS, 0);

    for (int r = 0; r < ROWS; r++) {
        int    i = top + r;
        cJSON *a = i < n ? cJSON_GetArrayItem(aircraft(), i) : NULL;
        bool   on = a && i == sel;

        lv_obj_set_style_bg_opa(rows[r], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        for (int k = 0; k < 5; k++) {
            lv_obj_set_style_text_color(cells[r][k], on ? TC(sel_text) : cJSON_IsTrue(item(a, "emerg")) ? TC(err) : TC(text), 0);
        }
        if (!a) {
            for (int k = 0; k < 5; k++) lv_label_set_text(cells[r][k], "");
            continue;
        }
        char   alt[24], sp[16], dist[32];
        double kmh, d = 0, b = 0;

        fmt_alt(alt, sizeof(alt), a);
        if (num(a, "kmh", &kmh))
            snprintf(sp, sizeof(sp), "%d km/h", (int)kmh);
        else
            snprintf(sp, sizeof(sp), "-");
        num(a, "dist_km", &d);
        num(a, "brg", &b);
        snprintf(dist, sizeof(dist), d < 10 ? "%.1f km  %s %d" : "%.0f km  %s %d", d, compass(b), (int)b);
        lv_label_set_text(cells[r][0], ident(a));
        lv_label_set_text(cells[r][1], str(a, "type"));
        lv_label_set_text(cells[r][2], alt);
        lv_label_set_text(cells[r][3], sp);
        lv_label_set_text(cells[r][4], dist);
    }
}

/* fmt added at the end of buf, cut at its size */
static void add(char *buf, size_t size, const char *fmt, ...) {
    size_t  l = strlen(buf);
    va_list ap;

    if (l + 1 >= size) return;
    va_start(ap, fmt);
    vsnprintf(buf + l, size - l, fmt, ap);
    va_end(ap);
}

static void show_detail(void) {
    cJSON *a = cJSON_GetArrayItem(aircraft(), sel);
    char   l2[200];
    double v;

    if (!a) {
        lv_label_set_text(detail, data ? _("No aircraft in range just now: only those a volunteer's receiver nearby "
                                           "picks up are there")
                                       : "");
        return;
    }
    /* The second line: altitude, speed, track, climb or descent, squawk; 3 spaces between */
    fmt_alt(l2, sizeof(l2), a);
    if (num(a, "kmh", &v)) add(l2, sizeof(l2), "   %d km/h", (int)v);
    if (num(a, "trk", &v)) {
        add(l2, sizeof(l2), "   ");
        add(l2, sizeof(l2), _("track %s %d"), compass(v), (int)v);
    }
    if (num(a, "vs", &v) && fabs(v) >= 60) add(l2, sizeof(l2), "   %+d m/min", (int)v);
    if (str(a, "squawk")[0]) {
        add(l2, sizeof(l2), "   ");
        add(l2, sizeof(l2), _("squawk %s"), str(a, "squawk"));
    }
    if (cJSON_IsTrue(item(a, "emerg"))) add(l2, sizeof(l2), "   #%06x %s#", theme_hex(TC_err), _("EMERGENCY"));
    lv_label_set_text_fmt(detail, "#%06x %s#   %s   %s   %s\n%s", theme_hex(TC_text_head), ident(a), str(a, "reg"),
                          str(a, "type"), str(a, "desc"), l2);
}

static void show_status(void) {
    char head[96], right[64];

    if (!frame.status) return;
    /* Where the plot is centred: GPS, Phone, or the locator */
    if (!strcmp(pos_src, "Locator"))
        snprintf(head, sizeof(head), "%s   #%06x %s %s#", _("FLIGHTS"), theme_hex(TC_text_dim), _("Locator"), params.qth.x);
    else if (pos_src[0])
        snprintf(head, sizeof(head), "%s   #%06x %s#", _("FLIGHTS"), theme_hex(TC_text_dim), _(pos_src));
    else
        snprintf(head, sizeof(head), "%s", _("FLIGHTS"));
    lv_label_set_text(frame.head_left, head);
    if (data)
        snprintf(right, sizeof(right), _("%d aircraft   %d km"), aircraft_n(), ranges_km[range_i]);
    else
        snprintf(right, sizeof(right), "%d km", ranges_km[range_i]);
    lv_label_set_text(frame.head_right, right);

    /* The data's source is always named, as its users are asked to */
    const char *credit = data ? str(data, "credit") : "";

    if (!credit[0]) credit = "airplanes.live, adsb.lol (ODbL)";
    if (!pos_src[0])
        lv_label_set_text_fmt(frame.status, "#%06x %s#", theme_hex(TC_warn),
                              _("No position: set the QTH locator in Settings > General, or plug in a GPS"));
    else if (updating)
        lv_label_set_text_fmt(frame.status, "%s   %s %s", _("Asking for the aircraft..."), _("Data:"), credit);
    else if (error[0])
        lv_label_set_text_fmt(frame.status, "#%06x %s#   %s %s", theme_hex(TC_warn), error, _("Data:"), credit);
    else if (data)
        lv_label_set_text_fmt(frame.status, "%s %s   %s %s", _("Data:"), credit, _("updated"), str(data, "time"));
    else
        lv_label_set_text_fmt(frame.status, "%s %s", _("Data:"), credit);
}

static void show(void) {
    if (!plot) return;
    show_rows();
    show_detail();
    show_status();
    lv_obj_invalidate(plot);
}

/* The chosen aircraft by its address, where it is in the new list */
static void keep_sel(void) {
    int    i = 0;
    cJSON *a;

    cJSON_ArrayForEach(a, aircraft()) {
        if (sel_hex[0] && !strcmp(str(a, "hex"), sel_hex)) {
            sel = i;
            return;
        }
        i++;
    }
    sel = 0;
}

static void choose(int i) {
    int n = aircraft_n();

    if (n == 0) return;
    sel = LV_CLAMP(0, i, n - 1);
    snprintf(sel_hex, sizeof(sel_hex), "%s", str(cJSON_GetArrayItem(aircraft(), sel), "hex"));
    show();
}

/* ---- asking: the helper in a thread of its own, its answer back on the LVGL thread ---- */

static void load_file(void) {
    FILE *f = fopen(DATA_FILE, "r");

    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *text = len > 0 && len < 1024 * 1024 ? malloc(len + 1) : NULL;
    if (text) text[fread(text, 1, len, f)] = 0;
    fclose(f);
    if (!text) return;

    cJSON *j = cJSON_Parse(text);
    free(text);
    if (!j) return;
    cJSON_Delete(data);
    data = j;
}

typedef struct {
    int  gen;
    int  next_s;
    char err[120];
} result_t;

typedef struct {
    int    gen, km;
    double lat, lon;
} request_t;

static void update_done(void *arg) {
    result_t *r = arg;

    if (r->gen != gen || !dialog.run) return;
    updating = false;
    snprintf(error, sizeof(error), "%s", r->err);
    next_at = time(NULL) + (r->next_s > 0 ? r->next_s : RETRY_S);
    if (!r->err[0]) {
        load_file();
        keep_sel();
    }
    show();
}

static void *update_thread(void *arg) {
    request_t in = *(request_t *)arg;
    result_t  r  = {.gen = in.gen};
    char      cmd[256], line[200] = "", l[200];

    free(arg);
    snprintf(cmd, sizeof(cmd), HELPER " update %.5f %.5f %d " DATA_FILE " 2>/dev/null", in.lat, in.lon, in.km);
    FILE *p = popen(cmd, "r");

    if (p) {
        while (fgets(l, sizeof(l), p)) snprintf(line, sizeof(line), "%s", l);
        pclose(p);
    }
    line[strcspn(line, "\n")] = 0;
    if (!strncmp(line, "ok ", 3)) {
        int n;

        if (sscanf(line + 3, "%d %d", &n, &r.next_s) != 2) r.next_s = RETRY_S;
    } else if (!strncmp(line, "error ", 6)) {
        snprintf(r.err, sizeof(r.err), "%s", line + 6);
    } else {
        snprintf(r.err, sizeof(r.err), "%s", _("No answer from the helper"));
    }
    scheduler_put(update_done, &r, sizeof(r));
    return NULL;
}

static void start_update(void) {
    pthread_t  t;
    request_t *q;
    double     lat, lon;

    if (updating) return;
    if (!station_position(&lat, &lon, &pos_src)) {
        next_at = time(NULL) + RETRY_S;
        show_status();
        return;
    }
    if (!(q = malloc(sizeof(*q)))) return;
    *q = (request_t){.gen = gen, .km = ranges_km[range_i], .lat = lat, .lon = lon};
    updating = true;
    error[0] = 0;
    show_status();
    if (pthread_create(&t, NULL, update_thread, q) == 0) {
        pthread_detach(t);
    } else {
        free(q);
        updating = false;
        next_at  = time(NULL) + RETRY_S;
    }
}

/* Each second: the next request when it is due, not while the screen is dark */
static void timer_cb(lv_timer_t *t) {
    (void)t;
    if (!updating && time(NULL) >= next_at && backlight_seen()) start_update();
}

/* ---- buttons and keys ---- */

static const char *range_label(void) {
    static char s[24];

    snprintf(s, sizeof(s), "Range:\n%d km", ranges_km[range_i]);
    return s;
}

static void range_cb(button_data_t *b) {
    (void)b;
    range_i = (range_i + 1) % RANGES_N;
    buttons_refresh(&btn_range);
    gen++;                  /* an answer on its way is for the range before */
    updating = false;
    cJSON_Delete(data);
    data = NULL;
    sel = top = 0;
    show();
    start_update();
}

static void update_cb(button_data_t *b) {
    (void)b;
    start_update();
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            choose(sel - 1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            choose(sel + 1);
            break;
        case LV_KEY_ENTER:
            start_update();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

static lv_obj_t *cell(lv_obj_t *parent, const lv_font_t *font, lv_coord_t x, lv_coord_t w, bool right) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w);
    if (right) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(l, x, 1);
    lv_label_set_text(l, "");
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    /* x and width of the columns: flight, type, altitude, speed, distance and bearing (the plot's size, and so where
     * the list starts, follows the buttons' height) */
    const lv_coord_t        col_x[5] = {6, 116, 166, 266, 376}, col_w[5] = {108, 48, 92, 102, LIST_W - 382};
    static const char *const heads[5] = {"Flight", "Type", "Altitude", "Speed", "Distance"};

    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);
    gen++;

    lv_obj_t *body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    plot = lv_obj_create(body);
    lv_obj_remove_style_all(plot);
    lv_obj_set_pos(plot, 1, 1);
    lv_obj_set_size(plot, PLOT, PLOT);
    lv_obj_clear_flag(plot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(plot, plot_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    lv_obj_t *head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_pos(head, LIST_X, 2);
    lv_obj_set_size(head, LIST_W, ROW_H);
    for (int k = 0; k < 5; k++) {
        lv_obj_t *l = cell(head, &sony_16, col_x[k], col_w[k], k >= 2);

        lv_obj_set_style_text_color(l, TC(text_dim), 0);
        lv_label_set_text(l, heads[k]);
    }
    for (int r = 0; r < ROWS; r++) {
        rows[r] = lv_obj_create(body);
        lv_obj_remove_style_all(rows[r]);
        lv_obj_set_pos(rows[r], LIST_X, 2 + ROW_H * (r + 1));
        lv_obj_set_size(rows[r], LIST_W, ROW_H);
        lv_obj_set_style_radius(rows[r], 4, 0);
        lv_obj_set_style_bg_color(rows[r], TC(sel_bg), 0);
        lv_obj_clear_flag(rows[r], LV_OBJ_FLAG_SCROLLABLE);
        for (int k = 0; k < 5; k++) cells[r][k] = cell(rows[r], &sony_18, col_x[k], col_w[k], k >= 2);
    }
    detail = lv_label_create(body);
    lv_obj_set_style_text_font(detail, &sony_18, 0);
    lv_obj_set_style_text_color(detail, TC(text), 0);
    lv_label_set_recolor(detail, true);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail, LIST_W - 6);
    lv_obj_set_pos(detail, LIST_X + 6, 6 + ROW_H * (ROWS + 1));

    buttons_load_page(&page);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    sel = top = 0;
    sel_hex[0] = 0;
    error[0]   = 0;
    updating   = false;
    pos_src    = "";
    next_at    = 0;
    show();
    start_update();
    timer = lv_timer_create(timer_cb, 1000, NULL);
}

static void destruct_cb(void) {
    gen++;                  /* an answer still on its way is dropped */
    updating = false;
    if (timer) lv_timer_del(timer);
    timer = NULL;
    plot = detail = NULL;
    frame.status = NULL;
    cJSON_Delete(data);
    data = NULL;
    lv_group_remove_obj(dialog.obj);
}
