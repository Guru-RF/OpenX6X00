/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the X6200's start in src/hw/x6200/hw_x6200.c (#53): register 32 at 0x0a, as Xiegu's
 *  firmware sets it, with the real control library on a base of the test's (see the Makefile). In order: the start
 *  (hw_control_init): the library's whole block with 0 there, then register 32 alone at 0x0a, once, after the base's
 *  host commands and the block, the library's copy of it at 0x0a and the line in the log; the keepalive's block
 *  (hw_resend_all) with 0x0a; the CW keyer's and the QSK time's registers after it (33, 34) set, and the block after
 *  that still with 0x0a; the library's init failing (the base's info not read), nothing written, and the next init
 *  writing it again after its own block. Then the base's TX outside its band table (HW_OPEN_TX, #134), byte 280 of
 *  the calibration block: the shut before radio.c's bias (the block's as read at the start), asked again with nothing
 *  sent, open and shut with the bias sent; left alone with a bias neither sent nor the start's, two reads that differ,
 *  byte 280 at 2; a write the base does not take, then sent again; the base having it as asked already. Every time
 *  the rest of the block as it was.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <aether_radio/x6200_control/control.h>
#include <aether_radio/x6200_control/low/flow.h>
#include <aether_radio/x6200_control/low/gpio.h>

#include "hw/hw.h"

#define LOG         "hw_x6200_test.log"
#define FD          100             /* the bus's descriptor */
#define BASE_ADDR   0x72
#define REG32_ADDR  (x6200_reg_32 * 4)
#define BLOCK_LEN   (2 + (x6200_last + 1) * 4)

static FILE *out;
static int   fails, checks;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    checks++;
    if (!ok) {
        fails++;
        fprintf(out, "FAIL ");
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fprintf(out, "\n");
    }
}

/* ---- The base: what it was sent, by address; what it answers ---------------------------------------------------- */

static uint8_t  sent[0x10000];
static int      xfers;              /* writes to the base, numbered */
static int      blocks, block_at;   /* the whole register block: how many, the last one's number */
static uint32_t block_reg32;        /* register 32 in the last block */
static int      reg32s, reg32_at;   /* register 32 written alone: how many, the last one's number */
static int      host_at;            /* the last host command (0xfffe) */
static int      info_fails;         /* reads of the base's info to fail: the library's init fails then */
static int      unknown;            /* transfers or files the base has nothing for */

/* The calibration block (#134): read at 0x100, written back whole at 0xf000 (the library at its init, HW_OPEN_TX) */
#define CAL_LEN     507
#define CAL_OPEN_TX 280
static uint8_t  cal_blk[CAL_LEN];
static int      cal_writes;         /* writes of the block */
static int      cal_reads;
static bool     cal_flaky;          /* every second read with one byte changed: two reads differ */
static bool     cal_deaf;           /* writes of the block not taken: the read back differs */

static void cal_init(void) {
    for (int i = 0; i < CAL_LEN; i++) {
        cal_blk[i] = (uint8_t)(i * 7);              /* the factory calibration, the test's */
    }
    cal_blk[CAL_OPEN_TX] = 0;                       /* a cold start: shut, as Xiegu's firmware leaves it */
    cal_blk[281] = 0x20; cal_blk[282] = 0x0d;       /* the bias: 3360 */
    cal_blk[283] = 0xec; cal_blk[284] = 0x0e;       /* 3820 */
}

static void cal_bias(uint16_t drive, uint16_t final) {
    cal_blk[281] = drive & 0xff; cal_blk[282] = drive >> 8;
    cal_blk[283] = final & 0xff; cal_blk[284] = final >> 8;
}

static uint32_t reg_sent(unsigned addr) {
    uint32_t v;

    memcpy(&v, sent + addr, sizeof(v));
    return v;
}

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);

int __wrap_open(const char *path, int flags, ...) {
    va_list ap;
    int     mode;

    if (!strcmp(path, "/dev/i2c-0")) {
        return FD;
    }
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    return __real_open(path, flags, mode);
}

int __wrap_close(int fd) {
    return fd == FD ? 0 : __real_close(fd);
}

int __wrap_usleep(useconds_t us) {
    (void)us;
    return 0;
}

static int base_read(unsigned addr, uint8_t *buf, unsigned len) {
    memset(buf, 0, len);
    if (addr == 0x2000 && len == 1) {
        buf[0] = 1;                                 /* ready */
    } else if (addr == 0 && len >= 0x80) {
        if (info_fails > 0) {
            info_fails--;
            errno = EIO;
            return -1;
        }
        strcpy((char *)buf + 0x41, "V1.0.8");
    } else if (addr == 0x100 && len == CAL_LEN) {
        memcpy(buf, cal_blk, CAL_LEN);
        if (cal_flaky && ++cal_reads % 2 == 0) {
            buf[100] ^= 0x10;
        }
    } else {
        unknown++;
        errno = EIO;
        return -1;
    }
    return 0;
}

int __wrap_ioctl(int fd, unsigned long req, ...) {
    va_list                     ap;
    struct i2c_rdwr_ioctl_data *d;

    va_start(ap, req);
    d = va_arg(ap, struct i2c_rdwr_ioctl_data *);
    va_end(ap);
    if (fd != FD || req != I2C_RDWR || d->nmsgs < 1 || d->nmsgs > 2 || d->msgs[0].addr != BASE_ADDR
        || d->msgs[0].len < 2) {
        unknown++;
        errno = EINVAL;
        return -1;
    }
    struct i2c_msg *m    = d->msgs;
    unsigned        addr = (unsigned)m[0].buf[0] << 8 | m[0].buf[1];

    if (d->nmsgs == 2) {
        return base_read(addr, m[1].buf, m[1].len);
    }
    unsigned len = m[0].len - 2;

    if (addr + len > sizeof(sent)) {
        unknown++;
        errno = EINVAL;
        return -1;
    }
    memcpy(sent + addr, m[0].buf + 2, len);
    xfers++;
    if (addr == 0 && m[0].len == BLOCK_LEN) {
        blocks++;
        block_at    = xfers;
        block_reg32 = reg_sent(REG32_ADDR);
    } else if (addr == REG32_ADDR && len == 4) {
        reg32s++;
        reg32_at = xfers;
    } else if (addr == 0xfffe) {
        host_at = xfers;
    } else if (addr == 0xf000 && len == CAL_LEN) {
        cal_writes++;
        if (!cal_deaf) {
            memcpy(cal_blk, m[0].buf + 2, CAL_LEN);
        }
    }
    return 0;
}

/* ---- The library's pins and packet line, which the start does not use ---------------------------------------- */

int x6200_pin_wifi, x6200_pin_usb, x6200_pin_light, x6200_pin_morse_key, x6200_pin_bb_reset;
static int stand_ins;

bool x6200_gpio_init() { stand_ins++; return false; }
void x6200_gpio_set(int pin, int value) { (void)pin; (void)value; stand_ins++; }
bool x6200_flow_init() { stand_ins++; return false; }
bool x6200_flow_restart() { stand_ins++; return false; }
bool x6200_flow_read(x6200_flow_t *pack) { (void)pack; stand_ins++; return false; }

/* ---- The log ------------------------------------------------------------------------------------------------- */

static int log_lines(const char *line) {
    FILE *f = fopen(LOG, "r");
    char  buf[256];
    int   n = 0;

    if (!f) {
        return -1;
    }
    while (fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\n")] = 0;
        n += !strcmp(buf, line);
    }
    fclose(f);
    return n;
}

#define LINE "base: register 32 = 0x0a, as Xiegu's firmware"

/* ---- The base's TX outside its band table (HW_OPEN_TX, #134) ------------------------------------------------- */

#define SAY "hw_x6200: TX outside the base's band table "

static uint8_t cal_snap[CAL_LEN];

/* The block as it was, but byte 280 and the bias */
static bool cal_kept(void) {
    return !memcmp(cal_blk, cal_snap, CAL_OPEN_TX) && !memcmp(cal_blk + 285, cal_snap + 285, CAL_LEN - 285);
}

/* One HW_OPEN_TX: what hw_set_i answers, the block's writes it made, byte 280 after it, the line it logged */
static void open_tx(const char *what, int v, bool ok, int writes, int byte, const char *line) {
    int before = cal_writes, lines = log_lines(line);

    check(hw_set_i(HW_OPEN_TX, v) == ok, "%s: hw_set_i(HW_OPEN_TX, %d) did not answer %d", what, v, ok);
    check(cal_writes - before == writes && cal_blk[CAL_OPEN_TX] == byte && cal_kept(),
          "%s: %d writes of the block, byte 280 %u, the rest %s (%d, %d, kept)", what, cal_writes - before,
          cal_blk[CAL_OPEN_TX], cal_kept() ? "kept" : "changed", writes, byte);
    check(!line[0] || log_lines(line) == lines + 1, "%s: \"%s\" not in the log", what, line);
}

static void open_tx_cases(void) {
    memcpy(cal_snap, cal_blk, CAL_LEN);
    check(cal_blk[CAL_OPEN_TX] == 1, "open TX: byte 280 %u after the library's init (1)", cal_blk[CAL_OPEN_TX]);

    /* A GUI started with the WR12 maybe out of its bypass: the shut before radio.c sends the bias (the library's copy
     * of register 41 at 0 from its init), the block's bias as read at the start */
    check(x6200_control_get(x6200_biasdrive_biasfinal) == 0, "open TX: the library's bias not 0 before radio.c's");
    open_tx("shut before the bias", 0, true, 1, 0, SAY "shut");
    open_tx("shut again", 0, true, 0, 0, "");

    /* radio.c's bias sent (#173): the block's still the start's, or the one sent, both known */
    hw_set_i(HW_BIAS_DRIVE, 3326);
    hw_set_i(HW_BIAS_FINAL, 3843);
    check(x6200_control_get(x6200_biasdrive_biasfinal) == (3843u << 16 | 3326u), "open TX: the library's bias 0x%08x",
          x6200_control_get(x6200_biasdrive_biasfinal));
    open_tx("open, the start's bias in the block", 1, true, 1, 1, SAY "opened");
    cal_bias(3326, 3843);
    open_tx("shut, the bias sent in the block", 0, true, 1, 0, SAY "shut");

    /* A bias neither sent nor the start's: left alone, tried again at the next ask, as long as it lasts */
    cal_bias(1234, 3843);
    open_tx("an unknown bias", 1, false, 0, 0, SAY "not opened: PA bias not a known one");
    open_tx("an unknown bias, again", 1, false, 0, 0, SAY "not opened: PA bias not a known one");
    cal_bias(3326, 3843);
    open_tx("the bias back", 1, true, 1, 1, SAY "opened");

    /* Two reads that differ, byte 280 neither 0 nor 1: left alone */
    cal_flaky = true;
    open_tx("two reads differ", 0, false, 0, 1, SAY "not shut: two reads differ");
    cal_flaky = false;
    cal_blk[CAL_OPEN_TX] = 2;
    open_tx("byte 280 at 2", 0, false, 0, 2, SAY "not shut: byte 280 neither 0 nor 1");
    cal_blk[CAL_OPEN_TX] = 1;

    /* A write the base does not take: not read back as sent, then sent again at the next ask */
    cal_deaf = true;
    open_tx("a write not taken", 0, false, 1, 1, SAY "not shut: not read back as sent");
    cal_deaf = false;
    open_tx("sent again", 0, true, 1, 0, SAY "shut");

    /* Not known (a refusal), the base having it as asked already: nothing written */
    cal_flaky = true;
    open_tx("not known", 1, false, 0, 0, SAY "not opened: two reads differ");
    cal_flaky = false;
    cal_blk[CAL_OPEN_TX] = 1;
    open_tx("open already", 1, true, 0, 1, SAY "open already");
}

int main(void) {
    /* The results on stdout; what the code under test prints (the library on stdout, ours on stderr) in the log */
    out = fdopen(dup(STDOUT_FILENO), "w");
    if (!out || !freopen(LOG, "w", stderr)) return 1;
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    dup2(STDERR_FILENO, STDOUT_FILENO);

    /* The start: the library's block with 0 there, register 32 alone at 0x0a after the host commands and the block,
     * once; the library's copy at 0x0a; one line in the log */
    cal_init();
    check(hw_control_init(), "the start: hw_control_init failed");
    check(blocks == 1 && block_reg32 == 0, "the start: %d blocks, register 32 at 0x%x in the library's own (1, 0)",
          blocks, block_reg32);
    check(reg32s == 1 && reg_sent(REG32_ADDR) == 0x0a, "the start: register 32 written %d times, 0x%x (once, 0x0a)",
          reg32s, reg_sent(REG32_ADDR));
    check(host_at && host_at < block_at && block_at < reg32_at,
          "the start: host commands at transfer %d, the block at %d, register 32 at %d (in that order)", host_at,
          block_at, reg32_at);
    check(x6200_control_get(x6200_reg_32) == 0x0a, "the start: the library's copy of register 32 is 0x%x",
          x6200_control_get(x6200_reg_32));
    check(log_lines(LINE) == 1, "the start: \"%s\" in the log %d times (once)", LINE, log_lines(LINE));

    /* The keepalive's block every second: 0x0a in it */
    check(hw_resend_all(), "the keepalive: hw_resend_all failed");
    check(blocks == 2 && block_reg32 == 0x0a, "the keepalive: %d blocks, register 32 at 0x%x in the last (2, 0x0a)",
          blocks, block_reg32);

    /* The CW keyer's register (33) and the QSK time's (34) after it set: register 32 not written again,
     * and still 0x0a in the next block */
    hw_set_i(HW_KEY_TONE, 700);
    hw_set_i(HW_QSK_TIME, 50);
    check(hw_resend_all(), "the settings: hw_resend_all failed");
    check(reg32s == 1 && blocks == 3 && block_reg32 == 0x0a,
          "the settings: register 32 written %d times, %d blocks, 0x%x in the last (1, 3, 0x0a)", reg32s, blocks,
          block_reg32);
    check((reg_sent(x6200_ks_km_kimb_cwtone_cwvol_cwtrain * 4) >> 12 & 0x7ff) == 700
              && (reg_sent(x6200_qsktime_kr * 4) & 0xffff) == 50,
          "the settings: the keyer's tone %u Hz and the QSK time %u ms in the block (700, 50)",
          reg_sent(x6200_ks_km_kimb_cwtone_cwvol_cwtrain * 4) >> 12 & 0x7ff, reg_sent(x6200_qsktime_kr * 4) & 0xffff);

    /* The library's init failing (radio.c tries again): nothing written; the next one writes it after its block */
    info_fails = 1;
    int before = xfers;
    check(!hw_control_init(), "a failed init: hw_control_init succeeded");
    check(xfers == before && reg32s == 1, "a failed init: %d transfers, register 32 written %d times (0, 1)",
          xfers - before, reg32s);
    check(hw_control_init(), "the init after it: hw_control_init failed");
    check(blocks == 4 && block_reg32 == 0 && reg32s == 2 && block_at < reg32_at && reg_sent(REG32_ADDR) == 0x0a,
          "the init after it: %d blocks, 0x%x in the library's, register 32 written %d times, 0x%x after it "
          "(4, 0, 2, 0x0a)", blocks, block_reg32, reg32s, reg_sent(REG32_ADDR));
    check(hw_resend_all() && block_reg32 == 0x0a, "the init after it: 0x%x in the keepalive's block", block_reg32);
    check(log_lines(LINE) == 2, "the init after it: the line in the log %d times (twice)", log_lines(LINE));

    open_tx_cases();

    check(!unknown && !stand_ins, "%d transfers the base has nothing for, %d calls of the pins or the packet line",
          unknown, stand_ins);
    fprintf(out, "%d checks, %d failed\n", checks, fails);
    fclose(out);
    if (!fails) remove(LOG);
    return fails != 0;
}
