/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The MFK knob on the main screen: the control it turns (one of those bound to it, or one
 * a button or a screen gives it) and whether turning changes its value or chooses another. */

#include "mfk_control.h"

#include "cluster_column.h"
#include "radio_controls.h"
#include "knob_info.h"
#include "theme.h"

extern "C" {
#include "msg_box.h"
}

mfk_state_t mfk_state = MFK_STATE_EDIT;
cfg_ctrl_t  mfk_ctrl  = CTRL_SPECTRUM_FACTOR;

/* The knob line, and the cluster column's highlight while the MFK moves through it */
static void show_ctrl(void) {
    knobs_set_mfk_param(mfk_ctrl);
    cluster_column_set_active(mfk_ctrl == CTRL_CLUSTER);
}

void mfk_init() {
    mfk_ctrl = controls_encoder_get_next(ENCODER_BIND_MFK, mfk_ctrl, 0);   /* the first one bound */
    show_ctrl();
}

/* Turn by diff (0: no change) and say the value, on the message line when the knob
 * lines are not shown; dim while the knob chooses */
void mfk_update(int16_t diff) {
    std::string msg(64, '\0');

    controls_encoder_update(mfk_ctrl, diff, msg);

    uint32_t color = mfk_state == MFK_STATE_EDIT ? theme_hex(TC_text) : theme_hex(TC_text_soft);

    if (!knobs_visible()) {
        msg_update_text_fmt("#%06x %s", color, msg.c_str());
    }
}

void mfk_change_ctrl(int16_t dir) {
    if (dir == 0) {
        LV_LOG_ERROR("Unknown direction for change control (0)");
        return;
    }
    mfk_ctrl = controls_encoder_get_next(ENCODER_BIND_MFK, mfk_ctrl, dir);
    show_ctrl();
    mfk_update(0);
}

/* Without the popup: a control the screen takes and gives back, not one the user chose */
void mfk_set_ctrl_quiet(cfg_ctrl_t ctrl) {
    mfk_ctrl  = ctrl;
    mfk_state = MFK_STATE_EDIT;
    knobs_set_mfk_state(true);
    show_ctrl();
}

void mfk_set_ctrl(cfg_ctrl_t ctrl) {
    mfk_set_ctrl_quiet(ctrl);
    mfk_update(0);
}

/* The cluster column was switched off while one of its MFK items was the active one */
void mfk_cluster_gone() {
    if (mfk_ctrl == CTRL_CLUSTER || mfk_ctrl == CTRL_CLUSTER_ORDER || mfk_ctrl == CTRL_CLUSTER_BANDS) {
        mfk_change_ctrl(+1);
    }
}
