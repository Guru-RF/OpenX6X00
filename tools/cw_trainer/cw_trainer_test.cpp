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
 *  100 ms timer, GUARD_MS, ANSWER_MS, NEXT_OK_MS, NEXT_BAD_MS, the own count and a new speed given as the group
 *  starts, and cw.cpp hands them to the receiver before its next block.
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
}

/* ---- Learn and Calls, end to end --------------------------------------------------------------------------------- */

/* dialog_cw_trainer.c */
#define TIMER_MS    100
#define GUARD_MS    600
#define ANSWER_MS   3000
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
    };
    std::vector<Text> texts;
    cw::Receiver      rx([&](const char *t, bool own) { texts.push_back({t, own}); }, [](bool) {});
    cw::AudioResampler rs(RATE);
    rx.set_floor_db(8.0f);
    rx.set_band(TONE - 250.0, TONE + 250.0);

    enum { READY, PLAYING, LISTEN, RESULT } st = READY;
    std::string target, heard;
    int         heard_n = 0, wpm = sc.wpm, own_req = -1, wpm_req = 0, told_wpm = 0;
    bool        heard_end = false, last_ok = false, own_taken = false, lost = false;
    double      idle = 0, listen_t = 0, last_rx_t = 0, result_t = 0, user_first_end = 1e12, user_last = -1;

    /* play(): the own count, the speed when it is new, then the encoder (cw_encoder.c send_morse); the user keys
     * it back */
    auto play = [&](double t) {
        if (sc.group > 0) {
            target.clear();
            for (int i = 0; i < sc.group; i++) target += koch[rng() % sc.level].c;
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

        double dit = 1200.0 / wpm, dah = 3 * dit, k = t;
        for (char c : target) {
            for (const char *p = morse_of(c); *p; p++) {
                double el = *p == '.' ? dit : dah;
                marks.push_back({k + sc.delay, k + el + sc.delay});
                k += el + dit;
            }
            k += dah - dit;
        }
        idle = k;

        /* the user hears the end of it at once, the capture the sound later; keyed back with the keyer: exact */
        double u = marks.back().off - sc.delay + urand(sc.react_min, sc.react_max);
        for (size_t i = 0; i < target.size(); i++) {
            for (const char *p = morse_of(target[i]); *p; p++) {
                double el = *p == '.' ? dit : dah;
                marks.push_back({u + sc.delay, u + el + sc.delay});
                u += el + dit;
            }
            if (i == 0) user_first_end = marks.back().off;
            u += dah - dit;
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
                       (heard_n >= (int)target.size() || heard_end || tick - last_rx_t > ANSWER_MS)) {
                last_ok = heard == target;
                r.own_answers += own_taken;
                if (!last_ok) {
                    r.wrong++;
                    r.guard_lost += lost;
                    printf("        group %d: \"%s\" heard as \"%s\"%s\n", r.rounds, target.c_str(), heard.c_str(),
                           lost ? " (its first character inside GUARD_MS)" : "");
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

        /* on_text, as the fragment's text reaches the LVGL thread */
        for (; seen < texts.size(); seen++) {
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
                heard_end = heard_n > 0;
            } else if (heard_n < (sc.group == 0 ? 12 : 6)) {
                heard += e.s == "<?>" ? "?" : e.s;
                heard_n++;
            }
        }
    }
    const cw::ElementTimer &et = cw::ReceiverPeek::timer(rx);
    r.word_dots = cw::ElementTimerPeek::word_from(et) / cw::ElementTimerPeek::dot(et);
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

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    morse_text_rules();
    element_timer_rules();
    end_to_end();
    printf("%d of %d CW trainer cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
