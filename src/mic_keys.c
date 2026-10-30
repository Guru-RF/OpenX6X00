/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* Hand mic keys. The base reports the key held down and 0 when it is let go. A mic key
 * goes to the main screen as EVENT_HKEY: press, release, and after HOLD_MS long and its
 * release. The digits type into the focused widget instead, except while a widget is
 * being edited (F-INP), where they are mic keys too; CE and F-INP always type.
 *
 * The codes are the X6100 mic's. The X6200's H-22 is another mic (F1-F3, HOME, the volume keys, A/B, three
 * columns of digits) and sends other codes for most of its keys, which are not known yet (#56): every code the
 * base reports goes into the log with the key it is taken for, so a press of each key gives the H-22's table.
 * Not while a text window is open: its text can be a WiFi password. */

#include "mic_keys.h"

#include "screen_light.h"
#include "gui_events.h"
#include "key_input.h"
#include "text_popup.h"

#include "radio_types.h"
#include <stdlib.h>

#define HOLD_MS 1000

enum { MIC_KEY, TYPED_UNLESS_EDITING, TYPED };

static const struct {
    uint32_t    code;
    hkey_t      key;    /* for a typed key: the character */
    uint8_t     use;
    const char *name;   /* the X6100 mic's label, for the log */
} keys[] = {
    {RADIO_HKEY_0, HKEY_0, TYPED_UNLESS_EDITING, "0"},     {RADIO_HKEY_1, HKEY_1, TYPED_UNLESS_EDITING, "1"},
    {RADIO_HKEY_2, HKEY_2, TYPED_UNLESS_EDITING, "2"},     {RADIO_HKEY_3, HKEY_3, TYPED_UNLESS_EDITING, "3"},
    {RADIO_HKEY_4, HKEY_4, TYPED_UNLESS_EDITING, "4"},     {RADIO_HKEY_5, HKEY_5, TYPED_UNLESS_EDITING, "5"},
    {RADIO_HKEY_6, HKEY_6, TYPED_UNLESS_EDITING, "6"},     {RADIO_HKEY_7, HKEY_7, TYPED_UNLESS_EDITING, "7"},
    {RADIO_HKEY_8, HKEY_8, TYPED_UNLESS_EDITING, "8"},     {RADIO_HKEY_9, HKEY_9, TYPED_UNLESS_EDITING, "9"},
    {RADIO_HKEY_DOT, HKEY_DOT, TYPED_UNLESS_EDITING, "."}, {RADIO_HKEY_CE, HKEY_CE, TYPED, "CE"},
    {RADIO_HKEY_FINP, HKEY_FINP, TYPED, "F-INP"},          {RADIO_HKEY_SPCH, HKEY_SPCH, MIC_KEY, "SPCH"},
    {RADIO_HKEY_TUNER, HKEY_TUNER, MIC_KEY, "TUNER"},      {RADIO_HKEY_XFC, HKEY_XFC, MIC_KEY, "XFC"},
    {RADIO_HKEY_UP, HKEY_UP, MIC_KEY, "UP"},               {RADIO_HKEY_DOWN, HKEY_DOWN, MIC_KEY, "DOWN"},
    {RADIO_HKEY_VM, HKEY_VM, MIC_KEY, "V/M"},              {RADIO_HKEY_NW, HKEY_NW, MIC_KEY, "NW"},
    {RADIO_HKEY_F1, HKEY_F1, MIC_KEY, "F1"},               {RADIO_HKEY_F2, HKEY_F2, MIC_KEY, "F2"},
    {RADIO_HKEY_MODE, HKEY_MODE, MIC_KEY, "MODE"},         {RADIO_HKEY_FIL, HKEY_FIL, MIC_KEY, "FIL"},
    {RADIO_HKEY_GENE, HKEY_GENE, MIC_KEY, "GENE"},
};

static event_hkey_t cur = {.state = HKEY_RELEASE};    /* the key down, or the last one */
static lv_timer_t  *hold_timer;

static void post(void) {
    event_hkey_t *e = malloc(sizeof(*e));

    *e = cur;
    event_send(lv_scr_act(), EVENT_HKEY, e);
}

static void hold_timer_stop(void) {
    if (hold_timer) {
        lv_timer_del(hold_timer);
        hold_timer = NULL;
    }
}

static void held_cb(lv_timer_t *t) {
    cur.state = HKEY_LONG;
    post();
    hold_timer = NULL;      /* one shot: LVGL deletes it */
}

static bool all_up(void) {
    return cur.state == HKEY_RELEASE || cur.state == HKEY_LONG_RELEASE;
}

void hkey_put(uint32_t code) {
    if (code == 0) {
        if (cur.state == HKEY_PRESS || cur.state == HKEY_LONG) {
            cur.state = cur.state == HKEY_PRESS ? HKEY_RELEASE : HKEY_LONG_RELEASE;
            if (cur.key != HKEY_UNKNOWN) {
                post();
            }
        }
        hold_timer_stop();
        return;
    }

    size_t i = 0;

    while (i < sizeof(keys) / sizeof(keys[0]) && keys[i].code != code) {
        i++;
    }
    /* Once per press: radio.c passes a code only when it changes. None while a text window is open: the digits
     * of a WiFi password typed on the mic would be in the log, which goes with bug reports (an unknown code too,
     * once the H-22's table says which digit it is) */
    if (!textarea_window_is_open()) {
        LV_LOG_USER("hand mic: key 0x%02X (%s)", (unsigned)code,
                    i < sizeof(keys) / sizeof(keys[0]) ? keys[i].name : "unknown");
    }
    if (i == sizeof(keys) / sizeof(keys[0])) {
        cur.key = HKEY_UNKNOWN;
    } else if (keys[i].use != MIC_KEY && keys[i].code != RADIO_HKEY_FINP && textarea_window_is_open()) {
        /* A text window open (a WiFi password, a call): digits, the dot and CE go into its text; they changed the
         * band before, the window holding the group in edit mode */
        if (all_up()) {
            textarea_window_type(keys[i].key);
            cur.state = HKEY_PRESS;
            cur.key   = HKEY_UNKNOWN;
        }
        return;
    } else if (keys[i].use == TYPED || (keys[i].use == TYPED_UNLESS_EDITING && !lv_group_get_editing(keyboard_group))) {
        /* Typed once per press: nothing while another key is still down */
        if (all_up()) {
            event_send_key(keys[i].key);
            cur.state = HKEY_PRESS;
            cur.key   = HKEY_UNKNOWN;
        }
        return;
    } else {
        cur.key = keys[i].key;
    }

    if (!all_up()) {
        return;
    }
    cur.state = HKEY_PRESS;
    backlight_tick();
    if (cur.key != HKEY_UNKNOWN) {
        post();
        hold_timer_stop();
        hold_timer = lv_timer_create(held_cb, HOLD_MS, NULL);
        lv_timer_set_repeat_count(hold_timer, 1);
    }
}
