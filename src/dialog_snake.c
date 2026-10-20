/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Waterfall Snake: a hidden game (long press on APP > GPS). The dialog is transparent, so the
 *  snake moves over the live waterfall; the main knob or the MFK turn it left or right, the MFK
 *  press pauses (or starts again after a game over), GEN or ESC leaves. The edges wrap around:
 *  only biting yourself ends the game. The best score stays in /mnt/snake_best.
 */
#include "dialog_snake.h"

#include "dialog.h"
#include "styles.h"
#include "theme.h"
#include "keyboard.h"
#include "events.h"
#include "main_screen.h"
#include "cluster_column.h"
#include "buttons.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CELL       20
#define COLS       (796 / CELL)
#define ROWS       (DIALOG_H / CELL)        /* DIALOG_H follows the button height: not constant */
#define MAX_LEN    (COLS * (DIALOG_H_MAX / CELL))
#define STEP_MS    180      /* the start: slow; 20 ms less every 100 points, down to 100 */
#define STEP_MIN   100
#define BEST_FILE  "/mnt/snake_best"

typedef struct {
    int8_t x, y;
} cell_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);

static void        pause_cb(button_data_t *b);
static void        restart_cb(button_data_t *b);
static void        exit_cb(button_data_t *b);
static const char *pause_label(void);

static button_data_t  b_pause   = {.type = BTN_TEXT_FN, .label_fn = pause_label, .press = pause_cb};
static button_data_t  b_restart = {.type = BTN_TEXT, .label = "Restart", .press = restart_cb};
static button_data_t  b_exit    = {.type = BTN_TEXT, .label = "Exit", .press = exit_cb};
static buttons_page_t page      = {.items = {&b_pause, &b_restart, NULL, NULL, &b_exit}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .rotary_cb    = rotary_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
};

dialog_t *dialog_snake = &dialog;

static cell_t      snake[MAX_LEN];
static int         len;
static cell_t      dir, food;
static int         turn;            /* -1 left, +1 right: one turn a step, the knob sends many */
static int         score, best;
static bool        over, paused;
static lv_obj_t   *field, *hud, *banner;
static lv_timer_t *timer;

static void best_load(void) {
    FILE *f = fopen(BEST_FILE, "r");

    best = 0;
    if (f) {
        if (fscanf(f, "%d", &best) != 1)
            best = 0;
        fclose(f);
    }
}

static void best_save(void) {
    FILE *f = fopen(BEST_FILE, "w");

    if (f) {
        fprintf(f, "%d\n", best);
        fclose(f);
    }
}

static bool on_snake(int x, int y) {
    for (int i = 0; i < len; i++)
        if (snake[i].x == x && snake[i].y == y)
            return true;
    return false;
}

static void place_food(void) {
    do {
        food.x = rand() % COLS;
        food.y = rand() % ROWS;
    } while (on_snake(food.x, food.y));
}

static void show_text(void) {
    lv_label_set_text_fmt(hud, "SCORE %04d   BEST %04d", score, best);
    if (over)
        lv_label_set_text_fmt(banner, "73, GAME OVER\n#%06x Score %d - MFK to call CQ again#", theme_hex(TC_text), score);
    else if (paused)
        lv_label_set_text_fmt(banner, "WATERFALL SNAKE\n#%06x Turn the knob to steer - MFK to start#", theme_hex(TC_text));
    if (over || paused)
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    buttons_refresh(&b_pause);
}

static void reset(void) {
    len = 4;
    for (int i = 0; i < len; i++) {
        snake[i].x = COLS / 3 - i;
        snake[i].y = ROWS / 2;
    }
    dir    = (cell_t){1, 0};
    turn   = 0;
    score  = 0;
    over   = false;
    paused = true;      /* the first MFK press or knob turn starts */
    place_food();
    lv_timer_set_period(timer, STEP_MS);
    show_text();
    lv_obj_invalidate(field);
}

static void step(lv_timer_t *t) {
    cell_t h;

    if (over || paused)
        return;
    if (turn) {         /* right: (x, y) -> (-y, x) on screen, y down */
        cell_t d = dir;
        dir.x = turn > 0 ? -d.y : d.y;
        dir.y = turn > 0 ? d.x : -d.x;
        turn  = 0;
    }
    h.x = (snake[0].x + dir.x + COLS) % COLS;
    h.y = (snake[0].y + dir.y + ROWS) % ROWS;

    bool eat = h.x == food.x && h.y == food.y;

    /* the tail moves away this step unless the snake grows */
    for (int i = 0; i < len - (eat ? 0 : 1); i++)
        if (snake[i].x == h.x && snake[i].y == h.y) {
            over = true;
            if (score > best) {
                best = score;
                best_save();
            }
            show_text();
            lv_obj_invalidate(field);
            return;
        }
    if (eat && len < MAX_LEN)
        len++;
    for (int i = len - 1; i > 0; i--)
        snake[i] = snake[i - 1];
    snake[0] = h;
    if (eat) {
        score += 10;
        if (score % 100 == 0) {
            uint32_t p = STEP_MS - 20 * (score / 100);
            lv_timer_set_period(timer, p < STEP_MIN ? STEP_MIN : p);
        }
        place_food();
        show_text();
    }
    lv_obj_invalidate(field);
}

static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t   *ctx = lv_event_get_draw_ctx(e);
    lv_obj_t        *obj = lv_event_get_target(e);
    lv_coord_t       x0  = obj->coords.x1, y0 = obj->coords.y1;
    lv_draw_rect_dsc_t d;
    lv_area_t        a;

    lv_draw_rect_dsc_init(&d);
    d.radius = 3;

    /* food: a hot signal, a soft glow around a bright core */
    d.bg_color = TC(warn);
    d.bg_opa   = LV_OPA_40;
    a.x1 = x0 + food.x * CELL - 2;
    a.y1 = y0 + food.y * CELL - 2;
    a.x2 = a.x1 + CELL + 3;
    a.y2 = a.y1 + CELL + 3;
    lv_draw_rect(ctx, &d, &a);
    d.bg_opa = LV_OPA_COVER;
    a.x1 += 7;
    a.y1 += 7;
    a.x2 -= 7;
    a.y2 -= 7;
    lv_draw_rect(ctx, &d, &a);

    /* snake: the head in the text colour, the body fading toward the tail */
    for (int i = len - 1; i >= 0; i--) {
        d.bg_color = i == 0 ? TC(text) : TC(ok);
        d.bg_opa   = i == 0 ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER - LV_OPA_50) * i / len);
        a.x1 = x0 + snake[i].x * CELL + 2;
        a.y1 = y0 + snake[i].y * CELL + 2;
        a.x2 = a.x1 + CELL - 5;
        a.y2 = a.y1 + CELL - 5;
        lv_draw_rect(ctx, &d, &a);
    }
}

static void steer(int d) {
    if (over)
        return;
    if (paused) {
        paused = false;
        show_text();
    }
    if (!turn)
        turn = d;
}

static const char *pause_label(void) {
    return over ? "Pause" : paused ? "Play" : "Pause";
}

static void pause_cb(button_data_t *b) {
    if (over)
        return;
    paused = !paused;
    show_text();
}

static void restart_cb(button_data_t *b) {
    reset();
}

static void exit_cb(button_data_t *b) {
    main_screen_close_app();
}

/* The knob was too touchy: its events are counted (the size of each grows with the acceleration),
 * a turn every KNOB_EVENTS the same way, then KNOB_PAUSE_MS of rest so one flick is one turn */
#define KNOB_EVENTS   6
#define KNOB_PAUSE_MS 350

static void rotary_cb(int32_t diff) {
    static int      acc;
    static uint32_t last_turn;

    if (!diff || lv_tick_elaps(last_turn) < KNOB_PAUSE_MS)
        return;
    if ((acc > 0 && diff < 0) || (acc < 0 && diff > 0))
        acc = 0;
    acc += diff > 0 ? 1 : -1;
    if (acc >= KNOB_EVENTS || acc <= -KNOB_EVENTS) {
        steer(acc > 0 ? 1 : -1);
        acc       = 0;
        last_turn = lv_tick_get();
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            main_screen_close_app();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            steer(-1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            steer(1);
            break;
        case LV_KEY_ENTER:
            if (over)
                reset();
            else
                paused = !paused;
            show_text();
            break;
    }
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    /* see-through but darkened: the live waterfall is the playing field, the snake stands out */
    lv_obj_set_style_bg_color(dialog.obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dialog.obj, LV_OPA_60, 0);
    lv_obj_set_style_bg_grad_dir(dialog.obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_img_opa(dialog.obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dialog.obj, 0, 0);
    lv_obj_set_style_shadow_width(dialog.obj, 0, 0);
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);   /* MFK turns arrive as keys */

    field = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, COLS * CELL, ROWS * CELL);
    lv_obj_set_pos(field, 0, 0);
    lv_obj_add_event_cb(field, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    hud = lv_label_create(dialog.obj);
    lv_obj_set_style_text_font(hud, &sony_22, 0);
    lv_obj_set_style_text_color(hud, TC(text), 0);
    lv_obj_set_style_bg_color(hud, TC(panel), 0);
    lv_obj_set_style_bg_opa(hud, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(hud, 6, 0);
    lv_obj_set_style_radius(hud, 4, 0);
    lv_obj_set_pos(hud, 8, 8);

    banner = lv_label_create(dialog.obj);
    lv_label_set_recolor(banner, true);
    lv_obj_set_style_text_font(banner, &sony_36, 0);
    lv_obj_set_style_text_color(banner, TC(warn), 0);
    lv_obj_set_style_text_align(banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(banner, TC(panel), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(banner, 14, 0);
    lv_obj_set_style_radius(banner, 6, 0);
    lv_obj_align(banner, LV_ALIGN_BOTTOM_MID, 0, -16);   /* the snake stays in sight */

    main_screen_lock_freq(true);
    cluster_column_hide(true);
    best_load();
    srand(time(NULL));
    timer = lv_timer_create(step, STEP_MS, NULL);
    reset();
}

static void destruct_cb(void) {
    lv_timer_del(timer);
    timer = NULL;
    main_screen_lock_freq(false);
    cluster_column_hide(false);
}
