"""The browser remote's commands as src/remote_server.c handles them: handle_text and what it calls for keys, the
PTT lease, the log and "set", extracted as they are, around stand-ins for the clock, the input devices, the GUI
thread's queue and the sockets. remote-scenarios.c drives them (26 October audit: C02, C03, C13)."""
import os
import re
from pathlib import Path

p = Path(os.environ['AUDIT_TMP']) / 'remote'     # its own radio.h and control_owner.c, apart from the B01 harness's
p.mkdir(exist_ok=True)
s = Path('src/remote_server.c').read_text()


def fn(name):
    m = re.search(r'^static [^\n;]*\b' + re.escape(name) + r'\([^;{]*\{', s, re.M)
    if not m:
        raise SystemExit('no ' + name + ' in src/remote_server.c')
    level, end = 1, m.end()
    while level:
        level += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[m.start():end]


def between(first, last):
    a = s.index(first)
    return s[a:s.index(last, a) + len(last)]


defines = '\n'.join(l for l in s.splitlines()
                    if re.match(r'#define (MAX_CLIENTS|MAX_HELD|TOKEN_LEN|SESS_MAX|PTT_LEASE_MS|PTT_LIMIT_MS|TEXT_MAX|IN_SIZE|LOG_ANSWERS)\b', l))
parts = [
    defines,
    'enum { ROLE_VIEW = 0, ROLE_OPERATE = 1, ROLE_ADMIN = 2 };',
    between('typedef struct {\n    int             fd;', '} client_t;'),
    between('typedef struct {\n    char     token[TOKEN_LEN + 1];', '} session_t;'),
    'static session_t sessions[SESS_MAX];\nstatic int nsessions;\nstatic client_t clients[MAX_CLIENTS];',
    between('static const struct {\n    const char *name;\n    uint16_t    code;\n} keys[] = {', '\n};'),
    between('enum { PIC_SHARP', '};\n'),
    between('static const struct { const char *name; int quality; bool half; } pics[PIC_N]', '};'),
    between('typedef enum { P_FREQ', '} pcmd_t;'),
    between('typedef enum { A_FT8_OPEN', '} acmd_t;'),
    'static pthread_mutex_t app_mux = PTHREAD_MUTEX_INITIALIZER;',
    between('static struct {                     /* the last log commands\' answers', 'static atomic_bool log_pending;'),
]
names = ['role_name', 'sess_key', 'log_sum', 'log_slot', 'log_queued', 'log_answer', 'log_query', 'log_flush',
         'app_apply', 'app_log_cmd', 'mode_supported', 'panel_put', 'panel_set', 'role_tell', 'ctl_tell', 'allowed',
         'ptt_down', 'ptt_lease', 'key_code', 'name_clean', 'handle_text']
pre = r'''
#include <ctype.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "control_owner.h"
#include "radio.h"
#define LV_LOG_USER(...) ((void)0)
#define LV_CLAMP(lo, v, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define VERSION "audit"
static uint64_t fake_ms = 1000000;
static uint64_t now_ms(void) { return fake_ms; }
radio_state_t radio_get_state(void) { return RADIO_RX; }
static int  keyed, key_downs;
static void key_emit(uint16_t code, bool down) {
    ctl_injected();
    if (code != BTN_TRIGGER_HAPPY4) return;
    keyed = down;
    key_downs += down;
    printf("%7.3f  radio: PTT %s\n", (fake_ms - 1000000) / 1000.0, down ? "down" : "up");
}
static bool knob_emit(const char *name, int n) { (void)name; (void)n; return true; }
typedef int ft8_rc_t;
typedef struct { int unused; } cell_data_t;
static bool app_ft8_cmd(const char *what, const char *arg) { (void)what; (void)arg; return true; }
static bool dialog_type_is_run(int d) { (void)d; return false; }
static int  dialog_ft8;
#define ACTION_APP_FT8 0
static void backlight_tick(void) {}
static void main_screen_start_app(int a) { (void)a; }
static void main_screen_close_app(void) {}
static void ft8_window_remote(ft8_rc_t rc, int32_t arg, const cell_data_t *c) { (void)rc; (void)arg; (void)c; }
static int  writes;
static bool dialog_field_qso_log(const char *text) { printf("        the day's log: %s\n", text); writes++; return true; }
static bool freq_locked, mode_locked, band_locked;
static bool main_screen_freq_locked(void) { return freq_locked; }
static bool main_screen_mode_locked(void) { return mode_locked; }
static bool main_screen_band_locked(void) { return band_locked; }
static bool cfg_is_valid_hw_freq(int32_t f) { return f >= 100000 && f <= 55000000; }
enum { radio_mode_sam = 8, radio_mode_last = 9 };
#define HW_CAP_SAM_WFM 1
static int  radio_caps(void) { return 0; }
static bool queue_full;
static struct { void (*fn)(void *); unsigned char arg[512]; } jobs[64];
static int  njobs;
static bool scheduler_put(void (*fn)(void *), void *arg, size_t n) {
    if (queue_full || njobs == 64 || n > sizeof(jobs[0].arg)) return false;
    jobs[njobs].fn = fn;
    memcpy(jobs[njobs].arg, arg, n);
    njobs++;
    return true;
}
static void panel_apply(void *arg) { (void)arg; }
static void gui_run(void) { for (int i = 0; i < njobs; i++) jobs[i].fn(jobs[i].arg); njobs = 0; }
static pthread_mutex_t sess_mux = PTHREAD_MUTEX_INITIALIZER;
static int  sess_find(const char *t) { (void)t; return -1; }
static void sess_touch(int i, const char *ip) { (void)i; (void)ip; }
static int  pair_try(const char *code, const char *name, const char *ip) { (void)code; (void)name; (void)ip; return -1; }
static void pair_show(char *o, size_t n) { (void)o; (void)n; }
static void sess_revoke(int i) { (void)i; }
static atomic_int  npanel;
static atomic_bool spots_resend;
'''
post = r'''
static char said[3][4096];
static void client_text(client_t *c, const char *t) {
    int i = (int)(c - clients);
    printf("%7.3f  -> page %d: %s\n", (fake_ms - 1000000) / 1000.0, i, t);
    strncat(said[i], t, sizeof(said[i]) - strlen(said[i]) - 2);
    strcat(said[i], "\n");
}
static void client_textf(client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void client_textf(client_t *c, const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); client_text(c, b);
}
static void sessions_tell(client_t *c) { (void)c; }
'''
(p / 'remote-commands.c').write_text(pre + '\n'.join(parts) + post + '\n'.join(fn(n) for n in names)
                                    + '\n#include "remote-scenarios.c"\n')
(p / 'radio.h').write_text('#pragma once\ntypedef enum { RADIO_RX = 0, RADIO_TX } radio_state_t;\nradio_state_t radio_get_state(void);\n')
(p / 'control_owner.c').write_text(Path('src/control_owner.c').read_text())    # the real one, with the stand-in radio.h
