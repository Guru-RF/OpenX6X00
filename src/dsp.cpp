/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

/* What the GUI does with the samples of the base board: the panorama (spectrum and
 * waterfall) from the IQ stream, the S-meter, the automatic waterfall colours, and the
 * audio for the decoders. Demodulation, filters, AGC and the speaker are on the base. */

#include "dsp.h"
#include "dsp_math.h"
#include "cat.h"

#include "cfg/settings_manager.h"

#include "cw.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include "dialog_msg_voice.h"

extern "C" {
    #include "audio.h"
    #include "meter.h"
    #include "radio.h"
    #include "recorder.h"
    #include "screen_rec.h"
    #include "rtty.h"
    #include "spectrum.h"
    #include "waterfall.h"

    #include <math.h>
    #include <pthread.h>
    #include <stdlib.h>
}

#define SPAN_HZ         100000      /* IQ span at zoom 1 */
#define LEVEL_DB        (-34.0f)    /* display level of a bin reading 1.0 */
/* S-meter level of full-scale power: the same reading as the previous code, so the
 * per-band calibrations stay valid */
#define SMETER_DB       (LEVEL_DB + 30.103f)
#define FRAME_MS        (1000 / 15)
#define OEM_SETTLE      4           /* OEM base: frames to drop after a tune */
#define FLOOR_PCT       0.25f       /* noise floor: percentile of the bin groups */
#define FLOOR_GROUP     8
#define COLOUR_BELOW    5.0f        /* waterfall black this far under the noise */
#define COLOUR_RANGE    48.0f

/* Averaged power spectrum of a sliding IQ window: every push of samples runs one FFT of
 * the last nfft samples (they overlap when the pushes are shorter), the powers add up
 * until take() hands out their mean */
class Panorama {
    size_t             nfft;
    windowcf           hist;
    std::vector<float> win;
    cfloat            *t, *f;
    fftplan            plan;
    std::vector<float> acc;
    size_t             filled = 0;
    size_t             count  = 0;

  public:
    explicit Panorama(size_t n) : nfft(n), win(n), acc(n, 0.0f) {
        hist = windowcf_create(n);
        t    = (cfloat *)calloc(n, sizeof(cfloat));
        f    = (cfloat *)calloc(n, sizeof(cfloat));
        plan = fft_create_plan(n, t, f, LIQUID_FFT_FORWARD, 0);
        dsp_window_bh4(win.data(), n);
    }

    void reset() {
        windowcf_reset(hist);
        filled = 0;
        count = 0;
        std::fill(acc.begin(), acc.end(), 0.0f);
    }

    bool ready() const {
        return count > 0;
    }

    void push(cfloat *x, size_t n) {
        windowcf_write(hist, x, n);
        filled += n;
        if (filled < nfft)
            return;

        cfloat *r;

        windowcf_read(hist, &r);
        for (size_t i = 0; i < nfft; i++)
            t[i] = r[i] * win[i];
        fft_execute(plan);
        for (size_t i = 0; i < nfft; i++)
            acc[i] += std::norm(f[i]);
        count++;
    }

    /* Mean power per bin, DC in the middle; starts a new average */
    void take(float *out) {
        float k = 1.0f / count;

        for (size_t i = 0; i < nfft; i++)
            out[i] = std::max(acc[(i + nfft / 2) % nfft] * k, 1e-20f);
        std::fill(acc.begin(), acc.end(), 0.0f);
        count = 0;
    }
};

static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static bool            ready;

static x6100_base_ver_t base_ver;
static bool             base_decim;     /* the base decimates the IQ for the zoom */
static bool             base_dc_block;  /* the base removes DC from the IQ */
static iirfilt_cccf     dc_block;

static uint8_t       zoom = 1;
static float         zoom_db;           /* narrower bins hold less noise: lift them back */
static firdecim_crcf decim_rx, decim_tx; /* old bases: zoom decimation done here */
static cfloat        iq[RADIO_SAMPLES * 2];     /* bf16 packets carry twice as many */
static cfloat        iq_dec[RADIO_SAMPLES * 2];

static Panorama *sp_rx, *sp_tx, *wf_rx, *wf_tx;

static float    sp_psd[SPECTRUM_NFFT];
static float    sp_shown[SPECTRUM_NFFT];
static uint64_t sp_time;
static uint32_t sp_freq;

static float    wf_lin[WATERFALL_NFFT];
static float    wf_db[WATERFALL_NFFT];

/* dsp_spectrum_snapshot: the last RX spectrum, copied out under its own lock */
static float           snap_db[WATERFALL_NFFT];
static uint32_t        snap_center, snap_width, snap_seq;
static pthread_mutex_t snap_mux = PTHREAD_MUTEX_INITIALIZER;
static uint64_t wf_time;

static uint32_t cur_freq;
static uint8_t  settle;                 /* frames still to drop after a tune */
static uint8_t  after_tx;               /* frames to skip for the colours after TX */

static DspFloorTrack     floor_track;
static uint64_t          floor_time;
static std::atomic<bool> floor_reset{true};

static int32_t      filter_from = 0;
static int32_t      filter_to   = 3000;
static x6100_mode_t cur_mode;

/* FT8 and other dialogs switch the panorama off to save CPU */
static std::atomic<bool> waterfall_enabled{true};
static std::atomic<bool> spectrum_enabled{true};
/* Spectrum off the screen: its FFT is not run (the walkie bandscope always needs it) */
static std::atomic<bool> spectrum_shown{true};

static firdecim_rrrf audio_decim;
static iirfilt_rrrf  audio_dc;
static float        *audio;

static void set_zoom(uint8_t z);

static uint8_t settle_frames() {
    return base_ver.rev < 8 ? OEM_SETTLE : 0;
}

/* Span of the waterfall FFT in Hz */
static int32_t wf_span() {
    return base_decim ? SPAN_HZ / zoom : SPAN_HZ;
}

static bool spectrum_on() {
    return spectrum_shown && spectrum_enabled.load(std::memory_order_relaxed);
}

/* * Panorama * */

static void feed(cfloat *x, uint16_t n, firdecim_crcf decim, Panorama *sp, Panorama *wf) {
    for (size_t i = 0; i < n; i++)
        iq[i] = cfloat(x[i].imag(), x[i].real());   /* the base sends Q, I */

    if (!base_dc_block)
        iirfilt_cccf_execute_block(dc_block, iq, n, iq);

    if (spectrum_on()) {
        if (decim) {
            size_t m = n / zoom;

            firdecim_crcf_execute_block(decim, iq, m, iq_dec);
            sp->push(iq_dec, m);
        } else {
            sp->push(iq, n);
        }
    }
    if (wf)
        wf->push(iq, n);   /* also when hidden: the S-meter reads it */
}

/* Keep the smoothed trace on the same frequencies when the centre moves */
static void spectrum_follow(uint32_t freq) {
    if (freq == sp_freq)
        return;

    int32_t shift = ((int64_t)freq - sp_freq) * zoom * SPECTRUM_NFFT / SPAN_HZ;
    int32_t n     = SPECTRUM_NFFT;

    if (shift >= n || shift <= -n) {
        std::fill(sp_shown, sp_shown + n, (float)S_MIN);
    } else if (shift > 0) {
        std::copy(sp_shown + shift, sp_shown + n, sp_shown);
        std::fill(sp_shown + n - shift, sp_shown + n, (float)S_MIN);
    } else if (shift < 0) {
        std::copy_backward(sp_shown, sp_shown + n + shift, sp_shown + n);
        std::fill(sp_shown, sp_shown - shift, (float)S_MIN);
    }
    sp_freq = freq;
}

static void spectrum_frame(Panorama *sp, uint64_t now, bool tx, uint32_t freq, uint8_t fft_dec) {
    if (now - sp_time <= FRAME_MS || !sp->ready())
        return;

    sp->take(sp_psd);
    spectrum_follow(freq);

    /* Quick to rise, slow to fall: a carrier shows at once, the noise trace stays calm
     * and a weak steady carrier stands out of it */
    for (size_t i = 0; i < SPECTRUM_NFFT; i++) {
        float v = 10.0f * log10f(sp_psd[i]) + LEVEL_DB + zoom_db;

        sp_shown[i] += (v - sp_shown[i]) * (v > sp_shown[i] ? 0.6f : 0.2f);
    }
    spectrum_data(sp_shown, SPECTRUM_NFFT, tx, freq, fft_dec);
    sp_time = now;
}

static bool waterfall_frame(Panorama *wf, uint64_t now) {
    /* Waterfall speed 4 and 5 run at 20 and 30 frames/s, the slower ones at 15 and
     * waterfall_data() averages them */
    static const uint8_t frame_ms[] = {FRAME_MS, FRAME_MS, FRAME_MS, 1000 / 20, 1000 / 30};
    uint8_t ms = frame_ms[std::clamp(cfg_sm.p_waterfall_speed.get(), 1, 5) - 1];

    if (now - wf_time <= ms || settle || !wf->ready())
        return false;

    wf->take(wf_lin);
    for (size_t i = 0; i < WATERFALL_NFFT; i++)
        wf_db[i] = 10.0f * log10f(wf_lin[i]) + LEVEL_DB + zoom_db;
    wf_time = now;
    return true;
}

/* * S-meter and colours * */

static void s_meter() {
    if (dialog_msg_voice_get_state() == MSG_VOICE_RECORD)
        return;

    float bin_hz = (float)wf_span() / WATERFALL_NFFT;
    float p      = dsp_band_power(wf_lin, WATERFALL_NFFT, bin_hz, filter_from, filter_to);

    meter_update(10.0f * log10f(p) + SMETER_DB, params.spectrum_beta.x * 0.01f);
}

static void auto_levels(uint64_t now) {
    if (after_tx) {
        after_tx--;
        return;
    }
    if (floor_reset.exchange(false))
        floor_track.reset();

    float dt  = std::min((now - floor_time) / 1000.0f, 0.5f);
    float bin = dsp_noise_floor(wf_lin, WATERFALL_NFFT, FLOOR_GROUP, FLOOR_PCT);
    float db  = floor_track.update(10.0f * log10f(std::max(bin, 1e-20f)), dt);

    floor_time = now;

    /* The S-meter greys out what is under the noise in the receive filter */
    float bin_hz = (float)wf_span() / WATERFALL_NFFT;

    meter_set_noise(db + 10.0f * log10f(dsp_band_bins(bin_hz, filter_from, filter_to) / WATERFALL_NFFT) + SMETER_DB);

    float min = std::clamp(db + LEVEL_DB + zoom_db - COLOUR_BELOW, (float)S_MIN, (float)S8);
    float max = min + COLOUR_RANGE;

    spectrum_update_min(min);
    waterfall_update_min(min);
    spectrum_update_max(max);
    waterfall_update_max(max);
}

/* * Entry points * */

void dsp_samples(cfloat *buf_samples, uint16_t size, bool tx, uint32_t base_freq, bool vary_freq, uint8_t fft_dec) {
    if (!ready)
        return;

    if (base_freq != 0 && base_freq != cur_freq) {
        if (llabs((int64_t)base_freq - cur_freq) > SPAN_HZ)
            floor_reset = true;   /* another band */
        cur_freq = base_freq;
        wf_rx->reset();
        sp_rx->reset();
    }
    if (fft_dec && fft_dec != zoom)
        set_zoom(fft_dec);

    uint64_t now = get_time();

    if (settle)
        settle--;

    Panorama *sp = tx ? sp_tx : sp_rx;
    Panorama *wf = vary_freq ? NULL : (tx ? wf_tx : wf_rx);

    pthread_mutex_lock(&mux);
    feed(buf_samples, size, tx ? decim_tx : decim_rx, sp, wf);
    if (spectrum_on())
        spectrum_frame(sp, now, tx, base_freq, fft_dec);
    pthread_mutex_unlock(&mux);

    if (!wf || !waterfall_frame(wf, now))
        return;

    s_meter();   /* on every frame, also with the waterfall hidden */

    uint32_t span = wf_span();

    if (!tx) {
        pthread_mutex_lock(&snap_mux);
        memcpy(snap_db, wf_db, sizeof(snap_db));
        snap_center = base_freq ? base_freq : cur_freq;
        snap_width  = span;
        snap_seq++;
        pthread_mutex_unlock(&snap_mux);
    }

    if (waterfall_enabled.load(std::memory_order_relaxed))
        waterfall_data(wf_db, WATERFALL_NFFT, tx, base_freq, span);

    if (tx)
        after_tx = 2;
    else
        auto_levels(now);
}

static void set_zoom(uint8_t z) {
    if (z == zoom)
        return;

    pthread_mutex_lock(&mux);
    zoom = z;

    if (decim_rx) {
        firdecim_crcf_destroy(decim_rx);
        firdecim_crcf_destroy(decim_tx);
        decim_rx = decim_tx = NULL;
    }
    if (zoom > 1 && !base_decim) {
        /* 80 dB stopband: strong signals out of the zoomed span do not fold into it.
         * The scale keeps the noise of a bin where it was at zoom 1. */
        decim_rx = firdecim_crcf_create_kaiser(zoom, 12, 80.0f);
        decim_tx = firdecim_crcf_create_kaiser(zoom, 12, 80.0f);
        firdecim_crcf_set_scale(decim_rx, sqrtf(1.0f / zoom));
        firdecim_crcf_set_scale(decim_tx, sqrtf(1.0f / zoom));
    }
    std::fill(sp_shown, sp_shown + SPECTRUM_NFFT, (float)S_MIN);
    sp_rx->reset();
    wf_rx->reset();
    floor_reset = true;
    pthread_mutex_unlock(&mux);
}

static void on_zoom_change(Subject *subj, void *user_data) {
    int32_t z = cfg_sm.p_mode_zoom.get();
    bool    old_oem = base_ver.rev < 8 && util_compare_version(base_ver, (x6100_base_ver_t){1, 1, 9, 0}) < 0;

    if (old_oem) {
        set_zoom(z);
        return;
    }
    zoom_db = 3.0f * log2f(z);
    floor_reset = true;
    /* The OEM base from 1.1.9 decimates but does not report the zoom back */
    if (base_ver.rev < 8)
        set_zoom(z);
}

static void on_filter_change(Subject *subj, void *user_data) {
    int32_t low   = cfg_sm.cp_cur_filter_low.get();
    int32_t high  = cfg_sm.cp_cur_filter_high.get();
    int32_t shift = cfg_sm.p_band_if_shift.get();

    switch (cfg_sm.cp_cur_mode.get()) {
        case x6100_mode_lsb:
        case x6100_mode_lsb_dig:
        case x6100_mode_cwr:    /* below the carrier */
            filter_from = shift - high;
            filter_to   = shift - low;
            break;

        case x6100_mode_am:
        case x6100_mode_nfm:    /* both sides */
            filter_from = shift - high;
            filter_to   = shift + high;
            break;

        default:
            filter_from = shift + low;
            filter_to   = shift + high;
            break;
    }
}

static void on_mode_change(Subject *subj, void *user_data) {
    cur_mode = (x6100_mode_t)cfg_sm.cp_cur_mode.get();
}

static void on_freq_change(Subject *subj, void *user_data) {
    settle = settle_frames();
    if (base_ver.rev < 8)
        cur_freq = static_cast<SubjectT<int32_t> *>(subj)->get();
}

static void on_spectrum_show(Subject *subj, void *user_data) {
    spectrum_shown = cfg_sm.p_spectrum_show.get() || cfg_sm.p_walkie.get();
}

void dsp_init() {
    base_ver = x6100_control_get_base_ver();
    base_decim = util_compare_version(base_ver, (x6100_base_ver_t){1, 1, 9, 0}) >= 0 || base_ver.rev >= 8;
    base_dc_block = base_ver.rev >= 8;

    sp_rx = new Panorama(SPECTRUM_NFFT);
    sp_tx = new Panorama(SPECTRUM_NFFT);
    wf_rx = new Panorama(WATERFALL_NFFT);
    wf_tx = new Panorama(WATERFALL_NFFT);
    std::fill(sp_shown, sp_shown + SPECTRUM_NFFT, (float)S_MIN);

    dc_block = iirfilt_cccf_create_dc_blocker(0.005f);
    settle = settle_frames();
    sp_time = wf_time = floor_time = get_time();

    /* Decoders: 1/4 of the capture rate, DC removed under 50 Hz */
    audio       = (float *)malloc(AUDIO_CAPTURE_RATE * sizeof(float));
    audio_decim = firdecim_rrrf_create_kaiser(AUDIO_DECIM, 7, 60.0f);
    firdecim_rrrf_set_scale(audio_decim, 1.0f / AUDIO_DECIM);
    audio_dc    = iirfilt_rrrf_create_dc_blocker(M_PI * 50.0f * AUDIO_DECIM / AUDIO_CAPTURE_RATE);

    cfg_sm.p_spectrum_show.subscribe_and_notify(on_spectrum_show);
    cfg_sm.p_walkie.subscribe(on_spectrum_show);
    cfg_sm.p_mode_zoom.subscribe_and_notify(on_zoom_change);

    cfg_sm.p_band_if_shift.subscribe(on_filter_change);
    cfg_sm.cp_cur_filter_low.subscribe(on_filter_change);
    cfg_sm.cp_cur_filter_high.subscribe_and_notify(on_filter_change);
    cfg_sm.cp_cur_mode.subscribe_and_notify(on_filter_change);
    cfg_sm.cp_cur_mode.subscribe_and_notify(on_mode_change);
    cfg_sm.cp_fg_freq.subscribe(on_freq_change);

    ready = true;
}

void dsp_reset() {
    settle = settle_frames();
    iirfilt_cccf_reset(dc_block);
    sp_rx->reset();
    sp_tx->reset();
    wf_rx->reset();
    wf_tx->reset();
}

void dsp_set_waterfall_enabled(bool enabled) {
    waterfall_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        settle = 4;
        wf_time = get_time();
    }
}

void dsp_set_spectrum_enabled(bool enabled) {
    spectrum_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        settle = 4;
        sp_time = get_time();
    }
}

/* * Audio for the decoders * */

void dsp_put_audio_samples(size_t nsamples, int16_t *samples) {
    if (!ready)
        return;

    static float   block[AUDIO_DECIM];
    static uint8_t fill;

    screen_rec_put_audio_samples(nsamples, samples);

    if (dialog_msg_voice_get_state() == MSG_VOICE_RECORD) {
        dialog_msg_voice_put_audio_samples(nsamples, samples);
        return;
    }
    if (recorder_is_on())
        recorder_put_audio_samples(nsamples, samples);

    void (*sink)(unsigned int, float *) = NULL;

    if (rtty_get_state() == RTTY_RX)
        sink = rtty_put_audio_samples;
    else if (cur_mode == x6100_mode_cw || cur_mode == x6100_mode_cwr)
        sink = cw_put_audio_samples;
    else if (dialog_need_audio())
        sink = dialog_audio_samples;

    if (!sink)
        return;

    size_t out = 0;

    for (size_t i = 0; i < nsamples; i++) {
        block[fill++] = samples[i] / 32768.0f;
        if (fill == AUDIO_DECIM) {
            float v;

            fill = 0;
            firdecim_rrrf_execute(audio_decim, block, &v);
            iirfilt_rrrf_execute(audio_dc, v, &audio[out++]);
        }
    }
    if (out)
        sink(out, audio);
}

bool dsp_spectrum_snapshot(float *db, uint16_t n, uint32_t *center_hz, uint32_t *width_hz, uint32_t *seq) {
    pthread_mutex_lock(&snap_mux);
    bool ok = snap_seq > 0 && n <= WATERFALL_NFFT;
    if (ok) {
        memcpy(db, snap_db, n * sizeof(float));
        *center_hz = snap_center;
        *width_hz  = snap_width;
        *seq       = snap_seq;
    }
    pthread_mutex_unlock(&snap_mux);
    return ok;
}
