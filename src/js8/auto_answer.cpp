/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 automatic answers and heartbeat scheduling
 */

#include "auto_answer.hpp"

#include "msg_kind.hpp"
#include "command_words.hpp"
#include "directed_msg.hpp"

#include "js8core/protocol/varicode.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace x6100 {
namespace js8 {

static std::string upper_case(const std::string &in) {
    std::string up(in);
    std::transform(up.begin(), up.end(), up.begin(),
                   [](char ch) { return static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); });
    return up;
}

static std::string strip_spaces(const std::string &in) {
    const std::size_t first = in.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    const std::size_t last = in.find_last_not_of(' ');
    return in.substr(first, last - first + 1);
}

static std::vector<std::string> word_list(const std::string &in) {
    std::vector<std::string> list;
    std::istringstream       stream(in);
    for (std::string word; stream >> word;) list.push_back(word);
    return list;
}

static std::string joined(const std::vector<std::string> &parts, const std::string &sep, std::size_t from = 0) {
    std::string all;
    for (std::size_t i = from; i < parts.size(); i++) {
        if (i > from) all += sep;
        all += parts[i];
    }
    return all;
}

// How long ago a station was heard, in the words of desktop's since():
// days, hours, minutes, seconds in steps of 15, or "now".
static std::string time_since(std::int64_t then_ms, std::int64_t now_ms) {
    const std::int64_t secs = (now_ms - then_ms) / 1000;
    const struct {
        std::int64_t unit;
        const char  *suffix;
    } scale[] = {{86400, "d"}, {3600, "h"}, {60, "m"}};
    for (const auto &step : scale)
        if (secs >= step.unit) return std::to_string(secs / step.unit) + step.suffix;
    if (secs < 15) return "now";
    return std::to_string(secs / 15 * 15) + "s";
}

// "MSG ID 3", followed by " +2" when two more wait behind it.
static std::string message_id(int id, int others) {
    std::string out = "MSG ID " + std::to_string(id);
    if (others > 0) out += " +" + std::to_string(others);
    return out;
}

static bool short_number(const std::string &s) {
    if (s.empty() || s.size() > 6) return false;
    return std::all_of(s.begin(), s.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

static bool begins(const std::string &s, const char *prefix) {
    return s.rfind(prefix, 0) == 0;
}

static Processed answer(AutoReply r) {
    Processed out;
    out.reply = std::move(r);
    return out;
}

// Applies processCommandActivity() to one directed message. Kept as an
// object so the many cases share the message, the settings and what
// desktop works out up front (to everyone, to one of our groups).
class Responder {
  public:
    Responder(const Incoming &in, const AutoSettings &s, const std::vector<Heard> &heard, const std::string &last_tx)
        : in_(in), set_(s), heard_(heard), last_tx_(last_tx), everyone_(is_allcall(in.to)),
          // Desktop's isGroupCallIncluded(); @APRSIS counts as a group too.
          group_(in.to == "@APRSIS" || std::find(s.groups.begin(), s.groups.end(), in.to) != s.groups.end()) {}

    bool for_us() const { return everyone_ || in_.to_me || group_; }
    bool group() const { return group_; }

    // `route` is the way back ("K2XYZ>N0XYZ") when the command arrived
    // inside a relay that stops here, else empty.
    Processed command(const Directed &d, const std::string &route) const;
    Processed heartbeat() const;

  private:
    // Every answer is addressed to the station that sent the message.
    AutoReply reply(const std::string &text, const std::string &command, ReplyKind kind) const {
        AutoReply r{text, in_.from, command, kind};
        r.allcall = everyone_;
        return r;
    }

    Processed question(const Directed &d, const std::string &answer_to, const std::string &asker) const;
    Processed relay(const Directed &d) const;
    Processed hold(const Directed &d) const;
    Processed keep(const Directed &d) const;
    Processed fetch(const Directed &d, const std::string &asker, const std::string &back) const;
    Processed list_waiting(const Directed &d, const std::string &asker, const std::string &back) const;
    Processed seen(const Directed &d, const std::string &back) const;
    Processed suggestion(const Directed &d) const;
    Processed from_aprs(const std::string &dest, const std::string &text) const;
    std::string waiting_note(const std::string &call, const std::string &group_call) const;

    const Incoming           &in_;
    const AutoSettings       &set_;
    const std::vector<Heard> &heard_;
    const std::string        &last_tx_;
    const bool                everyone_;
    const bool                group_;
};

Processed Responder::command(const Directed &d, const std::string &route) const {
    const std::string &what = d.cmd;

    // Inside a relay, desktop sends the answer to a question back along the
    // route. MSG and the QUERY commands find the asking station (the last
    // one on the route) by themselves.
    std::string answer_to = d.from;
    if (!route.empty() && js8core::protocol::varicode::is_command_autoreply(what) && !begins(what, " MSG") &&
        !begins(what, " QUERY"))
        answer_to = route;
    std::string asker = d.from;
    std::string back  = d.from;
    const std::size_t last_hop = route.rfind('>');
    if (last_hop != std::string::npos) {
        asker = route.substr(last_hop + 1);
        back  = route;
    }

    // Most commands get no answer when sent to everyone, and then go on to
    // the suggestions at the end like any unknown command.
    if (!everyone_) {
        if (what == " SNR?" || what == " INFO?" || what == " STATUS?" || what == " GRID?" || what == " HEARING?")
            return question(d, answer_to, asker);
        if (what == ">") return relay(d);
        if (what == " MSG TO:") return hold(d);
        if (what == " AGN?" && !group_) {
            const std::string again = strip_spaces(last_tx_);
            if (again.empty()) return {};
            return answer(reply(again, "AGN?", ReplyKind::Query));
        }
        if (what == " MSG") return keep(d);
        if (what == " QUERY") return fetch(d, asker, back);
    }
    if (what == " QUERY MSGS") return list_waiting(d, asker, back);
    if (what == " QUERY CALL") return seen(d, back);

    // What's left is never sent on its own and only for a message straight
    // to us; desktop doesn't answer any of it.
    if (!in_.to_me || !route.empty()) return {};
    return suggestion(d);
}

Processed Responder::question(const Directed &d, const std::string &answer_to, const std::string &asker) const {
    const std::string &what = d.cmd;
    std::string        text;
    if (what == " SNR?") {
        text = query_text(Query::SendSnr, answer_to, in_.snr, "");
    } else if (what == " GRID?") {
        text = query_text(Query::MyGrid, answer_to, 0, set_.my_grid);
    } else if (what == " INFO?" || what == " STATUS?") {
        // " INFO?" answers as "CALL INFO TEXT", likewise STATUS.
        const std::string value = strip_spaces(what == " INFO?" ? set_.info : set_.status);
        if (!value.empty()) text = answer_to + what.substr(0, what.size() - 1) + " " + upper_case(value);
    } else {
        // HEARING?: up to four stations heard lately, leaving out the one
        // asking and ourselves.
        std::string names;
        int         count = 0;
        for (const Heard &h : heard_) {
            if (count == 4) break;
            const std::string base = base_callsign(h.call);
            if (base == base_callsign(asker) || base == base_callsign(set_.my_call)) continue;
            names += " " + h.call;
            count++;
        }
        if (count > 0) text = answer_to + " HEARING" + names;
    }
    if (text.empty()) return {};
    return answer(reply(text, what.substr(1), ReplyKind::Query));
}

Processed Responder::relay(const Directed &d) const {
    if (!set_.relay) return {};
    // Forwarded while the text opens with the next station's callsign (but
    // never from a group message); otherwise the relay stops here.
    std::optional<std::string> onward = relay_next_hop(d.text);
    if (onward && !group_) return answer(reply(*onward + " *DE* " + d.from, ">", ReplyKind::Relay));
    if (begins(d.text, "ACK")) return {};

    const std::string route = joined(relay_path_calls(d.from, d.text), ">");
    // A command inside gets its own answer instead of the ACK.
    if (std::optional<std::pair<std::string, std::string>> inner = relayed_command(d.text)) {
        Directed carried = d;
        carried.cmd      = inner->first;
        carried.text     = inner->second;
        return command(carried, route);
    }
    return answer(reply(route + " ACK", ">", ReplyKind::MsgAck));
}

// Desktop's handling of what an APRS gateway sends to @APRSIS
// ("MSG TO:CALL text DE SENDER"): for our call it goes into the inbox as
// coming from "APRS", without an ACK.
Processed Responder::from_aprs(const std::string &dest, const std::string &text) const {
    Processed out;
    if (dest == upper_case(strip_spaces(set_.my_call))) {
        out.store.kind = StoreAction::Kind::Inbox;
        out.store.from = "APRS";
        out.store.to   = set_.my_call;
        out.store.path = "APRS";
        out.store.text = strip_spaces(text);
    }
    return out;
}

// MSG TO:CALL text: kept here for CALL (desktop's STORE) and ACKed.
Processed Responder::hold(const Directed &d) const {
    const std::vector<std::string> w = word_list(d.text);
    if (w.empty()) return {};
    const std::string body = strip_spaces(joined(w, " ", 1));
    if (in_.to == "@APRSIS") return from_aprs(w[0], body);
    // The relay switch on desktop turns off holding as well.
    if (!set_.relay) return {};
    const std::string route = joined(relay_path_calls(d.from, body), ">");
    Processed         out   = answer(reply(route + " ACK", "MSG TO:", ReplyKind::MsgAck));
    out.store.kind          = StoreAction::Kind::Held;
    out.store.from          = d.from;
    out.store.to            = base_callsign(w[0]);
    out.store.path          = route;
    out.store.text          = body;
    return out;
}

// MSG: into the inbox, also when sent to our group, and ACKed back along
// the route it came by.
Processed Responder::keep(const Directed &d) const {
    if (in_.to == "@APRSIS") {
        static const std::regex aprs_form("^TO:\\s*(\\S+)\\s+(.*)$");
        std::smatch             parts;
        if (std::regex_match(d.text, parts, aprs_form)) return from_aprs(parts.str(1), parts.str(2));
    }
    const std::string route = joined(relay_path_calls(d.from, d.text), ">");
    Processed         out   = answer(reply(route + " ACK", "MSG", ReplyKind::MsgAck));
    out.store.kind          = StoreAction::Kind::Inbox;
    out.store.from          = d.from;
    out.store.to            = in_.to;
    out.store.path          = route;
    out.store.text          = strip_spaces(d.text);
    return out;
}

// QUERY MSG n: the held message itself, with "NEXT MSG ID n [+k]" when
// there are more.
Processed Responder::fetch(const Directed &d, const std::string &asker, const std::string &back) const {
    const HeldMessages            *store = set_.held;
    const std::vector<std::string> w     = word_list(d.text);
    if (w.size() < 2 || w[0] != "MSG" || !short_number(w[1]) || store == nullptr) return {};
    const int                  id  = std::stoi(w[1]);
    std::optional<HeldMessage> msg = store->get(id);
    if (!msg || strip_spaces(msg->text).empty()) return {};
    // Anyone who asks may have a group message; the others go only to the
    // station they are for.
    const bool for_group = msg->is_group();
    if (!for_group && msg->to != asker && msg->to != base_callsign(asker)) return {};

    const std::int64_t now = in_.when_ms;
    // Waiting besides this one and the one named as next.
    int more = store->count_for(asker) + (group_ ? store->count_group_for(in_.to, asker, now) : 0) - 2;
    std::optional<int> next = store->lookahead_for(asker, id);
    if (!next && for_group) next = store->lookahead_group_for(in_.to, asker, id, now);

    std::string text = back + " MSG " + strip_spaces(msg->text) + " FROM " + msg->from;
    if (next) text += " NEXT " + message_id(*next, more);
    AutoReply r  = reply(text, "QUERY MSG " + std::to_string(id), ReplyKind::Stored);
    r.deliver_id = id;
    if (for_group) r.deliver_group_call = asker;
    return answer(r);
}

// "MSG ID n [+k]" for the first message held for `call`, or for its group
// when the message went to one of ours; "" when there is none.
std::string Responder::waiting_note(const std::string &call, const std::string &group_call) const {
    const HeldMessages *store = set_.held;
    const std::int64_t  now   = in_.when_ms;
    if (store == nullptr) return std::string();
    std::optional<int> first = store->next_for(call);
    int more = store->count_for(call) + (group_ ? store->count_group_for(in_.to, group_call, now) : 0) - 1;
    if (!first && group_) first = store->next_group_for(in_.to, group_call, now);
    return first ? message_id(*first, more) : std::string();
}

// QUERY MSGS: "YES MSG ID n [+k]", else NO (but not to everyone).
Processed Responder::list_waiting(const Directed &d, const std::string &asker, const std::string &back) const {
    const std::string note = waiting_note(asker, d.from);
    std::string       text;
    if (!note.empty()) text = back + " YES " + note;
    else if (!everyone_) text = back + " NO";
    else return {};
    AutoReply r = reply(text, "QUERY MSGS", ReplyKind::Stored);
    r.auto_only = true;
    return answer(r);
}

// QUERY CALL W1ABC?: "YES -12 (5m)" when we heard it, nothing otherwise.
Processed Responder::seen(const Directed &d, const std::string &back) const {
    const std::vector<std::string> calls = parse_callsigns(d.text);
    if (calls.empty()) return {};
    const std::string &target = calls[0];
    auto               match  = std::find_if(heard_.begin(), heard_.end(), [&](const Heard &h) {
        return h.call == target || base_callsign(h.call) == target;
    });
    if (match == heard_.end()) return {};
    const std::string snr = desktop_snr(match->snr);
    if (snr.empty()) return {};
    AutoReply r = reply(back + " YES " + snr + " (" + time_since(match->heard_ms, in_.when_ms) + ")", "QUERY CALL",
                        ReplyKind::Stored);
    r.auto_only = true;
    return answer(r);
}

// Help that only fills the Reply button.
Processed Responder::suggestion(const Directed &d) const {
    if (d.cmd == " HW CPY?") {
        // "How do you copy?": desktop leaves the answer to the operator; we
        // offer our SNR report.
        const std::string report = query_text(Query::SendSnr, d.from, in_.snr, "");
        if (report.empty()) return {};
        return answer(reply(report, "HW CPY?", ReplyKind::Suggest));
    }
    if (d.cmd == " ACK") return {};
    // A station holding a message for us ("YES MSG ID 3", "MSG ID 3" at the
    // end of a heartbeat answer, "RETRIEVE MSG 3"): offer to fetch it, as
    // desktop's popup does.
    const std::optional<int> offered = msg_id_offered(d.cmd + " " + d.text);
    if (!offered) return {};
    return answer(reply(upper_case(d.from) + " QUERY MSG " + std::to_string(*offered), "MSG ID", ReplyKind::Suggest));
}

// Another station's heartbeat: answered with how we hear it, plus the
// messages held here for it ("MSG ID 3 +1").
Processed Responder::heartbeat() const {
    std::string text = query_text(Query::SendSnr, in_.from, in_.snr, "");
    if (text.empty()) return {}; // no SNR to report
    // Desktop's form is "CALL HEARTBEAT SNR -08".
    text.replace(text.find(" SNR "), 5, " HEARTBEAT SNR ");
    const std::string note = waiting_note(in_.from, in_.from);
    if (!note.empty()) text += " " + note;
    AutoReply r = reply(text, "HEARTBEAT", ReplyKind::HeartbeatAck);
    r.allcall   = true;
    return answer(r);
}

bool starts_qso(const Incoming &msg) {
    // An answer to our own heartbeat is no call to us.
    return msg.to_me && msg.text.find(" HEARTBEAT SNR") == std::string::npos;
}

static Processed respond(const Incoming &msg, const AutoSettings &settings, const std::vector<Heard> &recent,
                         const std::string &last_sent) {
    if (settings.my_call.empty() || msg.from.empty() || msg.low_confidence) return {};
    // Our own transmissions, also under another prefix or suffix.
    if (base_callsign(msg.from) == base_callsign(settings.my_call)) return {};

    const Responder who(msg, settings, recent, last_sent);
    const bool      hb_from_other = msg.heartbeat && !msg.to_me && msg.text.find(" HEARTBEAT SNR") == std::string::npos &&
                               msg.text.find("@HB HEARTBEAT") != std::string::npos;
    if (hb_from_other) return who.heartbeat();
    if (!who.for_us()) return {}; // traffic between other stations

    std::optional<Directed> d = parse_directed(msg.text);
    if (!d.has_value()) return {};
    // Buffered commands with text are acted on only with a good checksum.
    if (!d->text.empty() && !msg.checksum_ok && is_checksummed_command(d->cmd)) return {};
    return who.command(*d, "");
}

Processed process(const Incoming &msg, const AutoSettings &settings, const std::vector<Heard> &recent,
                  const std::string &last_sent) {
    Processed out = respond(msg, settings, recent, last_sent);
    if (!is_x6100_group(msg.to)) return out;
    // The group's traffic is for people to read: nothing leaves the radio on
    // its own because of it.
    out.reply.reset();
    if (out.store.kind == StoreAction::Kind::Held) out.store = StoreAction{};
    return out;
}

std::optional<AutoReply> build_reply(const Incoming &msg, const AutoSettings &settings,
                                     const std::vector<Heard> &recent, const std::string &last_sent) {
    Processed p = process(msg, settings, recent, last_sent);
    return p.reply;
}

// Rate limit keys: "CALL|COMMAND" for answers to a station, and
// "@ALLCALL|CALL" for answers to anything sent to everyone (heartbeats too).
static std::string limit_key(const AutoReply &r) {
    if (r.allcall || r.kind == ReplyKind::HeartbeatAck) return "@ALLCALL|" + r.to;
    return r.to + "|" + r.command;
}

AutoPolicy::Action AutoPolicy::decide(const AutoReply &reply, const AutoSettings &settings, std::int64_t now) {
    if (reply.kind == ReplyKind::Suggest) return Action::Offer;

    const bool hb_answer = reply.kind == ReplyKind::HeartbeatAck;
    const bool automatic = settings.autoreply && !idle(now);
    if (hb_answer && !(automatic && settings.heartbeat && settings.hb_ack)) return Action::Ignore;
    // With AUTO off desktop puts the answer in its outgoing box, except for
    // @ALLCALL, QUERY MSGS and QUERY CALL, which it only answers with AUTO.
    if (!hb_answer && !automatic) return (reply.allcall || reply.auto_only) ? Action::Ignore : Action::Offer;

    std::int64_t window = 0;
    if (reply.allcall || hb_answer) window = HB_ACK_REPEAT_MS; // desktop's @ALLCALL cache
    else if (reply.kind == ReplyKind::Query) window = QUERY_REPEAT_MS;
    // ACKs, relays and held messages have no limit: a resend or a second
    // QUERY means the first didn't arrive.
    if (window == 0) return Action::Send;

    const auto last  = recent_.find(limit_key(reply));
    const bool again = last != recent_.end() && now - last->second < window;
    return again ? Action::Ignore : Action::Send;
}

void AutoPolicy::sent(const AutoReply &reply, std::int64_t now) {
    recent_[limit_key(reply)] = now;
}

void AutoPolicy::user_activity(std::int64_t now) {
    user_ms_ = now;
}

bool AutoPolicy::idle(std::int64_t now) const {
    return now - user_ms_ >= IDLE_MS;
}

std::int64_t next_heartbeat_ms(std::int64_t now, int minutes, std::mt19937 &rng) {
    const std::int64_t period = std::clamp(minutes, HB_MIN_INTERVAL, HB_MAX_INTERVAL) * 60;
    // Start of the next 15 s slot (or this one when exactly on it).
    std::int64_t due = (now / 1000 + 14) / 15 * 15 + 1 + period;
    std::uniform_real_distribution<float> chance(0, 1);
    if (chance(rng) < 0.25f) due += 15;
    return due * 1000;
}

} // namespace js8
} // namespace x6100
