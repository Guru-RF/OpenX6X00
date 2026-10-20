/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 QSOs and the ADIF log file
 *
 *  On desktop JS8Call the operator logs each contact by hand (Log QSO, F5)
 *  and it goes into js8call_log.adi. The dialog writes the same file, fills
 *  the form from the messages exchanged and asks whether to log once a
 *  contact that went both ways is closed with 73 or SK.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace x6100 {
namespace js8 {

struct Qso { ///< a contact with one station, as far as the messages tell
    std::string call;
    std::int64_t start_ms = 0;    ///< the first message between us, either direction
    std::int64_t last_ms = 0;     ///< the latest one
    std::optional<int> sent_snr;  ///< the report we sent ("CALL SNR -12")
    std::optional<int> rcvd_snr;  ///< the report they sent us
    std::optional<int> heard_snr; ///< our SNR on their latest decode
    std::string grid;             ///< their grid, if they sent one to us
    bool we_sent = false;         ///< we have messaged them (heartbeat acks don't count)
    bool they_sent = false;       ///< they have messaged us (heartbeat acks don't count)
    bool offered = false;         ///< the "log it?" question has been asked
};

/* Watches the messages between us and other stations, one Qso per base call. */
class QsoTracker final {
public: /* used by the dialog's operations and the tests */
    static constexpr std::int64_t EXPIRE_MS = 1'800'000; ///< half an hour of silence ends a QSO

    /* A decode "FROM: TO ...". Gives the call back when this message closes
     * (73 or SK) a QSO that went both ways and hasn't been offered for the
     * log yet. */
    std::optional<std::string> received(const std::string &from, const std::string &text, bool addressed, int db,
                                        const std::string &mine, std::int64_t now);

    /* One of our messages, "MYCALL: TO ...", as it goes on the air. Same
     * return as received(). */
    std::optional<std::string> sent(const std::string &text, const std::string &mine, std::int64_t now);

    /* The live QSO with `call`, matched on the base call ("EA8/G4ABC" is
     * G4ABC). */
    std::optional<Qso> get(const std::string &call, std::int64_t now) const;

    /* The QSO with `call` is in the log; whatever comes next is a new one. */
    void logged(const std::string &who);

    void clear() { live_.clear(); }

private: /* keyed by base call */
    Qso &touch(const std::string &call, std::int64_t now);

    std::map<std::string, Qso> live_;
};

/* The fields desktop JS8Call writes, plus TX power and the POTA / SOTA
 * references of an activation. */
struct LogEntry { ///< one log record
    std::string call, grid, name, comment;
    std::string rst_sent, rst_rcvd;       ///< e.g. "-12"; empty if unknown
    std::int64_t on_ms = 0, off_ms = 0;   ///< QSO start and end, ms since the epoch
    std::uint64_t freq_hz = 0;            ///< dial frequency plus the audio offset
    std::string my_call, my_grid, op_call;
    float tx_pwr_w = 0;                   ///< left out of the record when 0
    std::string pota_ref;                 ///< goes in MY_SIG_INFO, with MY_SIG POTA
    std::string sota_ref;                 ///< goes in MY_SOTA_REF
};

/* The ADIF band of a frequency in Hz ("20m"), "" outside the ham bands. */
std::string adif_band(std::uint64_t hz);

/* One ADIF record, closed by "<eor>" and a newline, in desktop JS8Call's
 * field order (MODE MFSK, SUBMODE JS8). */
std::string adif_record(const LogEntry &entry);

/* Adds a record to the ADIF file at `path`, putting the header in first
 * when the file is new or empty. On failure `err` says why. */
bool adif_append(const std::string &path, const LogEntry &entry, std::string &err);

/* A report as JS8 sends it: sign and two digits, "+05" / "-12". */
std::string format_snr(int db);

} // namespace js8
} // namespace x6100
