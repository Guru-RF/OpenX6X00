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
#include <deque>
#include <mutex>
#include <pthread.h>

extern "C" {
    #include "lvgl/lvgl.h"
}

#define QUEUE_MAX_SIZE  64
#define QUEUE_URGENT    16      /* room beyond that for what must get through: an unkey, a stop, a key */
#define WORK_BUDGET     32      /* items per scheduler_work(): a flooding producer leaves LVGL its turn */

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
    bool     coalesced; /* scheduler_put_coalesced: one of its fn in the queue at a time */
};

static std::deque<item_t> queue;
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

static bool put(scheduler_fn_t fn, void *arg, sync_t *sync, bool urgent, bool coalesced = false) {
    std::lock_guard<std::mutex> lock(m_mutex);
    /* One already waiting covers this ask: it shows what is there when it runs. A redraw asked at each DSP
     * frame (3 jobs every ~95 ms on the X6200) took all 64 places while the GUI thread was behind at the start,
     * and every other job was dropped for seconds (#33) */
    if (coalesced) {
        for (const item_t &i : queue) {
            if (i.coalesced && i.fn == fn) {
                return true;
            }
        }
    }
    size_t limit = urgent ? QUEUE_MAX_SIZE + QUEUE_URGENT : QUEUE_MAX_SIZE;
    if (queue.size() >= limit) {
        LV_LOG_ERROR("Scheduler queue overflow");
        return false;
    }
    queue.push_back({fn, arg, sync, coalesced});
    return true;
}

static bool put_copy(scheduler_fn_t fn, void *arg, size_t arg_size, bool urgent) {
    void *arg_copy = nullptr;
    if (arg_size) {
        arg_copy = malloc(arg_size);
        if (!arg_copy)
            return false;
        memcpy(arg_copy, arg, arg_size);
    }
    if (!put(fn, arg_copy, nullptr, urgent)) {
        free(arg_copy);
        return false;
    }
    return true;
}

bool scheduler_put(scheduler_fn_t fn, void * arg, size_t arg_size) {
    return put_copy(fn, arg, arg_size, false);
}

bool scheduler_put_urgent(scheduler_fn_t fn, void * arg, size_t arg_size) {
    return put_copy(fn, arg, arg_size, true);
}

bool scheduler_put_noargs(scheduler_fn_t fn) {
    return scheduler_put(fn, NULL, 0);
}

bool scheduler_put_coalesced(scheduler_fn_t fn) {
    return put(fn, nullptr, nullptr, false, true);
}

bool scheduler_call(scheduler_fn_t fn, void *arg) {
    if (scheduler_on_gui_thread()) {
        fn(arg);
        return true;
    }
    sync_t sync;
    if (!put(fn, arg, &sync, false)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(sync.m);
    sync.cv.wait(lock, [&sync]() { return sync.done; });
    return true;
}

void scheduler_work() {
    item_t item;
    for (int budget = WORK_BUDGET; budget; budget--) {
        {
            /* The emptiness test under the lock too. Out of the queue before it runs: a coalesced ask
             * meanwhile is for data newer than this run shows, and is queued */
            std::lock_guard<std::mutex> lock(m_mutex);
            if (queue.empty()) break;
            item = queue.front();
            queue.pop_front();
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
