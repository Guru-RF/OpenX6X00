/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive, reading rendered messages
 */

#pragma once

#include <string>

namespace x6100 {
namespace js8 {

/// The parts of a rendered message the window filters and colours by.
struct MessageClass { // see classify()
    std::string from;               ///< the sender, when the text begins "CALL:"
    std::string to;                 ///< the word after the sender, if a callsign or an @GROUP
    bool        heartbeat  = false; ///< "@HB HEARTBEAT ..." or "... HEARTBEAT ..."
    bool        snr_report = false; ///< "CALL SNR -12", the reply to a heartbeat or SNR?
    bool        cq         = false; ///< "CQ ..." or "@ALLCALL CQ ..."
    bool        to_me      = false; ///< sent to my_call, compared by base callsign
    bool        to_group   = false; ///< sent to an @GROUP, @ALLCALL included
};

/// Read text such as "K1ABC: W2XYZ SNR -12" or "K1ABC: @HB HEARTBEAT FN42".
/// `my_call` can be "" (then to_me stays false).
MessageClass classify(const std::string &message, const std::string &my_call);

enum class Checksum {
    None,    ///< not a checksummed command, or nothing to check
    Valid,   ///< checked and removed
    Invalid, ///< missing or wrong
};

/// Desktop JS8Call only acts on a buffered command (MSG, MSG TO:, QUERY,
/// relays with '>', ...) whose payload ends with the right checksum. Check it
/// here the same way. When it is Valid the checksum word is taken off the end
/// of `message`; in the other cases `message` is not changed.
Checksum verify_command_checksum(std::string &message);

/// True for a 4, 6, 8 or 10 character Maidenhead locator, upper case
/// ("JN35", "JN35SC12AB"). "RR73" is a sign-off, never a grid.
bool is_grid(const std::string &text);

/// The locator in a message body, or "": the word after "GRID" when it is
/// one, otherwise the last word that is. At most 6 characters are returned;
/// the logs and station list keep no more than the subsquare.
std::string find_grid(const std::string &text);

/// The grid to keep for a station: `got`, the one just heard, unless it is
/// empty or only a shorter form of `have` ("JN35" after "JN35SC").
std::string better_grid(const std::string &have, const std::string &got);

/// The home callsign inside a compound one: the longest '/'-separated part
/// that has a digit, upper-cased ("EA8/G4ABC/P" gives "G4ABC").
std::string base_callsign(const std::string &callsign);

/// Our firmware's own group. Joined by default from Settings, Groups; the
/// radio never answers it by itself.
inline constexpr const char *X6100_GROUP = "@X6100";

/// The message goes to @X6100, directly or relayed on from it ("@X6100>...").
bool is_x6100_group(const std::string &to);

} // namespace js8
} // namespace x6100
