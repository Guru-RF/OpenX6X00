/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receive: RX audio -> cw::Receiver (src/cw_decoder/) -> squelch -> panel,
 *  trainer or remote clients; the locked tone drives the tuning indicator.
 */

// First: the decoder headers bring <complex>, which liquid.h must see before
// it is included anywhere else
#include "cw_decoder/audio_resampler.h"

#include "cw.h"
#include "dsp.h"

#include "cfg/settings_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>

extern "C" {
    #include "audio.h"
    #include "cw_decoder.h"
    #include "cw_tune_ui.h"
}

/* Frames (8 ms) between updates of the tune indicator */
#define TUNE_UPDATE_FRAMES  12

static std::atomic<bool> started{false};

/* Settings: written by the LVGL thread (subscriptions), read by the audio thread */
static std::atomic<bool> decode_on{false};
static std::atomic<bool> tune_on{false};
static std::atomic<bool> trainer{false};
static int32_t key_tone = 0;
static int32_t band_lo;
static int32_t band_hi;

/* Receiver settings: stored by the subscriptions (LVGL thread), applied by
 * the audio thread before the next block */
static std::atomic<float> rx_threshold;
static std::atomic<float> rx_hpf;
static std::atomic<float> rx_lpf;
static std::atomic<bool>  rx_peak_on;
static std::atomic<float> rx_peak_tone;
static std::atomic<float> rx_peak_q;
static std::atomic<bool>  rx_dirty{false};

/* Audio thread */
static cw::Receiver       *receiver;
static cw::AudioResampler *resampler;
static bool                keyed = false;
static float               tone_freq;
static std::array<float, 3> freq_hist;
static size_t              freq_hist_w = 0;
static size_t              freq_hist_n = 0;
static uint8_t             frame_counter = 0;
static std::atomic<float>  rx_shift{0.0f};   /* audio Hz the VFO moved, for the audio thread */
static int32_t             last_freq;
static int                 wpm_sent = 0;

/* Squelch: noise keys the detector now and then and prints lone Es, Ts and Is. A character counts
 * when its keyed frames average SQ_MARGIN_DB over the Dec SNR threshold: on 40 m the noise's
 * characters averaged 6-11 dB, a real station's 14-28 dB (Dec SNR 8). The text opens after
 * SQ_OPEN such characters in a row, closes after SQ_CLOSE that are not, or after SQ_IDLE_FRAMES
 * with nothing keyed. The trainer (own keying) is not squelched.
 * ponytail: margin from one evening on 40 m; tune here if weak real signals stay closed */
#define SQ_MARGIN_DB    5.0f
#define SQ_OPEN         3
#define SQ_CLOSE        2
#define SQ_IDLE_FRAMES  375     /* 3 s of 8 ms frames */
static int         sq_keyed;              /* frames since the last character */
static float       sq_snr;                /* sum of the keyed frames' SNR (dB) */
static int         sq_idle;
static bool        sq_open = false;
static int         sq_good, sq_bad;
static std::string sq_pending;

static void on_flag(Subject *subj, void *user_data);
static void on_rx_param_change(Subject *subj, void *user_data);
static void on_freq_change(Subject *subj, void *user_data);

static void on_text(const char *text);
static void on_keyed(bool on);
static void on_frame();

void cw_init(void) {
    cfg_sm.p_cw_decoder.subscribe_and_notify(on_flag, &decode_on);
    cfg_sm.p_cw_tune.subscribe_and_notify(on_flag, &tune_on);

    cfg_sm.p_key_tone.subscribe(on_rx_param_change);
    cfg_sm.p_cw_decoder_snr.subscribe(on_rx_param_change);
    cfg_sm.p_cw_peak_on.subscribe(on_rx_param_change);
    cfg_sm.p_cw_peak_q.subscribe(on_rx_param_change);
    cfg_sm.cp_cur_filter_low.subscribe(on_rx_param_change);
    cfg_sm.cp_cur_filter_high.subscribe_and_notify(on_rx_param_change);
    last_freq = cfg_sm.cp_fg_freq.get();
    cfg_sm.cp_fg_freq.subscribe(on_freq_change);
    tone_freq = key_tone;

    receiver  = new cw::Receiver(on_text, on_keyed, on_frame);
    resampler = new cw::AudioResampler((float)AUDIO_CAPTURE_RATE / AUDIO_DECIM);
    started   = true;
}

/* The tone moves in the audio as the VFO turns: CW is the upper sideband (a higher
 * VFO lowers the tone), CW-R the lower one. Big jumps (band, memory) are left to
 * the tracker: the station is gone anyway. */
static void on_freq_change(Subject *subj, void *user_data) {
    int32_t freq = cfg_sm.cp_fg_freq.get();
    int32_t d    = freq - last_freq;
    int32_t mode = cfg_sm.cp_cur_mode.get();

    last_freq = freq;
    if ((mode == x6100_mode_cw || mode == x6100_mode_cwr) && d != 0 && d >= -1500 && d <= 1500) {
        float s = rx_shift.load();
        while (!rx_shift.compare_exchange_weak(s, s + (mode == x6100_mode_cw ? -d : d))) {}
    }
}

static void apply_rx_params() {
    static bool  peak_on;
    static float peak_tone, peak_q;

    float shift = rx_shift.exchange(0.0f);
    if (shift != 0.0f) {
        receiver->follow_vfo(shift);
        tone_freq += shift;
        for (auto &f : freq_hist) f += shift;
    }

    if (!rx_dirty.exchange(false)) {
        return;
    }
    receiver->set_floor_db(rx_threshold);
    receiver->set_band(rx_hpf, rx_lpf);

    /* Changing the peak filter re-seeds the noise floor: only on a change */
    if (peak_on != rx_peak_on || peak_tone != rx_peak_tone || peak_q != rx_peak_q) {
        peak_on = rx_peak_on;
        peak_tone = rx_peak_tone;
        peak_q = rx_peak_q;
        receiver->set_apf(peak_on, peak_tone, peak_q);
    }
}

void cw_put_audio_samples(unsigned int n, float *samples) {
    bool wanted = decode_on || tune_on || trainer;

    if (!started || !wanted)
        return;
    apply_rx_params();
    resampler->process(n, samples, *receiver);
}

/* Decoded text: to the panel or the trainer, with the WPM when it changes */
static bool squelch(const char *text) {
    bool space = text[0] == ' ';
    bool good  = sq_keyed > 0 && sq_snr / sq_keyed >= rx_threshold.load() + SQ_MARGIN_DB;

    if (!space) {
        sq_keyed = 0;
        sq_snr   = 0.0f;
    }
    /* Open: a weak character waits in sq_pending; printed only if a good one follows (a weak letter
     * of the station), dropped with the squelch closing if another weak one comes (the gaps' noise) */
    if (sq_open) {
        if (space) {
            if (!sq_bad) {
                return true;
            }
            sq_pending += text;
            return false;
        }
        if (good) {
            sq_bad = 0;
            if (sq_pending.empty()) {
                return true;
            }
            sq_pending += text;
            cw_decoder_text(sq_pending.c_str());
            sq_pending.clear();
            return false;
        }
        if (++sq_bad >= SQ_CLOSE) {
            sq_open = false;
            sq_good = 0;
            sq_pending.clear();
            return false;
        }
        sq_pending += text;
        return false;
    }
    if (space) {
        if (!sq_pending.empty()) {
            sq_pending += text;
        }
        return false;
    }
    if (!good) {
        sq_pending.clear();
        sq_good = 0;
        return false;
    }
    sq_pending += text;
    if (++sq_good >= SQ_OPEN) {
        sq_open = true;
        sq_bad  = 0;
        cw_decoder_text(sq_pending.c_str());
        sq_pending.clear();
    }
    return false;
}

static void on_text(const char *text) {
    if (!decode_on && !trainer)
        return;
    if (!trainer && !squelch(text))
        return;
    cw_decoder_text(text);

    int wpm = lroundf(receiver->wpm());
    if (wpm != wpm_sent) {
        wpm_sent = wpm;
        cw_decoder_wpm(wpm);
    }
}

static void on_keyed(bool on) {
    keyed = on;
}

/* Every 8 ms: follow the tone while keyed and locked (median of 3, then
 * smoothed). The tone the detector is locked on: the FFT peak of a frame jumps
 * to any other station or noise peak in the passband; and only a tone held
 * coherently: noise that keys the detector (the Es of an empty band) does not
 * light the indicator. */
static void on_frame() {
    bool tuned = keyed && receiver->locked();

    if (keyed) {
        sq_keyed++;
        sq_snr += receiver->snr_db();
        sq_idle = 0;
    } else if (++sq_idle >= SQ_IDLE_FRAMES && (sq_open || !sq_pending.empty())) {
        sq_open = false;
        sq_good = 0;
        sq_pending.clear();
    }

    if (tuned) {
        float freq = receiver->lock_hz();
        if ((freq >= band_lo) && (freq <= band_hi)) {
            freq_hist[freq_hist_w] = freq;
            freq_hist_w = (freq_hist_w + 1) % freq_hist.size();
            if (freq_hist_n < freq_hist.size()) {
                freq_hist_n++;
            }
            if (freq_hist_n == freq_hist.size()) {
                std::array<float, 3> s = freq_hist;
                std::sort(s.begin(), s.end());
                tone_freq += 0.5f * (s[1] - tone_freq);
            }
        }
    } else {
        freq_hist_n = 0;
    }

    if (++frame_counter >= TUNE_UPDATE_FRAMES) {
        frame_counter = 0;
        if (tuned) {
            cw_tune_set_freq(key_tone - tone_freq);
        }
    }
}

void cw_trainer_set(bool on) {
    trainer = on;
}

float cw_get_tone_freq(void) {
    return tone_freq;
}

int cw_get_wpm(void) {
    return wpm_sent;
}

static void on_flag(Subject *subj, void *user_data) {
    auto *flag = static_cast<std::atomic<bool> *>(user_data);

    *flag = static_cast<SubjectT<int32_t> *>(subj)->get() != 0;
}

static void on_rx_param_change(Subject *subj, void *user_data) {
    key_tone = cfg_sm.p_key_tone.get();
    band_lo  = cfg_sm.cp_cur_filter_low.get();
    band_hi  = cfg_sm.cp_cur_filter_high.get();

    rx_threshold = cfg_sm.p_cw_decoder_snr.get();
    rx_hpf = band_lo;
    rx_lpf = band_hi;
    rx_peak_on = cfg_sm.p_cw_peak_on.get();
    rx_peak_tone = key_tone;
    rx_peak_q = cfg_sm.p_cw_peak_q.get();
    rx_dirty = true;
}
