/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of radio.c's side of the X6200's WR12 (#134): what the test moves and reads around the real
 *  src/radio.c (fakes.c). See the Makefile.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "cfg/cfg_api.h"

/* The settings as the settings manager holds them (the defaults: VFO A on 7.074 MHz in use, B on 14.074 MHz, PWR
 * 5 W, VOX on, KEY Train off, WR12 on HOST off, the transverters off). A setting set notifies its observers in the
 * order they subscribed in, when it changed; the dial (cfg_fg_freq) and the other VFO's frequency (cfg_bg_freq) are
 * computed from the VFOs and the VFO in use, bound to them before radio.c subscribes, as the settings manager does */
void fake_cfg_init(void);
void set_i(ParamInt *p, int32_t v);         /* a band key, a memory, the switches: the setting itself */
void set_f(ParamFloat *p, float v);
void knob(int32_t freq);                    /* the knob: through the dial, cfg_fg_freq (its reverse sets the VFO) */

/* The GUI thread's queue (scheduler_put and the rest), run until it is empty; how many ran */
int gui_run(void);

/* The test's clock (get_time), moved on by hand */
void clock_add(uint64_t ms);

/* The screen's messages (msg_schedule_text_fmt and the rest): one with this text since said_clear */
bool said(const char *text);
void said_clear(void);

/* The base as radio.c left it, through the real hw_stub.c (hw_vfo_set_freq, hw_set_i and hw_set_f wrapped). After
 * every write, and at every wr12_want and the WR12 going out: with the VFO in use on the WR12's IF, or the WR12 out
 * of its bypass or asked out of it, the power at 0 W, VOX off, KEY Train on, neither PTT nor the modem set and the
 * base's own TX outside its band table shut (open TX at 0); with the dial where TX is refused (receive only, outside
 * the amateur bands with TX outside them off), the open TX at 0 or the power at 0 W: the hold covers the moment until
 * the dial has settled and the shut goes (#176). Else a violation (the first one's words kept). Where the open TX
 * must be 1, and Birdie Cal's shut, the cases look themselves: it goes once the dial has settled. The CW key's pin
 * (hw_pin_set wrapped) is set from another thread in one case: atomic */
typedef struct {
    int32_t vfo[2];
    int     cur;
    float   pwr;
    bool    vox, train, iq, ptt, modem;
    atomic_bool key;        /* the GUI's CW key */
    int     open_tx;        /* HW_OPEN_TX: 1 as x6200_control_init leaves it (the test may say 0) */
    int     open_tx_writes; /* tries to write it, those that failed too */
    int     open_tx_fails;  /* the test's: so many of the next tries fail, nothing reaches the base */
    int     open_tx_at_vfo; /* the open TX when a VFO last went somewhere */
    int     open_tx_at_cur; /* and when the VFO in use last changed */
    int     open_tx_at_key; /* and when PTT, the modem or the CW key last went down */
    float   pwr_at_cur;     /* the power when the VFO in use last changed */
    int     violations;
    char    first[200];
} base_t;

extern base_t base;

/* The stand-in WR12 driver (wr12.h): what radio.c asked last (wr12_want), and the state the test gives it */
typedef struct {
    bool    on, rx;
    int32_t hz;
    int     n;              /* calls */
} want_t;

extern want_t want;

extern int fake_errors;     /* the test asked the stand-ins for what the real ones could not do */

void wr12_found(void);      /* set up and in its bypass (wr12_ready), radio.c told as the driver tells it */
void wr12_went_out(void);   /* AT+BYPASS=0 answered: out of its bypass; only while radio.c asks for it */
void wr12_back_in(void);    /* AT+BYPASS=1 answered: in its bypass again, TX may go */
void wr12_lost(void);       /* let go (unplugged, not answering): not ready, out of its bypass as it was */
void wr12_gone(void);       /* unplugged GONE_MS: not out of its bypass any more */
void fake_start_out(void);  /* before radio_start: maybe out of its bypass, as after a crash (/tmp/wr12-out) */

/* The CW Trainer open or closed (cw_trainer_on), which radio.c is not told of */
void fake_trainer(bool open);

/* radio_thread.c: one turn of the radio thread at the base's open TX; the base in TX by itself (the hand mic's PTT)
 * or back in RX; a write of the open TX on its way, as open_tx_send has it (the radio lock held, open_tx_busy set),
 * and its end */
void radio_thread_turn(void);
void base_keys_itself(bool tx);
void open_tx_write_begin(void);
void open_tx_write_end(void);
