/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the panel on a PC. The screen is lv_drivers' SDL window. The front keys and the
 *  knobs are FIFOs under the data directory, named as the board's devices (board.h): the
 *  radio's own drivers (front_keys.c, knob_device.c, mfk_device.c) read them, the browser panel
 *  (remote_server.c) writes into them, and so does this file for the PC's keys:
 *
 *      F1..F5                  F1..F5          F6 GEN   F7 APP   F8 KEY   F9 MSG
 *      F10 DFN   F11 DFL       F12 LOCK
 *      Left/Right, wheel       main knob       Up/Down, Ctrl+wheel     MFK
 *      Shift+Up/Down, Shift+wheel  VOL         Enter MFK press         Shift+Enter VOL press
 *      PageUp/PageDown         band            Ctrl+1/2/3  AM / CW / SSB
 *      Ctrl+B A/B   Ctrl+R PRE   Ctrl+T ATU   Ctrl+V V/M   Ctrl+G AGC   Ctrl+F FST   Ctrl+P PTT
 *
 *  The keys taken here never reach the SDL keyboard driver, the rest of the keyboard types
 *  into the GUI as a USB keyboard would.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#include "host_panel.h"
#include "../board.h"
#include "../key_input.h"
#include "lv_drivers/sdl/sdl.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

enum { DEV_KEYPAD, DEV_MAIN, DEV_VOL, DEV_MFK, DEV_POWER, DEV_COUNT };

static const char *dev_path[DEV_COUNT] = {
    BOARD_DEV_KEYPAD, BOARD_DEV_MAIN, BOARD_DEV_VOL, BOARD_DEV_MFK, BOARD_DEV_POWER,
};
static int dev_fd[DEV_COUNT];

/* The front keys, as front_keys.c knows them */
static const struct {
    SDL_Keycode sym;
    uint16_t    mod;    /* KMOD_CTRL or 0 */
    uint16_t    code;
} keys[] = {
    {SDLK_F1, 0, BTN_TRIGGER_HAPPY13},  {SDLK_F2, 0, BTN_TRIGGER_HAPPY14},  {SDLK_F3, 0, BTN_TRIGGER_HAPPY15},
    {SDLK_F4, 0, BTN_TRIGGER_HAPPY19},  {SDLK_F5, 0, BTN_TRIGGER_HAPPY20},
    {SDLK_F6, 0, BTN_TRIGGER_HAPPY1},   /* GEN */
    {SDLK_F7, 0, BTN_TRIGGER_HAPPY7},   /* APP */
    {SDLK_F8, 0, BTN_TRIGGER_HAPPY2},   /* KEY */
    {SDLK_F9, 0, BTN_TRIGGER_HAPPY8},   /* MSG */
    {SDLK_F10, 0, BTN_TRIGGER_HAPPY3},  /* DFN */
    {SDLK_F11, 0, BTN_TRIGGER_HAPPY9},  /* DFL */
    {SDLK_F12, 0, BTN_TRIGGER_HAPPY25}, /* LOCK */
    {SDLK_PAGEDOWN, 0, BTN_TRIGGER_HAPPY5}, {SDLK_PAGEUP, 0, BTN_TRIGGER_HAPPY6},   /* band */
    {SDLK_1, KMOD_CTRL, BTN_TRIGGER_HAPPY10},   /* AM */
    {SDLK_2, KMOD_CTRL, BTN_TRIGGER_HAPPY11},   /* CW */
    {SDLK_3, KMOD_CTRL, BTN_TRIGGER_HAPPY12},   /* SSB */
    {SDLK_b, KMOD_CTRL, BTN_TRIGGER_HAPPY16},   /* A/B */
    {SDLK_r, KMOD_CTRL, BTN_TRIGGER_HAPPY17},   /* PRE */
    {SDLK_t, KMOD_CTRL, BTN_TRIGGER_HAPPY18},   /* ATU */
    {SDLK_v, KMOD_CTRL, BTN_TRIGGER_HAPPY22},   /* V/M */
    {SDLK_g, KMOD_CTRL, BTN_TRIGGER_HAPPY23},   /* AGC */
    {SDLK_f, KMOD_CTRL, BTN_TRIGGER_HAPPY24},   /* FST */
    {SDLK_p, KMOD_CTRL, BTN_TRIGGER_HAPPY4},    /* PTT */
};

#define VOL_PRESS BTN_TRIGGER_HAPPY21
#define MFK_PRESS BTN_TRIGGER_HAPPY27

static void emit(int dev, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev[2];

    memset(ev, 0, sizeof(ev));
    gettimeofday(&ev[0].time, NULL);
    ev[0].type  = type;
    ev[0].code  = code;
    ev[0].value = value;
    ev[1].time  = ev[0].time;
    ev[1].type  = EV_SYN;
    ev[1].code  = SYN_REPORT;
    if (dev_fd[dev] >= 0 && write(dev_fd[dev], ev, sizeof(ev)) < 0) {
        LV_LOG_WARN("host panel: %s: %s", dev_path[dev], strerror(errno));
    }
}

/* A knob turned n detents clockwise (negative: anticlockwise), as the hardware counts: the
 * main knob right turns positive, VOL and MFK left turns positive (knob_device.c, mfk_device.c) */
static void knob(int dev, int n) {
    uint16_t axis = dev == DEV_MAIN ? REL_X : dev == DEV_VOL ? REL_Y : REL_Z;
    int      dir  = dev == DEV_MAIN ? 1 : -1;

    for (int i = 0; i < abs(n); i++) {
        emit(dev, EV_REL, axis, n > 0 ? dir : -dir);
    }
}

/* Before SDL queues an event: the ones that are the radio's keys and knobs are taken here
 * (return 0 drops them), the rest go on to the SDL keyboard and mouse drivers */
static int filter(void *unused, SDL_Event *e) {
    if (e->type == SDL_MOUSEWHEEL) {
        SDL_Keymod m = SDL_GetModState();
        int        n = e->wheel.y;

        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            n = -n;
        }
        knob(m & KMOD_SHIFT ? DEV_VOL : m & KMOD_CTRL ? DEV_MFK : DEV_MAIN, n);
        return 0;
    }
    if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP) {
        return 1;
    }
    bool        down  = e->type == SDL_KEYDOWN;
    SDL_Keycode sym   = e->key.keysym.sym;
    uint16_t    mod   = e->key.keysym.mod;
    bool        ctrl  = mod & KMOD_CTRL;
    bool        shift = mod & KMOD_SHIFT;

    if (e->key.repeat && !(sym == SDLK_LEFT || sym == SDLK_RIGHT || sym == SDLK_UP || sym == SDLK_DOWN)) {
        return 0;   /* a held front key is one press (the long press is the driver's) */
    }
    switch (sym) {
        case SDLK_LEFT:  if (down) knob(DEV_MAIN, -1); return 0;
        case SDLK_RIGHT: if (down) knob(DEV_MAIN, 1); return 0;
        case SDLK_UP:    if (down) knob(shift ? DEV_VOL : DEV_MFK, 1); return 0;
        case SDLK_DOWN:  if (down) knob(shift ? DEV_VOL : DEV_MFK, -1); return 0;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (ctrl) {
                return 1;
            }
            emit(DEV_KEYPAD, EV_KEY, shift ? VOL_PRESS : MFK_PRESS, down ? 1 : 0);
            return 0;
        default:
            break;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (keys[i].sym == sym && (keys[i].mod ? ctrl : !ctrl)) {
            emit(DEV_KEYPAD, EV_KEY, keys[i].code, down ? 1 : 0);
            return 0;
        }
    }
    return 1;
}

static void fifo_make(int dev) {
    char dir[256];

    snprintf(dir, sizeof(dir), "%s/run", BOARD_DATA);
    mkdir(dir, 0755);
    unlink(dev_path[dev]);
    if (mkfifo(dev_path[dev], 0600) != 0) {
        fprintf(stderr, "host panel: mkfifo %s: %s\n", dev_path[dev], strerror(errno));
    }
    /* Read and write, so the FIFO is open whether or not the driver has it yet */
    dev_fd[dev] = open(dev_path[dev], O_RDWR | O_NONBLOCK);
    if (dev_fd[dev] < 0) {
        fprintf(stderr, "host panel: open %s: %s\n", dev_path[dev], strerror(errno));
    }
}

void host_panel_open(void) {
    mkdir(BOARD_DATA, 0755);
    for (int i = 0; i < DEV_COUNT; i++) {
        fifo_make(i);
    }
    sdl_init();
    SDL_SetEventFilter(filter, NULL);
}

/* What the window shows, landscape, for host_panel_dump() */
static uint32_t land[BOARD_DISP_W * BOARD_DISP_H];

void host_panel_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    lv_coord_t w = lv_area_get_width(area);

    for (lv_coord_t y = area->y1; y <= area->y2; y++) {
        for (lv_coord_t x = area->x1; x <= area->x2; x++) {
            if (x >= 0 && x < BOARD_DISP_W && y >= 0 && y < BOARD_DISP_H)
                land[y * BOARD_DISP_W + x] = px[(y - area->y1) * w + (x - area->x1)].full;
        }
    }
    sdl_display_flush(drv, area, px);
}

/* The screen as drm_dump() writes it on the radio: the portrait panel (480 x 800), XRGB8888, so
 * the same converters read both (tools/screenshots/raw2png.py) */
bool host_panel_dump(const char *path) {
    static uint32_t row[BOARD_DISP_H];
    FILE           *f = fopen(path, "wb");
    bool            ok = f != NULL;

    for (int py = 0; py < BOARD_DISP_W && ok; py++) {
        for (int px = 0; px < BOARD_DISP_H; px++) {
            row[px] = land[px * BOARD_DISP_W + (BOARD_DISP_W - 1 - py)] | 0xff000000;
        }
        ok = fwrite(row, 4, BOARD_DISP_H, f) == BOARD_DISP_H;
    }
    return f && fclose(f) == 0 && ok;
}

static lv_indev_drv_t mouse_drv, kbd_drv;

void host_panel_inputs(void) {
    lv_indev_drv_init(&mouse_drv);
    mouse_drv.type    = LV_INDEV_TYPE_POINTER;
    mouse_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&mouse_drv);

    lv_indev_drv_init(&kbd_drv);
    kbd_drv.type    = LV_INDEV_TYPE_KEYPAD;
    kbd_drv.read_cb = sdl_keyboard_read;
    lv_indev_set_group(lv_indev_drv_register(&kbd_drv), keyboard_group);
}
