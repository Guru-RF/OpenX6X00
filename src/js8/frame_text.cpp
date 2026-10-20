/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, frames to text
 */

#include "frame_text.hpp"

#include "js8core/protocol/varicode.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>

namespace x6100 {
namespace js8 {

namespace vc = js8core::protocol::varicode;

static bool space(char c) { return std::isspace((unsigned char)c) != 0; }

static std::string skip_leading_space(const std::string &s) {
    std::size_t i = 0;
    while (i < s.size() && space(s[i])) ++i;
    return s.substr(i);
}

// Up to `max` whitespace-separated words from the start of `s`.
static std::vector<std::string> leading_words(const std::string &s, std::size_t max) {
    std::vector<std::string> words;
    std::size_t              i = 0;
    while (words.size() < max) {
        while (i < s.size() && space(s[i])) ++i;
        if (i == s.size()) break;
        const std::size_t start = i;
        while (i < s.size() && !space(s[i])) ++i;
        words.push_back(s.substr(start, i - start));
    }
    return words;
}

// Data text that opens a message with two callsigns, "K1ABC W2XYZ ...", is
// shown with the sender marked: "K1ABC: W2XYZ ...". Later frames continue
// the text and are left alone.
static std::string mark_sender(const std::string &text, bool opens_message) {
    if (!opens_message) return text;

    std::size_t gap = 0;
    while (gap < text.size() && !space(text[gap])) ++gap;
    if (gap == text.size()) return text;                              // a single word
    if (text.find(':') < gap) return text;                            // already marked

    const auto words = leading_words(text, 2);
    if (words.size() < 2 || !is_callsign_like(words[0]) || !is_callsign_like(words[1])) return text;
    return words[0] + ": " + text.substr(gap + 1);
}

// "EA8" and "G4ABC" become "EA8/G4ABC"; either alone stays as it is.
static std::string join_compound(const std::vector<std::string> &parts) {
    const std::string prefix = parts.empty() ? std::string() : parts[0];
    const std::string base   = parts.size() < 2 ? std::string() : parts[1];
    if (prefix.empty()) return base;
    if (base.empty()) return prefix;
    return prefix + "/" + base;
}

// The CQ forms a heartbeat-style frame can carry, by their 3-bit code.
static constexpr const char *CQ_TEXT[8] = {
    "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", // 0-3
    "CQ FIELD", "CQ FD", "CQ CQ", "CQ",          // 4-7
};

static constexpr std::chrono::seconds COMPOUND_KEPT{60};
static constexpr float                COMPOUND_NEAR_HZ = 10.0f;

static std::string heartbeat_text(const std::string &raw, bool *ok) {
    std::uint8_t kind = 0, cq_code = 0;
    bool         is_cq = false;
    const auto   parts = vc::unpack_heartbeat_message(raw, &kind, &is_cq, &cq_code);
    *ok                = !parts.empty();
    if (!*ok) return {};

    std::string text = join_compound(parts);
    if (!text.empty()) text += ": ";
    // JS8Call 2.2 and later show every heartbeat status as plain HB.
    text += is_cq ? "@ALLCALL " + std::string(cq_code < 8 ? CQ_TEXT[cq_code] : "CQ") : "@HB HEARTBEAT";
    if (parts.size() > 2 && !parts[2].empty()) text += " " + parts[2];
    return text;
}

bool is_callsign_like(const std::string &word) {
    // '@' fails the character test, so @GROUP names are never callsigns.
    if (word.size() < 3 || word.size() > 12) return false;
    bool digit = false;
    for (unsigned char c : word) {
        if (std::isdigit(c))
            digit = true;
        else if (!std::isalpha(c) && c != '/')
            return false;
    }
    return digit;
}

std::string FrameRenderer::render(const std::string &raw, int *type_bits, float offset_hz, Clock::time_point now) {
    // Too short for a frame, or holding a space (so not a packed frame): pass it through.
    if (raw.size() < 12 || raw.find(' ') != std::string::npos) return raw;

    const bool opens = *type_bits & FRAME_FIRST;

    // Data comes first, as on desktop. A frame typed as data uses the fast
    // data packing and is never anything else.
    if (*type_bits & FRAME_DATA) {
        const std::string text = vc::unpack_fast_data_message(raw);
        return text.empty() ? raw : mark_sender(text, opens);
    }
    // Normal and Slow mark data inside the payload instead; set the type bit
    // so callers see data the same way at every speed.
    if (std::string text = vc::unpack_data_message(raw); !text.empty()) {
        *type_bits |= FRAME_DATA;
        return mark_sender(text, opens);
    }

    bool heartbeat = false;
    if (std::string text = heartbeat_text(raw, &heartbeat); heartbeat) return text;

    // A compound callsign on its own (kind 1) or a directed frame that goes
    // with one (kind 2).
    std::uint8_t  kind = 0, bits3 = 0;
    std::uint16_t number = 0;
    const auto    compound = vc::unpack_compound_message(raw, &kind, &number, &bits3);
    if (!compound.empty()) {
        std::string text;
        for (const auto &part : compound) {
            const std::string word = skip_leading_space(part);
            if (word.empty()) continue;
            if (!text.empty()) text += ' ';
            text += word;
        }

        const std::string &call = compound.front();
        if (kind == 1 && !call.empty()) {
            std::erase_if(heard_, [&](const HeardCompound &h) { return now - h.when > COMPOUND_KEPT; });
            heard_.push_back({call, offset_hz, now});
        }
        if (kind == 2 && !call.empty() && !text.empty()) {
            // The nearest compound call heard within 10 Hz sent this; the
            // first one wins a tie. Each is used once.
            auto best = heard_.end();
            for (auto it = heard_.begin(); it != heard_.end(); ++it) {
                const float d = std::fabs(offset_hz - it->offset_hz);
                if (d <= COMPOUND_NEAR_HZ && (best == heard_.end() || d < std::fabs(offset_hz - best->offset_hz)))
                    best = it;
            }
            if (best != heard_.end()) {
                text = best->call + ": " + text;
                heard_.erase(best);
            }
        }
        if (!text.empty()) return text;
    }

    // Directed: "FROM: TO COMMAND [NUMBER]", or directed free text.
    std::uint8_t dkind = 0;
    std::string  text;
    int          words = 0;
    for (const auto &part : vc::unpack_directed_message(raw, &dkind)) {
        const std::string word = skip_leading_space(part);
        if (word.empty()) continue;
        text += words == 0 ? "" : words == 1 ? ": " : " ";
        text += word;
        ++words;
    }
    return words ? text : raw;
}

} // namespace js8
} // namespace x6100
