/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW trainer. Runs only with the keyer in train mode (sidetone, no TX).
 *  Learn (Koch): the radio keys a group with the sidetone, the user keys it
 *  back with the paddle and the decoder (listening to the sidetone) tells
 *  if it was right. Practice: the keyed text is shown large.
 *  Calls (RufzXP style): the radio keys a callsign, the user keys it back;
 *  right: speed +1 WPM, wrong: -1 WPM. 50 calls make a run.
 *  Learn keeps the last results of every character (Stats view), saved in
 *  CWT_STATS_FILE together with today's totals and the Calls records.
 *
 *  Safety: key train is forced on while the app is open and restored when
 *  it closes, whatever closes it (destruct_cb runs for the close button,
 *  another app, a key page, or a mode change caught by the timer).
 *  The encoder is also told to never key while train is off.
 *  Calls changes the key speed: it is given back when leaving Calls too.
 */

#include "dialog_cw_trainer.h"

#include "buttons.h"
#include "cJSON.h"
#include "cfg/cfg_api.h"
#include "cw.h"
#include "cw_decoder.h"
#include "cw_encoder.h"
#include "dialog.h"
#include "events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "panel.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "styles.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TIMER_MS    100
#define GUARD_MS    600     /* the end of our own sidetone is still being decoded */
#define ANSWER_MS   3000    /* silence that ends an answer shorter than the group */
#define NEXT_OK_MS  1000    /* auto mode: pause before the next group */
#define NEXT_BAD_MS 2500    /* longer after a wrong answer, to read it */
#define WINDOW      10      /* results the accuracy is measured over */
#define PASS_PCT    90
#define HEARD_MAX   32
#define TEXT_MAX    400
#define TARGET_MAX  16
#define HIST_N      20      /* tries kept per character */
#define CALLS_RUN   50
#define CALLS_MIN   10      /* WPM */
#define CALLS_MAX   50
#define RESET_MS    3000    /* second press of Reset stats within this time */

#ifndef CWT_STATS_FILE
#define CWT_STATS_FILE "/mnt/cw_trainer.json"
#endif

#define X           16
#define W           (796 - 2 * X)
#define BOX_Y       44
#define BOX_H       (DIALOG_H - BOX_Y - 34)
#define SIDE_W      270
#define CALLS_X     18      /* Calls: call box, score box */
#define CALLS_W     480
#define SCORE_X     508
#define SCORE_W     274
#define TILE_W      64
#define TILE_H      52
#define TILE_GAP    10

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
_Static_assert(sizeof(koch) / sizeof(koch[0]) == 40, "stats arrays are sized for 40 characters");

typedef enum { MODE_LEARN, MODE_PRACTICE, MODE_CALLS } tmode_t;
typedef enum { ST_READY, ST_PLAYING, ST_LISTEN, ST_RESULT } state_t;

static tmode_t mode;
static state_t st;
static bool    active;      /* between construct and destruct */

/* Radio state to give back */
static bool           saved;
static int32_t        prev_train;
static bool           mode_changed;
static x6100_mode_t   prev_mode;

/* Learn */
static char     target[TARGET_MAX];
static char     heard[HEARD_MAX];
static int      heard_n;        /* decoded characters in heard[] */
static bool     heard_end;      /* word gap after the answer */
static uint32_t listen_t, last_rx_t;
static bool     last_ok;
static bool     autoplay;       /* Start: groups follow each other until Pause */
static uint32_t result_t;
static uint8_t  hist[WINDOW];   /* 1 = right, newest overwrites the oldest */
static int      hist_n, hist_pos;
static int      tried, right;

/* Stats: last HIST_N results of every Koch character (bit 0 = newest), today */
static struct {
    uint32_t mask[40];
    uint8_t  n[40];
    int      date;              /* yyyymmdd of the today fields */
    uint32_t tries, right, ms;
    uint32_t best_score;
    int      best_wpm;
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
static int  wpm_seen;

static lv_obj_t   *head, *head_right, *box, *big, *line1, *line2, *side_score, *side_chars, *hint, *text_lbl;
static lv_obj_t   *box_r, *calls_cont, *c_sent_cap, *c_call, *c_key_cap, *c_keyed, *c_res, *c_score, *c_stats;
static lv_obj_t   *stats_cont, *tile[HIST_N], *tile_c[HIST_N], *tile_p[HIST_N], *st_next_c, *st_today;
static lv_timer_t *timer;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void refresh_ui(void);
static void refresh_btn(button_data_t *b);

static void play_cb(button_data_t *b);
static void skip_cb(button_data_t *b);
static void mode_cb(button_data_t *b);
static void close_cb(button_data_t *b);
static void level_up_cb(button_data_t *b);
static void level_down_cb(button_data_t *b);
static void group_cb(button_data_t *b);
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
    return m == x6100_mode_cw || m == x6100_mode_cwr;
}

/* Decoder */

static void on_text(void *arg);
static void on_wpm(void *arg);

/* Audio thread: hand over to the LVGL thread */
static void sink_text(const char *t) {
    scheduler_put(on_text, (void *)t, strlen(t) + 1);
}

static void sink_wpm(int w) {
    scheduler_put(on_wpm, &w, sizeof(w));
}

static const char *morse_of(char c) {
    for (int i = 0; i < KOCH_N; i++) {
        if (koch[i].c == c) return koch[i].morse;
    }
    return "";
}

/* Morse of every character of s, separated by spaces */
static void morse_line(const char *s, char *out, size_t size) {
    out[0] = 0;
    for (; *s; s++) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, "%s%s", n ? "  " : "", morse_of(*s));
    }
}

static void on_wpm(void *arg) {
    if (!active) return;
    wpm_seen = *(int *)arg;
    if (mode == MODE_PRACTICE) refresh_ui();
}

static void on_text(void *arg) {
    const char *t = arg;
    if (!active) return;

    if (mode == MODE_PRACTICE) {
        size_t n = strlen(text), l = strlen(t);
        if (n + l >= TEXT_MAX) {    /* drop the oldest quarter */
            memmove(text, text + TEXT_MAX / 4, n - TEXT_MAX / 4 + 1);
            n = strlen(text);
        }
        snprintf(text + n, TEXT_MAX - n, "%s", t);
        refresh_ui();
        return;
    }

    /* Learn, Calls: only while listening, and not our own sidetone */
    if (st != ST_LISTEN || lv_tick_elaps(listen_t) < GUARD_MS) return;
    last_rx_t = lv_tick_get();
    if (t[0] == ' ') {
        heard_end = heard_n > 0;
    } else if (heard_n < (mode == MODE_CALLS ? 12 : 6)) {
        size_t n = strlen(heard);
        snprintf(heard + n, HEARD_MAX - n, "%s", strcmp(t, "<?>") == 0 ? "?" : t);
        heard_n++;
    }
    refresh_ui();
}

/* Stats */

static int koch_idx(char c) {
    for (int i = 0; i < KOCH_N; i++) {
        if (koch[i].c == c) return i;
    }
    return -1;
}

static int today_key(void) {
    time_t    t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* The today fields start again when the local date changes */
static void roll_today(void) {
    int d = today_key();
    if (stats.date != d) {
        stats.date = d;
        stats.tries = stats.right = stats.ms = 0;
    }
}

/* Accuracy of the last tries of a character, -1 when untried */
static int stat_pct(int i) {
    int n = stats.n[i];
    if (!n) return -1;
    return __builtin_popcount(stats.mask[i] & ((1u << n) - 1)) * 100 / n;
}

static void stat_add(int i, bool ok) {
    stats.mask[i] = ((stats.mask[i] << 1) | (ok ? 1 : 0)) & ((1u << HIST_N) - 1);
    if (stats.n[i] < HIST_N) stats.n[i]++;
}

/* Time of the round that just ended (capped: the user may have walked away) */
static void stat_time(void) {
    uint32_t d = lv_tick_elaps(round_t);
    stats.ms += d > 60000 ? 60000 : d;
}

static uint32_t json_u(const cJSON *o, const char *key, double max) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0) return 0;
    return (uint32_t)(v->valuedouble > max ? max : v->valuedouble);
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
            stats.tries      = json_u(root, "tries", 1e9);
            stats.right      = json_u(root, "right", 1e9);
            stats.ms         = json_u(root, "sec", 1e6) * 1000;
            stats.best_score = json_u(root, "best_score", 1e9);
            stats.best_wpm   = json_u(root, "best_wpm", 99);
            if (stats.right > stats.tries) stats.right = stats.tries;

            const cJSON *chars = cJSON_GetObjectItemCaseSensitive(root, "chars");
            for (int i = 0; i < KOCH_N; i++) {
                char         k[2] = {koch[i].c, 0};
                const cJSON *a    = cJSON_GetObjectItemCaseSensitive(chars, k);
                if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 2) continue;
                const cJSON *vn = cJSON_GetArrayItem(a, 0), *vm = cJSON_GetArrayItem(a, 1);
                if (!cJSON_IsNumber(vn) || !cJSON_IsNumber(vm)) continue;
                double n_     = vn->valuedouble, m_ = vm->valuedouble;
                stats.n[i]    = n_ < 0 ? 0 : (n_ > HIST_N ? HIST_N : (int)n_);
                stats.mask[i] = m_ < 0 ? 0 : ((uint32_t)(m_ > 4e9 ? 4e9 : m_) & ((1u << HIST_N) - 1));
            }
        }
        cJSON_Delete(root);
    }
    roll_today();
}

/* Atomic: a power loss leaves the old file or the new one */
static void stats_save(void) {
    cJSON *root  = cJSON_CreateObject();
    cJSON *chars = root ? cJSON_AddObjectToObject(root, "chars") : NULL;
    if (!chars) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddNumberToObject(root, "date", stats.date);
    cJSON_AddNumberToObject(root, "tries", stats.tries);
    cJSON_AddNumberToObject(root, "right", stats.right);
    cJSON_AddNumberToObject(root, "sec", stats.ms / 1000);
    cJSON_AddNumberToObject(root, "best_score", stats.best_score);
    cJSON_AddNumberToObject(root, "best_wpm", stats.best_wpm);
    for (int i = 0; i < KOCH_N; i++) {
        char   k[2] = {koch[i].c, 0};
        cJSON *a    = stats.n[i] ? cJSON_AddArrayToObject(chars, k) : NULL;
        if (!a) continue;
        cJSON_AddItemToArray(a, cJSON_CreateNumber(stats.n[i]));
        cJSON_AddItemToArray(a, cJSON_CreateNumber(stats.mask[i]));
    }
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
    int n = stats.n[i];
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

static int level(void) {
    int l = param_i_get(cfg_cwt_level);
    return l < 2 ? 2 : (l > KOCH_N ? KOCH_N : l);
}

static void reset_score(void) {
    hist_n = hist_pos = 0;
}

static void set_level(int l) {
    if (l < 2 || l > KOCH_N) return;
    param_i_set(cfg_cwt_level, l);
    reset_score();
}

static void new_target(void) {
    int len = param_i_get(cfg_cwt_group);
    int l   = level();
    for (int i = 0; i < len; i++) {
        /* the newest character comes up more often, then the weak ones */
        int k     = (rand() % 3 == 0) ? l - 1 : pick_char(l);
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
    heard[0] = 0;
    heard_n  = 0;
    heard_end = false;
    st       = ST_PLAYING;
    cw_encoder_send(target, false);
    return true;
}

static void evaluate(void) {
    last_ok = strcmp(heard, target) == 0;
    tried++;
    right += last_ok;
    hist[hist_pos] = last_ok;
    hist_pos       = (hist_pos + 1) % WINDOW;
    if (hist_n < WINDOW) hist_n++;
    st = ST_RESULT;
    result_t = lv_tick_get();

    roll_today();
    stats.tries++;
    stats.right += last_ok;
    stat_time();
    size_t tl = strlen(target), hl = strlen(heard);
    for (size_t i = 0; i < tl; i++) {
        int k = koch_idx(target[i]);
        if (k < 0) continue;
        /* a dropped or extra character shifts the rest: then only count what was heard */
        stat_add(k, last_ok || (hl == tl ? heard[i] == target[i] : strchr(heard, target[i]) != NULL));
    }
    stats_save();

    if (hist_n == WINDOW) {
        int good = 0;
        for (int i = 0; i < WINDOW; i++) good += hist[i];
        if (good * 100 >= PASS_PCT * WINDOW && level() < KOCH_N) {
            set_level(level() + 1);
            msg_update_text_fmt("Level %i: new character %c", level(), koch[level() - 1].c);
        }
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

static void calls_enter(void) {
    if (!speed_saved) {     /* a new run keeps the speed saved when Calls was entered */
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

/* The key speed is also the paddle speed: give the user's back */
static void calls_leave(void) {
    if (!speed_saved) return;
    param_i_set(cfg_key_speed, base_wpm);
    speed_saved = false;
}

/* Next call of the run, keyed at the speed the last answer earned */
static void start_call(void) {
    call_no++;
    call_wpm = next_wpm;
    param_i_set(cfg_key_speed, call_wpm);
    new_call();
    round_t = lv_tick_get();
    if (!play()) {
        autoplay = false;
        call_no--;
        st = ST_READY;
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

    if (mode == MODE_PRACTICE) return;
    if (st == ST_PLAYING && cw_encoder_state() == CW_ENCODER_IDLE) {
        st        = ST_LISTEN;
        listen_t  = lv_tick_get();
        last_rx_t = listen_t;
        refresh_ui();
    } else if (st == ST_LISTEN && heard_n > 0 &&
               (heard_n >= (int)strlen(target) || heard_end || lv_tick_elaps(last_rx_t) > ANSWER_MS)) {
        if (mode == MODE_CALLS) evaluate_call();
        else evaluate();
        refresh_ui();
    } else if (st == ST_RESULT && autoplay && lv_tick_elaps(result_t) > (last_ok ? NEXT_OK_MS : NEXT_BAD_MS)) {
        if (mode == MODE_CALLS) {
            start_call();
        } else {
            new_target();
            play();
        }
        refresh_ui();
    }
}

/* Buttons */

/* Nothing of a round stays when the mode or the view changes */
static void stop_round(void) {
    cw_encoder_stop();
    autoplay  = false;
    st        = ST_READY;
    target[0] = 0;
    heard[0]  = 0;
    heard_n   = 0;
}

static void play_cb(button_data_t *b) {
    (void)b;
    if (mode == MODE_PRACTICE || stats_view) return;
    if (mode == MODE_LEARN && st == ST_LISTEN) {    /* Replay */
        play();
    } else if (autoplay) {          /* Pause: the current group still ends */
        autoplay = false;
    } else {                        /* Start */
        autoplay = true;
        if (mode == MODE_CALLS) {
            if (run_done) calls_enter();    /* a new run */
            if (st != ST_PLAYING && st != ST_LISTEN) start_call();
        } else if (st != ST_PLAYING) {
            new_target();
            play();
        }
    }
    refresh_ui();
}

static void skip_cb(button_data_t *b) {
    (void)b;
    if (mode == MODE_PRACTICE || stats_view) return;
    if (mode == MODE_CALLS) {       /* Replay */
        if (st == ST_LISTEN) play();
    } else {
        cw_encoder_stop();
        new_target();
        play();
    }
    refresh_ui();
}

static void mode_cb(button_data_t *b) {
    (void)b;
    stop_round();
    calls_leave();
    mode       = (tmode_t)((mode + 1) % 3);
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
    if (mode != MODE_LEARN) return;
    set_level(level() + 1);
    refresh_ui();
}

static void level_down_cb(button_data_t *b) {
    (void)b;
    if (mode != MODE_LEARN) return;
    set_level(level() - 1);
    refresh_ui();
}

static void group_cb(button_data_t *b) {
    (void)b;
    if (mode != MODE_LEARN) return;
    param_i_set(cfg_cwt_group, param_i_get(cfg_cwt_group) % 5 + 1);
    refresh_ui();
}

/* The key speed is also the paddle speed of the keyer (Calls sets it itself) */
static void change_speed(int d) {
    if (mode == MODE_CALLS) return;
    int v = param_i_get(cfg_key_speed) + d;
    if (v < 5 || v > 50) return;
    param_i_set(cfg_key_speed, v);
    refresh_ui();
}

/* Stats view: nothing is playing while it is open */
static void stats_cb(button_data_t *b) {
    (void)b;
    if (mode != MODE_LEARN) return;
    stop_round();
    stats_view  = !stats_view;
    reset_armed = false;
    refresh_ui();
}

/* Two presses within RESET_MS */
static void reset_cb(button_data_t *b) {
    (void)b;
    if (mode != MODE_LEARN) return;
    if (reset_armed && lv_tick_elaps(reset_t) <= RESET_MS) {
        reset_armed = false;
        memset(stats.mask, 0, sizeof(stats.mask));
        memset(stats.n, 0, sizeof(stats.n));
        stats.tries = stats.right = stats.ms = 0;
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
    return autoplay ? "Pause" : "Start";
}

static const char *mode_label(void) {
    return mode == MODE_LEARN ? "Mode:\nLearn" : (mode == MODE_PRACTICE ? "Mode:\nPractice" : "Mode:\nCalls");
}

static const char *skip_label(void) {
    return mode == MODE_CALLS ? "Replay" : "Skip";
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
    snprintf(label_buf[2], sizeof(label_buf[2]), "Group\n%i", (int)param_i_get(cfg_cwt_group));
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

/* 2115 -> "2 115" */
static void fmt_num(uint32_t v, char *out, size_t size) {
    if (v >= 1000000) snprintf(out, size, "%u %03u %03u", (unsigned)(v / 1000000), (unsigned)(v / 1000 % 1000), (unsigned)(v % 1000));
    else if (v >= 1000) snprintf(out, size, "%u %03u", (unsigned)(v / 1000), (unsigned)(v % 1000));
    else snprintf(out, size, "%u", (unsigned)v);
}

static void refresh_calls(void) {
    char buf[200], n1[16], n2[16];
    bool rev  = st == ST_RESULT;
    int  done = (st == ST_PLAYING || st == ST_LISTEN) ? call_no - 1 : call_no;

    lv_label_set_text_fmt(head, "CW TRAINER - #%06x Calls#", theme_hex(TC_text));
    snprintf(buf, sizeof(buf), "Call %i/%i - %i WPM", call_no, CALLS_RUN, call_no ? call_wpm : next_wpm);
    lv_label_set_text(head_right, buf);

    show(c_sent_cap, rev);
    show(c_key_cap, st == ST_LISTEN || rev);
    lv_obj_set_style_text_color(c_call, lv_color_hex(rev ? theme_hex(TC_text) : (st == ST_READY ? COLOR_HEAD : COLOR_NEW)), 0);
    lv_label_set_text(c_call, rev ? target : (st == ST_PLAYING ? "Listen..." : (st == ST_LISTEN ? "Your turn" : "Press Start")));

    /* wrong characters in red once the call is revealed */
    char *p = buf;
    for (int i = 0; heard[i] && rev; i++) {
        bool bad = target[i] == 0 || heard[i] != target[i];
        p += bad ? sprintf(p, "#%06x %c#", theme_hex(TC_err), heard[i]) : sprintf(p, "%c", heard[i]);
    }
    *p = 0;
    lv_label_set_text(c_keyed, rev ? buf : heard);

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
    lv_label_set_text(hint, run_done && rev ? "Start: a new run of 50 callsigns" : "Key back the callsign with the paddle. Right: +1 WPM, wrong: -1 WPM.");

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

    lv_label_set_text_fmt(head, "CW TRAINER - #%06x Stats#", theme_hex(TC_text));
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
    snprintf(buf, sizeof(buf), "Today: %u tries, %u%% right, %u min", (unsigned)stats.tries,
             stats.tries ? (unsigned)(stats.right * 100 / stats.tries) : 0u, (unsigned)((stats.ms + 30000) / 60000));
    lv_label_set_text(st_today, buf);
    lv_label_set_text(hint, "Last 20 tries per character. Weak ones (red, yellow) come up more often in Learn.");
}

static void refresh_ui(void) {
    bool learn = mode == MODE_LEARN, calls = mode == MODE_CALLS, sv = learn && stats_view;
    int  wpm   = param_i_get(cfg_key_speed);
    char m1[48], m2[48], buf[160];

    set_btn(&btn_play, mode == MODE_PRACTICE || sv);
    set_btn(&btn_skip, mode == MODE_PRACTICE || sv);
    set_btn(&btn_lvl_u, !learn);
    set_btn(&btn_lvl_d, !learn);
    set_btn(&btn_group, !learn);
    set_btn(&btn_stats, !learn);
    set_btn(&btn_reset, !learn);
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
    show(big, learn && !sv);
    show(line1, learn && !sv);
    show(line2, learn && !sv);
    show(text_lbl, mode == MODE_PRACTICE);
    show(side_score, learn && !sv);
    show(side_chars, learn && !sv);

    if (calls) {
        refresh_calls();
        return;
    }
    if (sv) {
        refresh_stats();
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
    snprintf(buf, sizeof(buf), "%i WPM - group %i", wpm, (int)param_i_get(cfg_cwt_group));
    lv_label_set_text(head_right, buf);

    lv_label_set_text(line1, "");
    lv_label_set_text(line2, "");
    switch (st) {
        case ST_READY:
            lv_label_set_text(big, "Press Play");
            lv_obj_set_style_text_color(big, lv_color_hex(COLOR_HEAD), 0);
            lv_label_set_text(hint, "Start: groups follow each other, key each one back with the paddle");
            break;
        case ST_PLAYING:
            lv_label_set_text(big, "Listen...");
            lv_obj_set_style_text_color(big, lv_color_hex(COLOR_NEW), 0);
            lv_label_set_text(hint, "The radio keys the group (sidetone only)");
            /* The group stays on screen while playing and answering, to memorise it */
            morse_line(target, m1, sizeof(m1));
            snprintf(buf, sizeof(buf), "Sent:   #%06x %s#   %s", theme_hex(TC_accent), target, m1);
            lv_label_set_text(line1, buf);
            lv_label_set_text(line2, "");
            break;
        case ST_LISTEN:
            lv_label_set_text(big, heard[0] ? heard : "Your turn");
            lv_obj_set_style_text_color(big, lv_color_hex(heard[0] ? theme_hex(TC_text) : COLOR_NEW), 0);
            lv_label_set_text(hint, "Key the group with the paddle. Replay: hear it again");
            morse_line(target, m1, sizeof(m1));
            snprintf(buf, sizeof(buf), "Sent:   #%06x %s#   %s", theme_hex(TC_accent), target, m1);
            lv_label_set_text(line1, buf);
            morse_line(heard, m2, sizeof(m2));
            snprintf(buf, sizeof(buf), "Heard: #%06x %s#   %s", theme_hex(TC_text), heard, m2);
            lv_label_set_text(line2, heard[0] ? buf : "");
            break;
        case ST_RESULT:
            lv_label_set_text(big, last_ok ? "OK" : "WRONG");
            lv_obj_set_style_text_color(big, lv_color_hex(last_ok ? COLOR_OK : COLOR_BAD), 0);
            morse_line(target, m1, sizeof(m1));
            morse_line(heard, m2, sizeof(m2));
            snprintf(buf, sizeof(buf), "Sent:   #%06x %s#   %s", theme_hex(TC_accent), target, m1);
            lv_label_set_text(line1, buf);
            snprintf(buf, sizeof(buf), "Heard: #%s %s#   %s", last_ok ? "60ff60" : "ff6060", heard, m2);
            lv_label_set_text(line2, buf);
            lv_label_set_text(hint, autoplay ? "Next group follows. Pause: stop after this one" : "Start: resume. Skip: another group");
            break;
    }

    int good = 0;
    for (int i = 0; i < hist_n; i++) good += hist[i];
    snprintf(buf, sizeof(buf), "Score  %i / %i\nLast %i: %i / %i\nNext level at %i%%", right, tried, WINDOW, good, hist_n, PASS_PCT);
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
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_LEFT:
        case LV_KEY_DOWN:
            change_speed(-1);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_UP:
            change_speed(1);
            break;
        case LV_KEY_ENTER:
        case KEYBOARD_F4:
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

/* MFK press: the encoder sends a click, not ENTER. */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) play_cb(NULL);
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
        prev_mode = (x6100_mode_t)cparam_i_get(cfg_cur_mode);
        cparam_i_set(cfg_cur_mode, x6100_mode_cw);
    }
    main_screen_lock_mode(true);
    panel_hide();

    srand(time(NULL));
    mode = MODE_LEARN;
    st   = ST_READY;
    autoplay = false;
    target[0] = heard[0] = text[0] = 0;
    heard_n = tried = right = wpm_seen = 0;
    reset_score();
    stats_view = reset_armed = speed_saved = run_done = false;
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
