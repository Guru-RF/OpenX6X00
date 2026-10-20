/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 directed command parsing and relay routing
 *
 *  Splits a decoded "FROM: TO COMMAND text" into the parts desktop JS8Call
 *  works with when it decides what to answer, and finds where a relayed
 *  message goes next and the way back, using the same callsign patterns as
 *  desktop so a relay is forwarded, ended or answered by the same rules.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace x6100 {
namespace js8 {

struct Heard { // one station from the recently heard list
    std::string call;
    int snr = 0;               ///< last SNR, dB
    std::int64_t heard_ms = 0; ///< last heard, ms

    Heard() = default; // no station
    Heard(const char *call_text) : call(call_text) {}
    Heard(std::string call_text, int snr_db = 0, std::int64_t when_ms = 0)
        : call(std::move(call_text)), snr(snr_db), heard_ms(when_ms) {}
};

/// `cmd` keeps desktop's spelling, leading space included (" SNR?", " MSG",
/// " MSG TO:", " QUERY MSGS"); a relay is ">" and plain text is " ". `text`
/// is whatever follows, without leading spaces.
struct Directed { // a decoded directed message
    std::string from;
    std::string to;
    std::string cmd;
    std::string text;
};

/// Parse "FROM: TO CMD text", e.g. "N0XYZ: K2XYZ MSG TO: W1ABC HI" gives
/// from N0XYZ, to K2XYZ, cmd " MSG TO:", text "W1ABC HI", and the relay
/// "N0XYZ: K2XYZ > W1ABC HI" gives cmd ">". Empty unless there is a sender
/// and a destination.
std::optional<Directed> parse_directed(const std::string &line);

/// Where a relay goes next: when the text after '>' opens with a callsign and
/// then ' ' or '>', that station and the rest ("W1ABC HELLO" -> "W1ABC>HELLO",
/// sent with " *DE* FROM" added). Empty when the relay stops here.
std::optional<std::string> relay_next_hop(const std::string &relayed);

/// The stations a relay passed through, the sender first and then every
/// "*DE* CALL" or "VIA CALL" in the text from the last to the first; joined
/// with '>' this is the route back, e.g. "K2XYZ>N0XYZ".
std::vector<std::string> relay_path_calls(const std::string &sender, const std::string &relayed);

/// For a relay that stops here: the command it opens with, if desktop would
/// answer that command (SNR?, MSG, MSG TO:, QUERY MSGS, QUERY MSG n, ...),
/// and the words after it. "MSG TO:W1ABC HI *DE* N0XYZ" gives
/// " MSG TO:" and "W1ABC HI *DE* N0XYZ".
std::optional<std::pair<std::string, std::string>> relayed_command(const std::string &relayed);

/// Every word of `words` that is a valid callsign and not a grid locator.
std::vector<std::string> parse_callsigns(const std::string &words);

/// True for @ALLCALL and for @HB, where heartbeats are addressed.
bool is_allcall(const std::string &dest);

/// True for the buffered commands that carry a 16-bit checksum (MSG, MSG TO:,
/// QUERY, QUERY MSGS, QUERY CALL, CMD and relays): with text after them they
/// are trusted only once that checksum has been verified.
bool is_checksummed_command(const std::string &command);

/// A stored route shown the way desktop's inbox shows it:
/// "K2XYZ>N0XYZ" becomes "N0XYZ via K2XYZ".
std::string path_display(const std::string &route);

} // namespace js8
} // namespace x6100
