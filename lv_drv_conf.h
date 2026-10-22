/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - display and input drivers (lv_drivers 8.3)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  Only the drivers kept in lv_drivers/ can be switched on: DRM/KMS with the
 *  framebuffer as fallback for the screen, evdev, libinput and xkb for input.
 *  The rest is switched off; their code is not here.
 */

/* clang-format off */
#ifndef LV_DRV_CONF_H
#define LV_DRV_CONF_H

#include "lv_conf.h"

/* Bus hooks for SPI/parallel/I2C panels and touch chips: none on the X6100 */
#define LV_DRV_DELAY_INCLUDE  <stdint.h>
#define LV_DRV_DELAY_US(us)
#define LV_DRV_DELAY_MS(ms)
#define LV_DRV_DISP_INCLUDE   <stdint.h>
#define LV_DRV_DISP_CMD_DATA(val)
#define LV_DRV_DISP_RST(val)
#define LV_DRV_DISP_SPI_CS(val)
#define LV_DRV_DISP_SPI_WR_BYTE(data)
#define LV_DRV_DISP_SPI_WR_ARRAY(adr, n)
#define LV_DRV_DISP_PAR_CS(val)
#define LV_DRV_DISP_PAR_SLOW
#define LV_DRV_DISP_PAR_FAST
#define LV_DRV_DISP_PAR_WR_WORD(data)
#define LV_DRV_DISP_PAR_WR_ARRAY(adr, n)
#define LV_DRV_INDEV_INCLUDE  <stdint.h>
#define LV_DRV_INDEV_RST(val)
#define LV_DRV_INDEV_IRQ_READ 0
#define LV_DRV_INDEV_SPI_CS(val)
#define LV_DRV_INDEV_SPI_XCHG_BYTE(data) 0
#define LV_DRV_INDEV_I2C_START
#define LV_DRV_INDEV_I2C_STOP
#define LV_DRV_INDEV_I2C_RESTART
#define LV_DRV_INDEV_I2C_WR(data)
#define LV_DRV_INDEV_I2C_READ(last_read) 0

/* An SDL window instead of the panel: the host build (-DHOST_BUILD=ON) sets USE_SDL */
#ifndef USE_SDL
#define USE_SDL         0
#endif
#define USE_SDL_GPU     0
#if USE_SDL
#define SDL_HOR_RES         800
#define SDL_VER_RES         480
#define SDL_ZOOM            1
#define SDL_DOUBLE_BUFFERED 0
#define SDL_INCLUDE_PATH    <SDL2/SDL.h>
#define SDL_DUAL_DISPLAY    0
#endif

/* Drivers whose code is not in lv_drivers/: off */
#define USE_MONITOR     0
#define USE_WINDOWS     0
#define USE_WIN32DRV    0
#define USE_GTK         0
#define USE_WAYLAND     0
#define USE_SSD1963     0
#define USE_R61581      0
#define USE_ST7565      0
#define USE_GC9A01      0
#define USE_UC1610      0
#define USE_SHARP_MIP   0
#define USE_ILI9341     0
#define USE_XPT2046     0
#define USE_FT5406EE8   0
#define USE_AD_TOUCH    0
#define USE_MOUSE       0
#define USE_MOUSEWHEEL  0
#define USE_KEYBOARD    0

/* Screen: DRM/KMS on card0 or card1 ($DRM_CARD forces one), the framebuffer if that fails */
#ifndef USE_DRM
#define USE_DRM 1
#endif
#ifndef USE_FBDEV
#define USE_FBDEV 1
#endif
#ifndef USE_BSD_FBDEV
#define USE_BSD_FBDEV 0
#endif
#if USE_FBDEV || USE_BSD_FBDEV
#define FBDEV_PATH              "/dev/fb0"
#define FBDEV_DISPLAY_POWER_ON  1       /* power the display on at start */
#endif

/* Input devices through evdev, keyboards with an xkb key map */
#ifndef USE_EVDEV
#define USE_EVDEV 1
#endif
#ifndef USE_BSD_EVDEV
#define USE_BSD_EVDEV 0
#endif
#ifndef USE_LIBINPUT
#define USE_LIBINPUT 0
#endif
#ifndef USE_BSD_LIBINPUT
#define USE_BSD_LIBINPUT 0
#endif
#ifndef USE_XKB
#define USE_XKB 1
#endif

#if USE_LIBINPUT || USE_BSD_LIBINPUT
#define LIBINPUT_NAME   "/dev/input/event0"
#endif
#if USE_EVDEV || USE_BSD_EVDEV
#define EVDEV_NAME      "/dev/input/event0"
#define EVDEV_SWAP_AXES 0
#define EVDEV_CALIBRATE 0
#endif
#if (USE_LIBINPUT || USE_BSD_LIBINPUT || USE_EVDEV || USE_BSD_EVDEV) && USE_XKB
#define XKB_KEY_MAP { .rules = NULL, .model = "pc101", .layout = "us", .variant = NULL, .options = NULL }
#endif

#endif /*LV_DRV_CONF_H*/
