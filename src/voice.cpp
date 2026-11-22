/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - spoken feedback (accessibility)
 *
 *  From Jimr9's OpenX6X00 Speech Access (github.com/Jimr9/OpenX6X00-Speech-Access) and his X6100 Accessibility fork
 *  before it, whose engine was R1CBU's (Copyright (c) 2022-2023 Belousov Oleg aka R1CBU): what is said, when, the
 *  debounce of the knobs and the name said once and then its values are theirs.
 *
 *  An announcement is handed to one thread of this file, which waits its delay out, synthesizes it (speech_engine.c:
 *  RHVoice, loaded the first time) and plays it. The GUI thread never waits for that thread: a newer announcement
 *  replaces the one waiting and cuts the one playing within 20 ms of audio. While it plays the speaker is claimed
 *  (AUDIO_USER_SPEECH, audio_owner.h), so the base plays this app's audio and gives the speaker back after; with
 *  Bluetooth headphones on it plays in them and the speaker is left alone.
 *
 *  Off (Settings > Voice > Voice mode, the default) costs nothing: no thread, no library loaded, and every call
 *  in here returns at its first test. Nothing is said on the air: radio.c cuts an announcement before it keys.
 */

#include "voice.h"

#include "cfg/cfg_api.h"
#include "util.h"       /* C++ aware itself (helpers.h): outside the C block */

extern "C" {
#include <unistd.h>
#include <stdio.h>

#include "lvgl/lvgl.h"

#include "audio.h"
#include "btaudio.h"
#include "msg.h"
#include "radio.h"
#include "recorder.h"
#include "screen_light.h"
#include "speech_engine.h"
#include "voice_messages.h"
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <thread>

#define SURE_MS      1500   /* voice_sure(): what is said within this long is said whatever the mode */
#define STOP_WAIT_MS 250    /* voice_stop(): at most this long for the announcement playing to fall silent */

/* Every attempt to speak, see voice_say_count() */
static std::atomic<uint32_t> say_attempts{0};
static std::atomic<bool>     auto_speech{false};

/* The announcement being put together, on the GUI thread */
static char   buf[1024];
/* The length of the prompt in buf: unchanged from the last announcement, after a few repeats only what follows it
 * is said (a control's value changing, its name the same). 0: all of it, always */
static size_t prompt_len = 0;

/* What the speaking thread is given, under mux. The three are never destroyed: the thread waits on them for as long
 * as the program runs, and a condition variable destroyed at exit waits for its waiter, which never comes back (the
 * GUI would hang as it ends) */
static std::mutex              &mux  = *new std::mutex;
static std::condition_variable &wake = *new std::condition_variable;   /* something to say, or one taken back */
static std::condition_variable &idle = *new std::condition_variable;   /* the announcement playing has ended */
static bool                                  worker_up;
static char                                  pend[sizeof(buf)];
static size_t                                pend_prompt;
static bool                                  pend_set;
static std::chrono::steady_clock::time_point pend_due;
static std::atomic<bool>                     speaking{false};   /* from its delay's end to its last sample */
static std::atomic<bool>                     active{false};     /* voice_busy(): waiting or speaking */
/* One more with every announcement handed over and every voice_stop(): the one playing ends when it is not its own */
static std::atomic<uint32_t>                 gen{0};

static bool     sure_set;
static uint32_t sure_at;

/* The voices of the card image (buildroot/local-build.sh): RHVoice's four English voices under free terms */
voice_item_t voice_item[VOICES_NUM] = {
    {.name = "slt",  .label = "SLT (En, female)",  .welcome = "Hello. This is voice S L T"},
    {.name = "alan", .label = "Alan (En, male)",   .welcome = "Hello. This is voice Alan"},
    {.name = "clb",  .label = "CLB (En, female)",  .welcome = "Hello. This is voice C L B"},
    {.name = "bdl",  .label = "BDL (En, male)",    .welcome = "Hello. This is voice B D L"},
};

static int voice_index() {
    int n = param_i_get(cfg_voice_lang);

    return (n >= 0 && n < VOICES_NUM) ? n : 0;
}

/* Test builds: every request, refusal and start written to /tmp/voice.log (RAM, gone at power off) */
void voice_trace(const char *fmt, ...) {
    static FILE *f;
    static int   enabled = -1;
    va_list      args;

    if (enabled < 0) {
        enabled = access("/mnt/voice-trace", F_OK) == 0;
    }
    if (!enabled) {
        return;
    }
    if (!f) {
        f = fopen("/tmp/voice.log", "a");
        if (!f) {
            return;
        }
        setvbuf(f, NULL, _IOLBF, 0);
    }
    fprintf(f, "%u ", lv_tick_get());
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputs("\n", f);
}

bool voice_busy() {
    return active;
}

bool voice_available() {
    return speech_engine_available();
}

void voice_sure() {
    sure_at  = lv_tick_get();
    sure_set = true;
}

static bool forced() {
    return sure_set && lv_tick_elaps(sure_at) < SURE_MS;
}

/* Speech asked for: not Off, or a setting of the speech itself just changed. The first test of every call in here:
 * with speech Off nothing else is looked at, and the library is not loaded */
static bool wanted() {
    return param_i_get(cfg_voice_mode) != VOICE_OFF || forced();
}

static bool speech_on() {
    return wanted() && speech_engine_available();
}

/* ---- The speaking thread ---- */

static bool still_wanted(void *arg) {
    return gen.load() == (uint32_t)(uintptr_t)arg;
}

/* The announcement is about to be synthesized: the speaker taken now, not while the synthesizer is still loading */
static void audio_starts(void *arg) {
    bool *claimed = (bool *)arg;

    if (!*claimed) {
        audio_user_claim(AUDIO_USER_SPEECH);
        *claimed = true;
    }
}

static void say(const char *text, uint32_t mine, bool *claimed) {
    /* Bluetooth headphones on: the announcement in them, the speaker left as it is */
    char sink[128];
    bool phones = btaudio_hp_sink(sink, sizeof(sink));

    speech_say_t how = {};

    how.voice  = voice_item[voice_index()].name;
    how.rate   = param_i_get(cfg_voice_rate) / 100.0;
    how.pitch  = param_i_get(cfg_voice_pitch) / 100.0;
    how.volume = param_i_get(cfg_voice_volume) / 100.0;
    how.sink   = phones ? sink : NULL;
    how.go_on  = still_wanted;
    how.arg    = (void *)(uintptr_t)mine;
    if (!phones) {
        how.starts     = audio_starts;
        how.starts_arg = claimed;
    }
    voice_trace("speak: %s", text);
    voice_trace("spoke: %i", (int)speech_engine_say(text, &how));
}

static void worker() {
    static char     text[sizeof(buf)], prev[sizeof(buf)];
    static uint16_t repeated;

    std::unique_lock<std::mutex> lock(mux);

    for (;;) {
        while (!pend_set) {
            active = false;
            wake.wait(lock);
        }
        /* The delay: a newer announcement starts it again with its own, voice_stop() ends it */
        while (pend_set && std::chrono::steady_clock::now() < pend_due) {
            wake.wait_until(lock, pend_due);
        }
        if (!pend_set) {
            continue;
        }

        size_t   plen = pend_prompt;
        uint32_t mine = gen;

        snprintf(text, sizeof(text), "%s", pend);
        pend_set = false;
        speaking = true;
        lock.unlock();

        /* The same prompt as the last announcement's: only what follows it, the prompt again every few times */
        const char *ptr          = text;
        int         repeat_count = param_i_get(cfg_voice_repeat_count);

        if (plen > 0 && plen < sizeof(text) && repeat_count > 0 && strncmp(text, prev, plen) == 0) {
            repeated++;
            if (repeated >= repeat_count) {
                repeated = 0;
            } else {
                ptr = text + plen;
            }
        } else {
            repeated = 0;
        }
        snprintf(prev, sizeof(prev), "%s", text);

        bool claimed = false;

        say(ptr, mine, &claimed);

        /* Silent from here: voice_stop() waits for no more than this. The speaker is given back after it, so that a
         * transmission starting (radio.c, which may hold what the release needs) is not kept waiting */
        lock.lock();
        speaking = false;
        idle.notify_all();
        lock.unlock();
        if (claimed) {
            audio_user_release(AUDIO_USER_SPEECH);
        }
        lock.lock();
    }
}

/* buf to the speaking thread, said after say_delay microseconds unless a newer one comes first. Never waits */
static void start(uint32_t say_delay);

void voice_stop() {
    std::unique_lock<std::mutex> lock(mux);

    if (!worker_up) {
        return;         /* nothing was ever said */
    }
    voice_trace("stop");
    pend_set = false;
    gen++;
    wake.notify_all();
    idle.wait_for(lock, std::chrono::milliseconds(STOP_WAIT_MS), [] { return !speaking.load(); });
}

void voice_change_mode() {
    if (!speech_engine_available()) {
        msg_update_text_fmt("Speech is not on this card image");
        return;
    }
    voice_sure();

    switch (param_i_get(cfg_voice_mode)) {
        case VOICE_OFF:
            param_i_set(cfg_voice_mode, VOICE_LCD);
            voice_msg_hold(true);
            msg_update_text_fmt("Speech: when the screen is off");
            voice_msg_hold(false);
            voice_say_text_fmt("Speech when the screen is off");
            break;

        case VOICE_LCD:
            param_i_set(cfg_voice_mode, VOICE_ALWAYS);
            voice_msg_hold(true);
            msg_update_text_fmt("Speech: always");
            voice_msg_hold(false);
            voice_say_text_fmt("Speech always");
            break;

        default:
            param_i_set(cfg_voice_mode, VOICE_OFF);
            voice_msg_hold(true);
            msg_update_text_fmt("Speech: off");
            voice_msg_hold(false);
            voice_say_text_fmt("Speech off. Hold D F N for speech again");
            break;
    }
}

/* On the air in any way: a transmission, an ATU tuning cycle or an SWR scan, both of which key a carrier. Speech
 * waits for the receive state: never anything near the transmitter's audio path */
static bool on_air() {
    radio_state_t st = radio_get_state();

    return st == RADIO_TX || st == RADIO_ATU_START || st == RADIO_ATU_WAIT || st == RADIO_ATU_RUN ||
           st == RADIO_SWRSCAN;
}

static bool voice_enable() {
    say_attempts++;

    if (!speech_on()) {
        return false;
    }
    /* Never on the air: in Play mode the base sends this app's audio in place of the mic (tx_prepare() in
     * radio.c also cuts off what is still playing as a transmission starts) */
    if (recorder_is_on() || on_air() || dialog_msg_voice_get_state() == MSG_VOICE_RECORD) {
        voice_trace("refused: recorder %i, on air %i (state %i), voice message rec %i", recorder_is_on(), on_air(),
                    (int)radio_get_state(), dialog_msg_voice_get_state() == MSG_VOICE_RECORD);
        return false;
    }
    if (!forced() && param_i_get(cfg_voice_mode) == VOICE_LCD && !backlight_is_dark()) {
        voice_trace("refused: screen on, mode screen off only");
        return false;
    }
    /* Without interrupting, a new announcement is dropped while one speaks */
    if (speaking && !param_i_get(cfg_voice_interrupt)) {
        voice_trace("refused: speaking, interrupting off");
        return false;
    }
    return true;
}

/* voice_say_prefix(): said in front of the next announcement */
static char     prefix[96];
static uint32_t prefix_at;

static void prefix_timer_cb(lv_timer_t *t) {
    (void)t;
    if (prefix[0]) {
        char said[sizeof(prefix)];

        snprintf(said, sizeof(said), "%s", prefix);
        prefix[0] = '\0';
        voice_say_text_fmt("%s", said);     /* nothing came after it: alone */
    }
}

void voice_say_prefix(const char *text) {
    if (!text || !text[0] || !speech_on()) {
        prefix[0] = '\0';
        return;
    }
    snprintf(prefix, sizeof(prefix), "%s", text);
    prefix_at = lv_tick_get();
    lv_timer_set_repeat_count(lv_timer_create(prefix_timer_cb, 400, NULL), 1);
}

static void start(uint32_t say_delay) {
    if (prefix[0]) {
        if (lv_tick_elaps(prefix_at) < 400) {
            size_t plen = strlen(prefix) + 2;
            size_t blen = strlen(buf);

            if (plen + blen < sizeof(buf)) {
                memmove(buf + plen, buf, blen + 1);
                memcpy(buf, prefix, plen - 2);
                buf[plen - 2] = ',';
                buf[plen - 1] = ' ';
                prompt_len = prompt_len ? prompt_len + plen : 0;
            }
        }
        prefix[0] = '\0';
    }
    voice_trace("say (%u ms): %s", say_delay / 1000, buf);
    {
        std::lock_guard<std::mutex> lock(mux);

        if (!worker_up) {
            try {
                std::thread(worker).detach();
                worker_up = true;
            } catch (...) {
                return;
            }
        }
        snprintf(pend, sizeof(pend), "%s", buf);
        pend_prompt = prompt_len;
        pend_due    = std::chrono::steady_clock::now() + std::chrono::microseconds(say_delay);
        pend_set    = true;
        active      = true;
        gen++;          /* the one playing ends here */
    }
    wake.notify_all();
}

/* The wait before a knob's announcement: only the value it lands on is said. With interrupting on,
 * short (Settings > Voice), else the full second the engine always had */
static uint32_t voice_debounce_delay() {
    if (!param_i_get(cfg_voice_interrupt)) {
        return 1000000;
    }
    return (uint32_t)param_i_get(cfg_voice_interrupt_delay_ms) * 1000;
}

/* The VFO knob turns faster and longer than any other: a delay of its own */
static uint32_t voice_freq_debounce_delay() {
    if (!param_i_get(cfg_voice_interrupt)) {
        return 1000000;
    }
    return (uint32_t)param_i_get(cfg_voice_freq_interrupt_delay_ms) * 1000;
}

void voice_delay_say_text_fmt(const char *fmt, ...) {
    if (!voice_enable()) {
        return;
    }

    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    prompt_len = 0;
    start(voice_debounce_delay());
}

void voice_say_text_fmt(const char *fmt, ...) {
    if (!voice_enable()) {
        return;
    }

    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    prompt_len = 0;
    start(0);
}

void voice_say_prompted_fmt(const char *prompt, const char *fmt, ...) {
    if (!voice_enable()) {
        return;
    }

    size_t plen = strlen(prompt);

    if (plen >= sizeof(buf)) {
        plen = sizeof(buf) - 1;
    }
    memcpy(buf, prompt, plen);

    va_list args;

    va_start(args, fmt);
    vsnprintf(buf + plen, sizeof(buf) - plen, fmt, args);
    va_end(args);
    prompt_len = plen;
    start(voice_debounce_delay());
}

/* "14.074" or "7" or "14.074.5": the megahertz said first, so that after a few repeats on the same
 * band only the kilohertz are */
static size_t freq_text(uint64_t freq, char *out, size_t size) {
    uint16_t mhz, khz, hz;
    int      n;

    split_freq((int32_t)freq, &mhz, &khz, &hz);
    if (!hz && !khz) {
        snprintf(out, size, "%i", mhz);
        return 0;
    }
    n = snprintf(out, size, "%i.", mhz);
    size_t pos = (n > 0 && (size_t)n < size) ? (size_t)n : 0;

    /* Three digits each: 14.074 said "14.74" was the same as 14.740 */
    if (hz) {
        snprintf(out + pos, size - pos, "%03i.%03i", khz, hz);
    } else {
        snprintf(out + pos, size - pos, "%03i", khz);
    }
    return pos;
}

void voice_freq_text(uint64_t freq, char *out, size_t size) {
    if (out && size) {
        freq_text(freq, out, size);
    }
}

void voice_mode_text(const char *name, char *out, size_t size) {
    size_t n = 0;

    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    for (const char *p = name; p && *p && n + 12 < size; p++) {
        if (*p == '-' && p[1] == 'D' && !p[2]) {
            n += snprintf(out + n, size - n, " digital");
            break;
        }
        if (*p == '-' && p[1] == 'R' && !p[2]) {
            n += snprintf(out + n, size - n, " reverse");
            break;
        }
        if (n) {
            out[n++] = ' ';
        }
        out[n++] = *p;
        out[n] = '\0';
    }
}

static void voice_say_freq_impl(uint64_t freq, uint32_t say_delay) {
    if (!voice_enable()) {
        return;
    }
    prompt_len = freq_text(freq, buf, sizeof(buf));
    start(say_delay);
}

void voice_say_freq(uint64_t freq) {
    voice_say_freq_impl(freq, voice_freq_debounce_delay());
}

void voice_say_freq_now(uint64_t freq) {
    voice_say_freq_impl(freq, 0);
}

/* "F T 8, 14.074" in one announcement: two would cut each other off */
void voice_say_prompted_freq_now(const char *prompt, uint64_t freq) {
    if (!voice_enable()) {
        return;
    }

    int    n = snprintf(buf, sizeof(buf), "%s, ", prompt);
    size_t pos = (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;

    freq_text(freq, buf + pos, sizeof(buf) - pos);
    prompt_len = 0;
    start(0);
}

/* On and off in full every time: the name of a toggle is worth more than on a knob */
void voice_say_bool(const char *prompt, bool x) {
    voice_delay_say_text_fmt("%s is %s", prompt, x ? "on" : "off");
}

void voice_say_int(const char *prompt, int32_t x) {
    voice_say_prompted_fmt(prompt, " %i", x);
}

void voice_say_float(const char *prompt, float x) {
    voice_say_prompted_fmt(prompt, " %.1f", x);
}

void voice_say_float2(const char *prompt, float x) {
    voice_say_prompted_fmt(prompt, " %.2f", x);
}

void voice_say_text(const char *prompt, const char *x) {
    voice_say_prompted_fmt(prompt, " %s", x);
}

void voice_say_lang() {
    voice_delay_say_text_fmt("%s", voice_item[voice_index()].welcome);
}

/* ---- The helpers of voice.h ---- */

uint32_t voice_say_count() {
    return say_attempts.load();
}

void voice_count_bump() {
    say_attempts++;
}

void voice_auto_set(bool on) {
    auto_speech = on;
}

bool voice_auto_active() {
    return auto_speech && speech_on() && !on_air();
}

bool voice_is_on() {
    return param_i_get(cfg_voice_mode) != VOICE_OFF && speech_engine_available();
}

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void voice_clean_text(const char *in, char *out, size_t size) {
    size_t n = 0;

    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    auto put = [&](const char *s) {
        for (; *s && n + 1 < size; s++) {
            if (*s == ' ' && (n == 0 || out[n - 1] == ' ')) {
                continue;
            }
            if (*s == ',' && (n == 0 || out[n - 1] == ',')) {
                continue;
            }
            if (*s == ',' && n > 0 && out[n - 1] == ' ') {
                n--;
            }
            out[n++] = *s;
        }
    };

    for (const char *p = in; *p && n + 1 < size; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '#') {     /* LVGL colour codes: "#ff0000 red text#" */
            if (is_hex(p[1]) && is_hex(p[2]) && is_hex(p[3]) && is_hex(p[4]) && is_hex(p[5]) && is_hex(p[6]) &&
                p[7] == ' ') {
                p += 7;
            }
            continue;
        }
        if (c >= 0x80) {
            /* UTF-8: letters past ASCII kept ("Åland", "Sāo Paulo"), the icon fonts' symbols (the private use area,
             * U+E000-U+F8FF, where LVGL's LV_SYMBOL_ are) dropped, the degree sign said */
            int      len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            uint32_t cp  = len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
            bool     ok  = len > 1;

            for (int k = 1; k < len && ok; k++) {
                unsigned char cc = (unsigned char)p[k];

                ok = (cc & 0xC0) == 0x80;
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (!ok) {
                put(" ");
                continue;
            }
            if (cp == 0xB0) {
                put(" degrees ");
            } else if (cp >= 0xE000 && cp <= 0xF8FF) {
                put(" ");
            } else {
                char one[5];

                memcpy(one, p, len);
                one[len] = 0;
                put(one);
            }
            p += len - 1;
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            put(", ");
            continue;
        }
        if (c == '.' && p[1] == '.') {
            while (p[1] == '.') {
                p++;
            }
            put(" ");
            continue;
        }
        if (strchr("<>|_[]{}~`^*\\=\"", c)) {
            put(" ");
            continue;
        }
        if (c == '&') {
            put(" and ");
            continue;
        }
        char one[2] = {(char)c, 0};

        put(one);
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == ',')) {
        n--;
    }
    out[n] = '\0';
}

static thread_local bool msg_hold = false;

void voice_msg_hold(bool on) {
    msg_hold = on;
}

bool voice_msg_held() {
    return msg_hold;
}

/* Messages shown while on the air ("TX stopped: SWR 3.1", "TX 0 W", a TUNE's): kept, and said once the radio receives
 * again. Nothing is said on the air, and nothing of what the radio reported then is lost */
static char        held_msgs[400];
static lv_timer_t *held_timer;

static void held_timer_cb(lv_timer_t *t) {
    if (!held_msgs[0]) {
        lv_timer_del(t);
        held_timer = NULL;
        return;
    }
    if (on_air()) {
        return;
    }
    char said[sizeof(held_msgs)];

    snprintf(said, sizeof(said), "%s", held_msgs);
    held_msgs[0] = '\0';
    voice_say_clean(said, false);
}

void voice_say_msg(const char *text) {
    if (!speech_on()) {
        return;
    }
    if (on_air()) {
        if (!text || !text[0] || strstr(held_msgs, text)) {
            return;     /* the same one again (a message repeated during one over) */
        }
        size_t n = strlen(held_msgs);

        snprintf(held_msgs + n, sizeof(held_msgs) - n, "%s%s", n ? ". " : "", text);
        if (!held_timer) {
            held_timer = lv_timer_create(held_timer_cb, 250, NULL);
        }
        return;
    }
    voice_say_clean(text, false);
}

void voice_say_ctrl(const char *line) {
    if (!speech_on()) {
        return;
    }
    char        clean[sizeof(buf)];
    const char *colon;

    voice_clean_text(line, clean, sizeof(clean));
    voice_trace("ctrl: [%s] -> [%s]", line ? line : "(null)", clean);
    if (!clean[0]) {
        return;
    }
    colon = strstr(clean, ": ");
    if (!colon) {
        voice_delay_say_text_fmt("%s", clean);
        return;
    }

    char prompt[96];
    size_t n = (size_t)(colon - clean);

    if (n >= sizeof(prompt) - 2) {
        n = sizeof(prompt) - 3;
    }
    memcpy(prompt, clean, n);
    prompt[n++] = ',';
    prompt[n] = '\0';
    voice_say_prompted_fmt(prompt, " %s", colon + 2);
}

void voice_say_knob_state(const char *knob, bool choosing, const char *line) {
    if (!speech_on()) {
        return;
    }
    char        clean[sizeof(buf)];
    char        text[sizeof(buf)];
    const char *colon;

    voice_clean_text(line, clean, sizeof(clean));
    colon = strstr(clean, ": ");
    if (colon) {
        snprintf(text, sizeof(text), "%.*s, %s", (int)(colon - clean), clean, colon + 2);
    } else {
        snprintf(text, sizeof(text), "%s", clean);
    }
    voice_say_text_fmt("%s, %s, %s", knob, choosing ? "choose" : "change", text);
}

void voice_spell_calls(const char *in, char *out, size_t size) {
    size_t n = 0;

    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    while (in && *in && n + 1 < size) {
        const char *w = in;
        size_t      len = 0;
        bool        alpha = false, digit = false;

        while (w[len] && w[len] != ' ') {
            alpha |= (w[len] >= 'A' && w[len] <= 'Z') || (w[len] >= 'a' && w[len] <= 'z');
            digit |= w[len] >= '0' && w[len] <= '9';
            len++;
        }
        if (len) {
            if (n && n + 2 < size) {
                out[n++] = ',';
                out[n++] = ' ';
            }
            for (size_t i = 0; i < len && n + 2 < size; i++) {
                char c = w[i];

                if (c == '/') {
                    n += snprintf(out + n, size - n, "%s", i ? " stroke " : "");
                    continue;
                }
                out[n++] = c;
                if (alpha && digit && i + 1 < len && w[i + 1] != '/') {
                    out[n++] = ' ';
                }
            }
            out[n] = '\0';
        }
        in = w + len;
        while (*in == ' ') {
            in++;
        }
    }
    out[n < size ? n : size - 1] = '\0';
}

void voice_say_clean(const char *text, bool delayed) {
    if (!speech_on()) {
        return;
    }
    char clean[sizeof(buf)];

    voice_clean_text(text, clean, sizeof(clean));
    if (!clean[0]) {
        return;
    }
    if (delayed) {
        voice_delay_say_text_fmt("%s", clean);
    } else {
        voice_say_text_fmt("%s", clean);
    }
}
