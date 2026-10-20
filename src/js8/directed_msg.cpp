/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 directed command parsing and relay routing
 */

#include "directed_msg.hpp"

#include "js8core/protocol/varicode.hpp"

#include <cctype>
#include <regex>

namespace x6100 {
namespace js8 {

namespace varicode = js8core::protocol::varicode;

static std::string to_upper(const std::string &in) {
    std::string out(in);
    for (char &ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

// Only plain spaces count: that is all the decoder ever produces.
static std::string skip_spaces(const std::string &in) {
    std::size_t first = in.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    return in.substr(first);
}

static bool starts_with(const std::string &s, const std::string &prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

// Characters that can make up a callsign or a @GROUP name.
static bool call_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '/' || ch == '@';
}

// The commands of desktop's directed pattern, tried in its order. Those
// ending in '?' or ':' match as a prefix; the plain words need a space or
// the end of the text after them, so "MSGS" is not "MSG". QUERY CALL and
// QUERY MSGS come before QUERY for the same reason.
struct CommandWord {
    const char *word;
    bool        whole_word;
};
static const CommandWord COMMAND_WORDS[] = {
    {"AGN?", false},       {"QSL?", false},     {"HW CPY?", false},   {"MSG TO:", false},
    {"SNR?", false},       {"INFO?", false},    {"GRID?", false},     {"STATUS?", false},
    {"QUERY MSGS?", false}, {"HEARING?", false}, {"STATUS", true},     {"HEARING", true},
    {"QUERY CALL", true},  {"QUERY MSGS", true}, {"QUERY", true},     {"CMD", true},
    {"MSG", true},         {"NACK", true},      {"ACK", true},        {"73", true},
    {"YES", true},         {"NO", true},        {"HEARTBEAT SNR", true}, {"SNR", true},
    {"QSL", true},         {"RR", true},        {"SK", true},         {"FB", true},
    {"INFO", true},        {"GRID", true},      {"DIT DIT", true},
};

// Callsign as desktop's relay patterns write it: up to four characters and
// a slash in front, a base with exactly one digit, up to four after a slash.
static const std::string CALLSIGN_PATTERN = "(?:[A-Z0-9]{1,4}/)?"
                                     "[0-9A-Z]?[0-9A-Z][0-9][A-Z]?[A-Z]?[A-Z]?"
                                     "(?:/[A-Z0-9]{1,4})?";

// Grid locator as desktop leaves it out of callsign lists: AA00 or AA00AA.
static bool looks_like_grid(const std::string &w) {
    if (w.size() != 4 && w.size() != 6) return false;
    for (std::size_t i = 0; i < w.size(); i++) {
        char ch   = w[i];
        bool good = false;
        if (i < 2) good = ch >= 'A' && ch <= 'R';
        else if (i < 4) good = ch >= '0' && ch <= '9';
        else good = ch >= 'A' && ch <= 'X';
        if (!good) return false;
    }
    return true;
}

std::optional<Directed> parse_directed(const std::string &line) {
    const std::size_t sep = line.find(": ");
    if (sep == 0 || sep == std::string::npos) return {};

    Directed out;
    out.from = to_upper(line.substr(0, sep));

    const std::string body = skip_spaces(to_upper(line.substr(sep + 2)));
    std::size_t       len  = 0;
    while (len < body.size() && call_char(body[len])) ++len;
    if (len == 0) return {};
    out.to = body.substr(0, len);

    const std::string after = body.substr(len);
    // Desktop writes a relay as "TO>", our own display as "TO > W1ABC".
    if (starts_with(after, ">") || starts_with(after, " >")) {
        out.cmd  = ">";
        out.text = skip_spaces(after.substr(after.find('>') + 1));
        return out;
    }
    // "TO?" is how SNR? used to be written.
    if (starts_with(after, "?")) {
        out.cmd  = " SNR?";
        out.text = skip_spaces(after.substr(1));
        return out;
    }

    const std::string rest = skip_spaces(after);
    for (const CommandWord &c : COMMAND_WORDS) {
        const std::string word(c.word);
        if (!starts_with(rest, word)) continue;
        if (c.whole_word && rest.size() != word.size() && rest[word.size()] != ' ') continue;
        // Desktop treats "QUERY MSGS?" as QUERY MSGS.
        out.cmd  = word == "QUERY MSGS?" ? std::string(" QUERY MSGS") : " " + word;
        out.text = skip_spaces(rest.substr(word.size()));
        return out;
    }
    out.cmd  = " ";
    out.text = rest;
    return out;
}

std::optional<std::string> relay_next_hop(const std::string &relayed) {
    // Desktop's callToPattern: the callsign, then '>' or ' ', then a word.
    static const std::regex next_station("^(" + CALLSIGN_PATTERN + ")([> ])\\b");
    std::smatch             found;
    if (!std::regex_search(relayed, found, next_station)) return {};
    const std::size_t after_sep = static_cast<std::size_t>(found.position(2)) + 1;
    return found.str(1) + ">" + relayed.substr(after_sep);
}

std::vector<std::string> relay_path_calls(const std::string &sender, const std::string &relayed) {
    // Desktop's callDePattern; every hop it names goes in front of the ones
    // found before it, and the sender goes in front of them all.
    static const std::regex hop("\\s(\\*DE\\*|VIA)\\s(" + CALLSIGN_PATTERN + ")\\b");
    std::vector<std::string> found;
    for (std::sregex_iterator it(relayed.begin(), relayed.end(), hop), end; it != end; ++it)
        found.push_back((*it)[2].str());
    std::vector<std::string> route{sender};
    route.insert(route.end(), found.rbegin(), found.rend());
    return route;
}

std::optional<std::pair<std::string, std::string>> relayed_command(const std::string &relayed) {
    // Single spaces separate the words, so two spaces give an empty word,
    // as when desktop splits the text.
    std::vector<std::string> parts;
    for (std::size_t pos = 0;;) {
        const std::size_t space = relayed.find(' ', pos);
        if (space == std::string::npos) {
            parts.push_back(relayed.substr(pos));
            break;
        }
        parts.push_back(relayed.substr(pos, space - pos));
        pos = space + 1;
    }

    // The first word is the command: as written ("?" and ">") or, as for
    // all the others, with desktop's leading space. Only then is it taken
    // out of the words.
    std::size_t next    = 0;
    std::string command = parts[0];
    if (!varicode::is_command_allowed(command)) {
        command = " " + command;
        if (varicode::is_command_allowed(command)) next = 1;
    }

    // MSG TO:, QUERY MSGS and QUERY CALL are two words.
    if (next < parts.size()) {
        std::string &second = parts[next];
        if (command == " MSG" && starts_with(second, "TO:")) {
            command = " MSG TO:";
            if (second.size() == 3) ++next;
            else second.erase(0, 3);
        } else if (command == " QUERY" && (second == "MSGS" || second == "MSGS?")) {
            command = " QUERY MSGS";
            ++next;
        } else if (command == " QUERY" && second == "CALL") {
            command = " QUERY CALL";
            ++next;
        }
    }

    if (!varicode::is_command_allowed(command) || !varicode::is_command_autoreply(command)) return {};

    std::string tail;
    for (std::size_t i = next; i < parts.size(); i++) {
        if (i > next) tail += ' ';
        tail += parts[i];
    }
    return std::make_pair(command == "?" ? std::string(" SNR?") : command, tail);
}

std::vector<std::string> parse_callsigns(const std::string &words) {
    std::vector<std::string> calls;
    const std::string        up = to_upper(words);
    std::size_t              i  = 0;
    while (i < up.size()) {
        if (!call_char(up[i])) {
            ++i;
            continue;
        }
        std::size_t stop = i;
        while (stop < up.size() && call_char(up[stop])) ++stop;
        std::string word = up.substr(i, stop - i);
        if (varicode::is_valid_callsign(word, nullptr) && !looks_like_grid(word)) calls.push_back(word);
        i = stop;
    }
    return calls;
}

bool is_allcall(const std::string &dest) {
    for (const char *everyone : {"@ALLCALL", "@HB"})
        if (dest.find(everyone) != std::string::npos) return true;
    return false;
}

bool is_checksummed_command(const std::string &command) {
    const int checksum_bits = varicode::is_command_checksummed(command);
    return checksum_bits == 16;
}

std::string path_display(const std::string &route) {
    // Hops in reverse order. An empty hop prints as nothing, and no " via "
    // is put in front of anything while the text so far is still empty.
    std::vector<std::string> hops;
    std::size_t              pos = 0;
    while (pos < route.size()) {
        std::size_t gt = route.find('>', pos);
        if (gt == std::string::npos) gt = route.size();
        hops.push_back(route.substr(pos, gt - pos));
        pos = gt + 1;
    }
    std::string shown;
    for (std::size_t i = hops.size(); i-- > 0;) {
        if (!shown.empty()) shown += " via ";
        shown += hops[i];
    }
    return shown;
}

} // namespace js8
} // namespace x6100
