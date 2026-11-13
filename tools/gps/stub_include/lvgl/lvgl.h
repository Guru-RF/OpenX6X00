/* Host test shim (tools/gps): what gps.c takes of LVGL, the log and the subscription to the USB messages */
#pragma once

#include <stdint.h>

typedef struct _lv_obj_t lv_obj_t;

typedef struct {
    uint32_t    id;
    void       *user_data;
    void       *_priv_data;
    const void *payload;
} lv_msg_t;

typedef void (*lv_msg_subscribe_cb_t)(void *s, lv_msg_t *msg);

void *lv_msg_subscribe(uint32_t msg_id, lv_msg_subscribe_cb_t cb, void *user_data);

void test_log(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LV_LOG_USER(...)    test_log("User", __VA_ARGS__)
#define LV_LOG_WARN(...)    test_log("Warn", __VA_ARGS__)
#define LV_LOG_ERROR(...)   test_log("Error", __VA_ARGS__)
