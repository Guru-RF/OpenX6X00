/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 heard stations, like the Call Activity list of desktop JS8Call
 */

#include "heard_list.hpp"

#include "msg_kind.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sstream>

namespace x6100 {
namespace js8 {

/* "+05" or "-12": a sign followed by digits only, small enough for an int.
 * A longer number (a bad decode) is not taken as a report. */
static bool read_report(const std::string &tok, int &db) {
    if (tok.size() < 2 || (tok.front() != '-' && tok.front() != '+')) return false;
    const auto is_digit = [](char c) { return std::isdigit((unsigned char)c) != 0; };
    if (!std::all_of(tok.begin() + 1, tok.end(), is_digit)) return false;
    errno = 0;
    const long long v = std::strtoll(tok.c_str(), nullptr, 10);
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) return false;
    db = (int)v;
    return true;
}

void StationList::add(const StationEvent &heard, const std::string &mine) {
    if (heard.from.empty()) return;
    /* Our own transmission coming back through the receiver. */
    if (!mine.empty() && base_callsign(mine) == base_callsign(heard.from)) return;

    Station &s = by_call_[heard.from];
    s.call = heard.from;
    s.heard_ms = heard.when_ms;
    s.snr = heard.snr;
    s.freq_hz = heard.freq_hz;
    s.mode = heard.mode;
    if (is_x6100_group(heard.to)) s.x6100 = true;

    /* Only the part after "FROM:" counts, e.g. "@HB HEARTBEAT FN42",
     * "@ALLCALL CQ CQ CQ FN03", "K2XYZ GRID EM48AB", "K2XYZ SNR -12" or
     * "K2XYZ HEARTBEAT SNR -08". */
    const std::size_t colon = heard.text.find(':');
    const std::string body = colon == std::string::npos ? heard.text : heard.text.substr(colon + 1);

    /* Heartbeats and CQs end in a grid, and so does a GRID answer. */
    s.grid = better_grid(s.grid, find_grid(body));

    if (!heard.to_me) return;
    /* Whatever they send to us shows they hear us, which is also how
     * desktop decides on its star. An "SNR -12" in it is their report on
     * us; with several, the last one counts. */
    s.heard_me = true;
    s.heard_me_ms = heard.when_ms;
    std::istringstream in(body);
    std::string prev, tok;
    while (in >> tok) {
        int db = 0;
        if (prev == "SNR" && read_report(tok, db)) s.reported_snr = db;
        prev = tok;
    }
}

std::vector<Station> StationList::sorted(std::int64_t now) const {
    std::vector<Station> list;
    for (const auto &entry : by_call_) {
        if (now - entry.second.heard_ms < EXPIRE_MS) list.push_back(entry.second);
    }
    /* Stable, so equal times keep the map's (alphabetical) order. */
    std::stable_sort(list.begin(), list.end(), [](const Station &x, const Station &y) {
        if (x.heard_me && y.heard_me) return x.heard_me_ms > y.heard_me_ms;
        if (x.heard_me || y.heard_me) return x.heard_me;
        return x.heard_ms > y.heard_ms;
    });
    return list;
}

} // namespace js8
} // namespace x6100
