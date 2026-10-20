/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 alert words
 *
 *  Desktop JS8Call can play a sound on several events (CQ, heartbeat, ACK,
 *  a directed message, an inbox message, a new or worked call) and lets the
 *  user list highlight words: calls or plain words that are looked for as
 *  whole words in every decode, which is split at ':', '>' and blanks. On
 *  the radio either case beeps, and an alert word also highlights the row.
 */

#pragma once

#include <string>
#include <vector>

namespace x6100 {
namespace js8 {

/* The words list as the user typed it, e.g. "ve7abc, @pota  sota", into
 * {"VE7ABC", "@POTA", "SOTA"}: upper case, split at commas, spaces and
 * tabs, each word once, no more than 20 words. */
std::vector<std::string> parse_alert_words(const std::string &input);

/* The list joined with single spaces, "VE7ABC @POTA SOTA", for saving and
 * for the settings field. */
std::string format_alert_words(const std::vector<std::string> &list);

/* The first word of `list` found in the decode `text`, or "" if none is.
 * Words must match whole, as desktop does, except that a word that is a
 * callsign also matches any form of the sender's call: "VE7ABC" catches
 * "VE7ABC/P" and "VA7/VE7ABC". */
std::string alert_word_hit(const std::string &text, const std::string &sender,
                           const std::vector<std::string> &list);

} // namespace js8
} // namespace x6100
