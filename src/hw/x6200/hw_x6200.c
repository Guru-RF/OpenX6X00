/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200 behind hw.h: Aether Radio's X6200 control library (the base over its
 *  serial link and I2C, the GPIO pins), the base's register layout and its packet. The X6200's
 *  base computes the spectrum itself and sends 512 dB bins instead of IQ samples (HW_CAP_PSD),
 *  and it reports the POWER key in the packet. Built from hw/x6100/hw_x6100.c.
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU (the flow decoding, the init sequence)
 *  Copyright (c) 2026 RF.Guru
 */

#define _GNU_SOURCE
#include "../hw.h"
#include "../../base_guard.h"
#include "../../pa_bias.h"

#include <aether_radio/x6200_control/control.h>
#include <aether_radio/x6200_control/low/flow.h>
#include <aether_radio/x6200_control/low/gpio.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The GUI's enums (radio_types.h) are the library's numbers, except the modes: the X6200 has
 * SAM between AM and NFM (lib_mode() below) */
#define SAME(a, b) _Static_assert((int)(a) == (int)(b), #a " is not " #b)
SAME(RADIO_VFO_A, X6200_VFO_A);          SAME(RADIO_VFO_B, X6200_VFO_B);
SAME(radio_mode_lsb, x6200_mode_lsb);    SAME(radio_mode_lsb_dig, x6200_mode_lsb_dig);
SAME(radio_mode_usb, x6200_mode_usb);    SAME(radio_mode_usb_dig, x6200_mode_usb_dig);
SAME(radio_mode_cw, x6200_mode_cw);      SAME(radio_mode_cwr, x6200_mode_cwr);
SAME(radio_mode_am, x6200_mode_am);
SAME(radio_agc_off, x6200_agc_off);      SAME(radio_agc_slow, x6200_agc_slow);
SAME(radio_agc_fast, x6200_agc_fast);    SAME(radio_agc_auto, x6200_agc_auto);
SAME(radio_att_off, x6200_att_off);      SAME(radio_att_on, x6200_att_on);
SAME(radio_pre_off, x6200_pre_off);      SAME(radio_pre_on, x6200_pre_on);
SAME(radio_key_manual, x6200_key_manual); SAME(radio_key_auto_left, x6200_key_auto_left);
SAME(radio_key_auto_right, x6200_key_auto_right);
SAME(radio_iambic_a, x6200_iambic_a);    SAME(radio_iambic_b, x6200_iambic_b);
SAME(radio_mic_builtin, x6200_mic_builtin); SAME(radio_mic_handle, x6200_mic_handle);
SAME(radio_mic_auto, x6200_mic_auto);
SAME(RADIO_HKEY_SPCH, X6200_HKEY_SPCH);  SAME(RADIO_HKEY_TUNER, X6200_HKEY_TUNER);
SAME(RADIO_HKEY_XFC, X6200_HKEY_XFC);    SAME(RADIO_HKEY_UP, X6200_HKEY_UP);
SAME(RADIO_HKEY_DOWN, X6200_HKEY_DOWN);  SAME(RADIO_HKEY_VM, X6200_HKEY_VM);
SAME(RADIO_HKEY_NW, X6200_HKEY_NW);      SAME(RADIO_HKEY_F1, X6200_HKEY_F1);
SAME(RADIO_HKEY_F2, X6200_HKEY_F2);      SAME(RADIO_HKEY_1, X6200_HKEY_1);
SAME(RADIO_HKEY_2, X6200_HKEY_2);        SAME(RADIO_HKEY_3, X6200_HKEY_3);
SAME(RADIO_HKEY_4, X6200_HKEY_4);        SAME(RADIO_HKEY_5, X6200_HKEY_5);
SAME(RADIO_HKEY_6, X6200_HKEY_6);        SAME(RADIO_HKEY_7, X6200_HKEY_7);
SAME(RADIO_HKEY_8, X6200_HKEY_8);        SAME(RADIO_HKEY_9, X6200_HKEY_9);
SAME(RADIO_HKEY_DOT, X6200_HKEY_DOT);    SAME(RADIO_HKEY_0, X6200_HKEY_0);
SAME(RADIO_HKEY_CE, X6200_HKEY_CE);      SAME(RADIO_HKEY_MODE, X6200_HKEY_MODE);
SAME(RADIO_HKEY_FIL, X6200_HKEY_FIL);    SAME(RADIO_HKEY_GENE, X6200_HKEY_GENE);
SAME(RADIO_HKEY_FINP, X6200_HKEY_FINP);
#undef SAME

/* The GUI numbers SAM and WFM after NFM, the base has SAM before it */
static x6200_mode_t lib_mode(radio_mode_t m) {
    switch (m) {
        case radio_mode_nfm:    return x6200_mode_nfm;
        case radio_mode_sam:    return x6200_mode_sam;
        case radio_mode_wfm:    return x6200_mode_wfm;
        default:                return (x6200_mode_t)m;
    }
}

/* Bits of x6200_sple_atue_trx that key the radio from the GUI */
#define TX_BITS (x6200_iptt | x6200_modem_trx | x6200_atu_tune | x6200_swrscan_trx | x6200_calibration_trx)
/* And the one that has the base take the WR12's IF with I and Q swapped (#134), which the control library has no name
 * or setter for */
#define IQ_SWAP 0x800000

#define PSD_BINS    512     /* dB bins in a packet, the lowest frequency first */
#define PSD_OFFSET  126.0f  /* bin value 126 is 0 dBm: the stock X6200 GUI's constant */
#define PSD_SPAN_HZ 384000  /* the bins at decimation 0; halved per step (the stock GUI's FFT_FULL_WIDTH) */
#define PSD_MAX_DEC 3       /* the stock GUI goes no further (48 kHz); what the base does beyond is unknown */
#define SMETER_CORR 4       /* dB the stock GUI adds to the base's S-meter reading */

static x6200_flow_t *pack;
static float         psd_db[PSD_BINS];
static uint8_t       fft_dec;       /* the base's spectrum decimation as last set */
static char          ver_str[32] = "?";
static radio_base_ver_t ver;

/* The notch: the GUI sets on/off and auto apart, the base takes one mode */
static bool dnf_on, dnf_auto;

/* ---- The base lent to base_flash (hw_base_hold) -------------------------------------------- */

/* While base_flash (tools/base_update/base_flash.c, from dialog_base_update.c) writes the base's firmware, nothing of
 * the GUI's may reach it: no packet read from /dev/ttyS1 (the bootloader's answers are base_flash's), no write on the
 * I2C bus through the library's own entry below or its others (the whole block, the power-off, the notch and zoom),
 * no reset or key line moved. radio.c also keeps its thread out of hw_flow_read meanwhile (radio_base_hold) */
static atomic_bool held;

void hw_base_hold(bool on) {
    atomic_store(&held, on);
}

/* ---- The library's writes, watched (base_guard.h) ---------------------------------------- */

#define TRIES       3
#define RETRY_US    2000

static bool (*lib_cmd)(x6200_cmd_enum_t cmd, uint32_t arg);

__attribute__((constructor)) static void find_lib_cmd(void) {
    lib_cmd = (bool (*)(x6200_cmd_enum_t, uint32_t))dlsym(RTLD_NEXT, "x6200_control_cmd");
    if (!lib_cmd) {
        fprintf(stderr, "hw_x6200: x6200_control_cmd not found in the control library\n");
        abort();
    }
}

/* The library's own name: its setters call it through the PLT, so every write lands here (see
 * hw_x6100.c and base_guard.h for the why) */
bool x6200_control_cmd(x6200_cmd_enum_t cmd, uint32_t arg) {
    uint32_t prev = x6200_control_get(cmd);
    int      fails = 0;
    bool     ok;

    if (atomic_load(&held)) {
        return false;
    }

    while (!(ok = lib_cmd(cmd, arg)) && ++fails < TRIES) {
        usleep(RETRY_US);
    }
    if (fails) {
        base_guard_write_failed(cmd, ok);
    }
    if (cmd == x6200_sple_atue_trx && (prev & TX_BITS) && !(arg & TX_BITS)) {
        base_guard_unkeyed();
    }
    return ok;
}

/* ---- Init ----------------------------------------------------------------------------- */

bool hw_init(void) {
    if (!x6200_gpio_init()) {
        return false;
    }
    x6200_gpio_set(x6200_pin_morse_key, 1);     /* Morse key off */
    if (!x6200_flow_init()) {
        return false;
    }
    pack = malloc(sizeof(x6200_flow_t));
    return pack != NULL;
}

/* ---- The calibration block ---------------------------------------------------------------- */

#define BASE_I2C        "/dev/i2c-0"    /* the base on the bus and address the library uses */
#define BASE_ADDR       0x72
#define BASE_READY      0x2000          /* not 0 once the base is ready: x6200_control_init waits for it */
#define BASE_CAL        0x100           /* the calibration block */
#define BASE_CAL_WRITE  0xf000          /* the same block written back whole, as x6200_control_init does */
#define BASE_CAL_OPEN_TX 280            /* in it, 1: the base keys outside its own band table (HW_OPEN_TX) */
#define BASE_WAIT_S     60

static uint8_t cal[PA_BIAS_BLOCK_LEN];
static bool    cal_read;

/* A read as the library's own: the register's address high byte first, then the bytes, in one transfer */
static bool base_read(int fd, uint16_t reg, uint8_t *buf, uint16_t len) {
    uint8_t addr[2] = { reg >> 8, reg & 0xff };
    struct i2c_msg msgs[] = {
        { .addr = BASE_ADDR, .flags = 0, .len = sizeof(addr), .buf = addr },
        { .addr = BASE_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf },
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };

    return ioctl(fd, I2C_RDWR, &rdwr) >= 0;
}

/* A write of the same kind: the register's address high byte first, then the bytes, in one transfer */
static bool base_write(int fd, uint16_t reg, const uint8_t *buf, uint16_t len) {
    uint8_t        msg[2 + PA_BIAS_BLOCK_LEN];
    struct i2c_msg m = { .addr = BASE_ADDR, .flags = 0, .len = 2 + len, .buf = msg };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = &m, .nmsgs = 1 };

    if (len > PA_BIAS_BLOCK_LEN) {
        return false;
    }
    msg[0] = reg >> 8;
    msg[1] = reg & 0xff;
    memcpy(msg + 2, buf, len);
    return ioctl(fd, I2C_RDWR, &rdwr) >= 0;
}

/* The block as the base has it before x6200_control_init writes the whole register block, register 41 (the PA bias)
 * at 0 in it, and before radio.c sends the bias (#173). The library reads the block in there too but hands it to
 * nobody; a patch for that would need a new card image, as an update brings the GUI and not the library. So it is
 * read here first, on a descriptor of its own, as the library reads it: nothing else talks to the base yet (the
 * radio thread starts later), and the kernel does each transfer whole. It waits for the base as the library would,
 * a minute at most; the library then waits on by itself. The read gets the writes' tries (x6200_control_cmd above):
 * one failed transfer would leave this run on the fixed bias, and the base would keep that in the block for every
 * later start until it loses power */
static void cal_block_read(void) {
    int     fd = open(BASE_I2C, O_RDWR);
    uint8_t ready = 0;
    int     fails = 0;

    if (fd < 0) {
        fprintf(stderr, "hw_x6200: calibration block: %s: %s\n", BASE_I2C, strerror(errno));
        return;
    }
    for (int i = 0; i < BASE_WAIT_S && !(base_read(fd, BASE_READY, &ready, 1) && ready); i++) {
        sleep(1);
    }
    while (ready && !(cal_read = base_read(fd, BASE_CAL, cal, sizeof(cal))) && ++fails < TRIES) {
        usleep(RETRY_US);
    }
    if (!ready) {
        fprintf(stderr, "hw_x6200: calibration block not read (base not ready)\n");
    } else if (!cal_read) {
        fprintf(stderr, "hw_x6200: calibration block not read (%d tries)\n", TRIES);
    } else if (fails) {
        fprintf(stderr, "hw_x6200: calibration block read at try %d\n", fails + 1);
    }
    close(fd);
}

const uint8_t *hw_cal_block(size_t *len) {
    *len = cal_read ? sizeof(cal) : 0;
    return cal_read ? cal : NULL;
}

/* The base's TX outside its own band table (HW_OPEN_TX, #134, #176): byte 280 of the block, 1 open. x6200_control_init
 * sends the block back at 0xf000 with it at 1, at the start only; Xiegu's app sends it at 0 (fullband-tx=disable in
 * /etc/xgradio/xgradio.conf), and the base then keys only inside the band table of the same block (160 to 6 m; 60 m
 * 5.332-5.405 MHz), whatever the VFO and the TX bits say. The base keeps it in RAM: a cold start reads 0 again, and
 * saving the block is host_cmd 0x8005, which nothing here sends. Read from the base at every call: radio.c keeps
 * what the base has, and asks only for a change, or when it does not know.
 * The block holds the radio's factory calibration and the PA bias (#173), and this write comes in the middle of a
 * run, so only a block known good goes back: read twice and the same, byte 280 a 0 or a 1 (the library's own check).
 * As asked already: nothing written, but after a write of ours in doubt that left the factory calibration not as
 * before (put back, cal_fix). Else the bias in it a known one, its factory calibration as this run found it (ref
 * below), and after the write read back as sent, written once more if not (OPEN_TX_WRITES). Anything else leaves the
 * block alone. The known bias: what went to register 41 last (the library's copy, drive low, final high: 0 from
 * x6200_control_init until radio.c sends this radio's), or the block's as read at the start (cal). The base copies
 * register 41 into the block once radio.c has sent it (ea4iln's bench), not the library's 0, and maybe not at once:
 * a write just after finds the start's. radio.c writes from its radio thread, which starts after the bias is sent,
 * or onto the WR12's IF, once the WR12 is set up, later still: so pa_bias_send stays at the end of radio_start, where
 * the start was tried on a radio.
 * Under the radio lock, like every write of the library (radio.c), each transfer one I2C_RDWR the kernel runs whole:
 * about 0.2 s for the four of them at 100 kHz, 0.4 s at most when it is written twice. One line in the base log for
 * each call, its outcome (that log takes one line a second), but not the same line twice in a row: a write that
 * keeps failing the same way, tried again every minute (radio.c), has it once, and stderr each time */
#define OPEN_TX_WRITES  2               /* the block written at most so often in a call: once more when not read back */
#define OPEN_TX_REPAIRS 2               /* calls that put the factory calibration back after a write in doubt */
#define READ_BACK_US    10000           /* between the reads after a write, in case the base takes it a moment later */

/* The block's factory calibration as this run found it (ref): the block as read at the start (cal), or without that
 * the first two reads here. Byte 0 and bytes 280-284 are not looked at: a base 1.0.8 changed byte 0 by itself (10 to
 * 100 and back, #176), and the GUI sets the others (byte 280 here, the bias through register 41, which the base copies
 * in). Not as ref, the block is left alone and the bytes logged; found the same way again at the next call (odd),
 * with no write of ours in doubt, it is taken for ref from then on: a byte the base changes by itself, or a read at
 * the start gone wrong. Writing back what the base has changes nothing of its calibration, where refusing for the
 * whole run would leave the base keying where the GUI refuses, or keying nothing where it lets TX go, until the base
 * loses power. In doubt (a write of ours that failed, or was not read back as sent or not at all: it may have reached
 * the base wrongly) a block not as ref is ours to put right: ref's factory calibration written back, as the block had
 * it in the reads before the first such write (cal_fix), until a call finds it as ref again. Switching the radio off
 * and on brings the base's own back, battery in and supply connected too (ea4iln, #173: the whole block as at a cold
 * start) */
static uint8_t ref[PA_BIAS_BLOCK_LEN], odd[PA_BIAS_BLOCK_LEN];
static bool    ref_taken, odd_seen, in_doubt;
static int     repairs;                 /* calls that put it back since the last one found as ref */

static bool cal_read_tries(int fd, uint8_t *buf) {
    int fails = 0;

    while (!base_read(fd, BASE_CAL, buf, PA_BIAS_BLOCK_LEN) && ++fails < TRIES) {
        usleep(RETRY_US);
    }
    return fails < TRIES;
}

static bool cal_write_tries(int fd, const uint8_t *buf) {
    int fails = 0;

    while (!base_write(fd, BASE_CAL_WRITE, buf, PA_BIAS_BLOCK_LEN) && ++fails < TRIES) {
        usleep(RETRY_US);
    }
    return fails < TRIES;
}

/* What cal_diff looks at: the whole block; what the base keeps of a write (not byte 0 nor the bias, the base's own:
 * ref above); the factory calibration (not byte 280 either) */
enum { CAL_ALL, CAL_KEPT, CAL_FACTORY };

/* Whether cal_diff looks at byte i, for what */
static bool cal_counts(int i, int what) {
    bool own = i == 0 || (i >= PA_BIAS_DRIVE_AT && i < PA_BIAS_FINAL_AT + 2);

    return what == CAL_ALL || (!own && (what == CAL_KEPT || i != BASE_CAL_OPEN_TX));
}

/* The bytes two blocks differ in, the first one in *at */
static int cal_diff(const uint8_t *a, const uint8_t *b, int what, int *at) {
    int n = 0;

    for (int i = 0; i < PA_BIAS_BLOCK_LEN; i++) {
        if (!cal_counts(i, what)) {
            continue;
        }
        if (a[i] != b[i] && !n++) {
            *at = i;
        }
    }
    return n;
}

static uint32_t bias_in(const uint8_t *block) {
    return block[PA_BIAS_DRIVE_AT] | block[PA_BIAS_DRIVE_AT + 1] << 8 | (uint32_t)block[PA_BIAS_FINAL_AT] << 16
         | (uint32_t)block[PA_BIAS_FINAL_AT + 1] << 24;
}

/* A write of ours in doubt (ref above), and the block's factory calibration not as ref since: the bytes in *n, the
 * first one in *at. Found as ref, no longer in doubt */
static bool cal_spoilt(const uint8_t *blk, int *n, int *at) {
    if (!in_doubt) {
        return false;
    }
    if (!(*n = cal_diff(ref, blk, CAL_FACTORY, at))) {
        in_doubt = odd_seen = false;
        repairs = 0;
        return false;
    }
    return true;
}

/* blk spoilt (cal_spoilt, n bytes, the first at): ref's factory calibration put in it again, byte 0 and the bias left
 * as the base has them (cal_put then writes it). OPEN_TX_REPAIRS calls at most until one finds it as ref: a base that
 * takes a byte wrongly at every write is not written again and again, the block left alone then. False: not, why
 * says where; note: put back */
static bool cal_fix(uint8_t *blk, int n, int at, char *why, char *note, size_t size) {
    if (repairs >= OPEN_TX_REPAIRS) {
        snprintf(why, size,
                 "the calibration not as at the start at byte %d, 0x%02x then, 0x%02x now (%d in all), after a write "
                 "in doubt, put back %d times", at, ref[at], blk[at], n, repairs);
        return false;
    }
    repairs++;
    snprintf(note, size, "the calibration put back as at the start, byte %d 0x%02x, 0x%02x before (%d in all)", at,
             ref[at], blk[at], n);
    for (int i = 0; i < PA_BIAS_BLOCK_LEN; i++) {
        if (cal_counts(i, CAL_FACTORY)) {
            blk[i] = ref[i];
        }
    }
    return true;
}

/* The block's factory calibration as ref has it (above), no write of ours in doubt (cal_spoilt first). False: not,
 * why says where; note: ref taken anew */
static bool cal_as_ref(const uint8_t *blk, char *why, char *note, size_t size) {
    int n, at = 0, odd_at = 0;

    if (!ref_taken) {
        memcpy(ref, cal_read ? cal : blk, sizeof(ref));
        ref_taken = true;
    }
    if (!(n = cal_diff(ref, blk, CAL_FACTORY, &at))) {
        odd_seen = false;
        return true;
    }
    if (odd_seen && !cal_diff(odd, blk, CAL_FACTORY, &odd_at)) {
        snprintf(note, size, "the calibration taken as it is now, byte %d 0x%02x at the start, 0x%02x now (%d in all)",
                 at, ref[at], blk[at], n);
        memcpy(ref, blk, sizeof(ref));
        odd_seen = false;
        return true;
    }
    memcpy(odd, blk, sizeof(odd));
    odd_seen = true;
    snprintf(why, size, "the calibration not as at the start at byte %d, 0x%02x then, 0x%02x now (%d in all)", at,
             ref[at], blk[at], n);
    return false;
}

/* After a write: the block read back, again READ_BACK_US later when that read is not as sent, as the base keeps it
 * (CAL_KEPT). The bytes not as sent in the last read, the first one in *at and its value in *got; -1 none read */
static int read_back(int fd, const uint8_t *blk, int *at, uint8_t *got) {
    static uint8_t back[PA_BIAS_BLOCK_LEN];
    int            n = -1;

    for (int i = 0; i < 2 && n; i++) {
        if (i) {
            usleep(READ_BACK_US);
        }
        if (cal_read_tries(fd, back) && (n = cal_diff(blk, back, CAL_KEPT, at))) {
            *got = back[*at];
        }
    }
    return n;
}

/* blk, byte 280 changed, to the base and read back; written again while it comes back not as sent, OPEN_TX_WRITES in
 * all: a byte the base took wrongly is put back so, and one it changes by itself after a write is not fought for
 * long (byte 0 and the bias not at all: read_back). False: why says what went wrong, and the write is in doubt (ref
 * above): a failed transfer too, which may have reached the base in part */
static bool cal_put(int fd, const uint8_t *blk, char *why, char *note, size_t size) {
    uint8_t got = 0;
    int     n = 0, at = 0;

    for (int w = 1; w <= OPEN_TX_WRITES; w++) {
        if (!cal_write_tries(fd, blk)) {
            in_doubt = true;
            snprintf(why, size, "not written (%d tries)%s", TRIES, w > 1 ? " after one not read back as sent" : "");
            return false;
        }
        if ((n = read_back(fd, blk, &at, &got)) <= 0) {
            break;
        }
        if (w < OPEN_TX_WRITES) {
            size_t len = strlen(note);

            snprintf(note + len, size - len, "%swritten again, byte %d read back 0x%02x the first time (%d in all)",
                     len ? "; " : "", at, got, n);
        }
    }
    if (!n) {
        in_doubt = false;   /* as sent: the factory calibration as ref, or as cal_fix put it back */
        repairs = 0;
        return true;
    }
    in_doubt = true;
    if (n < 0) {
        snprintf(why, size, "written, not read back (%d tries, twice)", TRIES);
    } else {
        snprintf(why, size, "written %d times, not read back as sent at byte %d, 0x%02x sent, 0x%02x back (%d in all)",
                 OPEN_TX_WRITES, at, blk[at], got, n);
    }
    return false;
}

/* The call's line: on stderr, and in the base log unless it is the line before */
static void open_tx_say(bool on, const char *what, const char *why) {
    static char last[200];
    char        line[200];

    snprintf(line, sizeof(line), "open TX %d: %s%s%s", on, what, why[0] ? ": " : "", why);
    fprintf(stderr, "hw_x6200: TX outside the base's band table %s%s%s\n", what, why[0] ? ": " : "", why);
    if (strcmp(line, last)) {
        if (base_guard_log_free()) {
            snprintf(last, sizeof(last), "%s", line);   /* else the line is only counted there: again next time */
        }
        base_guard_log("%s", line);
    }
}

static bool open_tx_set(bool on) {
    static uint8_t blk[PA_BIAS_BLOCK_LEN], again[PA_BIAS_BLOCK_LEN];
    char           why[160] = "", note[160] = "";
    const char    *done = NULL;
    uint32_t       sent = x6200_control_get(x6200_biasdrive_biasfinal), bias;

    if (atomic_load(&held)) {
        return false;
    }
    int            fd = open(BASE_I2C, O_RDWR), n = 0, at = 0;
    bool           fix = false;

    if (fd < 0) {
        snprintf(why, sizeof(why), "%s: %s", BASE_I2C, strerror(errno));
    } else if (!cal_read_tries(fd, blk) || !cal_read_tries(fd, again)) {
        snprintf(why, sizeof(why), "not read (%d tries)", TRIES);
    } else if ((n = cal_diff(blk, again, CAL_ALL, &at))) {
        snprintf(why, sizeof(why), "two reads differ at byte %d (%d in all)", at, n);
    } else if (blk[BASE_CAL_OPEN_TX] > 1) {
        snprintf(why, sizeof(why), "byte 280 at %u, neither 0 nor 1", blk[BASE_CAL_OPEN_TX]);
    } else if (!(fix = cal_spoilt(blk, &n, &at)) && blk[BASE_CAL_OPEN_TX] == on) {
        done = on ? "open already" : "shut already";
    } else if ((bias = bias_in(blk)) != sent && !(cal_read && bias == bias_in(cal))) {
        snprintf(why, sizeof(why), "PA bias not a known one: %u/%u in the block, %u/%u sent, %u/%u at the start",
                 bias & 0xffff, bias >> 16, sent & 0xffff, sent >> 16, cal_read ? bias_in(cal) & 0xffff : 0,
                 cal_read ? bias_in(cal) >> 16 : 0);
    } else if (fix ? cal_fix(blk, n, at, why, note, sizeof(why)) : cal_as_ref(blk, why, note, sizeof(why))) {
        blk[BASE_CAL_OPEN_TX] = on;
        if (cal_put(fd, blk, why, note, sizeof(why))) {
            done = on ? "opened" : "shut";
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    open_tx_say(on, done ? done : on ? "not opened" : "not shut", done ? note : why);
    return done != NULL;
}

/* Register 32 as Xiegu's firmware sets it at the start: 0, then 0x0a, between the CW keyer's register and the QSK
 * time's (the library's copy of those writes, left out in its low/control.c); the library leaves it at 0. What it
 * does is not known, a TX timing value maybe: the X6200 sends a short RF pulse at the start of each transmission, well
 * above the steady level at low power, which another radio does not (#53) */
#define BASE_REG_32     0x0a

bool hw_control_init(void) {
    if (!cal_read) {
        cal_block_read();
    }
    if (!x6200_control_init()) {
        return false;
    }
    /* After the library's init, whose whole block has 0 there. Through x6200_control_cmd, so the library's copy of
     * the block keeps it: the block the keepalive sends every second has it too (hw_resend_all), and a failed write
     * goes out with the next one */
    x6200_control_cmd(x6200_reg_32, BASE_REG_32);
    fprintf(stderr, "base: register 32 = 0x%02x, as Xiegu's firmware\n", BASE_REG_32);
    return true;
}

void hw_bb_reset(void) {
    x6200_gpio_set(x6200_pin_bb_reset, 1);
    usleep(100000);
    x6200_gpio_set(x6200_pin_bb_reset, 0);
}

/* The base's info block: the library hands back its first 128 bytes; the firmware version is
 * the string at 0x41 (its own init prints it as "BASE fw version"). Kept as major.minor.patch
 * for the GUI, revision 0: the X6200's capabilities do not depend on it */
void hw_start(void) {
    const char *info = x6200_control_get_fw_version();
    const char *s    = info ? info + 0x41 : NULL;

    memset(&ver, 0, sizeof(ver));
    if (s && *s) {
        snprintf(ver_str, sizeof(ver_str), "%s", s);
        while (*s && !isdigit((unsigned char)*s)) {
            s++;
        }
        unsigned a = 0, b = 0, c = 0;
        if (sscanf(s, "%u.%u.%u", &a, &b, &c) >= 2) {
            ver = (radio_base_ver_t){a, b, c, 0};
        }
    }
}

radio_base_ver_t hw_base_ver(void) {
    return ver;
}

const char *hw_base_ver_str(void) {
    return ver_str;
}

/* The base narrows its spectrum for the zoom, finds the notch's carrier itself, sends the
 * spectrum ready-made, and has a compressor with a ratio. No IF shift, no TX calibration of
 * the X6100's kind, no CESSB, no TX filter. In CW and CW-R its spectrum stays centred on the
 * dial, and the tone rises as the dial goes up in both: a broadcast carrier on the dial sounded
 * at the key tone and sat in the middle of the base's spectrum, base 1.0.8 (#115) */
uint32_t hw_caps(void) {
    return HW_CAP_ZOOM | HW_CAP_DNF_AUTO | HW_CAP_PSD | HW_CAP_COMP | HW_CAP_TX_LEVEL | HW_CAP_SAM_WFM | HW_CAP_KEEPALIVE
         | HW_CAP_TX_NO_ALC | HW_CAP_CW_SPECTRUM_ON_DIAL | HW_CAP_CW_TONE_RISES;
}

/* ---- Settings ------------------------------------------------------------------------- */

/* The settings with a register of their own: compared before the write, read back by hw_get_i() */
static int reg_of(hw_param_t p) {
    switch (p) {
        case HW_RIT:            return x6200_rit;
        case HW_XIT:            return x6200_xit;
        case HW_AGC_TIME:       return x6200_agctime;
        case HW_ATU_NETWORK:    return x6200_atu_network;
        default:                return -1;
    }
}

int32_t hw_get_i(hw_param_t p) {
    int      reg = reg_of(p);
    uint32_t f;

    switch (p) {
        case HW_FILTER1_LOW:
            f = x6200_control_get(x6200_rxfilter);
            return (int16_t)(f >> 16);
        case HW_FILTER1_HIGH:
            f = x6200_control_get(x6200_rxfilter);
            return (int16_t)(f & 0xFFFF);
        default:
            return reg < 0 ? 0 : (int32_t)x6200_control_get((x6200_cmd_enum_t)reg);
    }
}

static void dnf_apply(void) {
    x6200_control_dnf_set(!dnf_on ? x6200_dnf_off : dnf_auto ? x6200_dnf_auto : x6200_dnf_manual);
}

bool hw_set_i(hw_param_t p, int32_t v) {
    int reg = reg_of(p);

    if (atomic_load(&held)) {
        return false;
    }
    if (reg >= 0) {
        if (x6200_control_get((x6200_cmd_enum_t)reg) == (uint32_t)v) {
            return false;
        }
        x6200_control_cmd((x6200_cmd_enum_t)reg, (uint32_t)v);
        return true;
    }
    switch (p) {
        case HW_VFO_CUR:        x6200_control_vfo_set((x6200_vfo_t)v); break;
        case HW_SPLIT:          x6200_control_split_set(v); break;
        case HW_RFG:            x6200_control_rfg_set(v); break;
        /* One squelch setting in the GUI, two on the base (voice and FM): both follow it */
        case HW_SQL:            x6200_control_sql_set(v); x6200_control_sql_fm_set(v); break;
        case HW_RXVOL:          x6200_control_rxvol_set(v); break;
        case HW_ATU_ENABLED:    x6200_control_atu_set(v); break;
        case HW_ATU_TUNE:       x6200_control_atu_tune(v); break;
        case HW_SWRSCAN:        x6200_control_swrscan_set(v); break;
        case HW_PTT:            x6200_control_ptt_set(v); break;
        case HW_MODEM:          x6200_control_modem_set(v); break;
        case HW_RECORD:         x6200_control_record_set(v); break;
        case HW_MIC:            x6200_control_mic_set((x6200_mic_sel_t)v); break;
        case HW_HMIC:           x6200_control_hmic_set(v); break;
        case HW_IMIC:           x6200_control_imic_set(v); break;
        case HW_MONI:           x6200_control_monitor_level_set(v); break;
        case HW_LINE_IN:        x6200_control_linein_set(v); break;
        case HW_LINE_OUT:       x6200_control_lineout_set(v); break;
        case HW_SPMODE:         x6200_control_spmode_set(v); break;
        case HW_CHARGER:        x6200_control_charger_set(v); break;
        case HW_BIAS_DRIVE:     x6200_control_bias_drive_set(v); break;
        case HW_BIAS_FINAL:     x6200_control_bias_final_set(v); break;
        /* The GUI asks for 100 kHz over 2^v; the base's steps are 384 kHz over 2^dec: one step
         * further than asked keeps the stream wider than the screen (192 kHz over 2^v), the GUI
         * takes the middle of it (hw_pack_t.psd_span) */
        case HW_FFT_DEC:
            fft_dec = v + 1 > PSD_MAX_DEC ? PSD_MAX_DEC : v + 1;
            x6200_control_fft_dec_set(fft_dec);
            break;
        /* One RX filter register: low and high halves. The second filter (the X6100's wider one
         * around the first) does not exist here */
        case HW_FILTER1_LOW:
            if (hw_get_i(HW_FILTER1_LOW) == v) return false;
            x6200_control_rx_filter_set_low(v);
            break;
        case HW_FILTER1_HIGH:
            if (hw_get_i(HW_FILTER1_HIGH) == v) return false;
            x6200_control_rx_filter_set_high(v);
            break;
        /* radio.c speaks the X6100's compressor: COMP_ON true is OFF there, and COMP_LEVEL is the ratio minus 2
         * (0-6). The X6200's bit is a plain on/off and its levels are off, 1:2, 1:4, 1:8 (x6200_comp_level_t;
         * gdyuldin's GUI sends the bit as it is): "Comp off" turned it on, and ratios over 1:5 went past the end */
        case HW_COMP_ON:        x6200_control_comp_set(!v); break;
        case HW_COMP_LEVEL:     x6200_control_comp_level_set(v + 2 <= 3 ? x6200_comp_1_2 : v + 2 <= 7 ? x6200_comp_1_4
                                                                                                  : x6200_comp_1_8);
                                break;
        case HW_VOX:            x6200_control_vox_set(v); break;
        case HW_VOX_GAIN:       x6200_control_vox_gain_set(v); break;
        case HW_VOX_AG:         x6200_control_vox_ag_set(v); break;
        case HW_VOX_DELAY:      x6200_control_vox_delay_set(v); break;
        case HW_KEY_TONE:       x6200_control_key_tone_set(v); break;
        case HW_KEY_SPEED:      x6200_control_key_speed_set(v); break;
        case HW_KEY_MODE:       x6200_control_key_mode_set((x6200_key_mode_t)v); break;
        case HW_IAMBIC_MODE:    x6200_control_iambic_mode_set((x6200_iambic_mode_t)v); break;
        case HW_KEY_VOL:        x6200_control_key_vol_set(v); break;
        case HW_KEY_TRAIN:      x6200_control_key_train_set(v); break;
        case HW_QSK_TIME:       x6200_control_qsk_time_set(v); break;
        case HW_AGC_HANG:       x6200_control_agc_hang_set(v); break;
        case HW_AGC_KNEE:       x6200_control_agc_knee_set(v); break;
        case HW_AGC_SLOPE:      x6200_control_agc_slope_set(v); break;
        case HW_DNF:            dnf_on = v; dnf_apply(); break;
        case HW_DNF_AUTO:       dnf_auto = v; dnf_apply(); break;
        case HW_DNF_CENTER:     x6200_control_dnf_center_set(v); break;
        case HW_DNF_WIDTH:      x6200_control_dnf_width_set(v); break;
        case HW_NB:             x6200_control_nb_set(v); break;
        case HW_NB_LEVEL:       x6200_control_nb_level_set(v); break;
        case HW_NB_WIDTH:       x6200_control_nb_width_set(v); break;
        case HW_NR:             x6200_control_nr_set(v); break;
        case HW_NR_LEVEL:       x6200_control_nr_level_set(v); break;
        /* Read and written back as the library's setters do with the bits beside it (radio.c holds its lock): the
         * library's copy of the register keeps it, and the keepalive sends that copy every second (hw_resend_all) */
        case HW_IQ_SWAP: {
            uint32_t r = x6200_control_get(x6200_sple_atue_trx);
            uint32_t n = v ? r | IQ_SWAP : r & ~(uint32_t)IQ_SWAP;

            if (n == r) return false;
            x6200_control_cmd(x6200_sple_atue_trx, n);
            break;
        }
        /* Not a register: byte 280 of the calibration block, read and written at each call (open_tx_set above) */
        case HW_OPEN_TX:        return open_tx_set(v != 0);
        /* The X6100 patched firmware's extras: this base has no register for them */
        case HW_TX_I_OFFSET:
        case HW_TX_Q_OFFSET:
        case HW_FILTER2_LOW:
        case HW_FILTER2_HIGH:
        case HW_IF_SHIFT_ON:
        case HW_IF_SHIFT_HZ:
        case HW_FM_EMP:
        case HW_TX_FILTER_LOW:
        case HW_TX_FILTER_HIGH:
        case HW_CESSB:
        case HW_CW_PEAK:
        case HW_CW_PEAK_Q:
            return false;
        default:
            fprintf(stderr, "hw_x6200: hw_set_i(%d) is not an integer setting\n", p);
            return false;
    }
    return true;
}

bool hw_set_f(hw_param_t p, float v) {
    if (atomic_load(&held)) {
        return false;
    }
    switch (p) {
        case HW_TXPWR_F:            x6200_control_txpwr_set(v); break;
        case HW_KEY_RATIO_F:        x6200_control_key_ratio_set(v); break;
        /* Not on this base */
        case HW_ADC_DAC_GAIN_F:
        case HW_DAC_GAIN_F:
        case HW_COMP_THRESHOLD_F:
        case HW_COMP_MAKEUP_F:
        case HW_CESSB_POWER_UP_F:
            return false;
        default:
            fprintf(stderr, "hw_x6200: hw_set_f(%d) is not a float setting\n", p);
            return false;
    }
    return true;
}

bool hw_vfo_set_freq(radio_vfo_t vfo, int32_t hz) {
    if (atomic_load(&held)) {
        return false;
    }
    if (x6200_control_get(vfo == RADIO_VFO_A ? x6200_vfoa_freq : x6200_vfob_freq) == (uint32_t)hz) {
        return false;
    }
    x6200_control_vfo_freq_set((x6200_vfo_t)vfo, hz);
    return true;
}

bool hw_vfo_set_mode(radio_vfo_t vfo, radio_mode_t mode) {
    x6200_mode_t m = lib_mode(mode);

    if (atomic_load(&held)) {
        return false;
    }
    if (x6200_control_get(vfo == RADIO_VFO_A ? x6200_vfoa_mode : x6200_vfob_mode) == (uint32_t)m) {
        return false;
    }
    x6200_control_vfo_mode_set((x6200_vfo_t)vfo, m);
    return true;
}

void hw_vfo_set_agc(radio_vfo_t vfo, radio_agc_t agc) {
    if (atomic_load(&held)) return;
    x6200_control_vfo_agc_set((x6200_vfo_t)vfo, (x6200_agc_t)agc);
}

void hw_vfo_set_att(radio_vfo_t vfo, bool on) {
    if (atomic_load(&held)) return;
    x6200_control_vfo_att_set((x6200_vfo_t)vfo, on ? x6200_att_on : x6200_att_off);
}

void hw_vfo_set_pre(radio_vfo_t vfo, bool on) {
    if (atomic_load(&held)) return;
    x6200_control_vfo_pre_set((x6200_vfo_t)vfo, on ? x6200_pre_on : x6200_pre_off);
}

void hw_poweroff(void) {
    if (atomic_load(&held)) return;     /* not in the middle of a firmware write: the base switches itself off after */
    x6200_control_poweroff();
}

/* The library reopens the bus and tries once more on its own, and reports nothing but errno */
bool hw_resend_all(void) {
    if (atomic_load(&held)) {
        return false;
    }
    errno = 0;
    x6200_control_idle();
    return errno == 0;
}

bool hw_tx_keyed_by_gui(void) {
    return (x6200_control_get(x6200_sple_atue_trx) & TX_BITS) != 0;
}

void hw_pin_set(hw_pin_t pin, bool on) {
    if (atomic_load(&held) && (pin == HW_PIN_BB_RESET || pin == HW_PIN_MORSE_KEY)) {
        return;
    }
    switch (pin) {
        case HW_PIN_WIFI:       x6200_gpio_set(x6200_pin_wifi, on ? 1 : 0); break;
        case HW_PIN_LIGHT:      x6200_gpio_set(x6200_pin_light, on ? 1 : 0); break;
        case HW_PIN_USB_CAT:    x6200_gpio_set(x6200_pin_usb, on ? 1 : 0); break;
        case HW_PIN_MORSE_KEY:  x6200_gpio_set(x6200_pin_morse_key, on ? 0 : 1); break;  /* active low */
        case HW_PIN_BB_RESET:   x6200_gpio_set(x6200_pin_bb_reset, on ? 1 : 0); break;
    }
}

/* ---- The packet stream -------------------------------------------------------------------- */

/* 552 bytes every ~35 ms: 512 spectrum bins as unsigned dB (126 = 0 dBm), the flags, the
 * meters (dbm is the S-meter, negated), the hand mic's key */
bool hw_flow_read(hw_pack_t *p) {
    if (atomic_load(&held) || !x6200_flow_read(pack)) {
        return false;
    }
    for (size_t i = 0; i < PSD_BINS; i++) {
        psd_db[i] = (float)pack->samples[i] - PSD_OFFSET;
    }
    p->samples    = NULL;
    p->n_samples  = 0;
    p->psd        = psd_db;
    p->n_psd      = PSD_BINS;
    p->psd_span   = PSD_SPAN_HZ >> fft_dec;
    p->smeter_dbm = -(int16_t)pack->dbm + SMETER_CORR;
    p->lo_freq    = 0;
    p->fft_dec    = 0;
    p->vary_freq  = false;
    p->tx         = pack->flag.tx;
    p->atu_status = pack->flag.atu_status;
    p->charging   = pack->flag.charging;
    p->vext_flag  = pack->flag.vext;
    p->bat_hot    = pack->flag.battery_high_temp;
    p->bat_cold   = pack->flag.battery_low_temp;
    p->pa_hot     = pack->flag.pa_high_temp;
    p->dcin_low   = pack->flag.dc_in_too_low;
    p->power_key  = pack->flag.power_key;
    p->bat_missing = pack->flag.battery_not_found;
    p->tx_power   = pack->tx_power;
    p->vswr       = pack->vswr;
    p->alc_level  = pack->alc_level;
    p->vext       = pack->vext;
    p->vbat       = pack->vbat;
    p->batcap     = pack->batcap;
    p->atu_params = pack->atu_params;
    p->hkey       = pack->hkey;
    return true;
}

void hw_flow_restart(void) {
    if (atomic_load(&held)) return;     /* it opens /dev/ttyS1 again and sets it up: base_flash has it */
    x6200_flow_restart();
}
