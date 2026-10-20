/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 automatic answers and heartbeat scheduling
 *
 *  Decides what to answer and what to keep for each decoded message, by the
 *  rules of desktop JS8Call (processCommandActivity() and
 *  scheduleHeartbeat()). Nothing goes out on its own unless the user turned
 *  on AUTO, HB or HB ACK; all three start off.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "msg_store.hpp"

namespace x6100 {
namespace js8 {

struct Incoming { // what the answering logic needs to know about a decoded message
    std::string from;
    std::string to;
    std::string text;
    bool to_me = false;
    bool to_group = false;
    bool heartbeat = false;      ///< a "@HB HEARTBEAT ..." or an answer to one
    bool low_confidence = false;
    bool checksum_ok = false;    ///< buffered command (MSG and the like) whose checksum verified
    int snr = 0;
    std::int64_t when_ms = 0;
};

struct AutoSettings { // the user's switches and station details
    bool autoreply = false; ///< the AUTO switch
    bool heartbeat = false; ///< the HB switch: send heartbeats now and then
    bool hb_ack = false;    ///< the HB ACK switch; like desktop it also needs AUTO and HB
    bool relay = true;      ///< desktop's "Disable message relay (>)" left unticked: relay and hold MSG TO:
    std::string my_call;
    std::string my_grid;
    std::string info;
    std::string status;
    std::vector<std::string> groups;    ///< our @GROUPs (desktop's My Groups), @ALLCALL aside
    const HeldMessages *held = nullptr; ///< what we hold for others, for QUERY MSGS, QUERY MSG n and HB answers
};

/// The kinds of answer:
///  - Query: the answer to a question; sent with AUTO, otherwise offered.
///  - HeartbeatAck: needs AUTO, HB and HB ACK all on.
///  - MsgAck: "CALL ACK" for a message stored here (inbox or held) or for a
///    relay that stops here; repeated for a resend, which means our ACK
///    was missed.
///  - Relay: a relay forwarded to the next station.
///  - Stored: about the held messages (YES MSG ID n, NO, the message itself,
///    RETRIEVE MSG) or a QUERY CALL answer.
///  - Suggest: never sent automatically, only put ready behind Reply, such
///    as "CALL QUERY MSG 3" when a station says it has a message for us.
enum class ReplyKind {
    Query,
    HeartbeatAck,
    MsgAck,
    Suggest,
    Relay,
    Stored,
};

struct AutoReply { // one answer, before the switches decide its fate
    std::string text;    ///< what to send, e.g. "N0XYZ SNR -12"
    std::string to;      ///< the station whose message this answers, even through a relay
    std::string command; ///< what it answers, e.g. "SNR?", or "HEARTBEAT"
    ReplyKind kind = ReplyKind::Query;
    int deliver_id = 0;             ///< held message carried by this answer: delivered once it's sent
    std::string deliver_group_call; ///< for a group message, the station it goes to
    bool auto_only = false;         ///< desktop sends it with AUTO on or not at all (QUERY MSGS, QUERY CALL)
    bool allcall = false;           ///< answers @ALLCALL: AUTO only, once per station every 15 minutes
};

/// What to keep of a message, as desktop does: a "MSG" to us or to our
/// group goes in the inbox together with its relay route; a "MSG TO:" is
/// held for the station it names.
struct StoreAction { // where a message goes, if anywhere
    enum class Kind { None, Inbox, Held };
    Kind kind = Kind::None;
    std::string from;
    std::string to;
    std::string path;
    std::string text;
};

struct Processed { // the outcome of process()
    std::optional<AutoReply> reply; ///< nothing to answer when empty
    StoreAction store;
};

/// processCommandActivity() of desktop JS8Call for a single decoded message,
/// before the switches are looked at (that is AutoPolicy's job): what to
/// store and what to answer.
///  - Questions to our call: SNR? (or just ?), GRID?, INFO?, STATUS?,
///    HEARING?, AGN?.
///  - Another station's heartbeat: an answer with its SNR, plus "MSG ID n
///    [+k]" when messages wait here for it.
///  - MSG to us or one of our groups: inbox and ACK. MSG TO:: held and ACK;
///    "MSG TO:@GROUP" is for any member of our groups.
///  - QUERY MSGS: "YES MSG ID n [+k]" or "NO". QUERY MSG n: the message,
///    with "NEXT MSG ID" when another waits. QUERY CALL.
///  - Relays: forwarded while the text opens with a callsign; otherwise
///    the relay ends here and gets an ACK back along its route, or the
///    answer to the command inside it.
///  - Suggestions: QUERY MSG n when a station offers a message ("MSG ID n",
///    "RETRIEVE MSG n"), our SNR report for HW CPY?.
/// Nothing for our own transmissions, low-confidence decodes, traffic
/// between other stations, or questions we have no answer for (no INFO
/// text set, and so on). `heard` lists recently heard stations, the most
/// recent first.
/// A message to @X6100 (is_x6100_group) gets no answer and nothing is held
/// to pass on, whether we are in the group or not; a MSG to it still goes in
/// the inbox.
Processed process(const Incoming &msg, const AutoSettings &settings, const std::vector<Heard> &recent,
                  const std::string &last_sent);

/// Just the answer from process().
std::optional<AutoReply> build_reply(const Incoming &msg, const AutoSettings &settings,
                                     const std::vector<Heard> &recent, const std::string &last_sent);

/// Whether `in` opens a QSO with us: anything to our call except an answer
/// to our own heartbeat.
bool starts_qso(const Incoming &msg);

/// The switches and rate limits that sit between an answer and the air.
class AutoPolicy { // what may go out, and when
  public:
    static constexpr std::int64_t QUERY_REPEAT_MS = 5 * 60 * 1000;   ///< same station, same question
    static constexpr std::int64_t HB_ACK_REPEAT_MS = 15 * 60 * 1000; ///< desktop's @ALLCALL cache, HB answers included
    static constexpr std::int64_t IDLE_MS = 60 * 60 * 1000;          ///< desktop's default idle watchdog

    enum class Action {
        Ignore,
        Send,
        Offer,
    };

    /// Send when the switches and the rate limits allow it. With AUTO off a
    /// question's answer is offered instead (desktop puts it in the outgoing
    /// text box), except for the answers desktop gives only with AUTO on.
    /// Only plain questions wait QUERY_REPEAT_MS per station, a limit of
    /// ours; ACKs, relays and held messages always go, as on desktop.
    Action decide(const AutoReply &reply, const AutoSettings &settings, std::int64_t now);

    /// Records that `reply` was queued, for the rate limits.
    void sent(const AutoReply &reply, std::int64_t now);

    /// Any key, button or knob press. After IDLE_MS without one nothing
    /// more goes out automatically.
    void user_activity(std::int64_t now);
    bool idle(std::int64_t now) const;

  private:
    std::map<std::string, std::int64_t> recent_;  // last time each answer went out, see the .cpp for the keys
    std::int64_t user_ms_ = 0;
};

/// When the next heartbeat goes out, worked out like desktop's
/// scheduleHeartbeat(): the next 15 s slot, plus 1 s, plus `minutes`,
/// and one in four times one slot later so stations drift apart.
std::int64_t next_heartbeat_ms(std::int64_t now, int minutes, std::mt19937 &rng);

constexpr int HB_MIN_INTERVAL = 5;      ///< minutes
constexpr int HB_MAX_INTERVAL = 30;     ///< minutes
constexpr int HB_DEFAULT_INTERVAL = 30; ///< minutes

} // namespace js8
} // namespace x6100
