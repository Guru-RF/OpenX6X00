/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Wavelog's live link, the GUI's side (wavelog_cat.h, #245)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "wavelog_cat.h"

#include "cfg/cfg_api.h"
#include "control_owner.h"
#include "field/flog.h"
#include "msg_box.h"
#include "radio.h"
#include "screen_light.h"
#include "screen_main.h"
#include "tx_player.h"

#include <stdio.h>
#include <strings.h>

/* radio_mode_t's names, as the web server's MODES */
static const char *const mode_names[] = {"LSB", "LSB-D", "USB", "USB-D", "CW", "CWR", "AM", "NFM", "SAM", "WFM"};

int wavelog_cat_mode(const char *name) {
    /* SAM and WFM are the X6200's alone, and no spot asks for them */
    for (int i = radio_mode_lsb; name && i <= radio_mode_nfm; i++)
        if (!strcasecmp(name, mode_names[i]))
            return i;
    return -1;
}

int wavelog_cat_state(char *buf, size_t size) {
    int32_t     hz   = cparam_i_get(cfg_fg_freq);
    int32_t     mode = cparam_i_get(cfg_cur_mode);
    const char *app  = main_screen_digital_app();
    float       pwr  = param_f_get(cfg_pwr);

    if (mode < radio_mode_lsb || mode > radio_mode_last)
        mode = radio_mode_usb;
    if (radio_digi_cap() && pwr > TX_PLAYER_MAX_PWR_W)   /* as radio.c holds the radio to it */
        pwr = TX_PLAYER_MAX_PWR_W;
    return snprintf(buf, size,
                    "{\"t\":\"radio\",\"hz\":%d,\"tx_hz\":%d,\"mode\":\"%s\",\"log_mode\":\"%s\",\"app\":\"%s\","
                    "\"pwr\":%.1f,\"tx\":%d}\n",
                    (int)hz, (int)radio_tx_freq(), mode_names[mode], field_qso_mode(app, mode, hz), app ? app : "",
                    pwr, ctl_transmitting() ? 1 : 0);
}

const char *wavelog_cat_tune(int32_t hz, int mode) {
    if (mode < radio_mode_lsb || mode > radio_mode_nfm)
        return "No such mode";
    if (ctl_transmitting())
        return "The radio is transmitting";
    if (!cfg_is_valid_hw_freq(hz))
        return "The radio does not receive there now";
    if (main_screen_freq_locked())
        return "The frequency is locked (an app, or the lock)";
    /* The operator's own click, as a turn of the knob: control back from a browser session or a TCI program, never
     * while the radio transmits (checked again under control's lock) */
    if (!ctl_claim(CTL_LOCAL, 0, "", true))
        return "The radio is transmitting";
    backlight_tick();
    main_screen_set_freq((uint64_t)hz);
    if (!main_screen_mode_locked())
        cparam_i_set(cfg_cur_mode, mode);
    int32_t now = cparam_i_get(cfg_cur_mode);   /* the app's while it holds the mode */
    msg_update_text_fmt("Wavelog: %d.%03d.%d %s%s", (int)(hz / 1000000), (int)(hz / 1000 % 1000),
                        (int)(hz % 1000 / 100), now >= radio_mode_lsb && now <= radio_mode_last ? mode_names[now] : "",
                        cfg_is_rx_only_freq(hz) ? " - RX only" : "");
    return NULL;
}
