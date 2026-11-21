/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - spoken feedback (accessibility), RHVoice
 *
 *  The engine of the X6100 Accessibility fork (github.com/Jimr9/X6100-Accessibility-Testing), itself
 *  R1CBU's (Copyright (c) 2022-2023 Belousov Oleg aka R1CBU), brought over: an announcement is
 *  synthesized on a thread of its own and played on a PulseAudio stream of RHVoice's, at 24 kHz.
 *  While it plays the speaker is claimed (AUDIO_USER_SPEECH, audio_owner.h), so BASE plays this
 *  app's audio and gives the speaker back after; with Bluetooth headphones on, it plays in them
 *  and the speaker is left alone.
 */

#include "voice.h"

#include "cfg/cfg_api.h"
#include "util.h"       /* C++ aware itself (helpers.h): outside the C block */

extern "C" {
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>

#include "lvgl/lvgl.h"

#include "audio.h"
#include "btaudio.h"
#include "msg.h"
#include "radio.h"
#include "recorder.h"
#include "screen_light.h"
#include "voice_messages.h"
}

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>

/* Set by voice_stop_current() to end the announcement playing, or still waiting, cooperatively: the
 * thread always does its own cleanup (the speaker given back), only sooner */
static std::atomic<bool>     stop_requested{false};

/* Every attempt to speak, see voice_say_count() */
static std::atomic<uint32_t> say_attempts{0};
static std::atomic<bool>     auto_speech{false};

#ifdef HAVE_RHVOICE
#include <RHVoice/core/engine.hpp>
#include <RHVoice/core/document.hpp>
#include <RHVoice/core/client.hpp>
#include <RHVoice/audio.hpp>

using namespace RHVoice;

class audio_player: public client {
public:
    explicit audio_player(const char *device);

    bool play_speech(const short *samples_buf, std::size_t count);
    void finish();

private:
    audio::playback_stream stream;
};

/* device: the Bluetooth headphones' sink, NULL for the default output */
audio_player::audio_player(const char *device) {
    stream.set_sample_rate(24000);
    stream.set_buffer_size(512);
    if (device) {
        stream.set_device(device);
    }
    stream.open();
}

bool audio_player::play_speech(const short *buf, std::size_t count) {
    if (stop_requested) {
        return false;
    }
    try {
        stream.write(buf, count);
        return true;
    } catch (...) {
        stream.close();
        return false;
    }
}

void audio_player::finish() {
    if (stream.is_open()) {
        stream.drain();
    }
}

static std::shared_ptr<engine> eng;
static voice_profile           profile;
#endif

static char                    buf[1024];
static char                    prev[1024];
static pthread_t               thread;
static uint32_t                delay = 0;
static std::atomic<bool>       run{false};
static uint16_t                repeated = 0;
static bool                    sure = false;
/* The length of the prompt in buf: unchanged from the last announcement, after a few repeats only
 * what follows it is said (a control's value changing, its name the same). 0: all of it, always */
static size_t                  prompt_len = 0;

voice_item_t voice_item[VOICES_NUM] = {
    {.name = "lyubov",      .label = "Lyubov (En)",  .welcome = "Hello. This is voice Lyubov" },
    {.name = "slt",         .label = "SLT (En)",     .welcome = "Hello. This is voice S L T"  },
    {.name = "alan",        .label = "Alan (En)",    .welcome = "Hello. This is voice Alan"   },
    {.name = "evgeniy-eng", .label = "Evgeniy (En)", .welcome = "Hello. This is voice Evgeniy"},
    {.name = "bdl",         .label = "BDL (En)",     .welcome = "Hello. This is voice B D L"  },
    {.name = "clb",         .label = "CLB (En)",     .welcome = "Hello. This is voice C L B"  },
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

static std::atomic<bool> active{false};     /* voice_busy(): from start() to the end of say_thread() */

static void *say_thread_body(void *arg);

static void *say_thread(void *arg) {
    void *r = say_thread_body(arg);

    active = false;
    return r;
}

bool voice_busy() {
    return active;
}

static void *say_thread_body(void *arg) {
    (void)arg;

    /* In steps, so that a newer announcement cuts one still waiting short */
    for (uint32_t waited = 0; waited < delay; waited += 10000) {
        if (stop_requested) {
            return NULL;
        }
        usleep(10000);
    }

    run = true;

#ifndef HAVE_RHVOICE
    run = false;
    sure = false;
    return NULL;
#else
    try {
        if (!eng) {
            eng = std::make_shared<engine>();
        }
        profile = eng->create_voice_profile(voice_item[voice_index()].name);
    } catch (...) {
        run = false;
        sure = false;
        return NULL;
    }

    char *ptr = buf;
    int   repeat_count = param_i_get(cfg_voice_repeat_count);

    if (prompt_len > 0 && repeat_count > 0) {
        if (strncmp(buf, prev, prompt_len) == 0) {
            repeated++;
            if (repeated >= repeat_count) {
                repeated = 0;
            } else {
                ptr = buf + prompt_len;
            }
        } else {
            repeated = 0;
        }
    } else {
        repeated = 0;
    }
    strcpy(prev, buf);

    /* Bluetooth headphones on: the announcement in them, the speaker left as it is */
    char sink[128];
    bool phones = btaudio_hp_sink(sink, sizeof(sink));

    if (!phones) {
        audio_user_claim(AUDIO_USER_SPEECH);
    }
    try {
        audio_player                   player(phones ? sink : nullptr);
        std::istringstream             text{ptr};
        std::istreambuf_iterator<char> text_start{text};
        std::istreambuf_iterator<char> text_end;
        std::unique_ptr<document>      doc =
            document::create_from_plain_text(eng, text_start, text_end, content_text, profile);

        doc->speech_settings.relative.rate   = param_i_get(cfg_voice_rate) / 100.0;
        doc->speech_settings.relative.pitch  = param_i_get(cfg_voice_pitch) / 100.0;
        doc->speech_settings.relative.volume = param_i_get(cfg_voice_volume) / 100.0;
        doc->set_owner(player);
        doc->synthesize();
        player.finish();
    } catch (...) {
    }
    if (!phones) {
        audio_user_release(AUDIO_USER_SPEECH);
    }

    run = false;
    sure = false;
    return NULL;
#endif
}

void voice_sure() {
    sure = true;
}

/* Until the announcement has been heard: run going false means RHVoice has handed over its last
 * samples, PulseAudio still plays about 400 ms of them */
void voice_wait_done() {
    while (run) {
        usleep(10000);
    }
    usleep(400000);
}

void voice_change_mode() {
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
            voice_say_text_fmt("Speech off. Tap power to hear where the radio is, hold D F N for speech again");
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

    /* Never on the air: in Play mode BASE sends this app's audio in place of the mic (tx_prepare() in
     * radio.c also cuts off what is still playing as a transmission starts) */
    if (recorder_is_on() || on_air() || dialog_msg_voice_get_state() == MSG_VOICE_RECORD) {
        voice_trace("refused: recorder %i, on air %i (state %i), voice message rec %i", recorder_is_on(), on_air(),
                    (int)radio_get_state(), dialog_msg_voice_get_state() == MSG_VOICE_RECORD);
        return false;
    }
    if (!sure) {
        switch (param_i_get(cfg_voice_mode)) {
            case VOICE_ALWAYS:
                break;

            case VOICE_LCD:
                if (!backlight_is_dark()) {
                    voice_trace("refused: screen on, mode screen off only");
                    return false;
                }
                break;

            default:
                voice_trace("refused: voice mode off");
                return false;
        }
    }
    /* Without interrupting, a new announcement is dropped while one speaks */
    if (run && !param_i_get(cfg_voice_interrupt)) {
        voice_trace("refused: speaking, interrupting off");
        return false;
    }
    return true;
}

/* Ends the announcement playing or waiting and waits for its cleanup: the speaker is given back
 * before another takes it. Safe with nothing running */
static std::mutex stop_mutex;

static void voice_stop_current() {
    std::lock_guard<std::mutex> lock(stop_mutex);

    stop_requested = true;
    if (thread) {
        pthread_join(thread, NULL);
        thread = 0;     /* never joined twice: voice_stop() then the next announcement's own stop */
    }
    stop_requested = false;
}

/* voice_say_prefix(): said in front of the next announcement */
static char     prefix[32];
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
    snprintf(prefix, sizeof(prefix), "%s", text ? text : "");
    prefix_at = lv_tick_get();
    lv_timer_set_repeat_count(lv_timer_create(prefix_timer_cb, 400, NULL), 1);
}

static void start(uint32_t say_delay) {
    voice_stop_current();
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
    delay = say_delay;
    active = true;
    if (pthread_create(&thread, NULL, say_thread, NULL) != 0) {
        thread = 0;
        active = false;
    }
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
    voice_stop_current();

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
    voice_stop_current();

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
    voice_stop_current();

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
    voice_stop_current();
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
    voice_stop_current();

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
    return auto_speech && !on_air();
}

bool voice_is_on() {
    return param_i_get(cfg_voice_mode) != VOICE_OFF;
}

void voice_stop() {
    voice_trace("stop");
    voice_stop_current();
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
