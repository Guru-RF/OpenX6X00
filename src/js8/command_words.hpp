/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 quick commands and the heartbeat offset
 */

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace x6100 {
namespace js8 {

enum class Query { ///< the one-press messages, with the text each one sends to CALL
    SnrQ,         ///< CALL SNR?      - what's my signal like?
    SendSnr,      ///< CALL SNR -12   - your signal here
    GridQ,        ///< CALL GRID?
    MyGrid,       ///< CALL GRID FN42AB
    InfoQ,        ///< CALL INFO?
    StatusQ,      ///< CALL STATUS?
    HearingQ,     ///< CALL HEARING?  - which stations do you copy?
    AgnQ,         ///< CALL AGN?      - say again
    RR,           ///< CALL RR
    SeventyThree, ///< CALL 73
};

/* An SNR written the way desktop JS8Call writes it: sign and at least two
 * digits ("+05", "-12"). Values beyond +-60 give "". */
std::string desktop_snr(int db);

/* The text of quick command `kind` to `call`. SendSnr needs `snr` and MyGrid
 * needs `grid`; when the one it needs is unusable the result is "". */
std::string query_text(Query kind, const std::string &call, int snr, const std::string &grid);

/* The heartbeat desktop JS8Call sends, e.g. "CALL: HEARTBEAT FN42" (grid cut
 * to 4 characters, left out if shorter). */
std::string heartbeat_text(const std::string &call, const std::string &grid);

struct OffsetActivity { ///< a station heard at some audio offset
    float offset_hz;       ///< where in the passband
    std::int64_t heard_ms; ///< when, wall clock ms
};

/* Picks an offset for our heartbeat the way desktop JS8Call does: somewhere in
 * [lo, hi) that is at least `width` Hz away from every station heard in the
 * last 30 seconds. Random `width` steps are tried first, then random offsets
 * anywhere in the range; if all of them are taken the answer is `lo`. */
int find_free_offset(const std::vector<OffsetActivity> &heard, std::int64_t now, std::mt19937 &gen, int lo = 500,
                     int hi = 1000, int width = 50);

} // namespace js8
} // namespace x6100
