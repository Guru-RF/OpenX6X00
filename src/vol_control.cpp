/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The VOL knob: as the MFK (mfk.cpp), with its state kept in its device (vol->state, the
 * keys it sends depend on it). In walkie mode it is the volume only. */

#include "vol_control.h"

#include "radio_controls.h"
#include "knob_info.h"
#include "theme.h"

extern "C" {
#include "main.h"
#include "msg_box.h"
#include "walkie.h"
}
#include "voice.h"

static cfg_ctrl_t vol_ctrl = CTRL_VOL;

void vol_init() {
    vol_ctrl = controls_encoder_get_next(ENCODER_BIND_VOL, vol_ctrl, 0);   /* the first one bound */
    knobs_set_vol_param(vol_ctrl);
}

void vol_update(int16_t diff) {
    std::string msg(64, '\0');

    controls_encoder_update(vol_ctrl, diff, msg);
    voice_trace("vol_update diff %i ctrl %i: [%s]", diff, (int)vol_ctrl, msg.c_str());
    if (!(vol_ctrl == CTRL_VOL && diff)) {     /* a turned volume: radio_change_vol() says it */
        voice_say_ctrl(msg.c_str());
    }

    uint32_t color = vol->state == VOL_STATE_EDIT ? theme_hex(TC_text) : theme_hex(TC_text_soft);

    if (!knobs_visible()) {
        voice_msg_hold(true);   /* said above already */
        msg_update_text_fmt("#%06x %s", color, msg.c_str());
        voice_msg_hold(false);
    }
}

void vol_change_ctrl(int16_t dir) {
    if (dir == 0) {
        LV_LOG_ERROR("Unknown direction for change control (0)");
        return;
    }
    if (walkie_on()) {
        return;
    }
    vol_ctrl = controls_encoder_get_next(ENCODER_BIND_VOL, vol_ctrl, dir);
    knobs_set_vol_param(vol_ctrl);
    vol_update(0);
}

void vol_say_state() {
    if (!voice_is_on()) {
        return;
    }

    std::string msg(64, '\0');

    controls_encoder_update(vol_ctrl, 0, msg);
    voice_say_knob_state("VOL", vol->state == VOL_STATE_SELECT, msg.c_str());
}

cfg_ctrl_t vol_get_ctrl() {
    return vol_ctrl;
}

void vol_set_ctrl(cfg_ctrl_t ctrl) {
    vol_ctrl   = ctrl;
    vol->state = VOL_STATE_EDIT;
    knobs_set_vol_param(vol_ctrl);
    knobs_set_vol_state(true);
    vol_update(0);
}
