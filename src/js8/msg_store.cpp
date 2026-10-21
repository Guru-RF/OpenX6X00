/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 inbox and store-and-forward messages
 */

#include "msg_store.hpp"

#include "msg_kind.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>

namespace x6100 {
namespace js8 {

// Both files hold one message per line with tab separated fields, the
// message text last (JS8 text has no tabs):
//   inbox: id, UTC ms, U or R, from, to, path, text
//   held:  id, UTC ms, H or D, from, for, path, RETRIEVE MSG time, fetched by, text
// Files from before the to/path columns (5 fields in the inbox, 6 in the
// held file) are still read.
static const char INBOX_FIRST_LINE[] = "# X6100 JS8 inbox v2: id, UTC ms, U(nread)/R(ead), from, to, path, message\n";
static const char HELD_FIRST_LINE[]  = "# X6100 JS8 held messages v2: id, UTC ms, H(eld)/D(elivered), from, for, path, "
                                "RETRIEVE MSG sent (ms), fetched by (group), message\n";

// Tabs and line breaks would break the file format; they become spaces.
static std::string flatten(std::string s) {
    std::replace_if(s.begin(), s.end(), [](char ch) { return ch == '\t' || ch == '\n' || ch == '\r'; }, ' ');
    return s;
}

// A value as it goes into the file: flattened and, like a C string, ending
// at a NUL byte.
static std::string field(const std::string &s) {
    return flatten(std::string(s.c_str()));
}

static std::vector<std::string> split_words(const std::string &s) {
    std::vector<std::string> found;
    std::istringstream       stream(s);
    std::string              w;
    while (stream >> w) found.push_back(w);
    return found;
}

static std::vector<std::string> split_tabs(const std::string &line) {
    std::vector<std::string> cells;
    std::size_t              from = 0;
    for (;;) {
        std::size_t tab = line.find('\t', from);
        cells.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
        if (tab == std::string::npos) return cells;
        from = tab + 1;
    }
}

// A number the way std::stoll reads it (leading blanks, a sign, digits,
// then anything), within [lo, hi]. False where std::stoi or std::stoll
// would throw, so a damaged line is skipped and the rest still loads.
static bool read_number(const std::string &s, long long lo, long long hi, long long &value) {
    const char *start = s.c_str();
    char       *stop  = nullptr;
    errno             = 0;
    const long long v = std::strtoll(start, &stop, 10);
    if (stop == start || errno == ERANGE || v < lo || v > hi) return false;
    value = v;
    return true;
}

// Reads every data line of a file (comments and blank lines skipped, a
// Windows line end tolerated) as its fields. False when the file exists
// but can't be read; a missing file is simply empty.
static bool read_records(const std::string &path, std::vector<std::vector<std::string>> &records) {
    std::ifstream in(path);
    if (!in) return access(path.c_str(), F_OK) != 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') continue; // blank or a comment
        if (line.back() == '\r') line.pop_back();
        records.push_back(split_tabs(line));
    }
    return true;
}

// Replaces `path` with `content` through a temporary file, synced before
// the rename so a power cut leaves either the old file or the new one.
static bool replace_file(const std::string &path, const std::string &content) {
    const std::string tmp = path + ".tmp";
    FILE             *out = std::fopen(tmp.c_str(), "w");
    if (out == nullptr) return false;
    bool good = std::fwrite(content.data(), 1, content.size(), out) == content.size();
    good      = good && std::fflush(out) == 0;
    good      = good && fsync(fileno(out)) == 0;   // an SD card error here is a failed save, not a success
    good = (std::fclose(out) == 0) && good;
    if (good && std::rename(tmp.c_str(), path.c_str()) == 0) {
        // The rename itself made durable: the directory
        std::string dir = path.substr(0, path.rfind('/'));
        int         dfd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
        return true;
    }
    std::remove(tmp.c_str());
    return false;
}

static bool all_digits(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

// A message number as the protocol writes it: 1 to 6 digits.
static bool message_number(const std::string &s) {
    return !s.empty() && s.size() <= 6 && all_digits(s);
}

template <typename T> static std::optional<T> find_id(const std::vector<T> &items, int id) {
    for (const T &item : items)
        if (item.id == id) return item;
    return {};
}

template <typename T> static bool erase_id(std::vector<T> &items, int id) {
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->id != id) continue;
        items.erase(it);
        return true;
    }
    return false;
}

// ---- inbox ----

bool Inbox::load(const std::string &file) {
    items_.clear();
    fresh_id_ = 1;
    std::vector<std::vector<std::string>> records;
    if (!read_records(file, records)) return false;
    for (const auto &f : records) {
        const bool with_route = f.size() == 7;
        long long  id = 0, when = 0;
        if (!with_route && f.size() != 5) continue;
        if (!read_number(f[0], INT_MIN, INT_MAX, id) || !read_number(f[1], LLONG_MIN, LLONG_MAX, when)) continue;
        InboxMessage msg;
        msg.id     = static_cast<int>(id);
        msg.utc_ms = when;
        msg.read = f[2] == "R";
        msg.from = f[3];
        msg.to   = with_route ? f[4] : std::string();
        msg.path = with_route && !f[5].empty() ? f[5] : msg.from;
        msg.text = f.back();
        fresh_id_ = std::max(fresh_id_, msg.id + 1);
        items_.push_back(std::move(msg));
    }
    return true;
}

bool Inbox::save(const std::string &file) const {
    std::string content = INBOX_FIRST_LINE;
    for (const InboxMessage &msg : items_) {
        content += std::to_string(msg.id) + '\t' + std::to_string(msg.utc_ms) + '\t' + (msg.read ? 'R' : 'U');
        for (const std::string *s : {&msg.from, &msg.to, &msg.path, &msg.text}) content += '\t' + field(*s);
        content += '\n';
    }
    return replace_file(file, content);
}

int Inbox::add(const std::string &from, const std::string &text, std::int64_t when, const std::string &to,
               const std::string &route, bool *inserted) {
    if (inserted) *inserted = false;
    for (const InboxMessage &old : items_) {
        bool resend = old.from == from && old.text == text && when - old.utc_ms < REPEAT_MS;
        if (resend) return old.id;
    }
    if (inserted) *inserted = true;

    InboxMessage msg;
    msg.id     = fresh_id_++;
    msg.utc_ms = when;
    msg.from   = flatten(from);
    msg.text   = flatten(text);
    msg.to     = flatten(to);
    msg.path   = flatten(route.empty() ? from : route);
    items_.push_back(msg);

    // Over the limit a message already read goes first, else the oldest.
    while (items_.size() > MAX_MESSAGES) {
        auto victim = std::find_if(items_.begin(), items_.end(), [](const InboxMessage &m) { return m.read; });
        if (victim == items_.end()) victim = items_.begin();
        items_.erase(victim);
    }
    return msg.id;
}

std::vector<InboxMessage> Inbox::list() const { // newest first
    std::vector<InboxMessage> newest_first(items_.rbegin(), items_.rend());
    return newest_first;
}

std::optional<InboxMessage> Inbox::get(int wanted) const {
    return find_id(items_, wanted);
}

bool Inbox::mark_read(int which) {
    auto it = std::find_if(items_.begin(), items_.end(), [which](const InboxMessage &m) { return m.id == which; });
    if (it == items_.end() || it->read) return false;
    it->read = true;
    return true;
}

bool Inbox::remove(int which) {
    return erase_id(items_, which);
}

int Inbox::unread() const { // still marked U
    int n = 0;
    for (const InboxMessage &msg : items_) n += msg.read ? 0 : 1;
    return n;
}

// ---- messages held for others ----

// Desktop matches a station's messages by the call as heard or its base.
static bool addressed_to(const HeldMessage &msg, const std::string &call) {
    return msg.to == call || msg.to == base_callsign(call);
}

static bool undelivered(const HeldMessage &msg) {
    return !msg.delivered && !msg.text.empty();
}

// A group message still on offer to `call`: young enough and not yet
// fetched by that station.
static bool offered_to(const HeldMessage &msg, const std::string &group, const std::string &call, std::int64_t now_ms) {
    if (msg.to != group || msg.text.empty()) return false;
    if (now_ms - msg.utc_ms >= HeldMessages::GROUP_WINDOW_MS) return false;
    return std::find(msg.got.begin(), msg.got.end(), call) == msg.got.end();
}

bool HeldMessages::load(const std::string &file) {
    items_.clear();
    fresh_id_ = 1;
    std::vector<std::vector<std::string>> records;
    if (!read_records(file, records)) return false;
    for (const auto &f : records) {
        const bool full = f.size() == 9;
        long long  id = 0, when = 0, told = 0;
        if (!full && f.size() != 6) continue;
        if (!read_number(f[0], INT_MIN, INT_MAX, id) || !read_number(f[1], LLONG_MIN, LLONG_MAX, when)) continue;
        if (full && !f[6].empty() && !read_number(f[6], LLONG_MIN, LLONG_MAX, told)) continue;
        HeldMessage msg;
        msg.id          = static_cast<int>(id);
        msg.utc_ms      = when;
        msg.notified_ms = told;
        msg.delivered = f[2] == "D";
        msg.from      = f[3];
        msg.to        = f[4];
        if (full) {
            msg.path = f[5];
            std::size_t start = 0;
            const std::string &who = f[7];
            while (start <= who.size()) {
                std::size_t comma = std::min(who.find(',', start), who.size());
                if (comma > start) msg.got.push_back(who.substr(start, comma - start));
                start = comma + 1;
            }
        }
        if (msg.path.empty()) msg.path = msg.from;
        msg.text  = f.back();
        fresh_id_ = std::max(fresh_id_, msg.id + 1);
        items_.push_back(std::move(msg));
    }
    return true;
}

bool HeldMessages::save(const std::string &file) const {
    std::string content = HELD_FIRST_LINE;
    for (const HeldMessage &msg : items_) {
        std::string fetched;
        for (const std::string &call : msg.got) fetched += (fetched.empty() ? "" : ",") + call;
        content += std::to_string(msg.id) + '\t' + std::to_string(msg.utc_ms) + '\t' + (msg.delivered ? 'D' : 'H');
        for (const std::string *s : {&msg.from, &msg.to, &msg.path}) content += '\t' + field(*s);
        content += '\t' + std::to_string(msg.notified_ms) + '\t' + std::string(fetched.c_str()) + '\t' + field(msg.text);
        content += '\n';
    }
    return replace_file(file, content);
}

int HeldMessages::add(const std::string &from, const std::string &to, const std::string &text, std::int64_t when,
                      const std::string &route, bool *inserted) {
    if (inserted) *inserted = false;
    const std::string target = base_callsign(to);
    for (const HeldMessage &old : items_) {
        bool resend = old.from == from && old.to == target && old.text == text && when - old.utc_ms < Inbox::REPEAT_MS;
        if (resend) return old.id;
    }
    if (inserted) *inserted = true;

    HeldMessage msg;
    msg.id     = fresh_id_++;
    msg.utc_ms = when;
    msg.from   = flatten(from);
    msg.to     = flatten(target);
    msg.path   = flatten(route.empty() ? from : route);
    msg.text   = flatten(text);
    items_.push_back(msg);

    // Over the limit, drop one that's done with (delivered, or a group
    // message past its two days), else the oldest.
    auto finished = [when](const HeldMessage &m) {
        return m.delivered || (m.is_group() && when - m.utc_ms > GROUP_WINDOW_MS);
    };
    while (items_.size() > MAX_MESSAGES) {
        auto victim = std::find_if(items_.begin(), items_.end(), finished);
        items_.erase(victim == items_.end() ? items_.begin() : victim);
    }
    return msg.id;
}

bool HeldMessages::is_for(int which, const std::string &call) const {
    std::optional<HeldMessage> msg = get(which);
    return msg.has_value() && addressed_to(*msg, call);
}

std::optional<int> HeldMessages::next_for(const std::string &station) const {
    for (const HeldMessage &msg : items_)
        if (undelivered(msg) && addressed_to(msg, station)) return msg.id;
    return {};
}

std::optional<int> HeldMessages::lookahead_for(const std::string &call, int current) const {
    // Like desktop: first a message for the call exactly as heard, and only
    // then one for its base call.
    for (const std::string &who : {call, base_callsign(call)}) {
        for (const HeldMessage &msg : items_)
            if (msg.id != current && undelivered(msg) && msg.to == who) return msg.id;
    }
    return {};
}

int HeldMessages::count_for(const std::string &station) const {
    // Messages are filed under base calls but desktop counts only the call
    // as heard, which finds nothing for "W1ABC/P"; both are counted here.
    int n = 0;
    for (const HeldMessage &msg : items_)
        if (undelivered(msg) && addressed_to(msg, station)) n++;
    return n;
}

std::optional<int> HeldMessages::next_group_for(const std::string &group, const std::string &member,
                                                std::int64_t now) const {
    for (const HeldMessage &msg : items_)
        if (offered_to(msg, group, member, now)) return msg.id;
    return {};
}

std::optional<int> HeldMessages::lookahead_group_for(const std::string &group, const std::string &member,
                                                     int current, std::int64_t now) const {
    for (const std::string &who : {member, base_callsign(member)}) {
        for (const HeldMessage &msg : items_)
            if (msg.id != current && offered_to(msg, group, who, now)) return msg.id;
    }
    return {};
}

int HeldMessages::count_group_for(const std::string &group, const std::string &member, std::int64_t now) const {
    int n = 0;
    for (const HeldMessage &msg : items_)
        if (offered_to(msg, group, member, now)) n++;
    return n;
}

bool HeldMessages::mark_group_delivered(int which, const std::string &member) {
    auto it = std::find_if(items_.begin(), items_.end(),
                           [which](const HeldMessage &m) { return m.id == which && m.is_group(); });
    if (it == items_.end()) return false;
    if (std::find(it->got.begin(), it->got.end(), member) != it->got.end()) return false;
    it->got.push_back(flatten(member));
    return true;
}

std::optional<std::pair<int, std::string>> HeldMessages::push_due(const std::vector<Heard> &recent,
                                                                  std::int64_t now_ms) {
    for (HeldMessage &msg : items_) {
        if (!undelivered(msg) || msg.to.empty() || msg.is_group()) continue;
        const bool around = std::any_of(recent.begin(), recent.end(), [&](const Heard &h) {
            bool same_station = h.call == msg.to || base_callsign(h.call) == msg.to;
            return same_station && now_ms - h.heard_ms <= PUSH_SEEN_MS;
        });
        if (!around) continue;
        if (msg.notified_ms != 0 && now_ms - msg.notified_ms < PUSH_REPEAT_MS) continue;
        msg.notified_ms = now_ms;
        return std::make_pair(msg.id, msg.to + " RETRIEVE MSG " + std::to_string(msg.id));
    }
    return {};
}

std::optional<HeldMessage> HeldMessages::get(int wanted) const {
    return find_id(items_, wanted);
}

bool HeldMessages::mark_delivered(int which) {
    for (HeldMessage &msg : items_) {
        if (msg.id != which || msg.delivered) continue;
        msg.delivered = true;
        return true;
    }
    return false;
}

bool HeldMessages::remove(int which) {
    return erase_id(items_, which);
}

std::vector<HeldMessage> HeldMessages::list() const { // newest first
    std::vector<HeldMessage> newest_first(items_.rbegin(), items_.rend());
    return newest_first;
}

int HeldMessages::waiting() const { // held, not delivered
    int n = 0;
    for (const HeldMessage &msg : items_) n += msg.delivered ? 0 : 1;
    return n;
}

// ---- reading message text ----

std::optional<std::pair<std::string, std::string>> msg_to_body(const std::string &line, const std::string &mine) {
    const std::size_t colon = line.find(':');
    if (mine.empty() || colon == std::string::npos) return {};
    const std::vector<std::string> w = split_words(line.substr(colon + 1));
    // MYCALL MSG TO:DEST BODY... or MYCALL MSG TO: DEST BODY...
    if (w.size() < 4 || w[1] != "MSG" || w[2].compare(0, 3, "TO:") != 0) return {};
    if (base_callsign(w[0]) != base_callsign(mine)) return {};
    std::size_t at        = 3;
    std::string recipient = w[2].substr(3);
    if (recipient.empty()) recipient = w[at++];
    if (recipient.empty() || recipient[0] == '@' || at >= w.size()) return {};
    std::string body = w[at];
    for (std::size_t i = at + 1; i < w.size(); i++) body += " " + w[i];
    return std::pair<std::string, std::string>(recipient, body);
}

std::optional<int> query_msg_id(const std::string &line) {
    const std::vector<std::string> w = split_words(line);
    for (std::size_t i = 2; i < w.size(); i++) {
        if (w[i - 2] == "QUERY" && w[i - 1] == "MSG") {
            // Only the first QUERY MSG counts, even with a bad number.
            if (!message_number(w[i])) return {};
            return std::stoi(w[i]);
        }
    }
    return {};
}

std::optional<std::string> msg_body(const std::string &line, const std::string &mine) {
    const std::size_t colon = line.find(':');
    if (mine.empty() || colon == std::string::npos) return {};
    const std::string              after = line.substr(colon + 1);
    const std::vector<std::string> w     = split_words(after);
    if (w.size() < 3 || w[1] != "MSG" || w[2].compare(0, 3, "TO:") == 0) return {};
    if (base_callsign(w[0]) != base_callsign(mine)) return {};

    // The body keeps its inner spacing: everything after " MSG ".
    const std::size_t start = after.find(" MSG ");
    if (start == std::string::npos) return {};
    const std::string body  = after.substr(start + 5);
    const std::size_t first = body.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return body.substr(first, body.find_last_not_of(' ') - first + 1);
}

std::optional<int> msg_id_offered(const std::string &line) {
    const std::vector<std::string> w = split_words(line);
    for (std::size_t i = 2; i < w.size(); i++) {
        const bool msg_id   = w[i - 2] == "MSG" && w[i - 1] == "ID";
        const bool retrieve = w[i - 2] == "RETRIEVE" && w[i - 1] == "MSG";
        if ((msg_id || retrieve) && message_number(w[i])) return std::stoi(w[i]);
    }
    return {};
}

std::optional<Signature> delivered_signature(const std::string &body) {
    // The pattern of desktop's message panel, applied to the text with
    // trailing spaces removed.
    static const std::regex signature("(?:^| )FROM (\\S+)(?: NEXT MSG ID (\\d+)(?: \\+\\d+)?)?$");
    const std::size_t       last    = body.find_last_not_of(' ');
    const std::string       trimmed = last == std::string::npos ? std::string() : body.substr(0, last + 1);
    std::smatch             hit;
    if (!std::regex_search(trimmed, hit, signature)) return {};
    Signature sig;
    sig.from = hit[1].str();
    // A next id too big for an int is as good as no signature: this text
    // comes over the air and must never stop the program.
    long long next = 0;
    if (hit[2].matched && !read_number(hit[2].str(), INT_MIN, INT_MAX, next)) return {};
    sig.next_id = static_cast<int>(next);
    return sig;
}

} // namespace js8
} // namespace x6100
