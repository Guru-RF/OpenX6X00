/* tools/front_keys: what front_keys.c and gui_events.h take of LVGL 8.3, its timers run by front_keys_test.c */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct _lv_obj_t   lv_obj_t;
typedef struct _lv_group_t lv_group_t;
typedef uint32_t           lv_event_code_t;
typedef int16_t            lv_coord_t;

typedef struct _lv_timer_t lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *);
struct _lv_timer_t {    /* as LVGL's */
    uint32_t      period;
    uint32_t      last_run;
    lv_timer_cb_t timer_cb;
    void         *user_data;
    int32_t       repeat_count;
};
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data);
void        lv_timer_del(lv_timer_t *t);
void        lv_timer_set_period(lv_timer_t *t, uint32_t period);
void        lv_timer_set_repeat_count(lv_timer_t *t, int32_t count);
void        lv_timer_reset(lv_timer_t *t);

enum { LV_KEY_UP = 17, LV_KEY_DOWN = 18, LV_KEY_BACKSPACE = 8, LV_KEY_ESC = 27 };
enum { LV_INDEV_STATE_RELEASED = 0, LV_INDEV_STATE_PRESSED };
enum { LV_INDEV_TYPE_NONE = 0, LV_INDEV_TYPE_POINTER, LV_INDEV_TYPE_KEYPAD };

typedef struct {
    uint32_t key;
    int      state;
    bool     continue_reading;
} lv_indev_data_t;

typedef struct _lv_indev_drv_t lv_indev_drv_t;
struct _lv_indev_drv_t {
    int       type;
    void      (*read_cb)(lv_indev_drv_t *drv, lv_indev_data_t *data);
    void     *user_data;
    uint16_t  long_press_time;
};
typedef struct {
    lv_indev_drv_t *driver;
} lv_indev_t;

void        lv_indev_drv_init(lv_indev_drv_t *drv);
lv_indev_t *lv_indev_drv_register(lv_indev_drv_t *drv);
void        lv_indev_set_group(lv_indev_t *indev, lv_group_t *group);

lv_obj_t *lv_scr_act(void);
int       lv_event_send(lv_obj_t *obj, lv_event_code_t code, void *param);

#define LV_LOG_WARN(...) ((void)0)
