/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 heard stations, like the Call Activity list of desktop JS8Call
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace x6100 {
namespace js8 {

struct Station { ///< one station in the list
    std::string call;
    std::string grid;             ///< newest grid from a heartbeat, CQ or GRID reply
    std::int64_t heard_ms = 0;    ///< wall clock ms of the latest decode
    int snr = 0;                  ///< our report on them, latest decode
    float freq_hz = 0;            ///< audio offset of the latest decode
    int mode = 0;                 ///< varicode submode of the latest decode (0, 1, 2 or 4)
    bool heard_me = false;        ///< they have sent something to us
    std::int64_t heard_me_ms = 0; ///< when they last did
    std::optional<int> reported_snr; ///< their report on us, once they've given one
    bool x6100 = false;           ///< they have sent something to @X6100
};

struct StationEvent { ///< what the list takes from one decoded message
    std::string from, to, text;
    bool to_me = false;
    int snr = 0;
    float freq_hz = 0;
    int mode = 0;             ///< varicode submode (0, 1, 2 or 4)
    std::int64_t when_ms = 0; ///< wall clock ms
};

/* The stations heard recently. Those that have called us come first, the
 * way desktop JS8Call stars them ("Hearing Your Station"). */
class StationList final {
public: /* used by the dialog's operations and the tests */
    static constexpr std::int64_t EXPIRE_MS = 3'600'000; ///< an hour without a decode drops a station

    /* Records one decode; our own call (any form of it) is ignored. */
    void add(const StationEvent &heard, const std::string &mine);

    /* The stations heard less than EXPIRE_MS before `now`: first those that
     * have called us, newest call first, then the others, newest decode
     * first. */
    std::vector<Station> sorted(std::int64_t now) const;

    void clear() { by_call_.clear(); }

private: /* keyed by the call exactly as decoded */
    std::map<std::string, Station> by_call_;
};

} // namespace js8
} // namespace x6100
