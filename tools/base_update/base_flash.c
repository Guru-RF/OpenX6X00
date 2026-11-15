/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The X6200's base firmware (one of Xiegu's .xgf files) written from the radio's Linux side, as Xiegu's
 *  own app 1.0.8 writes it from MENU > SYSTEM > UPGRADE, step for step, with its timeouts, its messages and its error
 *  handling (x6200_ui_v100: UiFwUpgrade::upgradeFirmware, XMWorker::slot_start, send_cmd, Xmodem::sendFile and
 *  waitRespond; the port as uart_create and BaseComm::start set it up; the I2C write as BaseCommIIC::iicWrite):
 *
 *   0. The port: /dev/ttyS1 opened read-write without a controlling terminal (blocking, as Xiegu's), raw 8N1 at
 *      1152000 baud with VMIN and VTIME 0 (every read returns at once), both queues flushed. Xiegu's app runs
 *      "civ stop" and pauses its own reading of the port; here the GUI is stopped (tools/base_update/flash.sh).
 *   1. The base's application asked for its bootloader: a 'c' on the port and host command 0x800a over I2C
 *      (/dev/i2c-0, address 0x72, register 0xfffe high byte first, the command low byte first: FF FE 0A 80), both
 *      again every 100 turns of a loop that sleeps 1 ms and reads up to 10240 bytes each turn. A read of one byte that
 *      is 'Y' or 'y' counts; more than 10 of them end the loop. 10000 empty reads ("[ERR] Wait IAP respond timeout")
 *      or 10000 others ("[ERR] Clear rx buffer timeout") end it too, and Xiegu's app goes on all the same: only the
 *      step after decides. The results of the write and of the I2C command are not looked at.
 *   2. send_cmd 'c' for 'y' ("[ERR] IAP failed, connect timeout": nothing erased yet), then 'r' for 'g' (erase the
 *      application: "[ERR] IAP failed, can't erase flash"), then 'p' for 'g' ("[ERR] IAP failed, can't start
 *      xmodem"). send_cmd: the byte written (result not looked at), up to 5000 one-byte reads 1 ms apart for the
 *      answer, other bytes skipped; written again up to 10 more times ("[ERR] no respond to cmd %c, retry %d").
 *   3. The file as it is, by XMODEM-1K with a CRC (the bootloader decrypts it itself, so no key is needed here or
 *      anywhere): a 2-byte read whose second byte is 'C' or 'c' within 10 s ("1k xmodem timeout.", "1k xmodem error"),
 *      then 1024-byte packets (STX, number from 1, its complement, the data with the last block padded with 0xFF,
 *      CRC-16/XMODEM high byte first), each answered by a 1-byte read within 10 s: ACK the next, CAN "1k xmodem
 *      transfer canceled.", NAK "1k xmodem bad packet", anything else or nothing "1k xmodem error". No retries. Then
 *      EOT, answered by ACK ("1k xmodem send finished."). A failure here is "[ERR] xmodem send file failed".
 *   4. Every way out ends the same in Xiegu's app ("finished.", "civ start", its reading of the port resumed); it
 *      neither resets the base nor sends it a command after. Here the exit status says which step failed.
 *
 *  Only on an X6200 or an X6100 (its device tree says xiegu,x6200 or xiegu,x6100; on any other radio nothing is sent
 *  to the base), only the files listed in known[] for that radio (by SHA-256: Xiegu's X6200 releases 1.0.3 to 1.0.8,
 *  Xiegu's X6100 V1.1.8 and V1.1.9, RF.Guru's X6100 test build with --test-ok) or, with --unknown-ok, one that passes
 *  the checks below and is not named for another radio, and only with nothing else holding /dev/ttyS1 or /dev/i2c-0
 *  (the GUI stopped first, tools/base_update/flash.sh, or lending the base, --held-by). From step 1 on, the hang-up,
 *  interrupt and terminate signals are ignored: a lost SSH session must not end the transfer half way.
 *
 *  The bootloader (the MCU's first sectors) is not erased by 'r': after a failed transfer the same run again should
 *  find it and write the application again, as Xiegu's app would. That is how Xiegu's design reads; it has not been
 *  tried on an X6200.
 *
 *      base_flash --check FILE     reads only: the file, who holds the base's port and bus, the base's version over
 *                                  I2C. It never asks the base for its bootloader.
 *      base_flash --flash FILE --yes
 *      base_flash --flash FILE --yes --recover
 *                                  after a transfer that failed past the erase: the base has no application to answer
 *                                  on I2C, so that check is left out; the bootloader is asked as before.
 *      base_flash --flash FILE --yes --held-by PID
 *                                  started by the GUI (dialog_base_update.c), which keeps /dev/ttyS1 and /dev/i2c-0 open
 *                                  but has lent us the base (radio_base_hold): its descriptors are let be.
 *      base_flash --identify FILE  the file only: "xiegu-base 1.0.8 SHA256" when it is one of Xiegu's known here.
 *      ... --unknown-ok            a file not known here taken (identified as "unknown") when it passes the checks
 *                                  an .xgf can be given without its key: a multiple of 16 bytes, 16 KiB to 512 KiB,
 *                                  and not a plain ARM image. With a warning; the screen and flash.sh ask twice.
 *
 *  The X6100 (its device tree says xiegu,x6100) is written another way into the same kind of bootloader, as Franco
 *  Venturi's x6100-base-experiments base-flasher does it (LGPL-2.1-or-later; it wrote the bench X6100's base on 24
 *  October 2026, r13 and back to Xiegu's V1.1.8), which agrees with what was read of the X6100's own app: no 'c', the
 *  erase 'r' given 60 s and 'p' 1 s, each answered by any byte that is not 0:
 *
 *   1. The port at 1500000 baud (the bootloader's rate on the X6100), its input flushed.
 *   2. Its key line (GPIO 203 on /dev/gpiochip1, the bootloader's strap: low asks for the bootloader) low and its reset
 *      line (204) high, 100 ms, the input flushed, the reset line low and let go, the port opened again.
 *   3. The bootloader's greeting, 'A' or 'a': up to 51 one-second reads 100 ms apart. The key line up and let go,
 *      greeting or not. No greeting: the base's application started again (the key line up, a 100 ms reset), exit 2.
 *   4. 'r' (erase, 60 s), 'p' (1 s), each written once; then two bytes within 10 s whose second is 'C' or 'c', and the
 *      file by XMODEM-1K as above. The bootloader starts the new application by itself (about a minute on 24
 *      October; 4 s on 15 November); if it does not, the GUI's own start resets a base that does not answer. For
 *      a minute after, its version text read over I2C mostly came without its first three bytes: the GUI reads it
 *      again.
 *
 *      base_flash --flash FILE --yes --dry-run
 *                                  the X6100's only: steps 1 to 3, then the base's application started again. Nothing
 *                                  is erased or written: it shows the way into the bootloader works on this radio.
 *      ... --test-ok               a test build listed in known[] (RF.Guru's X6100-Baseband) taken; never without it.
 *
 *  Exit status: 0 done, 1 refused before anything was sent to the base, 2 failed before the erase (the base's
 *  application is still there; switching the radio off and on brings it back), 3 failed after the erase (the base has
 *  no application: run the same again, and do not switch the radio off).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef UART                        /* other paths only for a test against a simulated bootloader */
#define UART        "/dev/ttyS1"
#endif
#ifndef I2C_BUS
#define I2C_BUS     "/dev/i2c-0"
#endif
#define BASE_ADDR   0x72
#define REG_VERSION 0x0000          /* 128 bytes, the base's info block (x6200_control_get_fw_version) */
#define REG_READY   0x2000          /* not 0 once the base is up (x6200_control_init waits for it) */
#define REG_HOST    0xfffe          /* host commands (x6200_control_host_cmd) */
#define HOST_IAP    0x800a          /* Xiegu's app: the base's application to its bootloader */
#ifndef BACK_WAIT_S
#define BACK_WAIT_S 60              /* after the write, seconds the base's new application is looked for on I2C */
#endif
#ifndef DT_COMPAT                   /* the radio's model, from its device tree (a test points it at a file of its own) */
#define DT_COMPAT   "/proc/device-tree/compatible"
#endif
#ifndef GPIO_CHIP                   /* the X6100's: the base's bootloader strap (its Morse key line) and its reset */
#define GPIO_CHIP   "/dev/gpiochip1"
#endif
#define LINE_STRAP  203             /* low while the base comes out of reset: it stays in its bootloader (key: active low) */
#define LINE_RESET  204             /* high holds the base in reset */
#ifndef GREET_TRIES                 /* the X6100 bootloader's greeting: reads of 1 s, 100 ms apart (fewer in a test) */
#define GREET_TRIES 51
#endif
#ifndef ERASE_MS                    /* the X6100's erase answered within (shorter in a test) */
#define ERASE_MS    60000
#endif

typedef enum { MODEL_NONE, MODEL_X6100, MODEL_X6200 } model_t;
static model_t model;           /* from the device tree; MODEL_NONE on a computer (--identify) */

#define SOH_STX 0x02
#define EOT     0x04
#define ACK     0x06
#define NAK     0x15
#define CAN     0x18

enum { EXIT_DONE = 0, EXIT_REFUSED = 1, EXIT_BEFORE_ERASE = 2, EXIT_AFTER_ERASE = 3 };

/* Xiegu's base files, as their update images bring them to /usr/firmware, each for one radio; and test builds, taken
 * only with --test-ok */
static const struct {
    const char *name, *sha256;
    long        size;
    const char *ver;
    model_t     model;
    bool        test;
} known[] = {     /* the X6200's as rootfs/usr/share/x6100/base_releases.txt has them (tools/base_update/test checks) */
    {"x6200_mcu_20260820141208.xgf (base 1.0.8, Aug 2026)",
     "9a19f97a5ad9e141a3663d222f984073bf0b18b38d4662604372a0036f9daa23", 524288, "1.0.8", MODEL_X6200, false},
    {"x6200_mcu_20260116092100.xgf (base 1.0.7, Jan 2026: Xiegu's factory build, not on xiegu.eu)",
     "9baa46b824d9b20ddb41c6c3b64dde4a3dfbc3bdcd0a0cbc600506cb7a1e2690", 401024, "1.0.7", MODEL_X6200, false},
    {"x6200_mcu_20250916150221.xgf (base 1.0.7, Sep 2025: xiegu.eu's update)",
     "d58382c210738fee5261e58819aef9f3829030bf0997d53511dab3355a8fb4e2", 524288, "1.0.7", MODEL_X6200, false},
    {"x6200_mcu_20250111134652.xgf (base 1.0.6, Jan 2025)",
     "dca90387b70d9550448006310a69ea65625bec87f402c10df789deeb13ff12f3", 524288, "1.0.6", MODEL_X6200, false},
    {"x6200_mcu_20240924203603.xgf (base 1.0.3, Sep 2024)",
     "61e59a52e7c007c427d31f9de3b296e5568225efb93a1fd8df46028d48ffc3f1", 524288, "1.0.3", MODEL_X6200, false},
    /* The X6100's: V1.1.9 as Xiegu's own system on the radio's internal memory has it (/usr/firmware), V1.1.8 from
     * Xiegu's update of September 2024 (APP 1.1.9, BASE 1.1.8: "V1.1.8 Sep 15 2024,15:22:37") */
    {"X6100_BBFW_V1.1.9_251127001.xgf (base V1.1.9, Nov 2025)",
     "87a3e96ea21abf16a14b0fb771aad19a9e0bf8c267f93ec71a09d38e59c25737", 392048, "V1.1.9", MODEL_X6100, false},
    {"X6100_BBFW_V1.1.8_240915003.xgf (base V1.1.8, Sep 2024)",
     "8516de0bdcb4a3dcbba1605a482250d17acad2402f89f5d3719db33d6c48348d", 391584, "V1.1.8", MODEL_X6100, false},
    /* RF.Guru's X6100-Baseband v0.1.0-alpha.2: Xiegu's V1.1.9 with CESSB, a compressor and a TX filter, its build
     * text "Nov 27 2025,tx1a2-test". TEST ONLY: not validated on a radio */
    {"X6100_BBFW_V1.1.9_251127001_tx1_alpha2_TEST.xgf (RF.Guru X6100-Baseband v0.1.0-alpha.2, a TEST build)",
     "7e1278064af5eda40dd8a5029c10ecd2ec6d4232880b274d04f9e70cf7b9881e", 405504, "V1.1.9-tx1a2-test", MODEL_X6100,
     true},
};
static const char *image_ver;   /* the loaded file's base version, known[].ver, or "unknown" (--unknown-ok) */
static char        image_sha[65];   /* and its SHA-256 */
static bool        unknown_ok;  /* --unknown-ok: a file not in known[] taken when it passes the checks below */
static bool        test_ok;     /* --test-ok: a test build in known[] taken */

static const char *model_name(model_t m) {
    return m == MODEL_X6100 ? "X6100" : m == MODEL_X6200 ? "X6200" : "radio";
}

#define UNKNOWN_MIN 16384       /* an .xgf not known here: AES blocks (a multiple of 16), no more than the 512 KiB */
#define UNKNOWN_MAX 524288      /* Xiegu's own files fill, and not a plain ARM image (the bootloader decrypts it) */

static struct timespec t0;

static void say(const char *fmt, ...) {
    struct timespec t;
    va_list         ap;

    clock_gettime(CLOCK_MONOTONIC, &t);
    printf("[%7.3f] ", (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void msleep(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};

    while (nanosleep(&t, &t) < 0 && errno == EINTR) {
    }
}

/* ---- SHA-256 (FIPS 180-4), for telling Xiegu's files ---------------------------------------------------------- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, k;

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]        = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

static void sha256_hex(const uint8_t *data, size_t n, char out[65]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t  tail[128] = {0};
    size_t   full = n / 64 * 64, rest = n - full, tn = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;

    for (size_t i = 0; i < full; i += 64)
        sha256_block(h, data + i);
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++)
        tail[tn - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tn; i += 64)
        sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++)
        sprintf(out + 8 * i, "%08x", h[i]);
}

/* ---- The file ------------------------------------------------------------------------------------------------- */

static uint8_t *image;
static long     image_len;

static int load(const char *path) {
    FILE       *f = fopen(path, "rb");
    char        hex[65];
    const char *name = NULL;

    if (!f) {
        say("%s: %s", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    image_len = ftell(f);
    rewind(f);
    if (image_len <= 0 || image_len > 1024 * 1024) {
        say("%s: %ld bytes, not a base image", path, image_len);
        fclose(f);
        return -1;
    }
    image = malloc(image_len);
    if (!image || fread(image, 1, image_len, f) != (size_t)image_len) {
        say("%s: cannot read it", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    sha256_hex(image, image_len, hex);
    memcpy(image_sha, hex, sizeof(image_sha));
    say("file %s: %ld bytes, %ld packets of 1024, sha256 %s", path, image_len, (image_len + 1023) / 1024, hex);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (image_len != known[i].size || strcmp(hex, known[i].sha256))
            continue;
        if (model != MODEL_NONE && known[i].model != model) {
            say("refused: it is the %s's %s, and this radio is an %s", model_name(known[i].model), known[i].name,
                model_name(model));
            return -1;
        }
        if (known[i].test && !test_ok) {
            say("refused: it is %s, for testing only: written only with --test-ok", known[i].name);
            return -1;
        }
        image_ver = known[i].ver;
        name      = known[i].name;
        break;
    }
    if (name) {
        say("it is %s%s", strstr(name, "RF.Guru") ? "" : "Xiegu's ", name);
        return 0;
    }
    /* Another radio's base, by its name: never written to this one's, --unknown-ok or not (its content can't tell:
     * Xiegu encrypts the X6100's, the X6200's and the G90's with the same key) */
    {
        static const char *for_x6200[] = {"x6100", "6100_", "g90", "g106", "x5105"};
        static const char *for_x6100[] = {"x6200", "6200_", "g90", "g106", "x5105"};
        const char       **other       = model == MODEL_X6100 ? for_x6100 : for_x6200;
        const char        *b           = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        char               low[256];
        size_t             i;

        for (i = 0; b[i] && i < sizeof(low) - 1; i++) low[i] = (char)tolower((unsigned char)b[i]);
        low[i] = '\0';
        for (i = 0; i < sizeof(for_x6200) / sizeof(for_x6200[0]); i++)
            if (strstr(low, other[i])) {
                say("refused: %s is named for another radio (%s): only the %s's base firmware is written here", b,
                    other[i], model == MODEL_X6100 ? "X6100" : "X6200");
                return -1;
            }
    }
    if (!unknown_ok) {
        say("not one of the %s base files known here: it is not written (--unknown-ok takes one that passes the "
            "checks)", model == MODEL_X6100 ? "X6100" : "X6200");
        return -1;
    }
    /* Unknown: what can be told of an encrypted file. A plain ARM image starts with its stack pointer (in SRAM,
     * 0x2000xxxx) and its reset vector (in flash, 0x08xxxxxx): sent as it is, the bootloader would decrypt it into
     * garbage */
    uint32_t sp = image[0] | image[1] << 8 | image[2] << 16 | (uint32_t)image[3] << 24;
    uint32_t pc = image[4] | image[5] << 8 | image[6] << 16 | (uint32_t)image[7] << 24;

    if (image_len % 16 || image_len < UNKNOWN_MIN || image_len > UNKNOWN_MAX) {
        say("refused: %ld bytes is not an .xgf as Xiegu makes them (a multiple of 16, %d to %d bytes)", image_len,
            UNKNOWN_MIN, UNKNOWN_MAX);
        return -1;
    }
    if ((sp & 0xfff00000u) == 0x20000000u && (pc & 0xff000000u) == 0x08000000u) {
        say("refused: it starts as a plain ARM image (stack 0x%08x, reset 0x%08x), not an encrypted .xgf", sp, pc);
        return -1;
    }
    image_ver = "unknown";
    say("WARNING: not one of Xiegu's files known here; it passes the checks (size, AES blocks, not a plain image), "
        "but what it holds can't be told. Written only because --unknown-ok was given");
    return 0;
}

/* ---- Who else has the base's port or bus ---------------------------------------------------------------------- */

static int held_by = -1;    /* --held-by: the GUI that lent us the base (radio_base_hold) keeps its descriptors */

static int holders(const char *dev) {
    DIR           *proc = opendir("/proc");
    struct dirent *p;
    int            n = 0;

    if (!proc)
        return 0;
    while ((p = readdir(proc))) {
        char  path[600], link[300], comm[64] = "?";
        DIR  *fds;
        FILE *cf;

        if (p->d_name[0] < '0' || p->d_name[0] > '9' || atoi(p->d_name) == getpid())
            continue;
        snprintf(path, sizeof(path), "/proc/%s/fd", p->d_name);
        if (!(fds = opendir(path)))
            continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", p->d_name);
        if ((cf = fopen(path, "r"))) {
            if (fgets(comm, sizeof(comm), cf))
                comm[strcspn(comm, "\n")] = 0;
            fclose(cf);
        }
        for (struct dirent *fd; (fd = readdir(fds));) {
            ssize_t l;

            snprintf(path, sizeof(path), "/proc/%s/fd/%s", p->d_name, fd->d_name);
            l = readlink(path, link, sizeof(link) - 1);
            if (l > 0 && (link[l] = 0, !strcmp(link, dev))) {
                if (atoi(p->d_name) == held_by) {
                    say("%s is open in %s (pid %s), which has lent us the base", dev, comm, p->d_name);
                } else {
                    say("%s is open in %s (pid %s)", dev, comm, p->d_name);
                    n++;
                }
                break;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return n;
}

/* ---- I2C, as Aether's library does it: the register's address high byte first --------------------------------- */

static int i2c = -1;

static bool base_read(uint16_t reg, uint8_t *buf, uint16_t len) {
    uint8_t                    a[2] = {reg >> 8, reg & 0xff};
    struct i2c_msg             m[2] = {{BASE_ADDR, 0, 2, a}, {BASE_ADDR, I2C_M_RD, len, buf}};
    struct i2c_rdwr_ioctl_data d    = {m, 2};

    return ioctl(i2c, I2C_RDWR, &d) >= 0;
}

static bool host_cmd(uint16_t cmd) {
    uint8_t                    b[4] = {REG_HOST >> 8, REG_HOST & 0xff, cmd & 0xff, cmd >> 8};
    struct i2c_msg             m    = {BASE_ADDR, 0, sizeof(b), b};
    struct i2c_rdwr_ioctl_data d    = {&m, 1};

    return ioctl(i2c, I2C_RDWR, &d) >= 0;
}

/* The base's version text, printable characters only: on the X6200 the string at 0x41 of its 128-byte info block, as
 * src/hw/x6200/hw_x6200.c reads it ("BASE fw version"); on the X6100 the block's own start ("V1.1.8 Sep 15
 * 2024,15:22:37"), as the X6100's control library reads it. False when the base does not answer */
static bool base_version(char *out, size_t n) {
    uint8_t v[128];
    size_t  k = 0;

    memset(v, 0, sizeof(v));
    if (!base_read(REG_VERSION, v, sizeof(v)))
        return false;
    for (size_t i = model == MODEL_X6100 ? 0 : 0x41; i < sizeof(v) && v[i] && k + 1 < n; i++)
        out[k++] = (v[i] >= 0x20 && v[i] < 0x7f) ? (char)v[i] : '.';
    out[k] = 0;
    return true;
}

/* The base's application answers: the X6200's says it is ready (0x2000), the X6100's has its version text, "V..."
 * (the X6100 has no ready register) */
static bool base_up(char *v, size_t n, uint8_t *ready) {
    *ready = 0;
#ifdef SIM_BASE_VERSION                 /* a test's simulated base, which has no I2C: its application answers */
    snprintf(v, n, "%s", SIM_BASE_VERSION);
    *ready = 1;
    return true;
#endif
    if (model == MODEL_X6200)
        return base_read(REG_READY, ready, 1) && base_version(v, n);
    if (!base_version(v, n))
        return false;
    *ready = v[0] == 'V';
    return true;
}

/* ---- The serial port, as Xiegu's app has it ---------------------------------------------------------------- */

static int uart = -1;

/* uart_create's settings (cfsetispeed and cfsetospeed B1152000; iflag less IGNBRK BRKINT PARMRK ISTRIP INLCR IGNCR
 * ICRNL IXON; oflag less OPOST ONLCR; cflag CS8, less CSTOPB PARENB; lflag less ECHO ECHONL ICANON ISIG IEXTEN;
 * VTIME and VMIN 0), then BaseComm::start's open: read-write, no controlling terminal, both queues flushed. The X6100's
 * bootloader at 1500000. Not blocking, unlike Xiegu's: another opener of the port (a GUI started in the middle) setting
 * VMIN to 1 would otherwise turn every read here into a wait for ever, past every timeout. CREAD is added should the
 * port not have it: without it nothing at all is received */
static int uart_open(void) {
    struct termios t;
    speed_t        speed = model == MODEL_X6100 ? B1500000 : B1152000;

    uart = open(UART, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart < 0) {
        say("%s: %s", UART, strerror(errno));
        return -1;
    }
    if (tcgetattr(uart, &t) < 0) {
        say("%s: %s", UART, strerror(errno));
        return -1;
    }
    cfsetispeed(&t, speed);
    cfsetospeed(&t, speed);
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~(OPOST | ONLCR);
    t.c_cflag &= ~(CSIZE | CSTOPB | PARENB);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cc[VTIME] = 0;
    t.c_cc[VMIN]  = 0;
    if (!(t.c_cflag & CREAD)) {
        say("%s had CREAD off: on", UART);
        t.c_cflag |= CREAD;
    }
    if (tcsetattr(uart, TCSANOW, &t) < 0) {
        say("%s: error in uart parameters: %s", UART, strerror(errno));
        return -1;
    }
    tcflush(uart, TCIOFLUSH);
    return 0;
}

/* A write whose result Xiegu's app does not look at: said here when it fails, and nothing else */
static void uart_write(const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(uart, p, n);

        if (w > 0) {
            p += w, n -= w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && errno == EAGAIN) {        /* the port's queue full: wait for room, 10 s at most */
            struct pollfd pf = {uart, POLLOUT, 0};

            if (poll(&pf, 1, 10000) > 0)
                continue;
            say("%s: write: no room in 10 s", UART);
            return;
        } else {
            say("%s: write: %s", UART, w < 0 ? strerror(errno) : "nothing written");
            return;
        }
    }
}

/* Xmodem::waitRespond: reads of up to 1024 bytes, 1 ms apart; one of exactly n bytes whose last is not 0 gives that
 * byte; after about s seconds, 0 */
static uint8_t wait_respond(int s, int n) {
    uint8_t buf[1024];

    if (n > 1023)
        n = 1023;
    if (n < 1)
        n = 1;
    for (int turn = 0;;) {
        ssize_t r = read(uart, buf, sizeof(buf));

        if (r == n && buf[n - 1])
            return buf[n - 1];
        turn = (turn + 1) % 1000;
        if (turn == 0) {
            if (s == 0)
                return 0;
            s--;
        }
        usleep(1000);
    }
}

/* send_cmd: cmd written, then up to 5000 one-byte reads 1 ms apart for want (other bytes passed over); written again
 * up to 10 more times */
static bool send_cmd(uint8_t cmd, uint8_t want) {
    for (int retry = 10;; retry--) {
        uart_write(&cmd, 1);
        for (int left = 5000;;) {
            uint8_t b;

            if (read(uart, &b, 1) > 0 && b == want)
                return true;
            if (--left == 0)
                break;
            msleep(1);
        }
        say("[ERR] no respond to cmd %c, retry %d", cmd, retry);
        if (retry == 0)
            return false;
    }
}

/* ---- The X6100: its bootloader asked for with two GPIO lines ---------------------------------------------------- */

/* Up to n bytes within ms milliseconds, gathered over as many reads as it takes; how many came */
static size_t read_for(uint8_t *buf, size_t n, int ms) {
    struct timespec now, end;
    size_t          got = 0;

    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += ms / 1000;
    end.tv_nsec += (ms % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L)
        end.tv_sec++, end.tv_nsec -= 1000000000L;
    while (got < n) {
        struct pollfd pf = {uart, POLLIN, 0};
        long          left;
        ssize_t       r;

        clock_gettime(CLOCK_MONOTONIC, &now);
        left = (end.tv_sec - now.tv_sec) * 1000L + (end.tv_nsec - now.tv_nsec) / 1000000L;
        if (left <= 0)
            break;
        if (poll(&pf, 1, (int)left) < 0 && errno != EINTR)
            break;
        r = read(uart, buf + got, n - got);
        if (r > 0)
            got += (size_t)r;
        else if (r < 0 && errno != EAGAIN && errno != EINTR)
            break;
        else if (!(pf.revents & POLLIN))
            continue;
        else
            msleep(1);
    }
    return got;
}

/* One line of GPIO_CHIP as an output at v, held while the descriptor returned is open (closed: let go). A value is
 * changed by asking for the line again, as the way that worked on the radio did. In a test the lines are a log */
static int line_hold(unsigned line, int v) {
#ifdef GPIO_LOG
    FILE *f = fopen(GPIO_LOG, "a");

    if (f) {
        fprintf(f, "%u=%d\n", line, v);
        fclose(f);
    }
    return open(GPIO_CHIP, O_RDONLY | O_CLOEXEC);
#else
    struct gpio_v2_line_request r;
    int                         chip = open(GPIO_CHIP, O_RDWR | O_CLOEXEC);

    if (chip < 0) {
        say("%s: %s", GPIO_CHIP, strerror(errno));
        return -1;
    }
    memset(&r, 0, sizeof(r));
    r.offsets[0]                  = line;
    r.num_lines                   = 1;
    r.config.flags                = GPIO_V2_LINE_FLAG_OUTPUT;
    r.config.num_attrs            = 1;
    r.config.attrs[0].attr.id     = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    r.config.attrs[0].attr.values = v ? 1 : 0;
    r.config.attrs[0].mask        = 1;
    snprintf(r.consumer, sizeof(r.consumer), "base_flash");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &r) < 0) {
        say("%s line %u: %s", GPIO_CHIP, line, strerror(errno));
        close(chip);
        return -1;
    }
    close(chip);
    return r.fd;
#endif
}

static void line_free(int *fd, unsigned line) {
    if (*fd < 0)
        return;
    close(*fd);
    *fd = -1;
#ifdef GPIO_LOG
    FILE *f = fopen(GPIO_LOG, "a");

    if (f) {
        fprintf(f, "%u free\n", line);
        fclose(f);
    }
#else
    (void)line;
#endif
}

/* The line held again at v (let go first: the value changed by asking again) */
static bool line_to(int *fd, unsigned line, int v) {
    line_free(fd, line);
    *fd = line_hold(line, v);
    return *fd >= 0;
}

/* The base reset with its key line up: it starts its application (after a dry run, or a bootloader that did not
 * greet). True when both lines moved */
static bool x6100_start_app(void) {
    int  strap = -1, rst = -1;
    bool ok    = line_to(&strap, LINE_STRAP, 1) && line_to(&rst, LINE_RESET, 1);

    if (ok) {
        msleep(100);
        ok = line_to(&rst, LINE_RESET, 0);
    }
    line_free(&rst, LINE_RESET);
    line_free(&strap, LINE_STRAP);
    say(ok ? "the base reset with its key line up: its application starts" :
             "the base's lines could not be moved: switch the radio off and on to start its application");
    return ok;
}

/* Steps 1-3 for the X6100: 1 the bootloader greeted, 0 it did not (the base was reset into it), -1 the lines could not
 * be taken (the base was not touched) */
static int x6100_enter(void) {
    int     strap = -1, rst = -1, greeted = 0;
    uint8_t b = 0;

    tcflush(uart, TCIFLUSH);
    say("asking for the bootloader: key line (%u) low, the base held in reset (%u) for 100 ms", LINE_STRAP, LINE_RESET);
    if (!line_to(&strap, LINE_STRAP, 0)) {
        return -1;
    }
    if (!line_to(&rst, LINE_RESET, 1)) {
        line_to(&strap, LINE_STRAP, 1);
        line_free(&strap, LINE_STRAP);
        return -1;
    }
    msleep(100);
    tcflush(uart, TCIFLUSH);
    line_to(&rst, LINE_RESET, 0);
    line_free(&rst, LINE_RESET);
    /* Opened again: the application's stream at another rate leaves the port with framing and overrun errors */
    close(uart);
    if (uart_open() < 0) {
        line_to(&strap, LINE_STRAP, 1);
        line_free(&strap, LINE_STRAP);
        return 0;
    }
    for (int i = 0; i < GREET_TRIES && !greeted; i++) {
        if (read_for(&b, 1, 1000) == 1 && (b & 0xdf) == 'A') {
            greeted = 1;
            break;
        }
        msleep(100);
    }
    line_to(&strap, LINE_STRAP, 1);         /* up and let go, greeting or not */
    line_free(&strap, LINE_STRAP);
    if (greeted)
        say("[OK] Get IAP respond ('%c'): the bootloader runs", b);
    else
        say("[ERR] Wait IAP respond timeout: no greeting from the bootloader in %d tries", GREET_TRIES);
    return greeted;
}

/* A command byte written once, and the first byte that is not 0 within ms as its answer: the X6100's app does not look
 * at what it is. True when one came */
static bool x6100_cmd(uint8_t cmd, int ms) {
    struct timespec t, now;

    clock_gettime(CLOCK_MONOTONIC, &t);
    uart_write(&cmd, 1);
    for (;;) {
        uint8_t b = 0;
        long    left;

        clock_gettime(CLOCK_MONOTONIC, &now);
        left = ms - ((now.tv_sec - t.tv_sec) * 1000L + (now.tv_nsec - t.tv_nsec) / 1000000L);
        if (left <= 0)
            return false;
        if (read_for(&b, 1, (int)left) == 1 && b) {
            say("'%c' answered 0x%02x", cmd, b);
            return true;
        }
    }
}

/* The answer XMODEM waits for: Xmodem::waitRespond on the X6200 (a read of exactly n bytes), any n bytes within s
 * seconds on the X6100; the last of them, 0 for none */
static uint8_t answer(int s, int n) {
    uint8_t buf[2] = {0, 0};

    if (model != MODEL_X6100)
        return wait_respond(s, n);
    if (n > 2)
        n = 2;
    return read_for(buf, (size_t)n, s * 1000) == (size_t)n ? buf[n - 1] : 0;
}

/* ---- The steps of XMWorker::slot_start -------------------------------------------------------------------------- */

/* Step 1. Ends by itself whatever comes: the step after decides */
static void to_bootloader(void) {
    static uint8_t buf[10240];
    int            empty_left = 10000, other_left = 10000, yes = 0, asks = 0, cmd_fails = 0;
    const uint8_t  c = 'c';

    for (int turn = 0;;) {
        if (turn == 0) {
            uart_write(&c, 1);
            if (!host_cmd(HOST_IAP) && cmd_fails++ == 0)
                say("host command 0x%04x on I2C: %s (as in Xiegu's app, not looked at)", HOST_IAP, strerror(errno));
            asks++;
        }
        turn = (turn + 1) % 100;
        msleep(1);

        ssize_t r = read(uart, buf, sizeof(buf));

        if (r <= 0) {
            if (--empty_left <= 0) {
                say("[ERR] Wait IAP respond timeout ('Y' %d times, %d asks)", yes, asks);
                return;
            }
        } else {
            if (r == 1 && (buf[0] & 0xdf) == 'Y')
                yes++;
            if (--other_left <= 0) {
                say("[ERR] Clear rx buffer timeout ('Y' %d times, %d asks)", yes, asks);
                return;
            }
        }
        if (yes > 10) {
            say("[OK] Get IAP respond ('Y' %d times, %d asks, %d of them unanswered on I2C)", yes, asks, cmd_fails);
            return;
        }
    }
}

static void put_packet(uint8_t seq, const uint8_t *data, uint8_t *pkt) {
    uint16_t crc = 0;

    pkt[0] = SOH_STX, pkt[1] = seq, pkt[2] = (uint8_t)~seq;
    memcpy(pkt + 3, data, 1024);
    for (int i = 0; i < 1024; i++) {                /* CRC-16/XMODEM: 0x1021, from 0 */
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = crc & 0x8000 ? (uint16_t)(crc << 1) ^ 0x1021 : (uint16_t)(crc << 1);
    }
    pkt[1027] = crc >> 8, pkt[1028] = crc & 0xff;
}

/* Step 3: Xmodem::sendFile */
static bool send_file(void) {
    uint8_t pkt[1029], block[1024];
    long    total = (image_len + 1023) / 1024, rem = image_len;
    uint8_t r;

    if (image_len <= 0 || !image)
        return false;
    r = answer(10, 2);
    if (r == 0) {
        say("1k xmodem timeout.");
        return false;
    }
    if ((r & 0xdf) != 'C') {
        say("1k xmodem error, = %d", r);
        return false;
    }
    say("1k xmodem start.");
    for (long seq = 1, at = 0;; seq++, at += 1024) {
        if (rem >= 1024) {
            memcpy(block, image + at, 1024);
            rem -= 1024;
        } else {
            memset(block, 0xff, sizeof(block));
            memcpy(block, image + at, rem);
            rem = 0;
        }
        put_packet((uint8_t)seq, block, pkt);
        uart_write(pkt, sizeof(pkt));
        r = answer(10, 1);
        if (r == CAN) {
            say("1k xmodem transfer canceled. (packet %ld of %ld)", seq, total);
            return false;
        }
        if (r == NAK) {
            say("1k xmodem bad packet, pn= %ld (of %ld)", seq, total);
            return false;
        }
        if (r != ACK) {
            say("1k xmodem error, = %d (packet %ld of %ld)", r, seq, total);
            return false;
        }
        if (seq % 64 == 0 || rem == 0)
            say("1k xmodem send packet: %ld , progress: %ld %%, rem: %ld", seq, 100 * (image_len - rem) / image_len, rem);
        if (rem == 0)
            break;
    }
    r = EOT;
    uart_write(&r, 1);
    r = answer(10, 1);
    if (r != ACK) {
        say("1k xmodem error, = %d (after EOT)", r);
        return false;
    }
    say("1k xmodem send finished.");
    return true;
}


/* The radio, from its device tree: an X6100 or an X6200. Anything that touches the base is refused on any other */
static model_t detect_model(void) {
    char   buf[512];
    FILE  *f = fopen(DT_COMPAT, "rb");
    size_t n;

    if (!f) return MODEL_NONE;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (size_t i = 0; i + 11 <= n; i++) {
        if (!memcmp(buf + i, "xiegu,x6200", 11)) return MODEL_X6200;
        if (!memcmp(buf + i, "xiegu,x6100", 11)) return MODEL_X6100;
    }
    return MODEL_NONE;
}

static int check(bool for_flash, bool recover) {
    char    v[160];
    int     busy;
    uint8_t ready = 0;

    if (model == MODEL_NONE) {
        say("refused: this radio is neither an X6100 nor an X6200 (%s has neither xiegu,x6100 nor xiegu,x6200), and "
            "nothing was sent to the base", DT_COMPAT);
        return EXIT_REFUSED;
    }
    say("this radio is an %s", model_name(model));
    busy = holders(UART) + holders(I2C_BUS) + (model == MODEL_X6100 ? holders(GPIO_CHIP) : 0);
    if (busy && for_flash) {
        say("refused: stop what holds them first (the GUI: tools/base_update/flash.sh does it)");
        return EXIT_REFUSED;
    }
    if (!busy)
        say("nothing else holds %s or %s%s%s", UART, I2C_BUS, model == MODEL_X6100 ? " or " : "",
            model == MODEL_X6100 ? GPIO_CHIP : "");
    i2c = open(I2C_BUS, O_RDWR);
    if (i2c < 0) {
        say("%s: %s", I2C_BUS, strerror(errno));
        return EXIT_REFUSED;
    }
    if (!base_up(v, sizeof(v), &ready)) {
        say("the base does not answer on I2C (0x%02x): %s", BASE_ADDR, strerror(errno));
        if (recover) {
            say("--recover: going on without it (a base with no application has nothing to answer with)");
            return EXIT_DONE;
        }
        return EXIT_REFUSED;
    }
    say("the base answers on I2C: %s%02x, version \"%s\"", model == MODEL_X6200 ? "ready 0x" : "up ", ready, v);
    if (for_flash && !ready) {
        if (recover) {
            say("--recover: going on all the same (the base may have no application to be ready with)");
            return EXIT_DONE;
        }
        say("refused: the base says it is not ready");
        return EXIT_REFUSED;
    }
    return EXIT_DONE;
}

/* After the write: a minute's look for the base's new application, by the clock (a locked bus makes each read take
 * seconds). BACK_OK: it answers, ready (the X6100's: the same whole version text twice); BACK_ANSWERS: it answered, but
 * not that (the X6100's application, started by its bootloader or by a reset after it, gave its version text without
 * its first three bytes on most reads for the next minute: bench X6100, 15 November 2026); BACK_NONE: no answer */
enum { BACK_NONE, BACK_ANSWERS, BACK_OK };

static int wait_back(void) {
    struct timespec until;
    char            last[160] = "";
    int             seen = -1, state;
    bool            up, answered = false;

    say("looking for the base's application on I2C for %d s", BACK_WAIT_S);
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += BACK_WAIT_S;
    for (;;) {
        struct timespec now;
        char            v[160];
        uint8_t         ready = 0;

        msleep(1000);
        up = base_up(v, sizeof(v), &ready);
        /* What the bus says, each time it changes: nothing, an answer without the version yet, a version */
        state = !up ? 0 : !ready ? 1 : 2;
        answered = answered || up;
        if (state != seen) {
            if (state == 0)
                say("I2C: no answer from the base");
            else
                say("I2C: the base answers, %s \"%s\"", state == 1 ? "without its version yet:" : "version", v);
            seen = state;
        }
        /* The X6100's: the same text twice, a second apart, and then 2 s more before the GUI starts and reads it (read
         * just after a reset it once came without its first three bytes, once empty) */
        if (up && ready && (model != MODEL_X6100 || !strcmp(v, last))) {
            say("the base is back: version \"%s\"", v);
            if (model == MODEL_X6100)
                msleep(2000);
            return BACK_OK;
        }
        if (ready)
            snprintf(last, sizeof(last), "%s", v);
        else
            last[0] = '\0';
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec >= until.tv_sec)
            return answered ? BACK_ANSWERS : BACK_NONE;
    }
}

/* The X6100: steps 1 to 4 of its own (see the top) */
static int flash_x6100(bool dry_run) {
    int e = x6100_enter();

    if (e < 0) {
        say("refused: the base's key and reset lines could not be taken; the base was not touched");
        return EXIT_REFUSED;
    }
    if (e == 0) {
        say("[ERR] IAP failed, connect timeout");
        x6100_start_app();
        say("FAILED before the erase: nothing was erased; the base's application should start again");
        return EXIT_BEFORE_ERASE;
    }
    if (dry_run) {
        say("dry run: the bootloader answered; nothing is erased or written");
        x6100_start_app();
        switch (wait_back()) {
            case BACK_NONE:
                say("the base's application did not answer again yet: the GUI's start resets a base that does not "
                    "answer");
                return EXIT_BEFORE_ERASE;
            case BACK_ANSWERS:
                say("dry run done: the way into the bootloader works on this radio, and the base's application answers "
                    "again (its version text not yet read whole twice: the GUI's start reads it again)");
                return EXIT_DONE;
        }
        say("dry run done: the way into the bootloader works on this radio, and the base runs as before");
        return EXIT_DONE;
    }
    say("[OK] Start IAP erase app flash");
    if (!x6100_cmd('r', ERASE_MS)) {
        say("[ERR] IAP failed, can't erase flash");
        say("FAILED at the erase: the base's application may be gone. Run the same with --recover; do not switch off");
        return EXIT_AFTER_ERASE;
    }
    say("[OK] Start IAP xmodem transfer");
    if (!x6100_cmd('p', 1000)) {
        say("[ERR] IAP failed, can't start xmodem");
        say("FAILED after the erase: the base has no application. Run the same with --recover; do not switch off");
        return EXIT_AFTER_ERASE;
    }
    say("[OK] xmodem started");
    if (!send_file()) {
        say("[ERR] xmodem send file failed");
        say("FAILED after the erase: the base has no application. Run the same with --recover; do not switch off");
        return EXIT_AFTER_ERASE;
    }
    say("finished.");
    /* The bootloader starts the new application by itself: it answered on I2C 4 s after the transfer (15 November
     * 2026, V1.1.8) */
    switch (wait_back()) {
        case BACK_NONE:
            say("written. The base has not started the new firmware yet: the GUI's start resets a base that does not "
                "answer (about 6 s); else switch the radio off and on. Then tools/base_update/flash.sh --check shows "
                "the version");
            break;
        case BACK_ANSWERS:
            say("written. The base's new firmware answers on I2C, its version text not yet read whole twice in a row "
                "(for a minute after a write it comes without its first bytes): the GUI's start reads it again. Then "
                "tools/base_update/flash.sh --check shows the version");
            break;
    }
    return EXIT_DONE;
}

int main(int argc, char **argv) {
    bool        do_flash = false, yes = false, recover = false, identify = false, dry_run = false;
    const char *path     = NULL;
    int         rc;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check") && i + 1 < argc)
            path = argv[++i];
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc)
            path = argv[++i], do_flash = true;
        else if (!strcmp(argv[i], "--yes"))
            yes = true;
        else if (!strcmp(argv[i], "--recover"))
            recover = true;
        else if (!strcmp(argv[i], "--dry-run"))
            dry_run = true;
        else if (!strcmp(argv[i], "--held-by") && i + 1 < argc)
            held_by = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--unknown-ok"))
            unknown_ok = true;
        else if (!strcmp(argv[i], "--test-ok"))
            test_ok = true;
        else if (!strcmp(argv[i], "--identify") && i + 1 < argc)
            path = argv[++i], identify = true;
        else
            path = NULL, i = argc;
    }
    if (!path || (do_flash && !yes) || (recover && !do_flash) || (dry_run && (!do_flash || recover)) ||
        (identify && do_flash)) {
        fprintf(stderr, "usage: %s --check FILE | --flash FILE --yes [--recover | --dry-run] [--held-by PID] | "
                        "--identify FILE  [--unknown-ok] [--test-ok]\n", argv[0]);
        return EXIT_REFUSED;
    }
    model = detect_model();
    if (load(path) < 0) {
        say("refused: nothing was sent to the base");
        return EXIT_REFUSED;
    }
    if (identify) {                     /* the file only: its version and SHA-256 on a line (dialog_base_update.c) */
        printf("xiegu-base %s %s\n", image_ver, image_sha);
        return EXIT_DONE;
    }
    rc = check(do_flash, recover);
    if (rc != EXIT_DONE || !do_flash) {
        if (!do_flash)
            say(rc == EXIT_DONE ? "check done: nothing was sent to the base" : "check failed");
        return rc;
    }
    if (dry_run && model != MODEL_X6100) {
        say("refused: --dry-run is the X6100's only (the X6200's bootloader is asked for over I2C, with no way back "
            "but a write); nothing was sent to the base");
        return EXIT_REFUSED;
    }
    if (uart_open() < 0)
        return EXIT_REFUSED;

    /* From here on the transfer finishes whatever happens to the session that started it */
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    say("%s the base: hang-up, interrupt and terminate signals ignored until it ends",
        dry_run ? "asking for the bootloader of" : "writing");

    if (model == MODEL_X6100) {
        say("started, %ld bytes%s", image_len, dry_run ? " (a dry run: not sent)" : "");
        return flash_x6100(dry_run);
    }
    say("started, %ld bytes", image_len);
    to_bootloader();
    say("[OK] Start IAP connection");
    if (!send_cmd('c', 'y')) {
        say("[ERR] IAP failed, connect timeout");
        say("FAILED before the erase: nothing was erased; the base's application should still be there");
        return EXIT_BEFORE_ERASE;
    }
    say("[OK] Start IAP erase app flash");
    if (!send_cmd('r', 'g')) {
        say("[ERR] IAP failed, can't erase flash");
        say("FAILED at the erase: the base's application may be gone. Run the same with --recover; do not switch off");
        return EXIT_AFTER_ERASE;
    }
    say("[OK] Start IAP xmodem transfer");
    if (!send_cmd('p', 'g')) {
        say("[ERR] IAP failed, can't start xmodem");
        say("FAILED after the erase: the base has no application. Run the same with --recover; do not switch off");
        return EXIT_AFTER_ERASE;
    }
    say("[OK] xmodem started");
    if (!send_file()) {
        say("[ERR] xmodem send file failed");
        say("FAILED after the erase: the base has no application. Run the same with --recover; do not switch off");
        return EXIT_AFTER_ERASE;
    }
    say("finished.");
    /* Xiegu's app ends here. Its bootloader did not start the new application by itself on the radio this was first
     * run on (2026-11-13): the I2C bus stayed locked ("mv64xxx: I2C bus locked") and the radio switched itself off
     * about two minutes later; switched on again, its base ran 1.0.8. So a minute's look, then the power cycle said */
    if (wait_back() != BACK_OK)
        say("written. The base did not start the new firmware by itself: switch the radio on again (it may switch "
            "itself off first, a minute or two from now); then tools/base_update/flash.sh --check shows the version");
    return EXIT_DONE;
}
