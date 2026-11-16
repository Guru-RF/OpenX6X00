/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Weather: the weather where the radio is, five days of forecast and the warnings in force there, in the
 *  place's own time. The x6100-meteo helper asks Open-Meteo for the weather, OpenStreetMap's Nominatim for the place's
 *  name and the country's weather service for its warnings (the US NWS, MeteoAlarm, Argentina's SMN, Brazil's INMET),
 *  else works them out from the forecast; it sends the position rounded to 0.1 degree, each service as its terms ask.
 *  Asked when the app opens, with Update, and every 15 minutes while it is open and the screen lit. The position:
 *  meteo_position (the GPS, the phone's, the QTH locator, else the country set in Settings > General). Each source is
 *  named in the status line, as its terms ask.
 *
 *  From condoruy's CX8ANM edition of OpenX6X00 (its Meteo app).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dialog_meteo.h"

#include "app_frame.h"
#include "buttons.h"
#include "cJSON.h"
#include "cfg/cfg_api.h"
#include "countries.h"
#include "events.h"
#include "i18n.h"
#include "keyboard.h"
#include "meteo.h"
#include "meteo_alerts.h"
#include "msg_box.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "styles.h"
#include "theme.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REFRESH_S   (15 * 60)
#define RETRY_S     120         /* after an error */
#define STRIP_H     30
#define TILE_W      168
#define DEG         "°"

typedef enum { VIEW_NOW, VIEW_DAYS, VIEW_WARN, VIEW_N } view_t;

/* Kept while the radio is on */
static view_t view = VIEW_NOW;

static app_frame_t frame;
static lv_obj_t   *body;
static lv_timer_t *timer;
static cJSON      *data;
static bool        updating;
static int         gen;             /* an answer for an earlier opening is dropped */
static time_t      next_at;
static char        error[120];
static const char *pos_src = "";

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void now_cb(button_data_t *b);
static void days_cb(button_data_t *b);
static void warn_cb(button_data_t *b);
static void update_cb(button_data_t *b);
static void notices_cb(button_data_t *b);
static const char *notices_label(void);

static button_data_t btn_now    = {.type = BTN_TEXT, .label = "Now", .press = now_cb};
static button_data_t btn_days   = {.type = BTN_TEXT, .label = "5 days", .press = days_cb};
static button_data_t btn_warn   = {.type = BTN_TEXT, .label = "Warnings", .press = warn_cb};
static button_data_t btn_update = {.type = BTN_TEXT, .label = "Update", .press = update_cb};
static button_data_t btn_notices = {.type = BTN_TEXT_FN, .label_fn = notices_label, .press = notices_cb};

static buttons_page_t page = {{&btn_now, &btn_days, &btn_warn, &btn_update, &btn_notices}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
};

dialog_t *dialog_meteo = &dialog;

/* The WMO weather codes Open-Meteo gives, in the GUI's language (the helper writes them in English) */
static const struct {
    int         code;
    const char *text;
} wmo[] = {
    {0, N_("Clear sky")},        {1, N_("Mainly clear")},       {2, N_("Partly cloudy")},
    {3, N_("Overcast")},         {45, N_("Fog")},               {48, N_("Rime fog")},
    {51, N_("Light drizzle")},   {53, N_("Drizzle")},           {55, N_("Dense drizzle")},
    {56, N_("Freezing drizzle")}, {57, N_("Dense freezing drizzle")}, {61, N_("Light rain")},
    {63, N_("Rain")},            {65, N_("Heavy rain")},        {66, N_("Freezing rain")},
    {67, N_("Heavy freezing rain")}, {71, N_("Light snow")},    {73, N_("Snow")},
    {75, N_("Heavy snow")},      {77, N_("Snow grains")},       {80, N_("Light showers")},
    {81, N_("Showers")},         {82, N_("Violent showers")},   {85, N_("Snow showers")},
    {86, N_("Heavy snow showers")}, {95, N_("Thunderstorm")},   {96, N_("Thunderstorm with hail")},
    {99, N_("Thunderstorm with heavy hail")},
};

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

static cJSON *day(int i) {
    return cJSON_GetArrayItem(item(data, "days"), i);
}

/* The number of o's k with dec decimals (0 or 1) and its unit, "--" without one; never "-0" */
static const char *fmt_num(char *b, size_t n, cJSON *o, const char *k, int dec, const char *unit) {
    double v, p = dec ? 10 : 1;

    if (!num(o, k, &v)) return "--";
    v = round(v * p) / p;
    if (v == 0) v = 0;
    snprintf(b, n, "%.*f%s", dec, v, unit);
    return b;
}

static const char *sky(cJSON *o) {
    double c;

    if (num(o, "code", &c))
        for (size_t i = 0; i < sizeof(wmo) / sizeof(wmo[0]); i++)
            if (wmo[i].code == (int)c) return _(wmo[i].text);
    return str(o, "text");
}

static const char *compass(double b) {
    static const char *const n[] = {N_("N"), N_("NE"), N_("E"), N_("SE"), N_("S"), N_("SW"), N_("W"), N_("NW")};

    return _(n[(int)((fmod(b, 360) + 360 + 22.5) / 45) % 8]);
}

/* The UV index in the WHO's words */
static const char *uv_word(double uv) {
    return uv < 3 ? _("low") : uv < 6 ? _("moderate") : uv < 8 ? _("high") : uv < 11 ? _("very high") : _("extreme");
}

static const char *tz(void) {
    const char *a = str(data, "tz_abbr");

    return a[0] ? a : str(data, "tz");
}

/* The place's own warning of the highest level (the helper keeps the service's order), NULL none; mine_n: how many
 * are the place's own */
static cJSON *worst_mine(int *mine_n) {
    cJSON *a, *w = NULL;
    double lv, wl = -1;

    *mine_n = 0;
    cJSON_ArrayForEach(a, item(data, "alerts")) {
        if (!cJSON_IsTrue(item(a, "mine"))) continue;
        (*mine_n)++;
        if (num(a, "level", &lv) && lv > wl) {
            wl = lv;
            w  = a;
        }
    }
    return w;
}

/* The service the warnings came from, as the status says it: "MeteoAlarm", "forecast" (none for the country, or it
 * did not answer: "MeteoAlarm+forecast") */
static const char *alerts_src(bool *service, bool *failed) {
    const char *s = str(data, "alerts_src");

    *failed  = strstr(s, "+forecast") != NULL;
    *service = s[0] && strcmp(s, "forecast") != 0;
    return s;
}

/* ---- drawing ---- */

/* fmt added at the end of buf, cut at its size */
static void add(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void add(char *buf, size_t size, const char *fmt, ...) {
    size_t  l = strlen(buf);
    va_list ap;

    if (l + 1 >= size) return;
    va_start(ap, fmt);
    vsnprintf(buf + l, size - l, fmt, ap);
    va_end(ap);
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y,
                      lv_coord_t w) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_recolor(l, true);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void tile(lv_coord_t x, lv_coord_t y, const char *name, const char *value) {
    lv_label_set_text(text(body, &sony_16, TC(text_dim), x, y, TILE_W), name);
    lv_label_set_text(text(body, &sony_22, TC(text_value), x, y + 18, TILE_W), value);
}

/* The line under the weather now: the place's own warning of the highest level on its colour, else what the
 * service said */
static void strip(lv_coord_t y) {
    lv_obj_t *s = lv_obj_create(body);
    int       mine_n;
    cJSON    *w = worst_mine(&mine_n);
    char      t[200], when[64], title[128];
    bool      service, failed;
    const char *src = alerts_src(&service, &failed);

    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, 6, y);
    lv_obj_set_size(s, APP_FRAME_W - 14, STRIP_H);
    lv_obj_set_style_radius(s, 4, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = text(s, &sony_20, TC(text), 8, 4, APP_FRAME_W - 30);

    lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
    if (w) {
        double   lv = 0;
        unsigned c;

        num(w, "level", &lv);
        c = meteo_level_color((int)lv);
        if (c) {
            /* 1bit: ink with paper letters */
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s, theme_shape.mono ? TC(text) : lv_color_hex(c), 0);
            lv_obj_set_style_text_color(l, theme_shape.mono ? TC(panel) : lv_color_hex(lv >= 4 ? 0xffffff : 0x000000), 0);
            lv_label_set_recolor(l, false);
        } else {
            lv_obj_set_style_text_color(l, TC(warn), 0);
        }
        meteo_warning_title(title, sizeof(title), w);
        meteo_warning_when(when, sizeof(when), w, tz());
        if (mine_n > 1)
            snprintf(t, sizeof(t), _("%s   %s   (%d warnings: Warnings)"), title, when, mine_n);
        else
            snprintf(t, sizeof(t), "%s   %s", title, when);
        lv_label_set_text(l, t);
        return;
    }
    if (failed)
        lv_label_set_text_fmt(l, _("#%06x %.*s did not answer:# none worked out from the forecast either"),
                          theme_hex(TC_warn), (int)(strstr(src, "+") - src), src);
    else if (service)
        lv_label_set_text_fmt(l, _("#%06x No warnings in force here# (%s)"), theme_hex(TC_ok), src);
    else
        lv_label_set_text_fmt(l, _("#%06x None worked out from the forecast# (no official warnings asked for this "
                                   "country)"),
                              theme_hex(TC_ok));
}

static void view_now(void) {
    cJSON     *c = item(data, "now"), *d0 = day(0), *sun = item(data, "sun");
    char       a[48], b[48], s[120];
    double     v, dir;
    lv_coord_t x0 = 250, x1 = x0 + TILE_W + 4, x2 = x1 + TILE_W + 4;

    /* Left: the temperature, the sky, how it feels, today's low and high, the sun */
    lv_label_set_text(text(body, &sony_44, TC(text_value), 12, 0, 232), fmt_num(a, sizeof(a), c, "temp", 1, " " DEG "C"));
    lv_label_set_text(text(body, &sony_22, TC(accent), 12, 54, 232), sky(c));
    snprintf(s, sizeof(s), _("Feels like %s"), fmt_num(a, sizeof(a), c, "feels", 0, " " DEG "C"));
    lv_label_set_text(text(body, &sony_18, TC(text_soft), 12, 82, 232), s);
    snprintf(s, sizeof(s), _("Low %s, high %s"), fmt_num(a, sizeof(a), d0, "tmin", 0, DEG),
             fmt_num(b, sizeof(b), d0, "tmax", 0, " " DEG "C"));
    lv_label_set_text(text(body, &sony_18, TC(text_soft), 12, 104, 232), s);
    if (str(sun, "rise")[0]) {
        snprintf(s, sizeof(s), _("Sun %s to %s"), str(sun, "rise"), str(sun, "set"));
        lv_label_set_text(text(body, &sony_18, TC(text_dim), 12, 126, 232), s);
    }

    /* Right: three rows of three */
    if (num(c, "wind_kmh", &v) && num(c, "wind_dir", &dir))
        snprintf(s, sizeof(s), "%s %.0f km/h", compass(dir), round(v));
    else
        snprintf(s, sizeof(s), "%s", fmt_num(a, sizeof(a), c, "wind_kmh", 0, " km/h"));
    tile(x0, 0, _("Wind"), s);
    tile(x1, 0, _("Gusts"), fmt_num(a, sizeof(a), c, "gust_kmh", 0, " km/h"));
    tile(x2, 0, _("Humidity"), fmt_num(a, sizeof(a), c, "hum", 0, " %"));
    tile(x0, 44, _("Pressure"), fmt_num(a, sizeof(a), c, "press", 0, " hPa"));
    snprintf(s, sizeof(s), "%s, %s", fmt_num(a, sizeof(a), d0, "prob", 0, " %"), fmt_num(b, sizeof(b), d0, "rain_mm", 1, " mm"));
    tile(x1, 44, _("Rain today"), s);
    tile(x2, 44, _("Clouds"), fmt_num(a, sizeof(a), c, "cloud", 0, " %"));
    tile(x0, 88, _("Dew point"), fmt_num(a, sizeof(a), c, "dew", 1, " " DEG "C"));
    tile(x1, 88, _("Visibility"), fmt_num(a, sizeof(a), c, "vis_km", 0, " km"));
    if (num(c, "uv", &v))
        snprintf(s, sizeof(s), "%.0f, %s", round(v) == 0 ? 0.0 : round(v), uv_word(v));
    else
        snprintf(s, sizeof(s), "--");
    tile(x2, 88, _("UV index"), s);

    strip(lv_obj_get_height(body) - STRIP_H - 6);
}

static void view_days(void) {
    char       a[32], b[32], s[96];
    double     v, dir;
    lv_coord_t w = (APP_FRAME_W - 16) / 5;

    for (int i = 0; i < 5; i++) {
        cJSON     *d = day(i);
        lv_coord_t x = 8 + i * w;

        if (!d) break;
        /* The day, its high and low, the sky, the rain and the wind */
        snprintf(s, sizeof(s), "%s %s", i == 0 ? _("Today") : meteo_weekday(str(d, "date")), str(d, "date") + 8);
        lv_label_set_text(text(body, &sony_20, TC(text_head), x, 2, w - 6), s);
        snprintf(s, sizeof(s), "%s #%06x %s#", fmt_num(a, sizeof(a), d, "tmax", 0, DEG), theme_hex(TC_text_dim),
                 fmt_num(b, sizeof(b), d, "tmin", 0, DEG));
        lv_label_set_text(text(body, &sony_26, TC(text_value), x, 26, w - 6), s);

        lv_obj_t *l = text(body, &sony_16, TC(accent), x, 58, w - 6);

        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, sky(d));
        snprintf(s, sizeof(s), "%s  %s", fmt_num(a, sizeof(a), d, "prob", 0, " %"), fmt_num(b, sizeof(b), d, "rain_mm", 1, " mm"));
        lv_label_set_text(text(body, &sony_16, TC(info), x, 98, w - 6), s);
        if (num(d, "wind_max", &v) && num(d, "wind_dir", &dir))
            snprintf(s, sizeof(s), "%s %s %.0f", _("Wind"), compass(dir), round(v));
        else
            snprintf(s, sizeof(s), "%s %s", _("Wind"), fmt_num(a, sizeof(a), d, "wind_max", 0, ""));
        lv_label_set_text(text(body, &sony_16, TC(text_soft), x, 118, w - 6), s);
        snprintf(s, sizeof(s), "%s %s", _("Gusts"), fmt_num(a, sizeof(a), d, "gust_max", 0, " km/h"));
        lv_label_set_text(text(body, &sony_16, TC(text_soft), x, 138, w - 6), s);
        if (num(d, "uv", &v) && v >= 3) {
            snprintf(s, sizeof(s), "UV %.0f", round(v));
            lv_label_set_text(text(body, &sony_16, TC(text_dim), x, 158, w - 6), s);
        }
    }
    if (!day(0))
        lv_label_set_text(text(body, &sony_22, TC(text_dim), 12, 8, APP_FRAME_W - 30), _("No forecast in the answer"));
}

/* One warning: its level's colour down the left side, its words as the service gave them */
static lv_coord_t warning_box(cJSON *a, lv_coord_t y) {
    static char t[8192];
    char        title[160], when[64], sent[32];
    double      lv = 0;
    unsigned    c;
    lv_obj_t   *box = lv_obj_create(body), *l;

    num(a, "level", &lv);
    c = meteo_level_color((int)lv);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 6, y);
    lv_obj_set_size(box, APP_FRAME_W - 30, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(box, 8, 0);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, theme_shape.mono || !c ? TC(text_dim) : lv_color_hex(c), 0);
    lv_obj_set_style_pad_left(box, 14, 0);
    lv_obj_set_style_pad_ver(box, 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    meteo_warning_title(title, sizeof(title), a);
    meteo_warning_when(when, sizeof(when), a, tz());
    meteo_time(sent, sizeof(sent), str(a, "sent"));
    snprintf(t, sizeof(t), "#%06x %s#  %s\n%s", theme_hex(TC_text_head), title,
             cJSON_IsTrue(item(a, "mine")) ? _("(here)") : _("(nearby)"), when);
    if (sent[0]) add(t, sizeof(t), "   %s %s", _("issued"), sent);
    if (str(a, "areas")[0]) add(t, sizeof(t), "\n#%06x %s#", theme_hex(TC_text_dim), str(a, "areas"));
    if (str(a, "desc")[0]) add(t, sizeof(t), "\n%s", str(a, "desc"));
    if (str(a, "instr")[0]) add(t, sizeof(t), "\n%s", str(a, "instr"));
    l = lv_label_create(box);
    lv_obj_set_style_text_font(l, &sony_18, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, APP_FRAME_W - 30 - 22);
    lv_label_set_text(l, t);
    lv_obj_update_layout(box);
    return y + lv_obj_get_height(box) + 10;
}

static void view_warn(void) {
    bool        service, failed;
    const char *src = alerts_src(&service, &failed);
    int         n = cJSON_GetArraySize(item(data, "alerts")), mine_n;
    lv_coord_t  y = 4;
    cJSON      *a;
    lv_obj_t   *head = text(body, &sony_18, TC(text_soft), 8, y, 0);
    char        t[400];

    worst_mine(&mine_n);
    lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(head, APP_FRAME_W - 30);
    if (failed)
        snprintf(t, sizeof(t), _("#%06x %.*s did not answer:# the warnings below are worked out from the forecast, "
                                 "not official"),
                 theme_hex(TC_warn), (int)(strstr(src, "+") - src), src);
    else if (service)
        snprintf(t, sizeof(t), _("%s: %d for this place, %d nearby; their texts as the service gives them"), src,
                 mine_n, n - mine_n);
    else
        snprintf(t, sizeof(t), "%s", _("No official warnings are asked for this country: these are worked out from "
                                       "Open-Meteo's forecast, not official"));
    /* MeteoAlarm asks for this note with its warnings */
    if (!strcmp(src, "MeteoAlarm"))
        add(t, sizeof(t), "\n#%06x %s#", theme_hex(TC_text_dim),
            _("Times may lag meteoalarm.org: the latest warnings are there"));
    lv_label_set_text(head, t);
    lv_obj_update_layout(head);
    y += lv_obj_get_height(head) + 8;

    /* The place's own first */
    for (int pass = 0; pass < 2; pass++)
        cJSON_ArrayForEach(a, item(data, "alerts")) {
            if (cJSON_IsTrue(item(a, "mine")) == (pass == 0)) y = warning_box(a, y);
        }
    if (!n)
        lv_label_set_text_fmt(text(body, &sony_22, TC(ok), 8, y, APP_FRAME_W - 30), "%s",
                              service ? _("No warnings in force here") : _("None worked out from the forecast"));
}

static void show_status(void) {
    char        head[200], right[96];
    static char st[600];
    cJSON      *c;

    if (!frame.status) return;
    /* The place, and where its position came from */
    if (data && str(data, "place")[0])
        snprintf(head, sizeof(head), "%s   #%06x %s#", _("WEATHER"), theme_hex(TC_text_soft), str(data, "place"));
    else
        snprintf(head, sizeof(head), "%s", _("WEATHER"));
    lv_label_set_text(frame.head_left, head);
    if (!strcmp(pos_src, "Locator"))
        snprintf(right, sizeof(right), "%s %s", _("Locator"), params.qth.x);
    else if (!strcmp(pos_src, "Country"))
        snprintf(right, sizeof(right), "%s %s", _("Country"), country_name(country_now()));
    else
        snprintf(right, sizeof(right), "%s", pos_src[0] ? _(pos_src) : "");
    if (data && str(data, "updated")[0]) add(right, sizeof(right), "   %s %s", str(data, "updated"), tz());
    lv_label_set_text(frame.head_right, right);

    /* Every source of what is shown is named, as their terms ask */
    st[0] = 0;
    if (!pos_src[0] && data)    /* without an answer the body says it */
        snprintf(st, sizeof(st), "#%06x %s#", theme_hex(TC_warn),
                 _("No position: plug in a GPS, or set the QTH locator or the country in Settings > General"));
    else if (updating)
        snprintf(st, sizeof(st), "%s", _("Asking for the weather..."));
    else if (error[0])
        snprintf(st, sizeof(st), "#%06x %s#", theme_hex(TC_warn), error);
    cJSON_ArrayForEach(c, item(data, "credits")) {
        if (cJSON_IsString(c)) add(st, sizeof(st), "%s%s", st[0] ? "   " : "", c->valuestring);
    }
    cJSON_ArrayForEach(c, item(data, "errors")) {
        if (cJSON_IsString(c)) add(st, sizeof(st), "   #%06x %s#", theme_hex(TC_warn), c->valuestring);
    }
    if (param_i_get(cfg_meteo_alerts))
        add(st, sizeof(st), "   %s", _("Notices on: the warnings asked every 30 minutes"));
    lv_label_set_text(frame.status, st);
}

static void show(void) {
    if (!body) return;
    lv_obj_clean(body);
    lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    if (view == VIEW_WARN)
        lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    else
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    show_status();
    if (!data || !item(data, "now")) {
        lv_obj_t *l = text(body, &sony_22, TC(text_dim), 12, 8, APP_FRAME_W - 30);

        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, !pos_src[0] ? _("No position: plug in a GPS, or set the QTH locator or the country in "
                                             "Settings > General > STATION")
                             : updating  ? _("Asking for the weather...")
                             : error[0]  ? error
                                         : _("No weather yet: Update (it needs the internet)"));
        return;
    }
    switch (view) {
        case VIEW_NOW:  view_now(); break;
        case VIEW_DAYS: view_days(); break;
        default:        view_warn(); break;
    }
}

/* ---- asking: the helper in a thread of its own, its answer back on the LVGL thread ---- */

static cJSON *load_file(const char *path) {
    FILE *f = fopen(path, "r");

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *t = len > 0 && len < 1024 * 1024 ? malloc(len + 1) : NULL;
    if (t) t[fread(t, 1, len, f)] = 0;
    fclose(f);
    if (!t) return NULL;

    cJSON *j = cJSON_Parse(t);
    free(t);
    return j;
}

typedef struct {
    int  gen;
    char err[120];
} result_t;

typedef struct {
    int  gen;
    char cmd[256];
} request_t;

static void update_done(void *arg) {
    result_t *r = arg;

    if (r->gen != gen || !dialog.run) return;
    updating = false;
    snprintf(error, sizeof(error), "%s", r->err);
    next_at = time(NULL) + (r->err[0] ? RETRY_S : REFRESH_S);
    if (!r->err[0]) {
        cJSON *j = load_file(METEO_FILE);

        if (j) {
            cJSON_Delete(data);
            data = j;
            meteo_alerts_seen(data);    /* shown here: no notice for them later */
        }
    }
    show();
}

static void *update_thread(void *arg) {
    request_t *q = arg;
    result_t   r = {.gen = q->gen};
    char       line[200] = "", l[200];
    FILE      *p = popen(q->cmd, "r");

    if (p) {
        while (fgets(l, sizeof(l), p)) snprintf(line, sizeof(line), "%s", l);
        pclose(p);
    }
    free(q);
    line[strcspn(line, "\n")] = 0;
    if (!strncmp(line, "error ", 6))
        snprintf(r.err, sizeof(r.err), "%s", line + 6);
    else if (strcmp(line, "ok"))
        snprintf(r.err, sizeof(r.err), "%s", _("No answer from the helper"));
    scheduler_put(update_done, &r, sizeof(r));
    return NULL;
}

/* asked: a person asked (the app opened, Update): only then may the helper look up the place's name */
static void start_update(bool asked) {
    pthread_t  t;
    request_t *q;
    double     lat, lon;
    char       args[160];

    if (updating) return;
    if (!meteo_position(&lat, &lon, &pos_src)) {
        next_at = time(NULL) + RETRY_S;
        show();
        return;
    }
    if (!(q = malloc(sizeof(*q)))) return;
    meteo_args(args, sizeof(args), lat, lon, pos_src);
    q->gen = gen;
    snprintf(q->cmd, sizeof(q->cmd), METEO_HELPER " update %s%s 2>/dev/null", asked ? "--asked " : "", args);
    updating = true;
    error[0] = 0;
    if (data)
        show_status();      /* the last answer stays while the new one comes */
    else
        show();
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
    if (!updating && time(NULL) >= next_at && backlight_seen()) start_update(false);
}

/* ---- buttons and keys ---- */

static void set_view(view_t v) {
    view = v;
    show();
}

static void now_cb(button_data_t *b) {
    (void)b;
    set_view(VIEW_NOW);
}

static void days_cb(button_data_t *b) {
    (void)b;
    set_view(VIEW_DAYS);
}

static void warn_cb(button_data_t *b) {
    (void)b;
    set_view(VIEW_WARN);
}

static void update_cb(button_data_t *b) {
    (void)b;
    start_update(true);
}

static const char *notices_label(void) {
    return param_i_get(cfg_meteo_alerts) ? "Notices:\nOn" : "Notices:\nOff";
}

/* The warnings checked in the background, a new one as a notice on any screen (meteo_alerts.c) */
static void notices_cb(button_data_t *b) {
    bool on = !param_i_get(cfg_meteo_alerts);

    (void)b;
    param_i_set(cfg_meteo_alerts, on);
    buttons_refresh(&btn_notices);
    if (on)
        msg_schedule_text_ms(6000, "Weather warning notices on\nthe warnings asked every 30 minutes");
    else
        msg_schedule_text_fmt("Weather warning notices off");
    show_status();
}

/* The MFK: the warnings' text up and down, else the next or the one before of the three views */
static void step(int d) {
    if (view == VIEW_WARN && body && lv_obj_get_scroll_bottom(body) + lv_obj_get_scroll_top(body) > 0) {
        lv_obj_scroll_by_bounded(body, 0, -d * 60, LV_ANIM_OFF);
        return;
    }
    set_view((view + VIEW_N + d) % VIEW_N);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            step(-1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            step(1);
            break;
        case LV_KEY_ENTER:
            start_update(true);
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

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);
    gen++;

    body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    buttons_load_page(&page);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    error[0] = 0;
    updating = false;
    pos_src  = "";
    next_at  = 0;
    /* The last answer while the new one comes: the same place, at most a little old */
    cJSON_Delete(data);
    data = load_file(METEO_FILE);
    if (data && time(NULL) - (time_t)cJSON_GetNumberValue(item(data, "fetched")) > 3 * 3600) {
        cJSON_Delete(data);
        data = NULL;
    }
    if (data) meteo_alerts_seen(data);  /* shown here: no notice for them later */
    start_update(true);
    show();
    timer = lv_timer_create(timer_cb, 1000, NULL);
}

static void destruct_cb(void) {
    gen++;                  /* an answer still on its way is dropped */
    updating = false;
    if (timer) lv_timer_del(timer);
    timer = NULL;
    body  = NULL;
    frame.status = NULL;
    cJSON_Delete(data);
    data = NULL;
    lv_group_remove_obj(dialog.obj);
}
