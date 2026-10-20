/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 alert words
 */

#include "alert_rules.hpp"

#include "msg_kind.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace x6100 {
namespace js8 {

/* Upper-cased pieces of `s` between any of the characters in `delims`.
 * strchr() also finds the terminating NUL, so a NUL inside `s` separates
 * pieces too. */
static std::vector<std::string> tokens_of(const std::string &s, const char *delims) {
    std::vector<std::string> pieces(1);
    for (const char ch : s) {
        if (std::strchr(delims, ch) == nullptr) {
            pieces.back() += (char)std::toupper((unsigned char)ch);
        } else if (!pieces.back().empty()) {
            pieces.emplace_back();
        }
    }
    if (pieces.back().empty()) pieces.pop_back();
    return pieces;
}

static bool contains(const std::vector<std::string> &v, const std::string &w) {
    return std::find(v.begin(), v.end(), w) != v.end();
}

std::vector<std::string> parse_alert_words(const std::string &input) {
    const std::size_t limit = 20;
    std::vector<std::string> list;
    for (const std::string &w : tokens_of(input, ", \t")) {
        if (list.size() == limit) break;
        if (!contains(list, w)) list.push_back(w);
    }
    return list;
}

std::string format_alert_words(const std::vector<std::string> &list) {
    std::string joined;
    for (std::size_t i = 0; i < list.size(); i++) {
        if (i > 0) joined += ' ';
        joined += list[i];
    }
    return joined;
}

std::string alert_word_hit(const std::string &text, const std::string &sender,
                           const std::vector<std::string> &list) {
    if (list.empty()) return std::string();
    const std::vector<std::string> seen = tokens_of(text, ":> \t");
    for (const std::string &w : list) {
        if (contains(seen, w)) return w;
        /* A word with a digit that isn't a @group is taken for a call. */
        const bool is_call = std::any_of(w.begin(), w.end(), [](char c) { return std::isdigit((unsigned char)c); });
        if (is_call && w[0] != '@' && !sender.empty() && base_callsign(w) == base_callsign(sender)) return w;
    }
    return std::string();
}

} // namespace js8
} // namespace x6100
