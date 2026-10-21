/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 inbox and store-and-forward messages
 *
 *  Two small stores kept as text files on the DATA partition of the SD card,
 *  one message per line so they can be read on a PC: the inbox, with the
 *  "MSG" messages sent to us (desktop JS8Call answers those with ACK), and
 *  the messages held here for other stations ("MSG TO:"), handed out when
 *  they ask for them.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "directed_msg.hpp"

namespace x6100 {
namespace js8 {

struct InboxMessage { // one message in the inbox
    int id = 0;
    std::int64_t utc_ms = 0; ///< when it arrived
    std::string from;
    std::string text;
    std::string to;   ///< my call or the group it was sent to
    std::string path; ///< route back as desktop records it, "K2XYZ>N0XYZ" when relayed
    bool read = false;
};

class Inbox { // "MSG" messages sent to us
  public:
    static constexpr std::size_t MAX_MESSAGES = 200;            ///< when full the oldest are dropped
    static constexpr std::int64_t REPEAT_MS = 30 * 60 * 1000;   ///< the same text again this soon is a resend

    /// Replaces the contents with the file's. No file yet means an empty
    /// inbox and true; a file that exists but cannot be opened gives false.
    bool load(const std::string &file);
    /// Writes a temporary file and renames it over `file`, so losing power
    /// halfway leaves the previous file intact.
    bool save(const std::string &file) const;

    /// Stores a message and returns its id. When `from` sent the same text
    /// less than REPEAT_MS ago it is a resend (our ACK got lost) and the
    /// first copy's id comes back instead. An empty `route` means `from`
    /// was heard directly.
    /// `inserted`, when given, tells whether a message was stored (false for a resend).
    int add(const std::string &from, const std::string &text, std::int64_t when, const std::string &to = "",
            const std::string &route = "", bool *inserted = nullptr);

    std::vector<InboxMessage> list() const; ///< newest first
    std::optional<InboxMessage> get(int wanted) const;
    bool mark_read(int which); ///< false if it was read already or isn't there
    bool remove(int which);
    int unread() const;
    std::size_t size() const { return items_.size(); }

  private:
    std::vector<InboxMessage> items_; // in arrival order
    int fresh_id_ = 1; // handed to the next message added
};

/// A message waiting here for another station. Desktop JS8Call stores
/// "MSG TO:" traffic and hands it over when that station sends QUERY MSGS
/// or QUERY MSG n. A message to a @GROUP is never used up: every member may
/// fetch it during two days, and `got` remembers who already did.
struct HeldMessage { // a message waiting here for another station
    int id = 0;
    std::int64_t utc_ms = 0; ///< when it arrived
    std::string from;
    std::string to;   ///< the base call, as desktop keeps it, or the @GROUP
    std::string text;
    std::string path; ///< the route it came by: "N0XYZ", or "K2XYZ>N0XYZ"
    bool delivered = false;
    std::int64_t notified_ms = 0; ///< when we last sent RETRIEVE MSG about it
    std::vector<std::string> got; ///< for a group message, who has fetched it

    bool is_group() const { return to.size() > 0 && to.front() == '@'; }
};

class HeldMessages { // "MSG TO:" messages for others
  public:
    static constexpr std::size_t MAX_MESSAGES = 100;
    static constexpr std::int64_t GROUP_WINDOW_MS = 2LL * 24 * 3600 * 1000; ///< how long group messages stay offered
    static constexpr std::int64_t PUSH_SEEN_MS = 15 * 60 * 1000;            ///< station heard within this time
    static constexpr std::int64_t PUSH_REPEAT_MS = 8LL * 3600 * 1000;       ///< RETRIEVE MSG at most this often

    bool load(const std::string &file);       ///< as Inbox::load()
    bool save(const std::string &file) const; ///< as Inbox::save()

    /// Keeps `text` from `from` for the base call of `to`. The same message
    /// again within Inbox::REPEAT_MS gives back the id it already has.
    int add(const std::string &from, const std::string &to, const std::string &text, std::int64_t when,
            const std::string &route = "", bool *inserted = nullptr);

    /// First message not yet delivered to `station` or its base call, as
    /// getNextMessageIdForCallsign() on desktop.
    std::optional<int> next_for(const std::string &station) const;
    /// The one after `current`, announced as "NEXT MSG ID n".
    std::optional<int> lookahead_for(const std::string &call, int current) const;
    /// How many wait for `station` (desktop's countUnreadForCallsign()).
    int count_for(const std::string &station) const;

    /// The same three for messages to `group` younger than two days that
    /// `member` has not fetched yet.
    std::optional<int> next_group_for(const std::string &group, const std::string &member, std::int64_t now) const;
    std::optional<int> lookahead_group_for(const std::string &group, const std::string &member, int current,
                                           std::int64_t now) const;
    int count_group_for(const std::string &group, const std::string &member, std::int64_t now) const;

    std::optional<HeldMessage> get(int wanted) const;
    /// True when message `which` is addressed to `call` or to its base call.
    bool is_for(int which, const std::string &call) const;
    bool mark_delivered(int which);
    /// Notes that `member` fetched group message `which`.
    bool mark_group_delivered(int which, const std::string &member);
    bool remove(int which);
    std::vector<HeldMessage> list() const; ///< the newest at the front
    int waiting() const;                   ///< still undelivered
    std::size_t size() const { return items_.size(); }

    /// Desktop's pushNotificationHandler(): picks the first message whose
    /// station we heard in the last 15 minutes and have not told about it
    /// for 8 hours, records the time, and gives "W1ABC RETRIEVE MSG 3" to
    /// send. Group messages are left out.
    std::optional<std::pair<int, std::string>> push_due(const std::vector<Heard> &recent, std::int64_t now_ms);

  private:
    std::vector<HeldMessage> items_; // in arrival order
    int fresh_id_ = 1; // handed to the next message added
};

/// Body of a "MSG TO:" addressed to my_call: "FROM: MYCALL MSG TO:W1ABC HELLO"
/// (or with a space after "TO:") gives {"W1ABC", "HELLO"}.
std::optional<std::pair<std::string, std::string>> msg_to_body(const std::string &line, const std::string &mine);

/// The number asked for in "FROM: MYCALL QUERY MSG 3".
std::optional<int> query_msg_id(const std::string &line);

/// "HELLO THERE" from "FROM: MYCALL MSG HELLO THERE" when MYCALL is my_call or
/// its base call. Nothing for "MSG TO:", which is for someone else.
std::optional<std::string> msg_body(const std::string &line, const std::string &mine);

/// Message number another station says it holds for us: "YES MSG ID 3",
/// a heartbeat answer ending in "MSG ID 3", or "RETRIEVE MSG 3".
std::optional<int> msg_id_offered(const std::string &line);

/// What desktop's inbox reads from the end of a delivered message: the
/// original sender from "... FROM N0XYZ", possibly followed by
/// "NEXT MSG ID 4 +2", and that next id (0 when there is none).
struct Signature { // see delivered_signature()
    std::string from; ///< who first sent the message
    int next_id = 0;
};
std::optional<Signature> delivered_signature(const std::string &body);

} // namespace js8
} // namespace x6100
