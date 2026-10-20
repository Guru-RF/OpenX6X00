/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 quick commands and the heartbeat offset
 */

#include "command_words.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace x6100 {
namespace js8 {

static std::string to_upper(const std::string &in) {
    std::string up;
    up.reserve(in.size());
    for (char c : in) up.push_back((char)std::toupper((unsigned char)c));
    return up;
}

/* dest + word + value, or "" when there is no value to send. */
static std::string with_value(const std::string &dest, const char *word, const std::string &value) {
    return value.empty() ? std::string() : dest + word + value;
}

/* Nobody heard in the last 30 s within `width` Hz of `hz`. The distance is
 * worked out in float, as the activity list stores it. */
static bool is_clear(const std::vector<OffsetActivity> &heard, std::int64_t now, int hz, int width) {
    for (const OffsetActivity &h : heard) {
        const bool recent = now - h.heard_ms < 30'000;
        if (recent && std::fabs(h.offset_hz - hz) < width) return false;
    }
    return true;
}

std::string desktop_snr(int db) {
    if (db > 60 || db < -60) return std::string();
    char text[8];
    std::snprintf(text, sizeof text, "%+03d", db);
    return text;
}

std::string query_text(Query kind, const std::string &call, int snr, const std::string &grid) {
    const std::string dest = to_upper(call);
    if (dest.empty()) return std::string();

    const char *tail = nullptr;
    switch (kind) {
    case Query::SnrQ: tail = " SNR?"; break;
    case Query::GridQ: tail = " GRID?"; break;
    case Query::InfoQ: tail = " INFO?"; break;
    case Query::StatusQ: tail = " STATUS?"; break;
    case Query::HearingQ: tail = " HEARING?"; break;
    case Query::AgnQ: tail = " AGN?"; break;
    case Query::RR: tail = " RR"; break;
    case Query::SeventyThree: tail = " 73"; break;
    case Query::SendSnr: return with_value(dest, " SNR ", desktop_snr(snr));
    case Query::MyGrid: return with_value(dest, " GRID ", to_upper(grid));
    }
    return tail ? dest + tail : std::string();
}

std::string heartbeat_text(const std::string &call, const std::string &grid) {
    std::string hb = to_upper(call) + ": HEARTBEAT";
    if (grid.size() >= 4) hb += " " + to_upper(grid.substr(0, 4));
    return hb;
}

int find_free_offset(const std::vector<OffsetActivity> &heard, std::int64_t now, std::mt19937 &gen, int lo, int hi,
                     int width) {
    const int steps = (hi - lo) / width;
    if (steps <= 0) return lo;

    /* Slots first: offsets on a `width` grid pack heartbeats neatly. */
    for (int tries = steps; tries > 0; tries--) {
        const int hz = lo + width * (int)(gen() % (unsigned)steps);
        if (is_clear(heard, now, hz, width)) return hz;
    }
    for (int tries = steps; tries > 0; tries--) {
        const int hz = lo + (int)(gen() % (unsigned)(hi - lo));
        if (is_clear(heard, now, hz, width)) return hz;
    }
    return lo;
}

} // namespace js8
} // namespace x6100
