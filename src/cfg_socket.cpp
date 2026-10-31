/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  The web configurator's local socket: one JSON object per line, one request per connection.
 *    {"cmd":"disp_get"}                               -> {"t":"disp", option: value..., "theme":name, "themes":[...]}
 *    {"cmd":"disp_set","text":name,"val":v}           -> {"t":"ok"} or {"t":"err","err":"..."}; "theme":name for the theme
 *    {"cmd":"eth_get"}                                -> {"t":"eth", ...}
 *    {"cmd":"eth_set","dhcp":0|1,"ip":..,"mask":..,"gw":..,"dns":..}
 *    {"cmd":"band_use","lo":hz,"hi":hz}               -> {"t":"band_use","use":0|1|2} (none, receive, transmit)
 *  The values are checked here and applied on the LVGL thread through the scheduler, as the
 *  Settings pages apply them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cfg_socket.h"

#include "cfg/settings_manager.h"
#include "cfg/settings_manager_api.h"
#include "wifi.h"

#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" {
    #include "band_plan.h"
    #include "cJSON.h"
    #include "params/params.h"
    #include "radio.h"
    #include "scheduler.h"
    #include "screen_light.h"
    #include "screen_main.h"
    #include "theme.h"
    #include "walkie.h"

    #include "lvgl/lvgl.h"
    #include <errno.h>
    #include <pthread.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/un.h>
    #include <unistd.h>
}

#define SOCK_PATH "/tmp/x6100_cfg.sock"
#define IN_SIZE   1024

/* Screen options of Settings > Display, by name: the ranges the page may set */
static const struct {
    const char         *name;
    Parameter<int32_t> *p;
    int32_t             min, max;
} disp_ints[] = {
    {"wf_mode", &cfg_sm.p_wf_mode, 0, 1},
    {"auto_level", &cfg_sm.p_auto_level_enabled, 0, 1},
    {"wf_speed", &cfg_sm.p_waterfall_speed, 1, 5},
    {"band_plan", &cfg_sm.p_band_plan, 0, 1},
    {"cluster_spots_wf", &cfg_sm.p_cluster_spots_wf, 0, 1},
    {"cluster_column", &cfg_sm.p_cluster_column, 0, 1},
    {"cluster_order", &cfg_sm.p_cluster_order, 0, 1},
    {"cluster_all_bands", &cfg_sm.p_cluster_all_bands, 0, 1},
    {"smeter_needle", &cfg_sm.p_smeter_needle, 0, 1},
    {"smeter_color", &cfg_sm.p_smeter_color, 0, 2},
    {"wf_palette", &cfg_sm.p_wf_palette, 0, 6},
    {"filter_color", &cfg_sm.p_filter_color, 0, 8},
    {"spectrum_line_color", &cfg_sm.p_spectrum_line_color, -1, 0xffffff},
    {"spectrum_fill_color", &cfg_sm.p_spectrum_fill_color, -1, 0xffffff},
    {"spectrum_fill_gradient", &cfg_sm.p_spectrum_fill_gradient, 0, 1},
    {"smeter_bar_color", &cfg_sm.p_smeter_bar_color, -1, 0xffffff},
    {"swr_color", &cfg_sm.p_swr_color, -1, 0xffffff},
    {"smeter_lamp", &cfg_sm.p_smeter_lamp, 0, 1},
    {"spectrum_show", &cfg_sm.p_spectrum_show, 0, 1},   /* live: main_screen.c follows them */
    {"spectrum_height", &cfg_sm.p_spectrum_height, 60, 160},
    {"digi_text_compact", &cfg_sm.p_digi_text_compact, 0, 1},
    {"knob_info", &cfg_sm.p_knob_info, 0, 1},
    {"walkie", &cfg_sm.p_walkie, 0, 1},                 /* live: walkie.c follows it */
    {"speaker_off", &cfg_sm.p_speaker_off, 0, 1},       /* live: radio.c follows it */
    {"indep_vfo", &cfg_sm.p_indep_vfo, 0, 1},           /* Station: independent VFOs */
    {"mode_from_plan", &cfg_sm.p_mode_from_plan, 0, 1}, /* Station: the mode follows the band plan */
    {"band_region", &cfg_sm.p_band_region, 0, BP_PLAN_COUNT - 1},  /* Station: the band plan; live, screen_main.c */
    {"birdie_marks", &cfg_sm.p_birdie_marks, 0, 2},     /* Main screen: Birdie Cal's carriers */
    {"zoom_step_per_mode", &cfg_sm.p_zoom_step_per_mode, 0, 1},   /* Main screen: live, zoom_step_scope follows it */
    {"power_display", &cfg_sm.p_power_display, 0, 2},  /* Main screen: live, clock.c reads it */
#ifdef BOARD_X6200
    {"wr12", &cfg_sm.p_wr12, 0, 1},                     /* live: screen_main.c follows it; the X6100 has no row */
#endif
};

static const struct {
    const char    *name;
    params_bool_t *b;
} disp_bools[] = {
    {"wf_center_line", &params.waterfall_center_line},
    {"wf_zoom", &params.waterfall_zoom},
    {"wf_smooth", &params.waterfall_smooth_scroll},
    {"mag_freq", &params.mag_freq},
    {"icon_menus", &params.icon_menus},
    {"mag_info", &params.mag_info},
    {"mag_alc", &params.mag_alc},
    {"spectrum_filled", &params.spectrum_filled},
    {"spectrum_peak", &params.spectrum_peak},
};

typedef struct {
    char    name[24];
    char    text[40];
    int32_t val;
} disp_set_t;

static bool get_int(cJSON *req, int32_t min, int32_t max, int32_t *out) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(req, "val");

    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max)
        return false;
    *out = (int32_t)v->valuedouble;
    return true;
}

/* LVGL thread */
static void disp_apply(void *arg) {
    disp_set_t *d = (disp_set_t *)arg;

    for (auto &o : disp_ints)
        if (!strcmp(d->name, o.name))
            o.p->set(d->val);
    for (auto &o : disp_bools)
        if (!strcmp(d->name, o.name))
            params_bool_set(o.b, d->val);
    if (!strcmp(d->name, "auto_level_offset")) {
        cfg_sm.p_auto_level_offset.set(d->val / 10.0f);
    } else if (!strcmp(d->name, "bright_normal") || !strcmp(d->name, "bright_idle")) {
        bool normal = d->name[7] == 'n';
        params_lock();
        if (normal)
            params.brightness_normal = d->val;
        else
            params.brightness_idle = d->val;
        params_unlock(normal ? &params.dirty.brightness_normal : &params.dirty.brightness_idle);
        if (normal)
            backlight_set_brightness(params.brightness_normal);
    } else if (!strcmp(d->name, "bright_timeout")) {
        params_lock();
        params.brightness_timeout = d->val;
        params_unlock(&params.dirty.brightness_timeout);
        backlight_tick();
    } else if (!strcmp(d->name, "screen_off_min")) {
        params_lock();
        params.screen_off_min = LV_CLAMP(0, d->val, 120);
        params_unlock(&params.dirty.screen_off_min);
        backlight_settings_changed();
    } else if (!strcmp(d->name, "screen_off_always")) {
        params_lock();
        params.screen_off_always = d->val != 0;
        params_unlock(&params.dirty.screen_off_always);
        backlight_settings_changed();
    } else if (!strcmp(d->name, "bright_buttons")) {
        backlight_set_buttons((buttons_light_t)d->val);
    } else if (!strcmp(d->name, "theme")) {
        /* as Settings: saved and applied live */
        params_str_set(&params.theme_name, d->text);
        params_flush();
        cfg_api_flush_all();
        main_screen_set_theme(d->text);
    }
}

static const char *disp_set(cJSON *req, disp_set_t *d) {
    cJSON *t = cJSON_GetObjectItemCaseSensitive(req, "text");
    if (!cJSON_IsString(t) || strlen(t->valuestring) >= sizeof(d->name))
        return "Invalid option";
    memset(d, 0, sizeof(*d));
    strcpy(d->name, t->valuestring);
    if (!strcmp(d->name, "theme")) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(req, "theme");

        if (radio_get_state() != RADIO_RX)
            return "Radio is transmitting";     /* the open app closes */
        if (walkie_on())
            return "Not in walkie mode";        /* its screen holds the buttons */
        char names[32][32];
        int  count = theme_list(names, 32);

        for (int i = 0; cJSON_IsString(n) && i < count; i++)
            if (!strcmp(n->valuestring, names[i])) {
                snprintf(d->text, sizeof(d->text), "%s", names[i]);
                return NULL;
            }
        return "Unknown theme";
    }
    int32_t min = 0, max = 1;
    bool    known = false;
    for (auto &o : disp_ints)
        if (!strcmp(d->name, o.name)) {
            min = o.min, max = o.max, known = true;
        }
    for (auto &o : disp_bools)
        if (!strcmp(d->name, o.name))
            known = true;
    if (!strcmp(d->name, "auto_level_offset"))
        min = -150, max = 150, known = true;   /* tenths of dB */
    if (!strcmp(d->name, "bright_normal") || !strcmp(d->name, "bright_idle"))
        min = -1, max = 9, known = true;
    if (!strcmp(d->name, "bright_timeout"))
        min = 5, max = 120, known = true;
    if (!strcmp(d->name, "bright_buttons"))
        min = 0, max = 2, known = true;
    if (!strcmp(d->name, "screen_off_min"))
        min = 0, max = 120, known = true;
    if (!strcmp(d->name, "screen_off_always"))
        min = 0, max = 1, known = true;
    if (!known)
        return "Unknown option";
    if (!get_int(req, min, max, &d->val))
        return "Invalid value";
    return NULL;
}

static int disp_get(char *buf, size_t size) {
    int  l = snprintf(buf, size, "{\"t\":\"disp\"");
    char names[32][32];
    int  count = theme_list(names, 32);

    for (auto &o : disp_ints)
        l += snprintf(buf + l, size - l, ",\"%s\":%d", o.name, (int)o.p->get());
    for (auto &o : disp_bools)
        l += snprintf(buf + l, size - l, ",\"%s\":%d", o.name, o.b->x ? 1 : 0);
    l += snprintf(buf + l, size - l,
                  ",\"auto_level_offset\":%d,\"bright_normal\":%d,\"bright_idle\":%d,\"bright_timeout\":%d,"
                  "\"bright_buttons\":%d,\"screen_off_min\":%d,\"screen_off_always\":%d,\"theme\":\"%s\",\"themes\":[",
                  (int)lroundf(cfg_sm.p_auto_level_offset.get() * 10), params.brightness_normal, params.brightness_idle,
                  params.brightness_timeout, (int)params.brightness_buttons, params.screen_off_min, params.screen_off_always ? 1 : 0,
                  params.theme_name.x);
    for (int i = 0; i < count && l < (int)size - 160; i++)   /* room for the band plan after them */
        l += snprintf(buf + l, size - l, "%s\"%s\"", i ? "," : "", names[i]);
    l += snprintf(buf + l, size - l, "]");
    /* The band plan in use and what Auto finds (the locator or the callsign: letters, digits and /) */
    const char *how;
    char        at[16];
    int         region = band_plan_auto(&how, at, sizeof(at));
    l += snprintf(buf + l, size - l, ",\"band_region_now\":%d,\"band_region_auto\":%d,\"band_region_how\":\"%s\","
                  "\"band_region_at\":\"%s\"}\n", band_plan_now(), region, how, at);
    return l;
}

/* Ethernet (wifi.cpp): the saved profile and the adapter's state, checked as Settings checks them */
static const char *eth_keys[ETH_FIELDS] = {"ip", "mask", "gw", "dns"};

static int eth_get(char *buf, size_t size) {
    eth_info_t in;

    eth_get_info(&in);
    int l = snprintf(buf, size, "{\"t\":\"eth\",\"present\":%d,\"connected\":%d,\"state\":\"%s\",\"iface\":\"%s\","
                     "\"addr\":\"%s\",\"gateway\":\"%s\",\"dhcp\":%d",
                     in.present, in.connected, in.state, in.iface, in.addr, in.gw, in.cfg.dhcp);
    for (int i = 0; i < ETH_FIELDS; i++)
        l += snprintf(buf + l, size - l, ",\"%s\":\"%s\"", eth_keys[i], in.cfg.field[i]);
    l += snprintf(buf + l, size - l, "}\n");
    return l;
}

static const char *eth_set(cJSON *req, eth_cfg_t *cfg) {
    cJSON *dhcp = cJSON_GetObjectItemCaseSensitive(req, "dhcp");

    memset(cfg, 0, sizeof(*cfg));
    if (!cJSON_IsNumber(dhcp) && !cJSON_IsBool(dhcp))
        return "Invalid mode";
    cfg->dhcp = cJSON_IsTrue(dhcp) || (cJSON_IsNumber(dhcp) && dhcp->valueint);
    for (int i = 0; i < ETH_FIELDS; i++) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(req, eth_keys[i]);

        if (v && !cJSON_IsString(v))
            return "Invalid value";
        if (v && strlen(v->valuestring) >= sizeof(cfg->field[i]))
            return "Value too long";
        if (v)
            strcpy(cfg->field[i], v->valuestring);
    }
    return eth_check(cfg);
}

static void eth_apply_cb(void *arg) {
    eth_apply((eth_cfg_t *)arg);
}

/* What the radio can do somewhere in [lo, hi] Hz (band_use: HF, the transverters that are on, the X6200's receive
 * ranges): the web Bands page checks a TX shift with it. Read only, answered from this thread */
static const char *band_use_get(cJSON *req, char *out, size_t size, int *l) {
    cJSON *lo = cJSON_GetObjectItemCaseSensitive(req, "lo");
    cJSON *hi = cJSON_GetObjectItemCaseSensitive(req, "hi");

    if (!cJSON_IsNumber(lo) || !cJSON_IsNumber(hi) || lo->valuedouble < -2e9 || hi->valuedouble > 2e9 ||
        lo->valuedouble > hi->valuedouble)
        return "Invalid range";
    *l = snprintf(out, size, "{\"t\":\"band_use\",\"use\":%d}\n",
                  (int)cfg_sm.band_use((int32_t)lo->valuedouble, (int32_t)hi->valuedouble));
    return NULL;
}

/* The answer to one request line */
static int handle_line(const char *line, char *out, size_t size) {
    cJSON      *req  = cJSON_Parse(line);
    cJSON      *name = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    const char *err  = NULL;
    int         l    = 0;

    if (!cJSON_IsString(name)) {
        err = "Bad request";
    } else if (!strcmp(name->valuestring, "disp_get")) {
        l = disp_get(out, size);
    } else if (!strcmp(name->valuestring, "disp_set")) {
        disp_set_t d;
        if (!(err = disp_set(req, &d)) && !scheduler_put(disp_apply, &d, sizeof(d)))
            err = "Busy, try again";    /* not "ok" for a change that will not happen (audit A21) */
    } else if (!strcmp(name->valuestring, "eth_get")) {
        l = eth_get(out, size);
    } else if (!strcmp(name->valuestring, "eth_set")) {
        eth_cfg_t cfg;
        if (!(err = eth_set(req, &cfg)) && !scheduler_put(eth_apply_cb, &cfg, sizeof(cfg)))
            err = "Busy, try again";
    } else if (!strcmp(name->valuestring, "band_use")) {
        err = band_use_get(req, out, size, &l);
    } else {
        err = "Unknown command";
    }
    cJSON_Delete(req);
    if (err)
        return snprintf(out, size, "{\"t\":\"err\",\"err\":\"%s\"}\n", err);
    return l ? l : snprintf(out, size, "{\"t\":\"ok\"}\n");
}

/* One request per connection: a line in, a line out, the socket closes */
static void serve(int fd) {
    char   in[IN_SIZE], out[2048];
    size_t n = 0;
    struct timeval tv = {3, 0};

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    while (n < sizeof(in) - 1) {
        ssize_t r = recv(fd, in + n, sizeof(in) - 1 - n, 0);
        if (r <= 0)
            break;
        n += r;
        if (memchr(in, '\n', n))
            break;
    }
    in[n] = 0;
    char *nl = strchr(in, '\n');
    if (nl) {
        *nl  = 0;
        int l = handle_line(in, out, sizeof(out));
        if (l > 0 && l < (int)sizeof(out))
            send(fd, out, l, MSG_NOSIGNAL);
    }
    close(fd);
}

static void *server_thread(void *arg) {
    int listen_fd = *(int *)arg;

    while (true) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EINTR)
                usleep(100000);
            continue;
        }
        serve(fd);
    }
    return NULL;
}

void cfg_socket_init(void) {
    static int         fd;
    struct sockaddr_un addr = {};
    pthread_t          th;

    unlink(SOCK_PATH);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        LV_LOG_ERROR("cfg_socket: can't listen on %s", SOCK_PATH);
        if (fd >= 0)
            close(fd);
        return;
    }
    chmod(SOCK_PATH, 0666);
    pthread_create(&th, NULL, server_thread, &fd);
    pthread_detach(th);
}
