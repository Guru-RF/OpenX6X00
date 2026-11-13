/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The radio's question when a TCI client on the network asks to transmit (tci_ask.h).
 *
 *  A box on the top layer, over the main screen, an app or Settings, with three choices: Allow once, Always, No.
 *  No is chosen when it opens, so a press meant for something else lets nothing on the air. It takes the keys while
 *  it is open: the MFK turns the choice and its press confirms, the keyboard's arrows and Enter do the same, Esc is
 *  No, the VOL knob still sets the volume; a touch on a choice takes that one. The focus goes back where it was when
 *  it closes, unless something else took it meanwhile (an app opened with its key). The TCI server keeps the time:
 *  no answer within its seconds is a No, and the box closes with a note. Should that word be lost, the box closes
 *  itself two seconds later and answers no answer.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "tci_ask.h"
#include "tci_server.h"

#include "fonts/font_select.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "key_input.h"
#include "msg_box.h"
#include "radio.h"
#include "screen_light.h"
#include "theme.h"

#include <stdio.h>

#define VF(n)   (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS - 1) : &sony_##n)
#define NOTE_MS 6000

enum { PICK_ONCE, PICK_ALWAYS, PICK_NO, PICKS };

static const char *const pick_name[PICKS]   = {"Allow once", "Always", "No"};
static const int         pick_answer[PICKS] = {TCI_ASK_ONCE, TCI_ASK_ALWAYS, TCI_ASK_NO};

static lv_obj_t   *box, *hint, *btn[PICKS];
static lv_obj_t   *back;            /* focused before the box took the keys */
static bool        back_editing;
static lv_timer_t *tick;
static uint32_t    cur_id;          /* the question on the screen, 0 none */
static uint32_t    last_id;         /* the last one shown, for the note after it */
static char        last_who[96];
static int         sel = PICK_NO;
static int         left_s;
static bool        picked;          /* answered: the box goes after the event that answered it */

static void show_sel(void) {
    for (int i = 0; i < PICKS; i++) {
        bool on = i == sel;

        lv_obj_set_style_bg_color(btn[i], on ? TC(sel_bg) : TC(chip), 0);
        lv_obj_set_style_border_color(btn[i], on ? TC(sel_border) : TC(popup_border), 0);
        lv_obj_set_style_border_width(btn[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn[i], 0), on ? TC(sel_text) : TC(text), 0);
    }
}

static void show_hint(void) {
    lv_label_set_text_fmt(hint, "MFK: choose, press: confirm   No in %d s", left_s > 0 ? left_s : 0);
}

/* The box gone, the keys back where they were if it still had them */
static void box_close(void) {
    if (!box) {
        return;
    }
    if (tick) {
        lv_timer_del(tick);
        tick = NULL;
    }
    if (lv_group_get_focused(keyboard_group) == box && back && lv_obj_is_valid(back) &&
        lv_obj_get_group(back) == keyboard_group) {
        lv_group_focus_obj(back);
        lv_group_set_editing(keyboard_group, back_editing);
    }
    lv_obj_del(box);
    box = hint = NULL;
    for (int i = 0; i < PICKS; i++) {
        btn[i] = NULL;
    }
    back   = NULL;
    cur_id = 0;
}

/* After the event: closing deletes the object that got it */
static void pick_async(void *arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;

    if (!box || id != cur_id) {
        return;
    }
    box_close();
    tci_server_answer(id, (tci_ask_t)pick_answer[sel]);
}

static void pick(int i) {
    lv_indev_t *indev = lv_indev_get_act();

    if (picked) {
        return;
    }
    picked = true;
    sel    = i;
    show_sel();
    /* the key that answered is still down: deleting the focused box would turn it into a long press repeating
     * into what has the focus next (text_popup.c) */
    if (indev) {
        lv_indev_wait_release(indev);
    }
    lv_async_call(pick_async, (void *)(uintptr_t)cur_id);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            if (!picked && sel > 0) {
                sel--;
                show_sel();
            }
            break;

        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            if (!picked && sel < PICKS - 1) {
                sel++;
                show_sel();
            }
            break;

        case LV_KEY_ENTER:
            pick(sel);
            break;

        case LV_KEY_ESC:
            pick(PICK_NO);
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

/* The MFK's press on the box (an encoder sends no Enter key to it, only the click); a touch on the box beside the
 * choices takes nothing */
static void box_click_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();

    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        pick(sel);
    }
}

/* A touch on a choice: that one */
static void btn_click_cb(lv_event_t *e) {
    pick((int)(intptr_t)lv_event_get_user_data(e));
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    left_s--;
    if (hint) {
        show_hint();
    }
    /* the server's close should have come at 0: the box does not stay up without it */
    if (left_s <= -2 && cur_id && !picked) {
        uint32_t id = cur_id;

        box_close();
        tci_server_answer(id, TCI_ASK_TIMEOUT);
    }
}

void tci_ask_show(uint32_t id, const char *who, int seconds) {
    const lv_coord_t w = theme_shape.vision ? 680 : 600;
    lv_obj_t        *obj, *row;

    box_close();
    backlight_tick();       /* the screen lit, the screensaver gone: the question is seen */
    cur_id = last_id = id;
    snprintf(last_who, sizeof(last_who), "%s", who);
    sel    = PICK_NO;
    left_s = seconds;
    picked = false;

    box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &popup_style, 0);
    lv_obj_set_size(box, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(box);

    obj = lv_label_create(box);
    lv_obj_set_style_text_font(obj, VF(18), 0);
    lv_obj_set_style_text_color(obj, TC(text_head), 0);
    lv_label_set_text(obj, "TCI");

    obj = lv_label_create(box);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(obj, VF(26), 0);
    lv_obj_set_style_text_color(obj, TC(text), 0);
    lv_label_set_text_fmt(obj, "%s wants to transmit", who);

    row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < PICKS; i++) {
        btn[i] = lv_obj_create(row);
        lv_obj_remove_style_all(btn[i]);
        lv_obj_set_size(btn[i], (w - 32 - 2 * 16) / PICKS, 52);
        lv_obj_set_style_bg_opa(btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn[i], theme_shape.radius_popup, 0);
        lv_obj_add_flag(btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn[i], btn_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        obj = lv_label_create(btn[i]);
        lv_obj_set_style_text_font(obj, VF(22), 0);
        lv_label_set_text(obj, pick_name[i]);
        lv_obj_center(obj);
    }

    hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, VF(16), 0);
    lv_obj_set_style_text_color(hint, TC(text_dim), 0);
    show_hint();
    show_sel();

    back         = lv_group_get_focused(keyboard_group);
    back_editing = lv_group_get_editing(keyboard_group);
    lv_group_add_obj(keyboard_group, box);
    lv_obj_add_event_cb(box, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(box, box_click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(box);
    lv_group_set_editing(keyboard_group, true);

    tick = lv_timer_create(tick_cb, 1000, NULL);
}

void tci_ask_close(uint32_t id, int why) {
    if (box && id == cur_id) {
        box_close();
    }
    if (id != last_id) {
        return;
    }
    switch (why) {
        case TCI_ASK_TIMEOUT:
            msg_schedule_text_ms(NOTE_MS, "%s: no answer, no transmission", last_who);
            break;
        case TCI_ASK_ALWAYS:
            msg_schedule_text_ms(NOTE_MS, "%s may transmit from now on (Settings > General > TCI SERVER)", last_who);
            break;
        case TCI_ASK_FULL:
            msg_schedule_text_ms(NOTE_MS, "%s may transmit until it disconnects: no room to keep it, forget one in "
                                          "Settings > General > TCI SERVER", last_who);
            break;
        default:
            break;
    }
}

bool tci_ask_open(void) {
    return box != NULL;
}
