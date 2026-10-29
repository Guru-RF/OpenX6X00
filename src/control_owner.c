/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Who controls the radio (control_owner.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "control_owner.h"
#include "radio.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define INJECT_MS 150   /* an input event this soon after an injection is the injection, not the operator */

static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static ctl_info_t      owner;
static ctl_info_t      lender;    /* who a takeover took it from (ctl_give_back), kind CTL_NONE: no one */
static atomic_uint     version;
static atomic_uint_fast64_t injected_ms;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

void ctl_init(void) {
    memset(&owner, 0, sizeof(owner));
    memset(&lender, 0, sizeof(lender));
}

bool ctl_transmitting(void) {
    return radio_get_state() != RADIO_RX;
}

/* Every change of holder forgets the lender: only ctl_claim's takeover names one, after it */
static void set_owner(ctl_kind_t kind, int id, const char *name) {
    lender.kind = CTL_NONE;
    owner.kind = kind;
    owner.id   = id;
    snprintf(owner.name, sizeof(owner.name), "%s", name ? name : "");
    owner.since_ms = now_ms();
    atomic_fetch_add(&version, 1);
}

bool ctl_claim(ctl_kind_t kind, int id, const char *name, bool take) {
    bool ok;

    pthread_mutex_lock(&mux);
    if (owner.kind == kind && owner.id == id) {
        ok = true;
    } else if (owner.kind == CTL_NONE || (take && !ctl_transmitting())) {
        ctl_info_t was = owner;

        set_owner(kind, id, name);
        lender = was;
        ok = true;
    } else {
        ok = false;
    }
    pthread_mutex_unlock(&mux);
    return ok;
}

void ctl_release(ctl_kind_t kind, int id) {
    pthread_mutex_lock(&mux);
    if (owner.kind == kind && owner.id == id)
        set_owner(CTL_NONE, 0, "");
    else if (lender.kind == kind && lender.id == id)
        lender.kind = CTL_NONE;     /* gone: nothing to give back to */
    pthread_mutex_unlock(&mux);
}

void ctl_give_back(ctl_kind_t kind, int id) {
    pthread_mutex_lock(&mux);
    if (owner.kind == kind && owner.id == id && lender.kind != CTL_NONE) {
        ctl_info_t to = lender;

        set_owner(to.kind, to.id, to.name);
    }
    pthread_mutex_unlock(&mux);
}

bool ctl_holds(ctl_kind_t kind, int id) {
    pthread_mutex_lock(&mux);
    bool h = owner.kind == kind && owner.id == id;
    pthread_mutex_unlock(&mux);
    return h;
}

void ctl_get(ctl_info_t *out) {
    pthread_mutex_lock(&mux);
    *out = owner;
    pthread_mutex_unlock(&mux);
}

uint32_t ctl_version(void) {
    return atomic_load(&version);
}

void ctl_injected(void) {
    atomic_store(&injected_ms, now_ms());
}

void ctl_local_input(void) {
    if (now_ms() - atomic_load(&injected_ms) < INJECT_MS)
        return;
    pthread_mutex_lock(&mux);
    if (owner.kind != CTL_LOCAL)
        set_owner(CTL_LOCAL, 0, "");
    pthread_mutex_unlock(&mux);
}
