/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The GUI's own event codes, and a queue for events sent from other threads: LVGL is not
 * thread safe, so they wait here until the GUI loop delivers them (event_obj_check()). */

#include "gui_events.h"

#include "key_input.h"

#include <pthread.h>
#include <stdlib.h>

#define QUEUE_SIZE 64       /* one slot stays free: 63 events at most */

uint32_t EVENT_ROTARY;
uint32_t EVENT_KEYPAD;
uint32_t EVENT_HKEY;
uint32_t EVENT_RADIO_TX;
uint32_t EVENT_RADIO_RX;
uint32_t EVENT_SCREEN_UPDATE;
uint32_t EVENT_MSG_UPDATE;
uint32_t EVENT_GPS;
uint32_t EVENT_BAND_UP;
uint32_t EVENT_BAND_DOWN;

typedef struct {
    lv_obj_t       *obj;            /* NULL: the object was deleted meanwhile */
    lv_event_code_t code;
    void           *param;          /* malloc'ed by the sender, freed once delivered */
} queued_t;

static queued_t        ring[QUEUE_SIZE];
static uint8_t         head, tail;    /* the next to write, the next to deliver */
static pthread_mutex_t lock;

void event_init() {
    uint32_t *ids[] = {&EVENT_ROTARY,        &EVENT_KEYPAD,     &EVENT_HKEY, &EVENT_RADIO_TX, &EVENT_RADIO_RX,
                       &EVENT_SCREEN_UPDATE, &EVENT_MSG_UPDATE, &EVENT_GPS,  &EVENT_BAND_UP,  &EVENT_BAND_DOWN};

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        *ids[i] = lv_event_register_id();
    }
    pthread_mutex_init(&lock, NULL);
}

static bool take(queued_t *q) {
    pthread_mutex_lock(&lock);
    bool any = tail != head;

    if (any) {
        *q   = ring[tail];
        tail = (tail + 1) % QUEUE_SIZE;
    }
    pthread_mutex_unlock(&lock);
    return any;
}

void event_obj_check() {
    queued_t q;

    while (take(&q)) {
        if (!q.obj) {
            /* its object is gone (event_obj_drop_deleted()) */
        } else if (q.code == LV_EVENT_REFRESH) {
            lv_obj_invalidate(q.obj);
        } else {
            lv_event_send(q.obj, q.code, q.param);
        }
        free(q.param);
    }
}

/* After a theme change deleted the widgets: what the threads queued for them is not sent */
void event_obj_drop_deleted() {
    pthread_mutex_lock(&lock);
    for (uint8_t i = tail; i != head; i = (i + 1) % QUEUE_SIZE) {
        if (!lv_obj_is_valid(ring[i].obj)) {
            ring[i].obj = NULL;
        }
    }
    pthread_mutex_unlock(&lock);
}

static bool refresh_waiting(lv_obj_t *obj) {
    for (uint8_t i = tail; i != head; i = (i + 1) % QUEUE_SIZE) {
        if (ring[i].obj == obj && ring[i].code == LV_EVENT_REFRESH) {
            return true;
        }
    }
    return false;
}

void event_send(lv_obj_t *obj, lv_event_code_t event_code, void *param) {
    pthread_mutex_lock(&lock);

    /* A refresh already waiting for this object covers this one too (the meter sends one per spectrum frame) */
    if (event_code == LV_EVENT_REFRESH && refresh_waiting(obj)) {
        pthread_mutex_unlock(&lock);
        return;
    }

    uint8_t next = (head + 1) % QUEUE_SIZE;

    if (next == tail) {
        pthread_mutex_unlock(&lock);
        LV_LOG_ERROR("Overflow");
        free(param);
        return;
    }
    ring[head] = (queued_t){.obj = obj, .code = event_code, .param = param};
    head       = next;
    pthread_mutex_unlock(&lock);
}

void event_send_key(int32_t key) {
    int32_t *k = malloc(sizeof(*k));

    *k = key;
    event_send(lv_group_get_focused(keyboard_group), LV_EVENT_KEY, k);
}
