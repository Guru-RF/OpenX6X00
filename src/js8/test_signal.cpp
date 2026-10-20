/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 test audio for tests and tools
 */

#include "test_signal.hpp"

#include "submodes.hpp"

#include "js8core/decoder.hpp"
#include "js8core/protocol/costas.hpp"
#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace x6100 {
namespace js8 {

namespace { // types and helpers for this file

using Symbols = std::array<int, js8core::kJs8NumSymbols>;

// One station's frames, already turned into tone numbers.
struct Transmission {
    const TestStation   *station;
    const Speed         *speed;
    std::vector<Symbols> frames;
};

Transmission encode(const TestStation &st) {
    namespace proto = js8core::protocol;
    Transmission tx{&st, &speed(st.speed), {}};
    const auto  &sync =
        proto::costas(tx.speed->original_costas ? proto::CostasType::Original : proto::CostasType::Modified);
    for (const auto &[frame, frame_type] :
         proto::varicode::build_message_frames(st.call, st.grid, "", st.text, false, false, tx.speed->varicode)) {
        Symbols tones{};
        js8core::legacy_encode(frame_type, sync, frame.c_str(), tones.data());
        tx.frames.push_back(tones);
    }
    return tx;
}

// Adds one frame as a continuous-phase 8-FSK burst starting at `first`.
void add_frame(std::vector<float> &audio, std::size_t first, const Symbols &tones, const Transmission &tx,
               int rate, double amplitude) {
    const double per_symbol = tx.speed->symbol_seconds() * rate;
    double       phase      = 0.0;
    for (int s = 0; s < js8core::kJs8NumSymbols; ++s) {
        const double hz   = tx.station->offset_hz + tones[s] * tx.speed->tone_spacing_hz();
        const double step = 2.0 * M_PI * hz / rate;
        // Symbol edges rounded to whole samples, so lengths never accumulate error.
        const std::size_t from = first + (std::size_t)std::llround(s * per_symbol);
        const std::size_t to   = std::min(first + (std::size_t)std::llround((s + 1) * per_symbol), audio.size());
        for (std::size_t j = from; j < to; ++j, phase += step) audio[j] += (float)(amplitude * std::sin(phase));
    }
}

} // types and helpers for this file

std::vector<float> make_test_band(const std::vector<TestStation> &band, int rate, float noise_rms,
                                  std::uint32_t noise_seed) {
    std::vector<Transmission> txs;
    std::size_t               longest_s = 0;
    for (const TestStation &st : band) {
        txs.push_back(encode(st));
        longest_s = std::max(longest_s, txs.back().frames.size() * txs.back().speed->period_s);
    }
    // Whole 30 s, and never less than one.
    const std::size_t seconds = std::max<std::size_t>((longest_s + 29) / 30 * 30, 30);
    std::vector<float> mix(seconds * rate, 0.0f);

    // The noise power that falls in 2500 Hz of the rate / 2 wide band; the
    // tone power is set against it.
    const double noise_in_2500 = (double)noise_rms * noise_rms * 2500.0 / (rate / 2.0);

    for (const Transmission &tx : txs) {
        const double amplitude = std::sqrt(2.0 * noise_in_2500 * std::pow(10.0, tx.station->snr_db / 10.0));
        for (std::size_t slot = 0; slot < tx.frames.size(); ++slot) {
            const std::size_t first =
                (slot * tx.speed->period_ms() + tx.speed->start_delay_ms) * (std::size_t)rate / 1000;
            add_frame(mix, first, tx.frames[slot], tx, rate, amplitude);
        }
    }

    std::mt19937                    gen(noise_seed);
    std::normal_distribution<float> gauss(0.0f, noise_rms);
    for (float &x : mix) x += gauss(gen);
    return mix;
}

} // namespace js8
} // namespace x6100
