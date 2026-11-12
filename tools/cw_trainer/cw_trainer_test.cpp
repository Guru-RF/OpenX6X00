/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the CW Trainer's answers against the real CW decoder (src/cw_decoder, #144); see the
 *  Makefile.
 *
 *  First the rules alone: which characters MorseText marks as our own (MorseText::own), and that the pause between
 *  the trainer's group and the answer is not learned (ElementTimer::turn), the word boundary moving up without it.
 *  Then Learn and Calls end to end: the radio keys a group with the sidetone, the user keys it back, and the decoder
 *  hears both as on the radio, through the app's 11025 Hz audio, the resampler and the capture's 100 ms fragments,
 *  the sound some way behind the key. The trainer's rules are dialog_cw_trainer.c's (tick_cb, on_text, play): its
 *  100 ms timer, GUARD_MS, the end of a short answer (cwt_answer_ms, the trainer's own: src/cw_trainer_answer.h),
 *  NEXT_OK_MS, NEXT_BAD_MS, the own count and a new speed given as the group starts, and cw.cpp hands them to the
 *  receiver before its next block, and the decoder's speed to the trainer after a character. Last the first groups
 *  after the trainer opens, over and over, as the lock finds the tone.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "audio_resampler.h"   /* first: the decoder brings <complex>, which liquid.h must see before it is included */

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "cw_trainer_answer.h"

namespace cw {
struct ReceiverPeek {
    static const ElementTimer &timer(const Receiver &r) { return r.timer_; }
};
struct ElementTimerPeek {
    static float word_from(const ElementTimer &t) { return t.word_from_; }
    static float dot(const ElementTimer &t) { return t.dot_ms_; }
};
} // namespace cw

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* The trainer's Koch order (dialog_cw_trainer.c): Learn uses the first `level`, a callsign any of them */
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

static const char *morse_of(char c) {
    for (int i = 0; i < KOCH_N; i++) {
        if (koch[i].c == c) return koch[i].morse;
    }
    return "";
}

static int elements_of(const std::string &s) {
    int n = 0;
    for (char c : s) n += (int)strlen(morse_of(c));
    return n;
}

/* ---- MorseText: our own characters ------------------------------------------------------------------------------ */

/* What came out, own characters with a * after them */
struct Out {
    std::string s;
    void        add(const char *t, bool own) {
        s += t;
        if (own) s += '*';
    }
};

static void put_code(cw::MorseText &m, const char *code) {
    for (const char *p = code; *p; p++) {
        if (p != code) m.put(cw::SYM_ELEM_GAP);
        m.put(*p == '.' ? cw::SYM_DIT : cw::SYM_DAH);
    }
}

static void morse_text_rules(void) {
    printf("MorseText: our own characters\n");
    {
        Out           o;
        cw::MorseText m([&](const char *t, bool own) { o.add(t, own); });
        m.own(3);
        put_code(m, "-.-");
        m.put(cw::SYM_WORD_GAP);
        put_code(m, "-.-");
        m.put(cw::SYM_WORD_GAP);
        check(o.s == "K* K ", "our K, a word gap, the user's K: \"%s\"", o.s.c_str());
    }
    {
        Out           o;
        cw::MorseText m([&](const char *t, bool own) { o.add(t, own); });
        m.own(6);
        put_code(m, "-.-");
        m.put(cw::SYM_CHAR_GAP);
        put_code(m, "--");
        bool ended_early = m.own_ended();
        put_code(m, "-");   /* ... the 6th: our last */
        bool ended = m.own_ended(), again = m.own_ended();
        m.put(cw::SYM_CHAR_GAP);   /* the user starts within a word gap: our O ends there, late */
        put_code(m, "-.-");
        m.put(cw::SYM_WORD_GAP);
        check(o.s == "K*O*K " && !ended_early && ended && !again,
              "our KO, the user's K a character gap after it: \"%s\", own_ended once, after our last dah", o.s.c_str());
    }
    {
        Out           o;
        cw::MorseText m([&](const char *t, bool own) { o.add(t, own); });
        m.own(2);
        put_code(m, "..-");   /* our M's first dah in two pieces, as the lock finds the tone */
        m.put(cw::SYM_WORD_GAP);
        put_code(m, "--");
        m.put(cw::SYM_WORD_GAP);
        check(o.s == "U* M ", "our M heard as ..-, one element past the count: still ours, the user's M his: \"%s\"",
              o.s.c_str());
    }
    {
        Out           o;
        cw::MorseText m([&](const char *t, bool own) { o.add(t, own); });
        m.own(2);
        put_code(m, "--");
        m.put(cw::SYM_ELEM_GAP);   /* the user keyed within 1.7 dots of our last dah */
        put_code(m, "-.-");
        m.put(cw::SYM_WORD_GAP);
        check(o.s == "<?>* ", "our M run into the user's K: one character, ours, \"%s\"", o.s.c_str());
    }
    {
        Out           o;
        cw::MorseText m([&](const char *t, bool own) { o.add(t, own); });
        m.own(4);   /* one of our four the decoder missed */
        put_code(m, "-.-");
        m.put(cw::SYM_WORD_GAP);
        put_code(m, "-.-");
        m.put(cw::SYM_WORD_GAP);
        check(o.s == "K* K ", "a dit of ours missed: the word gap ends the count, the user's K is his: \"%s\"",
              o.s.c_str());
    }
    {
        Out           o;
        cw::MorseText m([&](const char *t, bool own) { o.add(t, own); });
        put_code(m, "..-");
        m.own(3);   /* the group starts before the end of the user's last answer came out */
        m.put(cw::SYM_WORD_GAP);
        put_code(m, "-.-");
        m.put(cw::SYM_WORD_GAP);
        check(o.s == "U K* ", "a word gap before any of ours ends nothing: \"%s\"", o.s.c_str());
    }
    {
        Out           o;
        cw::MorseText m([&](const char *t, bool own) { o.add(t, own); });
        m.own(3);
        put_code(m, "-.-");
        m.own(0);   /* the count ended (stop_round) before the decoder ended the character */
        m.put(cw::SYM_WORD_GAP);
        put_code(m, "-");
        m.put(cw::SYM_WORD_GAP);
        check(o.s == "K* T ", "own(0): the character started keeps its mark, the next is not ours: \"%s\"", o.s.c_str());
    }
}

/* ---- ElementTimer: the pause between the turns ------------------------------------------------------------------- */

/* Keys a code into the timer at `dot` ms in its 8 ms steps, the edges on the steps; the symbols go to m. As
 * Receiver::detector_step does, our own elements are not learned, and after our own last one the timer is told of
 * the turn */
static void time_code(cw::ElementTimer &t, cw::MorseText &m, const char *code, int dot, int gap_after) {
    auto step = [&](int dir) {
        if (m.owning()) t.ours();
        cw::Sym s = t.step(dir, 0.0f);
        if (s != cw::SYM_NONE) {
            m.put(s);
            if (m.own_ended()) t.turn();
        }
    };
    for (const char *p = code; *p; p++) {
        int on = (*p == '.' ? 1 : 3) * dot, off = p[1] ? dot : gap_after;
        for (int ms = 0; ms < on; ms += cw::STEP_MS) step(ms == 0 ? 1 : 0);
        for (int ms = 0; ms < off; ms += cw::STEP_MS) step(ms == 0 ? -1 : 0);
    }
}

/* 30 groups of one K at 20 WPM, the user's K 400 ms after it (before the decoder's end of text, 8.5 dots), then 2 s
 * of silence; the word boundary at the end, and what came out */
static float turns(bool own, std::string &text) {
    cw::ElementTimer t;
    Out              o;
    cw::MorseText    m([&](const char *s, bool own) { o.add(s, own); });

    for (int i = 0; i < 30; i++) {
        if (own) m.own(3);
        time_code(t, m, "-.-", 60, 400);
        time_code(t, m, "-.-", 60, 2000);
    }
    text = o.s;
    return cw::ElementTimerPeek::word_from(t);
}

static void element_timer_rules(void) {
    std::string a, b, want;
    float       before = turns(false, a), after = turns(true, b);

    printf("ElementTimer: the pause before the answer\n");
    size_t apart = a.find("KK") == std::string::npos ? 30 : a.find("KK") / 4;   /* groups "K K " before it */
    check(before > 400.0f && apart == 8,
          "learned, the 400 ms pauses moved the word boundary from 300 ms (5 dots) to %.0f ms: from the 9th group on "
          "our K and the answer one word, %zu groups apart before (#144)", before, apart);
    for (int i = 0; i < 30; i++) want += "K* K ";
    check(after <= 330.0f && b == want, "a turn, not learned: the word boundary %.0f ms, every group \"K* K \"", after);

    /* The trainer keys down to 5 WPM, the decoder on the band stops at 10 */
    cw::ElementTimer r;
    r.expect(5);
    float band = r.wpm();
    r.trainer_range(true);
    r.expect(5);
    float open = r.wpm();
    r.trainer_range(false);
    float closed = r.wpm();
    check(std::fabs(band - 10.0f) < 0.01f && std::fabs(open - 5.0f) < 0.01f && std::fabs(closed - 10.0f) < 0.01f,
          "the speed range: 5 WPM given, on the band %.1f WPM, with the trainer open %.1f, after it closed %.1f", band,
          open, closed);
}

/* ---- The answer: a place for every character -------------------------------------------------------------------- */

static void answer_rules(void) {
    char        h[32] = "", shown[3 * 32], small[6];
    std::string want  = std::string("K") + CWT_NO_CHAR + CWT_NO_CHAR + "?R";

    for (const char *t : {"K", "<?>", "<AR>", "?", "R"}) cwt_answer_add(h, sizeof(h), t);
    printf("The answer: one place a character\n");
    check(want == h, "K, a code in no table, a prosign, ? and R from the decoder: \"%s\", the ? only for the ?", h);
    cwt_answer_shown(h, shown, sizeof(shown));
    cwt_answer_shown(h, small, sizeof(small));
    check(strcmp(shown, "K<?><?>?R") == 0 && strcmp(small, "K<?>") == 0,
          "shown as \"%s\", a place with no character as the decoder panel's <?>, the ? as ?; in 6 bytes whole places "
          "only, \"%s\"", shown, small);
}

/* ---- Learn and Calls, end to end --------------------------------------------------------------------------------- */

/* dialog_cw_trainer.c */
#define TIMER_MS    100
#define GUARD_MS    600
#define NEXT_OK_MS  1000
#define NEXT_BAD_MS 2500

static const double RATE   = 11025.0;   /* the app's audio: AUDIO_CAPTURE_RATE / AUDIO_DECIM */
static const int    FRAG   = 1102;      /* the capture's fragments, AUDIO_RATE_MS (100 ms) */
static const double TONE   = 700.0;     /* KEY Tone */
static const double SIDE   = 0.3;       /* the sidetone's level */
static const double NOISE  = 1e-3;
static const double EDGE   = 4.0;       /* ms, raised cosine */

struct Mark {
    double on, off;   /* sound time, ms */
};

struct Scenario {
    const char *what;
    int         wpm;
    double      delay;          /* key to sound in the capture, ms (and the fragments on top) */
    int         level, group;   /* Learn; group 0: Calls */
    int         rounds;
    double      react_min, react_max;   /* the user's answer after the group's sound ended, ms */
    int         user_wpm = 0;   /* a straight key at this speed (as exact), 0: the keyer, at the key speed */
    int         first    = 0;   /* Learn draws from the level's characters from this one on */
    bool        garble   = false;   /* the user keys every ? with a dit too many, ..--..., in no table */
    double      ratio    = 3.0;     /* KEY Ratio, dah to dit: the encoder's group and the keyer's answer */
    double      pause    = 0;       /* dots the user waits after the answer's first character, past its gap */
};

struct Result {
    int   own_answers = 0;   /* answers holding our own character */
    int   into_answer = 0;   /* groups started while the user was still keying */
    int   wrong       = 0;
    int   guard_lost  = 0;   /* ... of them with the user's first character inside GUARD_MS (as before #144) */
    int   late_own    = 0;   /* our own characters the decoder ended past GUARD_MS: once taken for the answer */
    int   rounds      = 0;
    bool  finished    = false;
    float word_dots   = 0;   /* the word boundary at the end, in dots */
    float wpm_end     = 0;   /* the decoder's speed at the end */
    int   key_wpm     = 0;   /* ... and the key's (Calls moves it) */
    int   garbled     = 0;   /* groups whose answer the user garbled (Scenario::garble) */
    int   garbled_ok  = 0;   /* ... counted right */
    int   garbled_no  = 0;   /* ... heard as no character where the ? was */
};

static Result learn(const Scenario &sc, unsigned seed) {
    std::mt19937      rng(seed);
    std::vector<Mark> marks;
    Result            r;
    auto              urand = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    std::normal_distribution<double> gauss(0.0, 1.0);

    struct Text {
        std::string s;
        bool        own;
        int         wpm;   /* the decoder's speed after it (cw.cpp on_text: cw_decoder_wpm) */
    };
    std::vector<Text> texts;
    cw::Receiver      rx([&](const char *t, bool own) { texts.push_back({t, own, (int)lroundf(rx.wpm())}); },
                         [](bool) {});
    cw::AudioResampler rs(RATE);
    rx.set_floor_db(8.0f);
    rx.set_band(TONE - 250.0, TONE + 250.0);
    rx.trainer_range(true);   /* cw.cpp: the trainer is open, before its first block */

    enum { READY, PLAYING, LISTEN, RESULT } st = READY;
    std::string target, heard;
    int         heard_n = 0, wpm = sc.wpm, own_req = -1, wpm_req = 0, told_wpm = 0, wpm_seen = 0;
    bool        heard_end = false, last_ok = false, own_taken = false, lost = false, garbled = false;
    double      idle = 0, listen_t = 0, last_rx_t = 0, result_t = 0, user_first_end = 1e12, user_last = -1;

    /* play(): the own count, the speed when it is new, then the encoder (cw_encoder.c send_morse); the user keys
     * it back */
    auto play = [&](double t) {
        if (sc.group > 0) {
            target.clear();
            for (int i = 0; i < sc.group; i++) target += koch[sc.first + rng() % (sc.level - sc.first)].c;
        } else {   /* a callsign of new_call()'s shape */
            static const char *const pre[] = {"DL#", "ON#", "G#", "EA8", "IK2", "VK#", "W#", "9A#", "HB9", "2E0"};
            target = pre[rng() % 10];
            for (char &c : target)
                if (c == '#') c = '0' + rng() % 10;
            for (int n = rng() % 5 < 2 ? 2 : 3; n > 0; n--) target += 'A' + rng() % 26;
            if (rng() % 10 == 0) target += "/P";
        }
        heard.clear();
        heard_n   = 0;
        heard_end = false;
        own_taken = lost = false;
        if (user_last >= 0 && t + sc.delay < user_last + 3 * 1200.0 / wpm) r.into_answer++;
        own_req = elements_of(target);
        if (wpm != told_wpm) wpm_req = told_wpm = wpm;

        double dit = 1200.0 / wpm, dah = sc.ratio * dit, k = t;   /* the character gap a dah too (send_morse) */
        for (char c : target) {
            for (const char *p = morse_of(c); *p; p++) {
                double el = *p == '.' ? dit : dah;
                marks.push_back({k + sc.delay, k + el + sc.delay});
                k += el + dit;
            }
            k += dah - dit;
        }
        idle = k;

        /* the user hears the end of it at once, the capture the sound later; keyed back with the keyer: exact,
         * its dahs at KEY Ratio (a straight key's 3 dots), the character gaps the user's own 3 dots */
        double u = marks.back().off - sc.delay + urand(sc.react_min, sc.react_max);
        double udit = sc.user_wpm > 0 ? 1200.0 / sc.user_wpm : dit, udah = (sc.user_wpm > 0 ? 3 : sc.ratio) * udit;
        garbled     = sc.garble && target.find('?') != std::string::npos;
        for (size_t i = 0; i < target.size(); i++) {
            for (const char *p = garbled && target[i] == '?' ? "..--..." : morse_of(target[i]); *p; p++) {
                double el = *p == '.' ? udit : udah;
                marks.push_back({u + sc.delay, u + el + sc.delay});
                u += el + udit;
            }
            if (i == 0) user_first_end = marks.back().off;
            u += 2 * udit;
            if (i == 0) u += sc.pause * udit;
        }
        user_last = marks.back().off;
        st        = PLAYING;
        r.rounds++;
    };

    auto sound = [&](double ms) {
        double a = 0.0;
        for (size_t i = marks.size(); i-- > 0 && marks.size() - i <= 64;) {
            const Mark &m = marks[i];
            if (ms < m.on || ms > m.off) continue;
            double x = 1.0;
            if (ms < m.on + EDGE) x = 0.5 - 0.5 * cos(M_PI * (ms - m.on) / EDGE);
            if (ms > m.off - EDGE) x = std::min(x, 0.5 - 0.5 * cos(M_PI * (m.off - ms) / EDGE));
            a = std::max(a, x);
        }
        return a;
    };

    std::vector<float> buf(FRAG);
    long               n = 0;
    double             tick = 1000.0;
    bool               done = false;

    while (!done && n < (long)(RATE * 3600)) {
        double t1 = 1000.0 * (n + FRAG) / RATE;

        /* tick_cb, up to the end of this fragment */
        for (; tick <= t1 && !done; tick += TIMER_MS) {
            if (st == READY) {
                if (tick >= 3000) play(tick);   /* Start, a while after the trainer opened */
            } else if (st == PLAYING && tick >= idle) {
                st       = LISTEN;
                listen_t = last_rx_t = tick;
            } else if (st == LISTEN && heard_n > 0 &&
                       (heard_n >= (int)target.size() || heard_end ||
                        tick - last_rx_t > cwt_answer_ms(wpm, wpm_seen, sc.ratio))) {
                last_ok = heard == target;
                r.own_answers += own_taken;
                if (garbled) {
                    std::string no = target;
                    std::replace(no.begin(), no.end(), '?', CWT_NO_CHAR);
                    r.garbled++;
                    r.garbled_ok += last_ok;
                    r.garbled_no += heard == no;
                }
                if (!last_ok) {
                    r.wrong++;
                    r.guard_lost += lost;
                    if (!garbled)
                        printf("        group %d: \"%s\" heard as \"%s\"%s\n", r.rounds, target.c_str(),
                               heard.c_str(), lost ? " (its first character inside GUARD_MS)" : "");
                }
                st       = RESULT;
                result_t = tick;
            } else if (st == RESULT && tick - result_t > (last_ok ? NEXT_OK_MS : NEXT_BAD_MS)) {
                if (r.rounds == sc.rounds) {
                    done = true;
                } else {
                    if (sc.group == 0) wpm += last_ok ? 1 : -1;   /* Calls: right +1 WPM, wrong -1 */
                    play(tick);
                }
            }
        }

        /* the audio thread (cw.cpp): cw_trainer_speed's and cw_trainer_own's before the block, then the block */
        if (wpm_req > 0) {
            rx.expect(wpm_req);
            wpm_req = 0;
        }
        if (own_req >= 0) {
            rx.own(own_req);
            own_req = -1;
        }
        for (int i = 0; i < FRAG; i++, n++) {
            double ms = 1000.0 * n / RATE;
            buf[i]    = (float)(SIDE * sound(ms) * sin(2.0 * M_PI * TONE * n / RATE) + NOISE * gauss(rng));
        }
        size_t seen = texts.size();
        rs.process(buf.size(), buf.data(), rx);

        /* on_text, as the fragment's text reaches the LVGL thread, and on_wpm after it */
        for (; seen < texts.size(); wpm_seen = texts[seen].wpm, seen++) {
            const Text &e     = texts[seen];
            bool        guard = st == LISTEN && t1 - listen_t < GUARD_MS;
            if (e.own) {
                r.late_own += st == LISTEN && !guard;
                continue;
            }
            if (st != LISTEN) continue;
            if (guard) {
                lost |= e.s != " " && t1 >= user_first_end;
                continue;
            }
            if (e.s != " " && t1 < user_first_end) own_taken = true;   /* it cannot be the user's yet */
            last_rx_t = t1;
            if (e.s == " ") {
                heard_end = cwt_space_ends(sc.group == 0, heard_n);
            } else if (heard_n < (sc.group == 0 ? 12 : 6)) {
                char h[32];   /* the trainer's heard[] */
                snprintf(h, sizeof(h), "%s", heard.c_str());
                cwt_answer_add(h, sizeof(h), e.s.c_str());
                heard = h;
                heard_n++;
            }
        }
    }
    const cw::ElementTimer &et = cw::ReceiverPeek::timer(rx);
    r.word_dots = cw::ElementTimerPeek::word_from(et) / cw::ElementTimerPeek::dot(et);
    r.wpm_end   = et.wpm();
    r.key_wpm   = wpm;
    r.finished  = done;
    if (!done) printf("        stopped after %d groups: no answer heard\n", r.rounds);
    return r;
}

static void end_to_end(void) {
    static const Scenario all[] = {
        {"Learn, group 1, level 2", 10, 30, 2, 1, 40, 300, 1200},
        {"Learn, group 1, level 2", 12, 30, 2, 1, 40, 300, 1200},
        {"Learn, group 1, level 2, answers late", 12, 30, 2, 1, 40, 800, 2000},
        {"Learn, group 1, level 2, sound 80 ms later", 15, 80, 2, 1, 40, 300, 1200},
        {"Learn, group 1, level 2", 20, 30, 2, 1, 120, 300, 1200},
        {"Learn, group 1, level 2, sound 150 ms later", 20, 150, 2, 1, 40, 300, 1200},
        {"Learn, group 1, level 2", 30, 30, 2, 1, 40, 300, 1200},
        {"Learn, group 1, all 40", 12, 30, 40, 1, 40, 300, 1200},
        {"Learn, group 1, all 40, sound 100 ms later", 18, 100, 40, 1, 60, 300, 1200},
        {"Learn, group 1, all 40", 25, 30, 40, 1, 40, 300, 1200},
        {"Learn, group 3, level 10", 15, 30, 10, 3, 40, 300, 1200},
        {"Learn, group 5, level 20", 12, 30, 20, 5, 40, 300, 1200},
        {"Learn, group 5, all 40", 20, 30, 40, 5, 40, 300, 1200},
        {"Calls, +1 WPM a call, from", 15, 30, 0, 0, 20, 400, 1200},
        /* a last character of 19 dots (0 ,) and the decoder's end of it, 3.3 s at 10 WPM: once past a fixed 3 s */
        {"Learn, group 5, all 40", 10, 30, 40, 5, 40, 300, 1200},
        /* the answer slower than the key speed: the end of a short answer by the decoder's speed, not the key's */
        {"Learn, group 3, all 40, a straight key at 10 WPM, KEY Speed", 14, 130, 40, 3, 40, 300, 1200, 10},
        /* the lowest key speed, the decoder down to it (trainer_range); answered 2 dots after the group or later
         * (sooner the answer runs into it, see MorseText) */
        {"Learn, group 1, all 40, answers after 2 dots", 5, 30, 40, 1, 40, 500, 1400},
        {"Learn, group 5, all 40, answers after 2 dots", 5, 130, 40, 5, 20, 500, 1400},
        /* a long KEY Ratio at the slow speeds: dahs of 800-1080 ms, once past the key detector's carrier floor after
         * 480 ms (KeyDetector::trainer_range) */
        {"Learn, group 5, all 40, KEY Ratio 4.5, answers after 2 dots", 5, 130, 40, 5, 20, 500, 1400, 0, 0, false, 4.5},
        {"Learn, group 3, all 40, KEY Ratio 4, answers after 2 dots", 6, 30, 40, 3, 40, 500, 1400, 0, 0, false, 4.0},
        /* a 0 at Ratio 4.5 (26.5 dots) and the decoder's end of it: once past 30 dots (cwt_answer_ms) */
        {"Learn, group 5, all 40, KEY Ratio 4.5", 10, 30, 40, 5, 40, 300, 1200, 0, 0, false, 4.5},
    };

    printf("Learn and Calls: the radio's group and the user's answer, through the decoder\n");
    for (const Scenario &sc : all) {
        Result r = learn(sc, 1);
        int    other = r.wrong - r.guard_lost;   /* wrong for another reason */

        check(r.finished && r.own_answers == 0 && r.into_answer == 0 && other == 0 && r.word_dots <= 5.5f,
              "%s, %d WPM: %d groups, our own character the answer %d times (late from the decoder %d times), a group "
              "into the answer %d, answers wrong %d (%d inside GUARD_MS), word boundary %.1f dots",
              sc.what, sc.wpm, r.rounds, r.own_answers, r.late_own, r.into_answer, r.wrong, r.guard_lost, r.word_dots);
    }
}

/* An answer the decoder cannot read is no character, not "?": in groups of 3 8 B and ? (the last four of level 32) the
 * user keys every ? with a dit too many. Once it came out as "?" and counted right */
static void no_character(void) {
    printf("An answer that is no character\n");
    for (int group : {1, 3}) {
        Scenario sc = {"Learn, 3 8 B ?, the ? keyed ..--...", 15, 30, 32, group, 40, 300, 1200};
        sc.first    = 28;
        sc.garble   = true;
        Result r    = learn(sc, 1);
        int    other = r.wrong - r.guard_lost - (r.garbled - r.garbled_ok);   /* wrong for another reason */

        check(r.finished && r.garbled > 0 && r.garbled_ok == 0 && r.garbled_no == r.garbled && other == 0,
              "%s, group %d, %d WPM: %d groups, %d with a ? keyed so: counted right %d times, the ? heard as no "
              "character %d times; other answers wrong %d", sc.what, group, sc.wpm, r.rounds, r.garbled, r.garbled_ok,
              r.garbled_no, other);
    }
}

/* The first groups after the trainer opens, over and over: the lock finds the tone with the first one, and a dah can
 * come in pieces then; the trainer's count runs out inside its last character, and the decoder learning the pieces
 * took the speed to 50 WPM. 4 groups a run, the sound 30-250 ms behind the key, 4 seeds, at 10, 12 and 15 WPM (faster,
 * the decoder ends the radio's last character inside GUARD_MS anyway) */
static void first_groups(void) {
    static const struct {
        const char *what;
        int         level, group;
    } kinds[] = {
        {"Learn, group 1, level 2", 2, 1},
        {"Learn, group 1, all 40", 40, 1},
        {"Learn, group 3, level 10", 10, 3},   /* level 10: no character of 5 or 6 elements */
        {"Calls", 0, 0},
        {"Learn, group 3, all 40", 40, 3},     /* with them: a fixed 3 s cut the answer at 10 WPM (cwt_answer_ms) */
    };

    printf("The first 4 groups after the trainer opens, the lock finding the tone\n");
    for (const auto &k : kinds) {
        int runs = 0, own = 0, into = 0, other = 0, lost = 0, stuck = 0, off = 0;
        for (int wpm : {10, 12, 15}) {
            for (double delay : {30.0, 60.0, 90.0, 130.0, 170.0, 210.0, 250.0}) {
                for (unsigned seed = 1; seed <= 4; seed++) {
                    Scenario sc = {k.what, wpm, delay, k.level, k.group, 4, 300, 1200};
                    Result   r  = learn(sc, seed);
                    runs++;
                    own += r.own_answers;
                    into += r.into_answer;
                    other += r.wrong - r.guard_lost;
                    lost += r.guard_lost;
                    stuck += !r.finished;
                    off += std::fabs(r.wpm_end - r.key_wpm) > 0.25f * r.key_wpm;
                }
            }
        }
        check(own == 0 && into == 0 && other == 0 && stuck == 0 && off == 0,
              "%s, 10-15 WPM, %d runs: our own character the answer %d times, a group into the answer %d, answers "
              "wrong %d (and %d inside GUARD_MS), no answer heard %d, the decoder's speed off at the end %d",
              k.what, runs, own, into, other, lost, stuck, off);
    }
}

/* Calls: the user waits a word gap or longer after the call's first character ("N 2JAH"), the decoder giving a space
 * there: one answer still, the space left out, right. Up to v0.3.9 the space ended it, "N" judged against the call
 * (#202) */
static void call_pauses(void) {
    printf("Calls, a pause inside the answer\n");
    for (double pause : {8.0, 15.0, 22.0}) {
        for (int wpm : {12, 20}) {
            Scenario sc = {"Calls, a pause after the first character", wpm, 30, 0, 0, 20, 400, 1200};
            sc.pause    = pause;
            Result r    = learn(sc, 1);
            int    other = r.wrong - r.guard_lost;

            check(r.finished && r.own_answers == 0 && r.into_answer == 0 && other == 0,
                  "%s, %.0f dots, from %d WPM: %d calls, answers wrong %d (%d inside GUARD_MS), our own character the "
                  "answer %d times", sc.what, pause, wpm, r.rounds, r.wrong, r.guard_lost, r.own_answers);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    morse_text_rules();
    element_timer_rules();
    answer_rules();
    end_to_end();
    no_character();
    call_pauses();
    first_groups();
    printf("%d of %d CW trainer cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
