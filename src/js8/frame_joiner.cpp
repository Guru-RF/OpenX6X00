/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, joining frames into messages
 */

#include "frame_joiner.hpp"

#include "submodes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <string_view>

namespace x6100 {
namespace js8 {

static bool space(char c) { return std::isspace((unsigned char)c) != 0; }

static std::string trimmed(const std::string &s) {
    constexpr const char *BLANK = " \t\r\n";
    const auto            first = s.find_first_not_of(BLANK);
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(BLANK) - first + 1);
}

static std::vector<std::string> words_of(const std::string &s) {
    std::vector<std::string> words;
    std::size_t              i = 0;
    for (;;) {
        while (i < s.size() && space(s[i])) ++i;
        if (i == s.size()) return words;
        const std::size_t start = i;
        while (i < s.size() && !space(s[i])) ++i;
        words.push_back(s.substr(start, i - start));
    }
}

static std::string to_upper(const std::string &s) {
    std::string up(s.size(), '\0');
    std::transform(s.begin(), s.end(), up.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return up;
}

// Stricter than is_callsign_like(): a letter is needed as well as a digit.
static bool plausible_call(const std::string &word) {
    if (word.size() < 3 || word.size() > 12) return false;
    bool letter = false, digit = false;
    for (unsigned char c : word) {
        if (std::isalpha(c))
            letter = true;
        else if (std::isdigit(c))
            digit = true;
        else if (c != '/')
            return false;
    }
    return letter && digit;
}

// A 4 or 6 character locator, upper case.
static bool short_grid(const std::string &w) {
    if (w.size() != 4 && w.size() != 6) return false;
    auto in = [](char c, char lo, char hi) { return c >= lo && c <= hi; };
    if (!in(w[0], 'A', 'R') || !in(w[1], 'A', 'R') || !in(w[2], '0', '9') || !in(w[3], '0', '9')) return false;
    return w.size() == 4 || (in(w[4], 'A', 'X') && in(w[5], 'A', 'X'));
}

// "+12", "-5", "7": the optional number after a command.
static bool small_number(std::string_view s) {
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) s.remove_prefix(1);
    if (s.empty() || s.size() > 3) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// What can follow the callsign in the directed half of a compound sender's
// message, with or without a number after it. Checked in this order.
static constexpr std::string_view DIRECTED_COMMANDS[] = {
    "AGN?", "QSL?",  "HW CPY?", "MSG TO:", "SNR?", "INFO?", "GRID?", "STATUS?", "QUERY MSGS?", "HEARING?", "STATUS",
    "HEARING", "QUERY CALL", "QUERY MSGS", "QUERY", "CMD", "MSG", "NACK", "ACK", "73", "YES", "NO", "HEARTBEAT SNR",
    "SNR", "QSL", "RR", "SK", "FB", "INFO", "GRID", "DIT DIT", ">", "?",
};

// `tail` is upper case, its words separated by single spaces.
static bool is_command_tail(const std::string &tail) {
    for (std::string_view cmd : DIRECTED_COMMANDS) {
        if (!std::string_view(tail).starts_with(cmd)) continue;
        if (tail.size() == cmd.size()) return true;
        if (tail[cmd.size()] == ' ' && small_number(std::string_view(tail).substr(cmd.size() + 1))) return true;
    }
    return false;
}

// "W2XYZ", "W2XYZ SNR -12", "W2XYZ MSG TO:": a directed frame of a compound sender.
static bool is_directed_after_compound(const std::string &text) {
    const auto words = words_of(text);
    if (words.empty() || !plausible_call(words[0])) return false;
    if (words.size() == 1) return true;
    std::string tail = words[1];
    for (std::size_t i = 2; i < words.size(); ++i) tail += " " + words[i];
    return is_command_tail(to_upper(tail));
}

// A compound sender's directed message comes as "CALL GRID" and then either
// "<....>: rest" or the bare directed part. Put the sender in front of the
// second frame and drop the first: "CALL: rest".
static void fold_compound_sender(std::vector<RxFrame> &msg) {
    if (msg.size() <= 1) return;
    RxFrame &head = msg[0];
    RxFrame &next = msg[1];
    if (head.is_data() || next.is_data() || !head.is_first() || head.is_last() || next.is_first()) return;

    const std::string intro = trimmed(head.text);
    const std::string body  = trimmed(next.text);
    const auto        words = words_of(intro);
    if (words.size() != 2 || !plausible_call(words[0]) || !short_grid(to_upper(words[1]))) return;
    const std::string sender = trimmed(intro.substr(0, intro.find(' ')));

    static const std::regex unknown_sender(R"(^<\.\.\.\.>:\s*(.+)$)");
    std::smatch             m;
    if (std::regex_match(body, m, unknown_sender)) {
        const std::string rest = trimmed(m[1].str());
        if (!rest.empty()) {
            head.text.clear();
            next.text = sender + ": " + rest;
            return;
        }
    }
    if (is_directed_after_compound(body)) {
        head.text.clear();
        next.text = sender + ": " + body;
        return;
    }
    // The renderer may already have put the compound call in front ("EA8/K1ABC:
    // G4XYZ"), from the frame it heard at this offset; then "CALL GRID" adds
    // nothing.
    if (body.starts_with(sender + ":")) head.text.clear();
}

std::string assemble_multipart_text(const std::vector<RxFrame> &parts) {
    // Data frames break words anywhere and keep their own spaces. A buffered
    // command loses the space between header and payload when it is packed,
    // so it's put back there.
    std::string    joined;
    const RxFrame *prev = nullptr;
    for (const RxFrame &f : parts) {
        if (f.text.empty()) continue;
        if (prev && !prev->is_data() && !space(prev->text.back()) && !space(f.text.front())) joined += ' ';
        joined += f.text;
        prev = &f;
    }
    return joined;
}

RxFrame MessageAssembler::finish(const Pending &p) const {
    std::vector<RxFrame> copy = p.got;
    fold_compound_sender(copy);

    RxFrame whole = p.got.back(); // the newest frame's SNR, time, offset...
    whole.text    = assemble_multipart_text(copy);
    whole.msg_id  = p.number;
    whole.partial = false;
    return whole;
}

void MessageAssembler::progress(const Pending &p) const {
    if (on_progress_) {
        RxFrame so_far = finish(p);
        so_far.partial = true;
        on_progress_(so_far);
    }
}

void MessageAssembler::add(const RxFrame &in) {
    flush_stale(in.timestamp_ms);

    // The message this frame may belong to: first in key order with the same
    // speed and an offset close enough.
    const float near  = (float)speed_from_varicode(in.mode).rx_threshold_hz;
    const auto  match = std::find_if(pending_.begin(), pending_.end(), [&](const auto &entry) {
        return entry.first.first == in.mode && std::fabs(in.freq_hz - (float)entry.first.second) <= near;
    });
    const Slot slot{in.mode, (int)std::lround(in.freq_hz)};

    if (in.is_first()) {
        // A new message replaces whatever was building at this offset.
        if (match != pending_.end()) pending_.erase(match);
        if (in.is_last()) {
            RxFrame single = in;
            single.msg_id  = ++last_id_;
            single.partial = false;
            on_done_(single);
        } else {
            progress(pending_[slot] = Pending{{in}, in.timestamp_ms, ++last_id_});
        }
        return;
    }

    if (match != pending_.end()) {
        const Slot key = match->first;
        Pending   &p   = match->second;
        p.got.push_back(in);
        p.newest_ms = in.timestamp_ms;
        if (!in.is_last()) {
            progress(p);
            return;
        }
        on_done_(finish(p));
        pending_.erase(key);
        return;
    }

    // The first frame was missed; go on with what there is.
    Pending p{{in}, in.timestamp_ms, ++last_id_};
    if (in.is_last()) {
        on_done_(finish(p));
        return;
    }
    progress(p);
    pending_[slot] = std::move(p);
}

void MessageAssembler::flush_stale(std::int64_t at_ms) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (at_ms - it->second.newest_ms <= IDLE_TIMEOUT_MS) {
            ++it;
            continue;
        }
        on_done_(finish(it->second));
        it = pending_.erase(it);
    }
}

bool DuplicateFilter::seen(int submode, const std::string &raw, float offset_hz, std::int64_t at_ms) {
    // 30 s is the longest slot (Slow); nothing older can still be a repeat.
    std::erase_if(history_, [&](const Seen &s) { return at_ms - s.at >= 30 * 1000; });

    const Speed &rules = speed_from_varicode(submode);
    for (const Seen &s : history_) {
        // A second less than the slot: the next slot's decode can come early.
        if (s.submode == submode && s.raw == raw && std::fabs(s.offset_hz - offset_hz) <= rules.rx_threshold_hz &&
            at_ms - s.at < rules.period_ms() - 1000)
            return true;
    }
    history_.push_back({submode, raw, offset_hz, at_ms});
    return false;
}

} // namespace js8
} // namespace x6100
