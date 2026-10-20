/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, reading rendered messages
 */

#include "msg_kind.hpp"

#include "frame_text.hpp"

#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace x6100 {
namespace js8 {

static std::string to_upper(const std::string &s) {
    std::string up(s.size(), '\0');
    std::transform(s.begin(), s.end(), up.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return up;
}

static std::vector<std::string> words_of(const std::string &s) {
    std::vector<std::string> words;
    std::string              word;
    for (char c : s) {
        if (!std::isspace((unsigned char)c)) {
            word += c;
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

static bool has_digit(std::string_view s) {
    for (char c : s)
        if (std::isdigit((unsigned char)c)) return true;
    return false;
}

bool is_x6100_group(const std::string &to) {
    const std::string_view group = X6100_GROUP;
    if (to.size() < group.size() || std::string_view(to).substr(0, group.size()) != group) return false;
    return to.size() == group.size() || to[group.size()] == '>';
}

std::string base_callsign(const std::string &callsign) {
    const std::string whole = to_upper(callsign);
    std::string_view  best;
    std::size_t       start = 0;
    while (start < whole.size()) {
        std::size_t end = whole.find('/', start);
        if (end == std::string::npos) end = whole.size();
        const std::string_view part(whole.data() + start, end - start);
        if (part.size() > best.size() && has_digit(part)) best = part; // the first of equal lengths wins
        start = end + 1;
    }
    return best.empty() ? whole : std::string(best);
}

MessageClass classify(const std::string &message, const std::string &my_call) {
    MessageClass result;

    // "CALL: body" names the sender; anything else before a colon is body.
    std::string body  = message;
    const auto  colon = message.find(':');
    if (colon != std::string::npos && colon != 0 && is_callsign_like(message.substr(0, colon))) {
        result.from = to_upper(message.substr(0, colon));
        body        = message.substr(colon + 1);
    }

    const auto w = words_of(to_upper(body));
    if (w.empty()) return result;
    const bool two = w.size() >= 2;

    result.heartbeat = w[0] == "@HB" || (two && w[1] == "HEARTBEAT");
    result.cq        = w[0].compare(0, 2, "CQ") == 0 || (w[0] == "@ALLCALL" && two && w[1] == "CQ");

    if (w[0][0] == '@') {
        result.to       = w[0];
        result.to_group = true;
        return result; // groups get no SNR report flag and are never "to me"
    }
    if (!is_callsign_like(w[0])) return result;

    result.to         = w[0];
    result.snr_report = w.size() >= 3 && w[1] == "SNR";
    result.to_me      = !my_call.empty() && base_callsign(w[0]) == base_callsign(my_call);
    return result;
}

Checksum verify_command_checksum(std::string &message) {
    // Expected shape: "FROM: TO" + command + payload + " " + checksum, where
    // TO is a callsign or @GROUP running up to a space or a relay '>'.
    const auto sep = message.find(": ");
    if (sep == std::string::npos) return Checksum::None;
    const std::size_t to_begin = sep + 2;
    std::size_t       to_end   = message.find_first_of(" >", to_begin);
    if (to_end == std::string::npos) to_end = message.size();
    if (to_end == to_begin) return Checksum::None;

    // Commands that buffer a payload, as they appear after TO. A longer one
    // comes before any shorter one it starts with (" MSG TO:" before " MSG").
    // Our renderer writes a relay as "TO > VIA", hence the spaced form.
    struct Buffered {
        std::string_view seen;
        const char      *command;
    };
    static constexpr Buffered COMMANDS[] = {
        {" QUERY MSGS?", " QUERY MSGS?"}, {" QUERY MSGS", " QUERY MSGS"}, {" QUERY CALL", " QUERY CALL"},
        {" MSG TO:", " MSG TO:"},         {" QUERY", " QUERY"},           {" GRID", " GRID"},
        {" MSG", " MSG"},                 {" CMD", " CMD"},               {">", ">"},
        {" >", ">"},
    };
    const std::string_view after_to = std::string_view(message).substr(to_end);
    const Buffered        *found    = nullptr;
    for (const auto &c : COMMANDS) {
        if (after_to.starts_with(c.seen)) {
            found = &c;
            break;
        }
    }
    if (!found || !js8core::protocol::varicode::is_command_buffered(found->command)) return Checksum::None;
    const int sum_bits = js8core::protocol::varicode::is_command_checksummed(found->command);
    if (sum_bits != 16 && sum_bits != 32) return Checksum::None;

    // The payload without its outer spaces. A command with nothing after it
    // (a bare "QUERY MSGS") has no checksum to check.
    std::string_view payload = after_to.substr(found->seen.size());
    const auto       first   = payload.find_first_not_of(' ');
    if (first == std::string_view::npos) return Checksum::None;
    payload = payload.substr(first, payload.find_last_not_of(' ') - first + 1);

    // 32-bit checksums are 6 characters, 16-bit ones 3; a space must precede it.
    const std::size_t width = sum_bits == 32 ? 6 : 3;
    if (payload.size() < width + 2 || payload[payload.size() - width - 1] != ' ') return Checksum::Invalid;
    const std::string sum(payload.substr(payload.size() - width));
    const std::string text(payload.substr(0, payload.size() - width - 1));
    const bool        ok = sum_bits == 32 ? js8core::protocol::varicode::checksum32_valid(sum, text)
                                          : js8core::protocol::varicode::checksum16_valid(sum, text);
    if (ok) {
        // The checksum is the last word of the whole message: cut it and the
        // spaces before it.
        message.erase(message.find_last_not_of(' ') + 1 - width);
        while (!message.empty() && message.back() == ' ') message.pop_back();
    }
    return ok ? Checksum::Valid : Checksum::Invalid;
}

bool is_grid(const std::string &text) {
    // Field (A-R), square (0-9), then optionally subsquare (A-X), extended
    // square (0-9) and extended subsquare (A-X), each a pair of characters.
    const std::size_t n = text.size();
    if (n < 4 || n > 10 || n % 2 != 0 || text == "RR73") return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = text[i];
        bool       ok;
        if (i < 2)
            ok = c >= 'A' && c <= 'R';
        else if (i / 2 % 2 == 1)
            ok = c >= '0' && c <= '9';
        else
            ok = c >= 'A' && c <= 'X';
        if (!ok) return false;
    }
    return true;
}

std::string find_grid(const std::string &text) {
    const auto w = words_of(text);
    for (std::size_t i = 1; i < w.size(); ++i)
        if (w[i - 1] == "GRID" && is_grid(w[i])) return w[i].substr(0, 6);
    for (std::size_t i = w.size(); i-- > 0;)
        if (is_grid(w[i])) return w[i].substr(0, 6);
    return {};
}

std::string better_grid(const std::string &have, const std::string &got) {
    // Nothing new, or the same place given less precisely: keep what we have.
    const bool keep = got.empty() || (got.size() < have.size() && have.starts_with(got));
    return keep ? have : got;
}

} // namespace js8
} // namespace x6100
