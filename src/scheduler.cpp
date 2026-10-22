/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#include "scheduler.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <queue>

extern "C" {
    #include "lvgl/lvgl.h"
}

#define QUEUE_MAX_SIZE  64

/* A caller waiting for its item (scheduler_call) */
struct sync_t {
    std::mutex              m;
    std::condition_variable cv;
    bool                    done = false;
};

struct item_t {
    scheduler_fn_t fn;
    void    *arg;
    sync_t  *sync;      /* NULL: arg is our copy, freed after the call */
};

static std::queue<item_t> queue;
static std::mutex m_mutex;

static pthread_t gui_thread;
static bool      gui_thread_known = false;

void scheduler_init() {
    gui_thread       = pthread_self();
    gui_thread_known = true;
}

bool scheduler_on_gui_thread() {
    return gui_thread_known && pthread_equal(pthread_self(), gui_thread);
}

static bool put(scheduler_fn_t fn, void *arg, sync_t *sync) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (queue.size() > QUEUE_MAX_SIZE){
        LV_LOG_ERROR("Scheduler queue overflow");
        return false;
    }
    queue.push({fn, arg, sync});
    return true;
}

void scheduler_put(scheduler_fn_t fn, void * arg, size_t arg_size) {
    void *arg_copy = nullptr;
    if (arg_size) {
        arg_copy = malloc(arg_size);
        memcpy(arg_copy, arg, arg_size);
    }
    if (!put(fn, arg_copy, nullptr)) {
        free(arg_copy);
    }
}

void scheduler_put_noargs(scheduler_fn_t fn) {
    return scheduler_put(fn, NULL, 0);
}

bool scheduler_call(scheduler_fn_t fn, void *arg) {
    if (scheduler_on_gui_thread()) {
        fn(arg);
        return true;
    }
    sync_t sync;
    if (!put(fn, arg, &sync)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(sync.m);
    sync.cv.wait(lock, [&sync]() { return sync.done; });
    return true;
}

void scheduler_work() {
    item_t item;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);   /* the emptiness test under the lock too */
            if (queue.empty()) break;
            item = queue.front();
            queue.pop();
        }
        item.fn(item.arg);
        if (item.sync) {
            std::lock_guard<std::mutex> lock(item.sync->m);
            item.sync->done = true;
            item.sync->cv.notify_one();
        } else if (item.arg) {
            free(item.arg);
        }
    }
}
