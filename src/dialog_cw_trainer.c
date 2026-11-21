/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW trainer. Runs only with the keyer in train mode (sidetone, no TX).
 *  Learn (Koch): the radio keys a group with the sidetone, the user keys it
 *  back with the paddle and the decoder (listening to the sidetone) tells
 *  if it was right. Practice: the keyed text is shown large.
 *  Calls (RufzXP style): the radio keys a callsign, the user keys it back;
 *  right: speed +1 WPM, wrong: -1 WPM. 50 calls make a run.
 *  Copy (#290): the radio keys a group, the user types it on a USB or
 *  Bluetooth keyboard (or picks it with the MFK from keys on screen). Learn
 *  could be passed by keying back the dits and dahs heard, the character
 *  never recognised; here the group is not shown until the answer is in.
 *  Copy has its own level, group length and stats: copying a character is
 *  not keying it back.
 *  Learn and Copy keep the last results of every character (Stats view), saved
 *  in CWT_STATS_FILE together with today's totals and the Calls records.
 *
 *  Safety: key train is forced on while the app is open and restored when
 *  it closes, whatever closes it (destruct_cb runs for the close button,
 *  another app, a key page, or a mode change caught by the timer).
 *  The encoder is also told to never key while train is off. With train on
 *  radio.c holds the TX power at 0 W (#127): in Manual the X6200's base keyed
 *  the transmitter at PWR from the KEY jack all the same. That holds for the
 *  trainer's own keying too.
 *  Calls changes the key speed: it is given back when leaving Calls too. With
 *  TX speed Fixed (#270) it does not: the calls are keyed at their own speed,
 *  and the key speed, the paddle's, stays the user's.
 *  The trainer's groups and the answers keyed back are use of the radio, as a
 *  key is (in_use): the screensaver stays away while it is worked.
 */

#include "dialog_cw_trainer.h"
#include "voice.h"
#include "board.h"

#include "buttons.h"
#include "cJSON.h"
#include "cfg/cfg_api.h"
#include "cw.h"
#include "cw_decoder.h"
#include "cw_encoder.h"
#include "cw_trainer_answer.h"
#include "cw_trainer_score.h"
#include "dialog.h"
#include "events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "panel.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "styles.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TIMER_MS    100
#define GUARD_MS    600     /* the end of our own sidetone is still being decoded */
#define NEXT_OK_MS  1000    /* auto mode: pause before the next group */
#define NEXT_BAD_MS 2500    /* longer after a wrong answer, to read it */
#define PRESS_MS    1000    /* Start, Replay: a breath between the press and the group or call (#202) */
#define WINDOW      CWT_WINDOW  /* results the accuracy is measured over */
#define PASS_PCT    CWT_PASS
#define HEARD_MAX   32
#define TEXT_MAX    400
#define TARGET_MAX  16
#define HIST_N      CWT_HIST    /* tries kept per character */
#define CALLS_RUN   50
#define CALLS_MIN   10      /* WPM */
#define CALLS_MAX   50
#define RESET_MS    3000    /* second press of Reset stats within this time */

#ifndef CWT_STATS_FILE
#define CWT_STATS_FILE BOARD_DATA "/cw_trainer.json"
#endif

#define X           16
#define W           (796 - 2 * X)
#define BOX_Y       44
#define BOX_H       (DIALOG_H - BOX_Y - 34)
#define SIDE_W      270
#define LINE_W      (W - SIDE_W - 24)   /* Learn's Sent and Heard lines: up to 12 px before the side panel */
#define CALLS_X     18      /* Calls: call box, score box */
#define CALLS_W     480
#define SCORE_X     508
#define SCORE_W     274
#define TILE_W      64
#define TILE_H      52
#define TILE_GAP    10
#define COPY_LINE1  54      /* Copy: Sent and Typed above the keys */
#define COPY_LINE2  88
#define KEYS_Y      128     /* Copy: the keys on screen, two rows of up to 21 */
#define KEYS_ROW    21
#define KEY_W       32
#define KEY_H       28
#define KEY_GAP     3

#define COLOR_HEAD  theme_hex(TC_text_head)
#define COLOR_DIM   theme_hex(TC_text_dim)
#define COLOR_OK    theme_hex(TC_ok)
#define COLOR_BAD   theme_hex(TC_err)
#define COLOR_NEW   theme_hex(TC_accent)
#define COLOR_NONE  theme_hex(TC_text_faint)

/* Koch order (as LCWO): the first `level` characters are in use */
static const struct {
    char        c;
    const char *morse;
} koch[] = {
    {'K', "-.-"},   {'M', "--"},    {'R', ".-."},   {'S', "..."},    {'U', "..-"},    {'A', ".-"},   {'P', ".--."},
    {'T', "-"},     {'L', ".-.."},  {'O', "---"},   {'W', ".--"},    {'I', ".."},     {'.', ".-.-.-"}, {'N', "-."},
    {'J', ".---"},  {'E', "."},     {'F', "..-."},  {'0', "-----"},  {'Y', "-.--"},   {',', "--..--"}, {'V', "...-"},
    {'G', "--."},   {'5', "....."}, {'/', "-..-."}, {'Q', "--.-"},   {'9', "----."},  {'Z', "--.."},  {'H', "...."},
    {'3', "...--"}, {'8', "---.."}, {'B', "-..."},  {'?', "..--.."}, {'4', "....-"},  {'2', "..---"},  {'7', "--..."},
    {'C', "-.-."},  {'1', ".----"}, {'D', "-.."},   {'6', "-...."},  {'X', "-..-"},
};
#define KOCH_N ((int)(sizeof(koch) / sizeof(koch[0])))
_Static_assert(sizeof(koch) / sizeof(koch[0]) == CWT_CHARS, "stats arrays are sized for 40 characters");

/* Copy, last: Mode goes through the others as before */
typedef enum { MODE_LEARN, MODE_PRACTICE, MODE_CALLS, MODE_COPY } tmode_t;
typedef enum { ST_READY, ST_PLAYING, ST_LISTEN, ST_RESULT } state_t;

static tmode_t mode;
static state_t st;
static bool    active;      /* between construct and destruct */

/* Radio state to give back */
static bool           saved;
static int32_t        prev_train;
static bool           mode_changed;
static radio_mode_t   prev_mode;

/* Learn */
static char     target[TARGET_MAX];
static char     heard[HEARD_MAX];
static int      heard_n;        /* decoded characters in heard[] */
static bool     heard_end;      /* word gap after the answer (Learn; cwt_space_ends) */
static uint32_t listen_t, last_rx_t;
static int      told_wpm;       /* the speed the decoder was told (cw_trainer_speed), 0 none */
static bool     last_ok;
static bool     autoplay;       /* Start: groups follow each other until Pause */
static bool     play_due;       /* Start or Replay pressed: played once PRESS_MS have passed (tick_cb) */
static bool     due_start;      /* the one due is Start's: refused, back to Ready rather than to the answer */
static uint32_t play_t;
static uint32_t result_t;
static cwt_session_t ses_learn, ses_copy;  /* this session's groups, the last WINDOW of them for the level */

/* Stats: last HIST_N results of every Koch character, today; Learn's and Copy's */
static struct {
    cwt_stats_t learn, copy;
    int         date;           /* yyyymmdd of the today fields */
    uint32_t    best_score;
    int         best_wpm;
} stats;
static uint32_t round_t;        /* start of the round, for the minutes */
static bool     stats_view;
static bool     reset_armed;    /* Reset stats pressed once, waiting for the second */
static uint32_t reset_t;

/* Calls */
static int      call_no;        /* 1..CALLS_RUN, 0 before the first */
static int      call_wpm;       /* speed of the current call */
static int      next_wpm;
static int      prev_wpm;       /* result line: speed before the change */
static int      base_wpm;       /* the user's keyer speed, given back on leaving */
static bool     speed_saved;
static uint32_t score;
static int      calls_right;
static bool     run_done;

/* Practice */
static char text[TEXT_MAX];
static int  wpm_seen;       /* the decoder's speed, also for the end of an answer (cwt_answer_ms) */

/* Copy: the answer typed is heard[] (heard_n places, heard_end: Enter or OK ended it) */
static char koch_set[KOCH_N + 1];  /* the 40 characters, in the Koch order */
static bool sent_ok[TARGET_MAX];    /* the result, each place of the group: copied */
static bool typed_ok[HEARD_MAX];    /* each place of the answer: right */
static int  key_sel;                /* the key on screen the MFK is on */
static bool kbd_seen;               /* a keyboard there at the last look */

static lv_obj_t   *head, *head_right, *box, *big, *line1, *line2, *side_score, *side_chars, *hint, *text_lbl;
static lv_obj_t   *box_r, *calls_cont, *c_sent_cap, *c_call, *c_key_cap, *c_keyed, *c_res, *c_score, *c_stats;
static lv_obj_t   *stats_cont, *tile[HIST_N], *tile_c[HIST_N], *tile_p[HIST_N], *st_next_c, *st_today;
static lv_obj_t   *keys_cont, *key_o[KOCH_N + 2], *key_l[KOCH_N + 2];     /* Copy: the characters, Del, OK */
static lv_timer_t *timer;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void refresh_ui(void);
static void refresh_btn(button_data_t *b);
static void spaced(const char *in, char *out, size_t size);

static void play_cb(button_data_t *b);
static void skip_cb(button_data_t *b);
static void mode_cb(button_data_t *b);
static void close_cb(button_data_t *b);
static void level_up_cb(button_data_t *b);
static void level_down_cb(button_data_t *b);
static void group_cb(button_data_t *b);
static bool tx_fixed(void);
static void stats_cb(button_data_t *b);
static void reset_cb(button_data_t *b);
static void clear_cb(button_data_t *b);
static const char *play_label(void);
static const char *mode_label(void);
static const char *level_up_label(void);
static const char *level_down_label(void);
static const char *group_label(void);
static const char *skip_label(void);
static const char *stats_label(void);
static const char *reset_label(void);

static button_data_t btn_p1 = {.type = BTN_TEXT, .label = "(CWT 1:3)", .press = button_next_page_cb, .hold = button_prev_page_cb};
static button_data_t btn_p2 = {.type = BTN_TEXT, .label = "(CWT 2:3)", .press = button_next_page_cb, .hold = button_prev_page_cb};
static button_data_t btn_p3 = {.type = BTN_TEXT, .label = "(CWT 3:3)", .press = button_next_page_cb, .hold = button_prev_page_cb};
static button_data_t btn_play  = {.type = BTN_TEXT_FN, .label_fn = play_label, .press = play_cb};
static button_data_t btn_skip  = {.type = BTN_TEXT_FN, .label_fn = skip_label, .press = skip_cb};
static button_data_t btn_mode  = {.type = BTN_TEXT_FN, .label_fn = mode_label, .press = mode_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_cb};
static button_data_t btn_lvl_u = {.type = BTN_TEXT_FN, .label_fn = level_up_label, .press = level_up_cb};
static button_data_t btn_lvl_d = {.type = BTN_TEXT_FN, .label_fn = level_down_label, .press = level_down_cb};
static button_data_t btn_group = {.type = BTN_TEXT_FN, .label_fn = group_label, .press = group_cb};
static button_data_t btn_close2 = {.type = BTN_TEXT, .label = "Close", .press = close_cb};
static button_data_t btn_stats = {.type = BTN_TEXT_FN, .label_fn = stats_label, .press = stats_cb};
static button_data_t btn_reset = {.type = BTN_TEXT_FN, .label_fn = reset_label, .press = reset_cb};
static button_data_t btn_clear = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};
static button_data_t btn_close3 = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static buttons_page_t page_1 = {{&btn_p1, &btn_play, &btn_skip, &btn_mode, &btn_close}};
static buttons_page_t page_2 = {{&btn_p2, &btn_lvl_u, &btn_lvl_d, &btn_group, &btn_close2}};
static buttons_page_t page_3 = {{&btn_p3, &btn_stats, &btn_reset, &btn_clear, &btn_close3}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .key_cb       = key_cb,
    .btn_page     = &page_1,
};

dialog_t *dialog_cw_trainer = &dialog;

const char *dialog_cw_trainer_target(void) {
    return target;
}

/* Safety */

/* Key train on. False when it could not be set: nothing must be keyed then. */
static bool force_train(void) {
    if (!param_i_get(cfg_key_train)) {
        param_i_set(cfg_key_train, 1);
    }
    return param_i_get(cfg_key_train) != 0;
}

static bool is_cw(void) {
    int32_t m = cparam_i_get(cfg_cur_mode);
    return m == radio_mode_cw || m == radio_mode_cwr;
}

/* The radio in use, as after a key (#263): the screensaver's and the screen off's minutes again from now, and a
 * screensaver that came while the trainer waited for an answer goes as the answer comes. In train mode the keyer only
 * sounds the sidetone, which the X6100's base does not report as TX, and the KEY jack's paddle or key is the base's,
 * no key the GUI sees: up to v0.3.12 nothing of the trainer counted, and the screensaver came over it after its
 * minutes, while groups were played and answered */
static void in_use(void) {
    backlight_tick();
}

/* Decoder */

static void on_text(void *arg);
static void on_wpm(void *arg);

/* Audio thread: hand over to the LVGL thread. Our own group is not the answer, however late
 * the decoder ends its last character (cw_trainer_own) */
static void sink_text(const char *t, bool own) {
    if (own) return;
    scheduler_put(on_text, (void *)t, strlen(t) + 1);
}

static void sink_wpm(int w) {
    scheduler_put(on_wpm, &w, sizeof(w));
}

/* The decoder told the speed of the keying it hears next, when it is new: the trainer's group or call (play), the
 * answer (tick_cb) */
static void tell_speed(int wpm) {
    if (wpm != told_wpm) {
        cw_trainer_speed(wpm);
        told_wpm = wpm;
    }
}

static const char *morse_of(char c) {
    for (int i = 0; i < KOCH_N; i++) {
        if (koch[i].c == c) return koch[i].morse;
    }
    return "";
}

/* Dits and dahs of a group or callsign */
static int elements_of(const char *s) {
    int n = 0;
    for (; *s; s++) n += (int)strlen(morse_of(*s));
    return n;
}

/* Morse of every character of s, separated by spaces; a place with no character (CWT_NO_CHAR) as "<?>" */
static void morse_line(const char *s, char *out, size_t size) {
    out[0] = 0;
    for (; *s; s++) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, "%s%s", n ? "  " : "", *s == CWT_NO_CHAR ? CWT_NO_CHAR_SHOWN : morse_of(*s));
    }
}

static void on_wpm(void *arg) {
    if (!active) return;
    wpm_seen = *(int *)arg;
    if (mode == MODE_PRACTICE) refresh_ui();
}

static void on_text(void *arg) {
    const char *t = arg;
    if (!active || mode == MODE_COPY) return;   /* Copy: the answer is typed, keying is none */

    if (mode == MODE_PRACTICE) {
        size_t n = strlen(text), l = strlen(t);
        in_use();                   /* the user keys: what the decoder hears of it */
        if (n + l >= TEXT_MAX) {    /* drop the oldest quarter, a letter of two bytes (Å) whole */
            size_t drop = TEXT_MAX / 4;

            while (((unsigned char)text[drop] & 0xC0) == 0x80) drop++;
            memmove(text, text + drop, n - drop + 1);
            n = strlen(text);
        }
        snprintf(text + n, TEXT_MAX - n, "%s", t);
        refresh_ui();
        return;
    }

    /* Learn, Calls: only while listening, and not our own sidetone */
    if (st != ST_LISTEN || lv_tick_elaps(listen_t) < GUARD_MS) return;
    in_use();       /* the answer keyed */
    last_rx_t = lv_tick_get();
    if (t[0] == ' ') {
        heard_end = cwt_space_ends(mode == MODE_CALLS, heard_n);
    } else if (heard_n < (mode == MODE_CALLS ? 12 : 6)) {
        cwt_answer_add(heard, HEARD_MAX, t);    /* "<?>" and prosigns as CWT_NO_CHAR, never "?" */
        heard_n++;
    }
    refresh_ui();
}

/* Stats */

static int today_key(void) {
    time_t    t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* The stats and the session of the mode: Copy's own, Learn's for the others (Calls' minutes count in Learn's
 * today, as before) */
static cwt_stats_t *cur(void) {
    return mode == MODE_COPY ? &stats.copy : &stats.learn;
}

static cwt_session_t *ses(void) {
    return mode == MODE_COPY ? &ses_copy : &ses_learn;
}

/* The today fields start again when the local date changes */
static void roll_today(void) {
    int d = today_key();
    if (stats.date != d) {
        stats.date = d;
        stats.learn.tries = stats.learn.right = stats.learn.ms = 0;
        stats.copy.tries = stats.copy.right = stats.copy.ms = 0;
    }
}

/* Accuracy of the last tries of a character, -1 when untried */
static int stat_pct(int i) {
    return cwt_stats_pct(cur(), i);
}

/* Time of the round that just ended (capped: the user may have walked away) */
static void stat_time(void) {
    uint32_t d = lv_tick_elaps(round_t);
    cur()->ms += d > 60000 ? 60000 : d;
}

static uint32_t json_u(const cJSON *o, const char *key, double max) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) return 0;
    return (uint32_t)(v->valuedouble > max ? max : v->valuedouble);
}

/* Today's totals and the characters of Learn (the file's top level, as up to v0.3.12) or of Copy (its "copy") */
static void set_load(const cJSON *o, cwt_stats_t *s) {
    s->tries = json_u(o, "tries", 1e9);
    s->right = json_u(o, "right", 1e9);
    s->ms    = json_u(o, "sec", 1e6) * 1000;
    if (s->right > s->tries) s->right = s->tries;

    const cJSON *chars = cJSON_GetObjectItemCaseSensitive(o, "chars");
    for (int i = 0; i < KOCH_N; i++) {
        char         k[2] = {koch[i].c, 0};
        const cJSON *a    = cJSON_GetObjectItemCaseSensitive(chars, k);
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 2) continue;
        const cJSON *vn = cJSON_GetArrayItem(a, 0), *vm = cJSON_GetArrayItem(a, 1);
        if (!cJSON_IsNumber(vn) || !cJSON_IsNumber(vm)) continue;
        double n_  = vn->valuedouble, m_ = vm->valuedouble;
        s->n[i]    = n_ < 0 ? 0 : (n_ > HIST_N ? HIST_N : (int)n_);
        s->mask[i] = m_ < 0 ? 0 : ((uint32_t)(m_ > 4e9 ? 4e9 : m_) & ((1u << HIST_N) - 1));
    }
}

/* Missing or corrupt file: start empty */
static void stats_load(void) {
    char buf[8192];

    memset(&stats, 0, sizeof(stats));
    FILE *f = fopen(CWT_STATS_FILE, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;

        cJSON *root = cJSON_Parse(buf);
        if (cJSON_IsObject(root)) {
            stats.date       = json_u(root, "date", 99991231);
            stats.best_score = json_u(root, "best_score", 1e9);
            stats.best_wpm   = json_u(root, "best_wpm", 99);
            set_load(root, &stats.learn);

            const cJSON *copy = cJSON_GetObjectItemCaseSensitive(root, "copy");
            if (cJSON_IsObject(copy)) set_load(copy, &stats.copy);
        }
        cJSON_Delete(root);
    }
    roll_today();
}

/* False: out of memory */
static bool set_save(cJSON *o, const cwt_stats_t *s) {
    cJSON *chars = cJSON_AddObjectToObject(o, "chars");
    if (!chars) return false;
    cJSON_AddNumberToObject(o, "tries", s->tries);
    cJSON_AddNumberToObject(o, "right", s->right);
    cJSON_AddNumberToObject(o, "sec", s->ms / 1000);
    for (int i = 0; i < KOCH_N; i++) {
        char   k[2] = {koch[i].c, 0};
        cJSON *a    = s->n[i] ? cJSON_AddArrayToObject(chars, k) : NULL;
        if (!a) continue;
        cJSON_AddItemToArray(a, cJSON_CreateNumber(s->n[i]));
        cJSON_AddItemToArray(a, cJSON_CreateNumber(s->mask[i]));
    }
    return true;
}

/* Atomic: a power loss leaves the old file or the new one */
static void stats_save(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *copy = root ? cJSON_AddObjectToObject(root, "copy") : NULL;
    if (!copy || !set_save(root, &stats.learn) || !set_save(copy, &stats.copy)) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddNumberToObject(root, "date", stats.date);
    cJSON_AddNumberToObject(root, "best_score", stats.best_score);
    cJSON_AddNumberToObject(root, "best_wpm", stats.best_wpm);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", CWT_STATS_FILE);
    FILE *f  = fopen(tmp, "w");
    bool  ok = false;
    if (f) {
        ok = fputs(out, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
        ok = fclose(f) == 0 && ok;
    }
    if (ok) rename(tmp, CWT_STATS_FILE);
    else remove(tmp);
    cJSON_free(out);
}

/* Weak or little tried characters come up more often */
static int char_weight(int i) {
    int n = cur()->n[i];
    if (!n) return 60;
    return 10 + (100 - stat_pct(i)) * 2 + (HIST_N - n);
}

static int pick_char(int l) {
    int total = 0;
    for (int i = 0; i < l; i++) total += char_weight(i);
    int r = rand() % total;
    for (int i = 0; i < l; i++) {
        r -= char_weight(i);
        if (r < 0) return i;
    }
    return l - 1;
}

/* Learn */

/* Copy has its own level and group length */
static ParamInt *level_param(void) {
    return mode == MODE_COPY ? cfg_cwt_copy_level : cfg_cwt_level;
}

static ParamInt *group_param(void) {
    return mode == MODE_COPY ? cfg_cwt_copy_group : cfg_cwt_group;
}

static int level(void) {
    int l = param_i_get(level_param());
    return l < 2 ? 2 : (l > KOCH_N ? KOCH_N : l);
}

static void set_level(int l) {
    if (l < 2 || l > KOCH_N) return;
    param_i_set(level_param(), l);
    cwt_session_level(ses());
}

/* The newest character of the level comes up in one place of three while it is new, less and
 * less over its first HIST_N tries; after that its weight alone counts, as every other one's.
 * A fixed one in three for good put X, the last of the 40, on every third place at the top
 * level however well it was known */
static bool newest_extra(int l) {
    int n = cur()->n[l - 1];

    return n < HIST_N && rand() % (3 * HIST_N) < HIST_N - n;
}

static void new_target(void) {
    int len = param_i_get(group_param());
    int l   = level();
    for (int i = 0; i < len; i++) {
        /* the newest character comes up more often while it is new, then the weak ones */
        int k     = newest_extra(l) ? l - 1 : pick_char(l);
        target[i] = koch[k].c;
    }
    target[len] = 0;
    round_t     = lv_tick_get();
}

static bool play(void) {
    if (!force_train()) {
        msg_update_text_fmt("Key train is off: not playing");
        return false;
    }
    in_use();       /* a group or a call to listen to */
    heard[0] = 0;
    heard_n  = 0;
    heard_end = false;
    st       = ST_PLAYING;
    /* The decoder ends our last character when the user starts keying, or after 8.5 dots of
     * silence: at slow speeds, or with the audio's delay, that was past GUARD_MS, and it was taken
     * for the answer. The next group then came while the user was still keying, and the two ran
     * together into one character not in the table: "?" (#144) */
    cw_trainer_own(elements_of(target));
    /* The decoder at the key speed, as the paddle's: opened at 10 WPM, the decoder, still near 20, misread the second
     * answer. Told when the speed is new, so it learns a straight key's from there. Calls with TX speed Fixed key the
     * call at its own speed, and the decoder hears it at the slower of the two, at the key speed again as the answer
     * is due (tick_cb). A call slower than the key speed, heard at that, had its character gaps taken for word gaps,
     * which end our own run (MorseText::own), and the rest of the call was the answer; a faster one only runs its
     * characters together, all ours the same. Heard at its own speed, a call at 50 WPM, the decoder's fastest, lost a
     * dit now and then, and the gap left ended our own run too (tools/cw_trainer) */
    int key = param_i_get(cfg_key_speed);
    int wpm = mode == MODE_CALLS && tx_fixed() ? call_wpm : key;

    tell_speed(wpm < key ? wpm : key);
    cw_encoder_send_wpm(target, wpm);
    return true;
}

/* Start and Replay: the group or call PRESS_MS after the press, so it does not start while the hand is still on the
 * key (#202). Meanwhile the screen says "Listen..." and keying is not taken for an answer: answers count only while
 * listening. Start's group or call is chosen at the press (Calls: its number and speed in the head line at once), and
 * so is Replay's, the same again. Up to v0.3.12 Start keyed the first one at once */
static void play_after_press(bool start) {
    cw_encoder_stop();
    st        = ST_PLAYING;
    play_due  = true;
    due_start = start;
    play_t    = lv_tick_get();
}

static void replay(void) {
    play_after_press(false);
}

/* Start refused (key train could not be set): back to Ready, a call not counted */
static void start_refused(void) {
    autoplay = false;
    if (mode == MODE_CALLS) call_no--;
    st = ST_READY;
}

/* Learn and Copy: the answer against the group. Copy's typed answer is lined up with the group where a character
 * was missed or typed twice (cwt_copy_check), and the result shows which places were right */
static void evaluate(void) {
    size_t tl = strlen(target), hl = strlen(heard);

    last_ok = strcmp(heard, target) == 0;
    if (mode == MODE_COPY) {
        cwt_copy_check(target, heard, sent_ok, typed_ok);
    } else {
        for (size_t i = 0; i < tl; i++) {
            /* a dropped or extra character shifts the rest: then only count what was heard */
            sent_ok[i] = last_ok || (hl == tl ? heard[i] == target[i] : strchr(heard, target[i]) != NULL);
        }
    }
    cwt_session_add(ses(), last_ok);
    st = ST_RESULT;
    result_t = lv_tick_get();

    roll_today();
    stat_time();
    cwt_stats_group(cur(), koch_set, target, sent_ok, last_ok);
    stats_save();

    if (cwt_session_pass(ses()) && level() < KOCH_N) {
        set_level(level() + 1);
        msg_update_text_fmt("Level %i: new character %c", level(), koch[level() - 1].c);
    }
}

/* Calls */

/* Prefix patterns, # is a random digit; the suffix is 2 or 3 random letters */
static const char *const prefixes[] = {
    "IU1", "IK2", "IZ#", "I0",  "IW#", "DL#", "DK#", "DJ#", "G#",  "M0",  "M#",  "2E0", "EA#", "EA8", "EC#", "F#",
    "ON#", "PA#", "PE#", "SP#", "SQ#", "OK#", "OM#", "HA#", "YO#", "LZ#", "9A#", "S5#", "UA#", "UR#", "JA#", "JH#",
    "VK#", "ZL#", "K#",  "W#",  "N#",  "AA#", "VE#", "PY#", "LU#", "ZS#", "4X#", "OH#", "SM#", "LA#", "OZ#", "HB9",
    "OE#", "SV#", "CT#", "TA#", "LY#", "ES#", "YL#", "9H#",
};
#define PREFIX_N ((int)(sizeof(prefixes) / sizeof(prefixes[0])))

/* Prefixes of a visitor: EA8/G4XX */
static const char *const visitor[] = {"EA8/", "EA6/", "IS0/", "OE/", "HB9/", "F/", "DL/", "G/", "ON/", "SV9/"};
#define VISITOR_N ((int)(sizeof(visitor) / sizeof(visitor[0])))

static void new_call(void) {
    char base[16], *o = base;
    for (const char *p = prefixes[rand() % PREFIX_N]; *p; p++) *o++ = *p == '#' ? '0' + rand() % 10 : *p;
    for (int n = rand() % 5 < 2 ? 2 : 3; n > 0; n--) *o++ = 'A' + rand() % 26;
    *o = 0;

    int r = rand() % 20;
    if (r < 2) snprintf(target, sizeof(target), "%s/P", base);
    else if (r < 3) snprintf(target, sizeof(target), "%s/M", base);
    else if (r < 4) snprintf(target, sizeof(target), "%s%s", visitor[rand() % VISITOR_N], base);
    else snprintf(target, sizeof(target), "%s", base);
}

/* Calls, TX speed: Follows sets the key speed to each call's (as before), Fixed (#270) leaves it as the user set it,
 * so the paddle keeps the user's own speed while the calls go up and down */
static bool tx_fixed(void) {
    return param_i_get(cfg_cwt_tx_fixed) != 0;
}

static void calls_enter(void) {
    /* A new run keeps the speed saved when Calls was entered; with TX speed Fixed the key speed is the user's all
     * along, and the new run starts from it */
    if (!speed_saved || tx_fixed()) {
        base_wpm    = param_i_get(cfg_key_speed);
        speed_saved = true;
    }
    call_no     = 0;
    score       = 0;
    calls_right = 0;
    run_done    = false;
    next_wpm    = base_wpm < CALLS_MIN ? CALLS_MIN : (base_wpm > CALLS_MAX ? CALLS_MAX : base_wpm);
    call_wpm    = next_wpm;
}

/* The key speed is also the paddle speed: give the user's back (TX speed Fixed never moved it) */
static void calls_leave(void) {
    if (!speed_saved) return;
    if (!tx_fixed()) param_i_set(cfg_key_speed, base_wpm);
    speed_saved = false;
}

/* TX speed Follows <-> Fixed, in Calls. To Fixed the user's speed is back on the paddle at once; to Follows the one
 * given back on leaving is the key speed as it is now (the knob sets it while Fixed) */
static void tx_speed_toggle(void) {
    bool fixed = !tx_fixed();

    param_i_set(cfg_cwt_tx_fixed, fixed);
    if (fixed) param_i_set(cfg_key_speed, base_wpm);
    else base_wpm = param_i_get(cfg_key_speed);
}

/* Next call of the run, keyed at the speed the last answer earned: at once after a result, PRESS_MS after the press
 * of Start (pressed) */
static void start_call(bool pressed) {
    call_no++;
    call_wpm = next_wpm;
    if (!tx_fixed()) param_i_set(cfg_key_speed, call_wpm);    /* Follows: the paddle at the call's speed too */
    new_call();
    round_t = lv_tick_get();
    if (pressed) {
        play_after_press(true);
    } else if (!play()) {
        start_refused();
    }
}

static void evaluate_call(void) {
    last_ok  = strcmp(heard, target) == 0;
    prev_wpm = call_wpm;
    if (last_ok) {
        calls_right++;
        score += strlen(target) * call_wpm;
        next_wpm = call_wpm < CALLS_MAX ? call_wpm + 1 : CALLS_MAX;
        if (call_wpm > stats.best_wpm) stats.best_wpm = call_wpm;
    } else {
        next_wpm = call_wpm > CALLS_MIN ? call_wpm - 1 : CALLS_MIN;
    }
    if (score > stats.best_score) stats.best_score = score;
    roll_today();
    stat_time();
    st       = ST_RESULT;
    result_t = lv_tick_get();
    if (call_no >= CALLS_RUN) {
        run_done = true;
        autoplay = false;
    }
    stats_save();
}

static void tick_cb(lv_timer_t *t) {
    (void)t;

    /* The paddle must not transmit: train back on, whoever turned it off */
    if (!param_i_get(cfg_key_train)) {
        cw_encoder_stop();
        force_train();
    }
    /* Mode changed away (band memory, CAT): the trainer ends */
    if (!is_cw()) {
        dialog_destruct();
        return;
    }

    if (reset_armed && lv_tick_elaps(reset_t) > RESET_MS) {
        reset_armed = false;
        refresh_btn(&btn_reset);
    }
    /* Copy: a keyboard plugged in or taken out, the hint line says so */
    if (mode == MODE_COPY && keyboard_ready() != kbd_seen) {
        refresh_ui();
    }

    if (mode == MODE_PRACTICE) return;
    if (play_due) {
        if (lv_tick_elaps(play_t) >= PRESS_MS) {
            play_due = false;
            if (!play()) {                  /* key train off: Start back to Ready, Replay to the answer as it was */
                if (due_start) {
                    start_refused();
                } else {
                    st       = ST_LISTEN;
                    listen_t = last_rx_t = lv_tick_get();
                }
            }
            refresh_ui();
        }
        return;
    }
    if (st == ST_PLAYING && cw_encoder_state() == CW_ENCODER_IDLE) {
        st        = ST_LISTEN;
        listen_t  = lv_tick_get();
        last_rx_t = listen_t;
        tell_speed(param_i_get(cfg_key_speed));     /* the answer, at the key speed (TX speed Fixed: not the call's) */
        refresh_ui();
    } else if (mode == MODE_COPY) {
        /* The answer ends at the group's length, or with Enter or OK: no silence ends it, the user may think it over.
         * After a right one the next group follows; after a wrong one the trainer waits (Next), the right one on
         * screen, to be heard again (Replay) */
        if (st == ST_LISTEN && (heard_end || heard_n >= (int)strlen(target))) {
            evaluate();
            refresh_ui();
        } else if (st == ST_RESULT && autoplay && last_ok && cw_encoder_state() == CW_ENCODER_IDLE &&
                   lv_tick_elaps(result_t) > NEXT_OK_MS) {
            new_target();
            play();
            refresh_ui();
        }
    } else if (st == ST_LISTEN && heard_n > 0 &&
               (heard_n >= (int)strlen(target) || heard_end ||
                lv_tick_elaps(last_rx_t) > cwt_answer_ms(param_i_get(cfg_key_speed), wpm_seen,
                                                         param_f_get(cfg_key_ratio)))) {
        if (mode == MODE_CALLS) evaluate_call();
        else evaluate();
        refresh_ui();
    } else if (st == ST_RESULT && autoplay && lv_tick_elaps(result_t) > (last_ok ? NEXT_OK_MS : NEXT_BAD_MS)) {
        if (mode == MODE_CALLS) {
            start_call(false);
        } else {
            new_target();
            play();
        }
        refresh_ui();
    }
}

/* Copy */

/* Learn and Copy: Koch groups, a level, Stats */
static bool koch_mode(void) {
    return mode == MODE_LEARN || mode == MODE_COPY;
}

/* Copy's answer is being typed: the group playing (not in the second before it) or played, the answer not ended */
static bool answering(void) {
    return mode == MODE_COPY && !stats_view && !play_due && (st == ST_PLAYING || st == ST_LISTEN) && !heard_end;
}

/* A character of the answer, typed or picked on screen; the answer ends at the group's length (tick_cb) */
static void copy_type(char c) {
    bool added;

    in_use();
    added = cwt_copy_add(heard, (int)strlen(target), c);
    if (added) heard_n++;
    /* speech: the character echoed once the group is played, not over its sidetone */
    if (added && st == ST_LISTEN && voice_auto_active()) {
        char one[2] = {c, 0}, sp[16];

        spaced(one, sp, sizeof(sp));
        voice_say_text_fmt("%s", sp);
    }
    refresh_ui();
}

static void copy_rub_out(void) {
    if (cwt_copy_del(heard)) {
        heard_n--;
        if (voice_auto_active()) voice_say_text_fmt("deleted");
    }
    refresh_ui();
}

/* Start, Next: a new group PRESS_MS after the press, as Learn's first; the ones after a right answer follow it */
static void copy_next(void) {
    autoplay = true;
    new_target();
    play_after_press(true);
}

/* Enter (OK on screen): the answer ends, the places not typed missed; else the next group */
static void copy_enter(void) {
    if (answering()) {
        heard_end = true;
    } else if (!play_due && (st == ST_READY || st == ST_RESULT)) {
        copy_next();
    }
    refresh_ui();
}

/* The group again with its result on screen: after a wrong answer, the right one to hear. A right one's next group
 * follows NEXT_OK_MS after it */
static void review(void) {
    if (!force_train()) {
        msg_update_text_fmt("Key train is off: not playing");
        return;
    }
    in_use();
    cw_trainer_own(elements_of(target));
    cw_encoder_send_wpm(target, param_i_get(cfg_key_speed));
    result_t = lv_tick_get();
}

/* MFK press while the answer is due: the key on screen it is on, a character, Del or OK */
static void copy_press_key(void) {
    int l = level();

    key_sel = cwt_copy_sel(key_sel, l + 2, 0);
    if (key_sel < l) copy_type(koch[key_sel].c);
    else if (key_sel == l) copy_rub_out();
    else copy_enter();
}

/* The keyboard types the answer (Backspace rubs out, Enter ends it), and the MFK turns over the keys on screen while
 * it is due. Otherwise the MFK and the arrow keys set the speed, as in the other modes (key_cb). True: taken */
static bool copy_key(uint32_t key) {
    lv_indev_t *indev = lv_indev_get_act();
    bool        knob  = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;
    char        c     = cwt_copy_char(key, koch_set);

    if (stats_view) return false;
    if (knob && answering() && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        key_sel = cwt_copy_sel(key_sel, level() + 2, key == LV_KEY_LEFT || key == LV_KEY_DOWN ? -1 : 1);
        refresh_ui();
        return true;
    }
    if (key == LV_KEY_ENTER || key == '\r') {
        copy_enter();
        return true;
    }
    if (key == LV_KEY_BACKSPACE) {
        if (answering()) copy_rub_out();
        return true;
    }
    if (c) {                        /* out of an answer a character does nothing */
        if (answering()) copy_type(c);
        return true;
    }
    return false;
}

/* Buttons */

/* Nothing of a round stays when the mode or the view changes */
static void stop_round(void) {
    cw_encoder_stop();
    cw_trainer_own(0);
    autoplay   = false;
    play_due   = false;
    st         = ST_READY;
    target[0]  = 0;
    heard[0]  = 0;
    heard_n   = 0;
}

static void play_cb(button_data_t *b) {
    (void)b;
    if (mode == MODE_PRACTICE || stats_view) return;
    if (mode == MODE_COPY) {
        if (st == ST_READY || (st == ST_RESULT && (!last_ok || !autoplay))) {
            copy_next();            /* Start, Next */
        } else {
            autoplay = !autoplay;   /* Pause: the current group still ends; Start: the next ones follow it */
        }
    } else if (mode == MODE_LEARN && st == ST_LISTEN) {    /* Replay */
        replay();
    } else if (autoplay) {          /* Pause: the current group still ends */
        autoplay = false;
    } else {                        /* Start */
        autoplay = true;
        if (mode == MODE_CALLS) {
            if (run_done) calls_enter();    /* a new run */
            if (st != ST_PLAYING && st != ST_LISTEN) start_call(true);
        } else if (st != ST_PLAYING) {
            new_target();
            play_after_press(true);
        }
    }
    refresh_ui();
}

static void skip_cb(button_data_t *b) {
    (void)b;
    if (mode == MODE_PRACTICE || stats_view) return;
    if (mode == MODE_CALLS) {       /* Replay */
        if (st == ST_LISTEN) replay();
    } else if (mode == MODE_COPY) { /* Replay: the group to answer (the answer starts again), or the one checked */
        if (st == ST_LISTEN) replay();
        else if (st == ST_RESULT) review();
    } else {
        cw_encoder_stop();
        play_due = false;           /* played now: a Start or Replay still due played the new group again */
        new_target();
        play();
    }
    refresh_ui();
}

static void mode_cb(button_data_t *b) {
    (void)b;
    stop_round();
    calls_leave();
    mode       = (tmode_t)((mode + 1) % 4);
    stats_view = false;
    reset_armed = false;
    text[0]    = 0;
    if (mode == MODE_CALLS) calls_enter();
    refresh_ui();
}

static void close_cb(button_data_t *b) {
    (void)b;
    dialog_destruct();
}

static void level_up_cb(button_data_t *b) {
    (void)b;
    if (!koch_mode()) return;
    set_level(level() + 1);
    refresh_ui();
}

static void level_down_cb(button_data_t *b) {
    (void)b;
    if (!koch_mode()) return;
    set_level(level() - 1);
    refresh_ui();
}

/* Learn, Copy: the group's length. Calls: TX speed */
static void group_cb(button_data_t *b) {
    (void)b;
    if (mode == MODE_CALLS) tx_speed_toggle();
    else if (koch_mode()) param_i_set(group_param(), param_i_get(group_param()) % 5 + 1);
    else return;
    refresh_ui();
}

/* The key speed is also the paddle speed of the keyer. Calls sets it itself, but with TX speed Fixed: there it is the
 * user's own, and before the run's first call the calls start from it too */
static void change_speed(int d) {
    if (mode == MODE_CALLS && !tx_fixed()) return;
    int v = param_i_get(cfg_key_speed) + d;
    if (v < 5 || v > 50) return;
    param_i_set(cfg_key_speed, v);
    if (mode == MODE_CALLS && call_no == 0) next_wpm = v < CALLS_MIN ? CALLS_MIN : v;
    refresh_ui();
}

/* Stats view: nothing is playing while it is open */
static void stats_cb(button_data_t *b) {
    (void)b;
    if (!koch_mode()) return;
    stop_round();
    stats_view  = !stats_view;
    reset_armed = false;
    refresh_ui();
}

/* Two presses within RESET_MS. The mode's stats only: Learn's, or Copy's */
static void reset_cb(button_data_t *b) {
    (void)b;
    if (!koch_mode()) return;
    if (reset_armed && lv_tick_elaps(reset_t) <= RESET_MS) {
        cwt_stats_t *c = cur();

        reset_armed = false;
        memset(c->mask, 0, sizeof(c->mask));
        memset(c->n, 0, sizeof(c->n));
        c->tries = c->right = c->ms = 0;
        stats_save();
        msg_update_text_fmt("Stats reset");
    } else {
        reset_armed = true;
        reset_t     = lv_tick_get();
    }
    refresh_ui();
}

static void clear_cb(button_data_t *b) {
    (void)b;
    text[0] = 0;
    refresh_ui();
}

static const char *play_label(void) {
    if (st == ST_LISTEN && mode == MODE_LEARN) return "Replay";
    if (st == ST_RESULT && mode == MODE_COPY && (!last_ok || !autoplay)) return last_ok ? "Start" : "Next";
    return autoplay ? "Pause" : "Start";
}

static const char *mode_label(void) {
    static const char *const names[] = {"Mode:\nLearn", "Mode:\nPractice", "Mode:\nCalls", "Mode:\nCopy"};

    return names[mode];
}

static const char *skip_label(void) {
    return mode == MODE_CALLS || mode == MODE_COPY ? "Replay" : "Skip";
}

static const char *stats_label(void) {
    return stats_view ? "Back" : "Stats";
}

static const char *reset_label(void) {
    return reset_armed ? "Sure?" : "Reset\nstats";
}

static char label_buf[3][24];

static const char *level_up_label(void) {
    snprintf(label_buf[0], sizeof(label_buf[0]), "Level +\n%i", level());
    return label_buf[0];
}

static const char *level_down_label(void) {
    snprintf(label_buf[1], sizeof(label_buf[1]), "Level -\n%i", level());
    return label_buf[1];
}

static const char *group_label(void) {
    if (mode == MODE_CALLS) return tx_fixed() ? "TX speed:\nFixed" : "TX speed:\nFollows";
    snprintf(label_buf[2], sizeof(label_buf[2]), "Group\n%i", (int)param_i_get(group_param()));
    return label_buf[2];
}

/* Screen */

static void set_btn(button_data_t *b, bool disabled) {
    if (b->disabled != disabled) buttons_disabled(b, disabled);
}

static void refresh_btn(button_data_t *b) {
    if (b->disp_btn) buttons_refresh(b);
}

static void show(lv_obj_t *o, bool on) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* "KMR" said "K M R": one character at a time, never read as a word */
static void spaced(const char *in, char *out, size_t size) {
    size_t n = 0;

    out[0] = 0;
    for (; in && *in && n + 10 < size; in++) {
        if (*in == CWT_NO_CHAR)
            n += snprintf(out + n, size - n, "%sunknown", n ? " " : "");
        else
            n += snprintf(out + n, size - n, "%s%c", n ? " " : "", *in);
    }
}

/* 2115 -> "2 115" */
static void fmt_num(uint32_t v, char *out, size_t size) {
    if (v >= 1000000) snprintf(out, size, "%u %03u %03u", (unsigned)(v / 1000000), (unsigned)(v / 1000 % 1000), (unsigned)(v % 1000));
    else if (v >= 1000) snprintf(out, size, "%u %03u", (unsigned)(v / 1000), (unsigned)(v % 1000));
    else snprintf(out, size, "%u", (unsigned)v);
}

static void refresh_calls(void) {
    char buf[200], n1[16], n2[16], shown[HEARD_MAX * 3], one[2];
    bool rev  = st == ST_RESULT;
    int  done = (st == ST_PLAYING || st == ST_LISTEN) ? call_no - 1 : call_no;

    lv_label_set_text_fmt(head, "CW TRAINER - #%06x Calls#", theme_hex(TC_text));
    snprintf(buf, sizeof(buf), "Call %i/%i - %i WPM", call_no, CALLS_RUN, call_no ? call_wpm : next_wpm);
    lv_label_set_text(head_right, buf);

    show(c_sent_cap, rev);
    show(c_key_cap, st == ST_LISTEN || rev);
    lv_obj_set_style_text_color(c_call, lv_color_hex(rev ? theme_hex(TC_text) : (st == ST_READY ? COLOR_HEAD : COLOR_NEW)), 0);
    lv_label_set_text(c_call, rev ? target : (st == ST_PLAYING ? "Listen..." : (st == ST_LISTEN ? "Your turn" : "Press Start")));

    /* Wrong characters in red once the call is revealed, each place against the callsign's own place. A place with
     * no character is a red "<?>", three characters wide: the places after it move to the right, each still in its
     * own colour. They never stood exactly under the callsign's in this font (a W is four times as wide as an I),
     * and the colours go by place, not by where a character stands; "<?>" in one place's width would take a font
     * too small to read */
    size_t k = 0;
    buf[0]   = 0;
    for (int i = 0; heard[i] && rev && k < sizeof(buf); i++) {
        bool        bad = target[i] == 0 || heard[i] != target[i];
        const char *s   = cwt_place_shown(heard[i], one);
        k += bad ? snprintf(buf + k, sizeof(buf) - k, "#%06x %s#", theme_hex(TC_err), s)
                 : snprintf(buf + k, sizeof(buf) - k, "%s", s);
    }
    lv_label_set_text(c_keyed, rev ? buf : cwt_answer_shown(heard, shown, sizeof(shown)));

    buf[0] = 0;
    if (rev) {
        snprintf(buf, sizeof(buf), "%s - speed %i -> %i WPM", last_ok ? "Right" : "Wrong", prev_wpm, next_wpm);
        if (run_done) {
            fmt_num(score, n1, sizeof(n1));
            size_t n = strlen(buf);
            snprintf(buf + n, sizeof(buf) - n, "\n#%06x Run complete - final score %s#", theme_hex(TC_accent), n1);
        }
    }
    lv_obj_set_style_text_color(c_res, lv_color_hex(last_ok ? COLOR_OK : COLOR_BAD), 0);
    lv_label_set_text(c_res, buf);
    {
        static int said_done = -1;

        if (!rev) {
            said_done = -1;
        } else if (done != said_done && voice_auto_active()) {
            char sp[64], fin[48] = "";

            said_done = done;
            spaced(target, sp, sizeof(sp));
            if (run_done) {
                fmt_num(score, n1, sizeof(n1));
                snprintf(fin, sizeof(fin), ". Run complete, score %s", n1);
            }
            if (last_ok)
                voice_say_text_fmt("Right, %i W P M%s", next_wpm, fin);
            else
                voice_say_text_fmt("Wrong, it was %s, %i W P M%s", sp, next_wpm, fin);
        }
    }
    if (run_done && rev) lv_label_set_text(hint, "Start: a new run of 50 callsigns");
    else if (rev && strchr(heard, CWT_NO_CHAR)) lv_label_set_text(hint, CWT_NO_CHAR_SHOWN " = not a character. Right: +1 WPM, wrong: -1 WPM.");
    else if (tx_fixed())
        lv_label_set_text_fmt(hint, "Key back the callsign at your %i WPM. Right: +1 WPM, wrong: -1 WPM.",
                              (int)param_i_get(cfg_key_speed));
    else lv_label_set_text(hint, "Key back the callsign with the paddle. Right: +1 WPM, wrong: -1 WPM.");

    fmt_num(score, n1, sizeof(n1));
    lv_label_set_text(c_score, n1);
    fmt_num(stats.best_score, n2, sizeof(n2));
    snprintf(buf, sizeof(buf), "Right %i / %i\nBest speed %i WPM\nBest score %s", calls_right, done, stats.best_wpm, n2);
    lv_label_set_text(c_stats, buf);
}

static void refresh_stats(void) {
    char buf[160];
    int  l   = level();
    int  end = l + 3 > KOCH_N ? KOCH_N : l + 3;
    int  beg = end > HIST_N ? end - HIST_N : 0;

    if (mode == MODE_COPY) lv_label_set_text_fmt(head, "CW TRAINER - #%06x Copy stats#", theme_hex(TC_text));
    else lv_label_set_text_fmt(head, "CW TRAINER - #%06x Stats#", theme_hex(TC_text));
    snprintf(buf, sizeof(buf), "Level %i - %i WPM", l, (int)param_i_get(cfg_key_speed));
    lv_label_set_text(head_right, buf);

    for (int t = 0; t < HIST_N; t++) {
        int i = beg + t;
        show(tile[t], i < end);
        if (i >= end) continue;
        int      pct = stat_pct(i);
        uint32_t col = pct < 0 ? COLOR_NONE : (pct >= 90 ? COLOR_OK : (pct >= 75 ? COLOR_NEW : COLOR_BAD));
        snprintf(buf, sizeof(buf), "%c", koch[i].c);
        lv_label_set_text(tile_c[t], buf);
        if (pct < 0) snprintf(buf, sizeof(buf), "-");
        else snprintf(buf, sizeof(buf), "%i%%", pct);
        lv_label_set_text(tile_p[t], buf);
        lv_obj_set_style_border_color(tile[t], lv_color_hex(col), 0);
        lv_obj_set_style_text_color(tile_p[t], lv_color_hex(col), 0);
    }

    snprintf(buf, sizeof(buf), "%c", l < KOCH_N ? koch[l].c : '-');
    lv_label_set_text(st_next_c, buf);

    roll_today();
    const cwt_stats_t *c = cur();
    snprintf(buf, sizeof(buf), "Today: %u tries, %u%% right, %u min", (unsigned)c->tries,
             c->tries ? (unsigned)(c->right * 100 / c->tries) : 0u, (unsigned)((c->ms + 30000) / 60000));
    lv_label_set_text(st_today, buf);
    if (mode == MODE_COPY)
        lv_label_set_text(hint, "Last 20 tries per character. Weak ones (red, yellow) come up more often in Copy.");
    else lv_label_set_text(hint, "Last 20 tries per character. Weak ones (red, yellow) come up more often in Learn.");
}

/* A line of Learn into the label l, Sent or Heard: its head, the characters as shown in color, then the dits and dahs
 * of as many of the places as fit before the side panel, whole places only. A "<?>" takes three characters' room in
 * the answer and again in the dits and dahs: two of them in a group of 5 could run into the side panel, and so could
 * long characters on either line (five ? in the default font, a third to half of the groups of 5 at level 40 in the
 * 1bit theme's). Cut at the label's edge instead, half a place stayed: a lone "<", or a dah that looked like a dit.
 * Copy's lines (ok not NULL): each place in color where ok says, else in red; shown is the places then */
static void group_line(lv_obj_t *l, const char *head, uint32_t color, const char *shown, const char *places,
                       const bool *ok) {
    const lv_font_t *font = lv_obj_get_style_text_font(l, 0);
    lv_coord_t       ls   = lv_obj_get_style_text_letter_space(l, 0);
    char             buf[200], part[HEARD_MAX], m[48], col[TARGET_MAX * 12 + 1];
    size_t           k = 0;

    col[0] = 0;
    for (size_t i = 0; ok && places[i] && i < TARGET_MAX; i++) {
        k += snprintf(col + k, sizeof(col) - k, "#%06x %c#", ok[i] ? color : COLOR_BAD, places[i]);
    }
    for (size_t n = strlen(places);; n--) {
        memcpy(part, places, n);
        part[n] = 0;
        morse_line(part, m, sizeof(m));
        if (ok) snprintf(buf, sizeof(buf), "%s%s   %s", head, col, m);
        else snprintf(buf, sizeof(buf), "%s#%06x %s#   %s", head, color, shown, m);
        if (n == 0 || lv_txt_get_width(buf, strlen(buf), font, ls, LV_TEXT_FLAG_RECOLOR) <= LINE_W) break;
    }
    lv_label_set_text(l, buf);
}

/* Copy: the group hidden until the answer is in, the places typed so far and one _ for each still to type; then the
 * group and the answer with their dits and dahs, each place in red where it was missed or wrong. Under them the keys
 * on screen, the one the MFK is on lit while an answer is due */
static void refresh_copy(void) {
    cwt_session_t *s   = ses();
    int            l   = level(), len = (int)strlen(target);
    bool           kbd = keyboard_ready();
    char           buf[200];

    kbd_seen = kbd;
    key_sel  = cwt_copy_sel(key_sel, l + 2, 0);     /* the level went down under it */
    lv_label_set_text_fmt(head, "CW TRAINER - #%06x Copy#", theme_hex(TC_text));
    snprintf(buf, sizeof(buf), _("%i WPM - group %i"), (int)param_i_get(cfg_key_speed), (int)param_i_get(group_param()));
    lv_label_set_text(head_right, buf);
    lv_label_set_text(line1, "");
    lv_label_set_text(line2, "");

    if (st == ST_READY) {
        lv_label_set_text(big, "Press Start");
        lv_obj_set_style_text_color(big, lv_color_hex(COLOR_HEAD), 0);
        lv_label_set_text(hint, kbd ? "Start: groups follow each other, type each one on the keyboard"
                                    : "No keyboard found: plug in a USB or Bluetooth one, or use the MFK on the keys");
    } else if (play_due) {
        lv_label_set_text(big, "Listen...");
        lv_obj_set_style_text_color(big, lv_color_hex(COLOR_NEW), 0);
        lv_label_set_text(hint, "The radio keys the group (sidetone only)");
    } else if (st == ST_PLAYING || st == ST_LISTEN) {
        size_t k = 0;

        buf[0] = 0;
        for (int i = 0; i < len && k < sizeof(buf); i++) {
            if (i < heard_n) k += snprintf(buf + k, sizeof(buf) - k, "%s%c", i ? " " : "", heard[i]);
            else k += snprintf(buf + k, sizeof(buf) - k, "%s#%06x _#", i ? " " : "", COLOR_DIM);
        }
        lv_label_set_text(big, buf);
        lv_obj_set_style_text_color(big, TC(text), 0);
        lv_label_set_text(hint, kbd ? "Type what you hear. Enter: done, Backspace: rub out. Replay: hear it again"
                                    : "No keyboard: turn the MFK to a key, press it to enter. Replay: hear it again");
    } else {
        lv_label_set_text(big, last_ok ? "OK" : "WRONG");
        lv_obj_set_style_text_color(big, lv_color_hex(last_ok ? COLOR_OK : COLOR_BAD), 0);
        group_line(line1, _("Sent:   "), theme_hex(TC_accent), target, target, sent_ok);
        if (heard[0]) group_line(line2, _("Typed: "), COLOR_OK, heard, heard, typed_ok);
        else lv_label_set_text_fmt(line2, "%s#%06x -#", _("Typed: "), COLOR_BAD);
        if (!last_ok)
            lv_label_set_text(hint, kbd ? "Replay: hear the right one again. Next or Enter: another group"
                                        : "Replay: hear the right one again. Next or MFK press: another group");
        else if (autoplay) lv_label_set_text(hint, "Next group follows. Pause: stop after this one");
        else lv_label_set_text(hint, kbd ? "Start or Enter: the next group" : "Start or MFK press: the next group");
    }

    snprintf(buf, sizeof(buf), _("Level %i\nScore  %i / %i\nLast %i: %i / %i\nNext level at %i%%"), l, s->right,
             s->tried, WINDOW, cwt_session_good(s), s->n, PASS_PCT);
    lv_label_set_text(side_score, buf);

    for (int i = 0; i < KOCH_N + 2; i++) {
        bool on = i < l + 2, lit = answering() && i == key_sel;

        show(key_o[i], on);
        if (!on) continue;
        if (i < l) {
            char c[2] = {koch[i].c, 0};

            lv_label_set_text(key_l[i], c);
            lv_obj_set_style_text_font(key_l[i], &sony_22, 0);
        } else {
            lv_label_set_text(key_l[i], i == l ? "Del" : "OK");
            lv_obj_set_style_text_font(key_l[i], &sony_16, 0);
        }
        lv_obj_set_style_bg_color(key_o[i], lit ? TC(focus) : TC(panel), 0);
        lv_obj_set_style_border_color(key_o[i], lit ? TC(focus) : TC(panel_border), 0);
        /* the level's newest character as in Learn's list */
        lv_obj_set_style_text_color(key_l[i], lit ? TC(focus_text) : (i == l - 1 ? TC(accent) : TC(text)), 0);
    }

    /* Speech: the key on screen the MFK turns to, and the result with what was sent and typed */
    {
        static int said_tried = -1, said_key = -1;

        if (!answering()) {
            said_key = -1;
        } else if (said_key < 0) {
            said_key = key_sel;             /* the answer begins: said when the MFK moves */
        } else if (key_sel != said_key) {
            said_key = key_sel;
            if (voice_auto_active()) {
                char one[2] = {key_sel < l ? koch[key_sel].c : 0, 0}, sp[16];

                spaced(one, sp, sizeof(sp));
                voice_say_text_fmt("%s", key_sel < l ? sp : key_sel == l ? "Delete" : "OK, done");
            }
        }
        if (st == ST_RESULT && !play_due && s->tried != said_tried) {
            said_tried = s->tried;
            if (voice_auto_active()) {
                char sp1[64], sp2[64];

                spaced(target, sp1, sizeof(sp1));
                spaced(heard, sp2, sizeof(sp2));
                if (last_ok)
                    voice_say_text_fmt("Right, %s. Score %i of %i", sp1, s->right, s->tried);
                else
                    voice_say_text_fmt("Wrong, sent %s, typed %s. Score %i of %i", sp1, sp2[0] ? sp2 : "nothing",
                                       s->right, s->tried);
            }
        }
    }
}

static void refresh_ui(void) {
    bool learn = mode == MODE_LEARN, calls = mode == MODE_CALLS, copy = mode == MODE_COPY, koch_m = koch_mode();
    bool sv    = koch_m && stats_view;
    int  wpm   = param_i_get(cfg_key_speed);
    char buf[160], shown[HEARD_MAX * 3];

    set_btn(&btn_play, mode == MODE_PRACTICE || sv);
    set_btn(&btn_skip, mode == MODE_PRACTICE || sv);
    set_btn(&btn_lvl_u, !koch_m);
    set_btn(&btn_lvl_d, !koch_m);
    set_btn(&btn_group, !koch_m && !calls);    /* Calls: TX speed */
    set_btn(&btn_stats, !koch_m);
    set_btn(&btn_reset, !koch_m);
    set_btn(&btn_clear, mode != MODE_PRACTICE);
    refresh_btn(&btn_play);
    refresh_btn(&btn_skip);
    refresh_btn(&btn_mode);
    refresh_btn(&btn_lvl_u);
    refresh_btn(&btn_lvl_d);
    refresh_btn(&btn_group);
    refresh_btn(&btn_stats);
    refresh_btn(&btn_reset);

    /* Calls has two boxes, the others one */
    lv_obj_set_pos(box, calls ? CALLS_X : X, BOX_Y);
    lv_obj_set_width(box, calls ? CALLS_W : W);
    show(box, true);
    show(box_r, calls);
    show(calls_cont, calls);
    show(stats_cont, sv);
    show(big, koch_m && !sv);
    show(line1, koch_m && !sv);
    show(line2, koch_m && !sv);
    show(text_lbl, mode == MODE_PRACTICE);
    show(side_score, koch_m && !sv);
    show(side_chars, learn && !sv);
    show(keys_cont, copy && !sv);
    /* Copy: Sent and Typed higher up, the keys under them */
    lv_obj_set_y(line1, copy ? COPY_LINE1 : 90);
    lv_obj_set_y(line2, copy ? COPY_LINE2 : 140);

    if (calls) {
        refresh_calls();
        return;
    }
    if (sv) {
        refresh_stats();
        return;
    }
    if (copy) {
        refresh_copy();
        return;
    }
    if (mode == MODE_PRACTICE) {
        lv_label_set_text_fmt(head, "CW TRAINER - #%06x Practice#", theme_hex(TC_text));
        snprintf(buf, sizeof(buf), "%i WPM (keyer %i)", wpm_seen, wpm);
        lv_label_set_text(head_right, buf);
        lv_label_set_text(text_lbl, text[0] ? text : "Key with the paddle: the text appears here");
        lv_obj_set_style_text_color(text_lbl, lv_color_hex(text[0] ? theme_hex(TC_text) : COLOR_DIM), 0);
        lv_obj_update_layout(box);
        lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
        lv_label_set_text(hint, "No transmission: key train is on. Clear: page 3");
        return;
    }

    lv_label_set_text_fmt(head, "CW TRAINER - #%06x Learn (Koch)#", theme_hex(TC_text));
    snprintf(buf, sizeof(buf), "%i WPM - group %i", wpm, (int)param_i_get(group_param()));
    lv_label_set_text(head_right, buf);

    lv_label_set_text(line1, "");
    lv_label_set_text(line2, "");
    cwt_answer_shown(heard, shown, sizeof(shown));
    switch (st) {
        case ST_READY:
            lv_label_set_text(big, "Press Start");  /* the button's word, as in Calls; up to v0.3.12 "Play" (#264) */
            lv_obj_set_style_text_color(big, lv_color_hex(COLOR_HEAD), 0);
            lv_label_set_text(hint, "Start: groups follow each other, key each one back with the paddle");
            break;
        case ST_PLAYING:
            lv_label_set_text(big, "Listen...");
            lv_obj_set_style_text_color(big, lv_color_hex(COLOR_NEW), 0);
            lv_label_set_text(hint, "The radio keys the group (sidetone only)");
            /* The group stays on screen while playing and answering, to memorise it */
            group_line(line1, "Sent:   ", theme_hex(TC_accent), target, target, NULL);
            lv_label_set_text(line2, "");
            break;
        case ST_LISTEN:
            lv_label_set_text(big, heard[0] ? shown : "Your turn");
            lv_obj_set_style_text_color(big, lv_color_hex(heard[0] ? theme_hex(TC_text) : COLOR_NEW), 0);
            lv_label_set_text(hint, "Key the group with the paddle. Replay: hear it again");
            group_line(line1, "Sent:   ", theme_hex(TC_accent), target, target, NULL);
            if (heard[0]) group_line(line2, "Heard: ", theme_hex(TC_text), shown, heard, NULL);
            break;
        case ST_RESULT: {
            static int said_tried = -1;
            int        tried      = ses()->tried;     /* one more with each group keyed back (cw_trainer_score.c) */

            if (tried != said_tried && voice_auto_active()) {
                char sp1[64], sp2[64];

                said_tried = tried;
                spaced(target, sp1, sizeof(sp1));
                spaced(heard, sp2, sizeof(sp2));
                if (last_ok)
                    voice_say_text_fmt("Right");
                else
                    voice_say_text_fmt("Wrong, sent %s, heard %s", sp1, sp2[0] ? sp2 : "nothing");
            }
        }
            lv_label_set_text(big, last_ok ? "OK" : "WRONG");
            lv_obj_set_style_text_color(big, lv_color_hex(last_ok ? COLOR_OK : COLOR_BAD), 0);
            group_line(line1, "Sent:   ", theme_hex(TC_accent), target, target, NULL);
            group_line(line2, "Heard: ", last_ok ? COLOR_OK : COLOR_BAD, shown, heard, NULL);
            snprintf(buf, sizeof(buf), "%s%s", strchr(heard, CWT_NO_CHAR) ? CWT_NO_CHAR_SHOWN " = not a character. " : "",
                     autoplay ? "Next group follows. Pause: stop after this one" : "Start: resume. Skip: another group");
            lv_label_set_text(hint, buf);
            break;
    }

    cwt_session_t *s = ses();
    snprintf(buf, sizeof(buf), "Score  %i / %i\nLast %i: %i / %i\nNext level at %i%%", s->right, s->tried, WINDOW,
             cwt_session_good(s), s->n, PASS_PCT);
    lv_label_set_text(side_score, buf);

    /* Characters in use, the newest highlighted */
    char *p = buf;
    int   l = level();
    for (int i = 0; i < l; i++) {
        p += i == l - 1 ? sprintf(p, "#%06x %c#", theme_hex(TC_accent), koch[i].c) : sprintf(p, "%c", koch[i].c);
        *p++ = ' ';
    }
    *p = 0;
    char cbuf[200];
    snprintf(cbuf, sizeof(cbuf), "Level %i\n%s", l, buf);
    lv_label_set_text(side_chars, cbuf);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (mode == MODE_COPY && copy_key(key)) return;
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_DOWN:
            change_speed(-1);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_UP:
            change_speed(1);
            break;
        case LV_KEY_ENTER:
            play_cb(NULL);
            break;
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

/* MFK press: the encoder sends a click, not ENTER. In Copy it enters the key on screen it is on while an answer is
 * due, and is Enter otherwise (Start, Next) */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    if (mode != MODE_COPY || stats_view) play_cb(NULL);
    else if (answering()) copy_press_key();
    else copy_enter();
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_recolor(l, true);
    return l;
}

/* Transparent holder: shown or hidden as one */
static lv_obj_t *cont(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, 0, 0);
    return o;
}

static lv_obj_t *panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t w) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, BOX_H);
    lv_obj_set_pos(o, x, BOX_Y);
    lv_obj_set_style_bg_color(o, TC(side), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, TC(panel_border), 0);
    lv_obj_set_style_border_width(o, theme_shape.border, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    return o;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    /* Train first: nothing below may key the radio without it */
    prev_train = param_i_get(cfg_key_train);
    saved      = true;
    force_train();
    cw_encoder_require_train(true);

    /* CW mode: the decoder listens only there */
    mode_changed = !is_cw();
    if (mode_changed) {
        prev_mode = (radio_mode_t)cparam_i_get(cfg_cur_mode);
        cparam_i_set(cfg_cur_mode, radio_mode_cw);
    }
    main_screen_lock_mode(true);
    panel_hide();

    srand(time(NULL));
    mode = MODE_LEARN;
    st   = ST_READY;
    autoplay = false;
    play_due = false;
    told_wpm = 0;
    target[0] = heard[0] = text[0] = 0;
    heard_n = wpm_seen = key_sel = 0;
    memset(&ses_learn, 0, sizeof(ses_learn));
    memset(&ses_copy, 0, sizeof(ses_copy));
    stats_view = reset_armed = speed_saved = run_done = false;
    for (int i = 0; i < KOCH_N; i++) koch_set[i] = koch[i].c;
    koch_set[KOCH_N] = 0;
    stats_load();

    btn_p1.next = &page_2;
    btn_p1.prev = &page_3;
    btn_p2.next = &page_3;
    btn_p2.prev = &page_1;
    btn_p3.next = &page_1;
    btn_p3.prev = &page_2;

    head = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_obj_set_pos(head, X, 10);
    head_right = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_obj_set_width(head_right, 300);
    lv_obj_set_style_text_align(head_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(head_right, X + W - 300, 10);

    /* Learn: result on the left, score and characters on the right. Practice: the
     * text uses the whole box and scrolls. */
    box = panel(dialog.obj, X, W);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    text_lbl = label(box, &sony_44, theme_hex(TC_text));
    lv_obj_set_width(text_lbl, W - 24);
    lv_label_set_long_mode(text_lbl, LV_LABEL_LONG_WRAP);

    big = label(box, &sony_44, COLOR_HEAD);
    lv_obj_set_width(big, W - SIDE_W - 40);
    lv_obj_set_pos(big, 0, 0);
    line1 = label(box, &sony_28, COLOR_DIM);
    lv_obj_set_pos(line1, 0, 90);
    line2 = label(box, &sony_28, COLOR_DIM);
    lv_obj_set_pos(line2, 0, 140);
    /* group_line() fits their text; what still does not fit is cut, not wrapped */
    lv_obj_set_width(line1, LINE_W);
    lv_label_set_long_mode(line1, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(line2, LINE_W);
    lv_label_set_long_mode(line2, LV_LABEL_LONG_CLIP);
    side_score = label(box, &sony_22, theme_hex(TC_text_value));
    lv_obj_set_width(side_score, SIDE_W - 12);
    lv_obj_set_pos(side_score, W - SIDE_W - 12, 0);
    side_chars = label(box, &sony_22, theme_hex(TC_text_value));
    lv_label_set_long_mode(side_chars, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(side_chars, SIDE_W - 12);
    lv_obj_set_pos(side_chars, W - SIDE_W - 12, 100);

    /* Stats: a grid of tiles for the characters of the level and the next ones */
    stats_cont = cont(box, W - 26, BOX_H - 26);
    for (int i = 0; i < HIST_N; i++) {
        tile[i] = lv_obj_create(stats_cont);
        lv_obj_remove_style_all(tile[i]);
        lv_obj_set_size(tile[i], TILE_W, TILE_H);
        lv_obj_set_pos(tile[i], (i % 10) * (TILE_W + TILE_GAP), (i / 10) * (TILE_H + TILE_GAP));
        lv_obj_set_style_bg_color(tile[i], TC(side), 0);
        lv_obj_set_style_bg_opa(tile[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile[i], 2, 0);
        lv_obj_set_style_radius(tile[i], 6, 0);
        tile_c[i] = label(tile[i], &sony_24, theme_hex(TC_text));
        lv_obj_align(tile_c[i], LV_ALIGN_TOP_MID, 0, 0);
        tile_p[i] = label(tile[i], &sony_14, COLOR_NONE);
        lv_obj_align(tile_p[i], LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    lv_obj_t *row = cont(stats_cont, W - 26, 40);
    lv_obj_set_pos(row, 0, 2 * TILE_H + TILE_GAP + 6);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_label_set_text(label(row, &sony_22, theme_hex(TC_text_value)), "Next new character:");
    st_next_c = label(row, &sony_30, COLOR_NEW);
    char tail[48];
    snprintf(tail, sizeof(tail), "(at %i%% on the current set)", PASS_PCT);
    lv_label_set_text(label(row, &sony_20, COLOR_DIM), tail);
    st_today = label(stats_cont, &sony_22, theme_hex(TC_text_value));
    lv_obj_set_pos(st_today, 0, 2 * TILE_H + TILE_GAP + 52);

    /* Copy: the level's characters, then Del and OK, under the lines; the MFK picks one without a keyboard */
    keys_cont = cont(box, KEYS_ROW * (KEY_W + KEY_GAP), 2 * KEY_H + KEY_GAP);
    lv_obj_set_pos(keys_cont, 0, KEYS_Y);
    for (int i = 0; i < KOCH_N + 2; i++) {
        key_o[i] = lv_obj_create(keys_cont);
        lv_obj_remove_style_all(key_o[i]);
        lv_obj_clear_flag(key_o[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(key_o[i], KEY_W, KEY_H);
        lv_obj_set_pos(key_o[i], (i % KEYS_ROW) * (KEY_W + KEY_GAP), (i / KEYS_ROW) * (KEY_H + KEY_GAP));
        lv_obj_set_style_bg_opa(key_o[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(key_o[i], 1, 0);
        lv_obj_set_style_radius(key_o[i], 4, 0);
        key_l[i] = label(key_o[i], &sony_22, theme_hex(TC_text));
        lv_obj_center(key_l[i]);
    }

    /* Calls: the callsign on the left, the score on the right */
    calls_cont = cont(box, CALLS_W - 26, BOX_H - 26);
    c_sent_cap = label(calls_cont, &sony_16, COLOR_DIM);
    lv_label_set_text(c_sent_cap, "SENT");
    lv_obj_set_pos(c_sent_cap, 0, 0);
    c_call = label(calls_cont, &sony_44, theme_hex(TC_text));
    lv_obj_set_style_text_letter_space(c_call, 4, 0);
    lv_obj_set_pos(c_call, 0, 20);
    c_key_cap = label(calls_cont, &sony_16, COLOR_DIM);
    lv_label_set_text(c_key_cap, "YOU KEYED");
    lv_obj_set_pos(c_key_cap, 0, 84);
    c_keyed = label(calls_cont, &sony_44, theme_hex(TC_text));
    lv_obj_set_style_text_letter_space(c_keyed, 4, 0);
    lv_obj_set_pos(c_keyed, 0, 104);
    c_res = label(calls_cont, &sony_22, COLOR_OK);
    lv_obj_set_pos(c_res, 0, 168);

    box_r = panel(dialog.obj, SCORE_X, SCORE_W);
    lv_obj_clear_flag(box_r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cap = label(box_r, &sony_16, COLOR_DIM);
    lv_label_set_text(cap, "SCORE");
    lv_obj_set_pos(cap, 0, 0);
    c_score = label(box_r, &sony_40, COLOR_NEW);
    lv_obj_set_pos(c_score, 0, 20);
    c_stats = label(box_r, &sony_22, theme_hex(TC_text_value));
    lv_obj_set_pos(c_stats, 0, 84);

    hint = label(dialog.obj, &sony_18, COLOR_DIM);
    lv_obj_set_pos(hint, X, DIALOG_H - 30);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    active = true;
    cw_decoder_set_sink(sink_text, sink_wpm);
    cw_trainer_set(true);
    timer = lv_timer_create(tick_cb, TIMER_MS, NULL);
    refresh_ui();
}

/* Every exit: the close button, another app, a key page, the mode timer */
static void destruct_cb(void) {
    active = false;
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    cw_trainer_set(false);
    cw_decoder_set_sink(NULL, NULL);
    cw_encoder_stop();
    cw_encoder_require_train(false);
    calls_leave();
    stats_save();

    if (mode_changed && is_cw()) {  /* not when the user changed the mode */
        cparam_i_set(cfg_cur_mode, prev_mode);
    }
    mode_changed = false;
    if (saved) {
        param_i_set(cfg_key_train, prev_train);
        saved = false;
    }
    lv_group_remove_obj(dialog.obj);
    main_screen_lock_mode(false);
    panel_update_visibility(false);
}
