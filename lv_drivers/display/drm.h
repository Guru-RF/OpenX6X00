/**
 * @file drm.h
 *
 */

#ifndef DRM_H
#define DRM_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#ifndef LV_DRV_NO_CONF
#ifdef LV_CONF_INCLUDE_SIMPLE
#include "lv_drv_conf.h"
#else
#include "../../lv_drv_conf.h"
#endif
#endif

#if USE_DRM

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/
/* false: no usable DRM device, nothing was changed on the screen (use fbdev) */
bool drm_init(void);
/* true: the GUI is on the overlay plane and what it leaves transparent shows
 * the primary plane, where the waterfall is drawn (drm_wf_present) */
bool drm_planes(void);
/* LVGL renders portrait 480x800 from now on: areas are turned 180 (the knobs on
 * top), not 90, and the waterfall plane is left alone; false: landscape again */
void drm_set_portrait(bool on);
/* flush_cb, partial rendering: area-sized color_p */
void drm_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p);
/* A strip (waterfall, spectrum) into the primary plane: portrait columns
 * x .. x + w - 1, all 800 rows of src (w pixels each, XRGB) */
void drm_wf_present(const uint32_t *src, int32_t x, int32_t w);
/* The same with each row of src a ring: column x shows src[head], then on */
void drm_wf_present_ring(const uint32_t *src, int32_t x, int32_t w, int32_t head);
/* Shows what was flushed or presented since the last call, at the next vblank:
 * once per main loop, after lv_timer_handler() */
void drm_commit(void);
/* The screen as /dev/fb0 would hold it (480x800 BGRX) into path; false if not active */
bool drm_dump(const char *path);
/* dst = top (ARGB) over bottom (XRGB), n pixels */
void drm_blend_under(uint32_t *dst, const uint32_t *top, const uint32_t *bottom, int32_t n);
/* The primary plane, portrait 480x800 XRGB (NULL without planes) */
const uint32_t * drm_wf_pixels(void);
/* A landscape 800x480 ARGB copy of the GUI (snapshot, recorder) completed
 * with the waterfall under its transparent parts; nothing without planes */
void drm_compose_landscape(uint32_t *px);

#endif  /*USE_DRM*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*DRM_H*/
