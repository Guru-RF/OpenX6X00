/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 QSOs and the ADIF log file
 */

#include "qso_book.hpp"

#include "msg_kind.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace x6100 {
namespace js8 {

/* ---- reading the messages ---- */

/* The text after the first ':' (all of it if there is none). */
static std::string after_colon(const std::string &text) {
    const std::size_t at = text.find(':');
    return at == std::string::npos ? text : text.substr(at + 1);
}

/* The words after "FROM: TO": {"SNR", "-12"} for "K2XYZ: N0XYZ SNR -12". */
static std::vector<std::string> words_after_to(const std::string &text) {
    std::istringstream in(after_colon(text));
    std::vector<std::string> list;
    std::string w;
    bool first = true;
    while (in >> w) {
        if (!first) list.push_back(w);
        first = false;
    }
    return list;
}

/* A sign and digits that fit an int, e.g. "-12". A longer number is junk
 * from a bad decode, not a report: it counts as no report at all. */
static bool report_value(const std::string &r, int &db) {
    if (r.size() < 2 || (r[0] != '-' && r[0] != '+')) return false;
    if (!std::all_of(r.begin() + 1, r.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }))
        return false;
    errno = 0;
    const long long v = std::strtoll(r.c_str(), nullptr, 10);
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) return false;
    db = (int)v;
    return true;
}

/* The first "SNR <report>" pair. */
static std::optional<int> report_in(const std::vector<std::string> &w) {
    for (std::size_t i = 1; i < w.size(); i++) {
        int db = 0;
        if (w[i - 1] == "SNR" && report_value(w[i], db)) return db;
    }
    return {};
}

/* "HEARTBEAT SNR ...": the automatic answer to a heartbeat, not a QSO. */
static bool heartbeat_ack(const std::vector<std::string> &w) {
    return w.size() > 1 && w[0] == "HEARTBEAT" && w[1] == "SNR";
}

/* A word that closes a QSO, trailing punctuation aside: "73", "SK",
 * "RR73", "73!". */
static bool closes_qso(const std::vector<std::string> &w) {
    return std::any_of(w.begin(), w.end(), [](std::string word) {
        while (!word.empty() && std::ispunct((unsigned char)word.back())) word.pop_back();
        return word == "73" || word == "SK" || word == "RR73";
    });
}

/* At least 3 characters of capitals, digits and '/', with both a letter
 * and a digit and not a @group. */
static bool callsign_like(const std::string &tok) {
    if (tok.size() < 3 || tok[0] == '@') return false;
    bool has_letter = false, has_digit = false;
    for (const char c : tok) {
        const unsigned char u = (unsigned char)c;
        if (std::isupper(u)) {
            has_letter = true;
        } else if (std::isdigit(u)) {
            has_digit = true;
        } else if (c != '/') {
            return false;
        }
    }
    return has_letter && has_digit;
}

/* The call to put in the log prompt, once per QSO, when `closing` ends a
 * QSO in which both sides have sent something. */
static std::optional<std::string> offer(Qso &q, bool closing) {
    if (closing && q.we_sent && q.they_sent && !q.offered) {
        q.offered = true;
        return q.call;
    }
    return {};
}

/* ---- QsoTracker ---- */

/* The QSO with `call`, started afresh if there is none or the last one
 * went quiet too long ago, and marked as active now. */
Qso &QsoTracker::touch(const std::string &call, std::int64_t now) {
    const std::string key = base_callsign(call);
    auto found = live_.find(key);
    if (found != live_.end() && now - found->second.last_ms > EXPIRE_MS) {
        live_.erase(found);
        found = live_.end();
    }
    if (found == live_.end()) {
        Qso fresh;
        fresh.start_ms = now;
        found = live_.emplace(key, fresh).first;
    }
    Qso &q = found->second;
    q.call = call;
    q.last_ms = now;
    return q;
}

std::optional<std::string> QsoTracker::received(const std::string &from, const std::string &text, bool addressed,
                                                int db, const std::string &mine, std::int64_t now) {
    if (from.empty() || mine.empty()) return {};
    const std::string base = base_callsign(from);
    if (base == base_callsign(mine)) return {};

    if (!addressed) {
        /* Not for us, but it still tells how we hear a station we're
         * working. */
        const auto open = live_.find(base);
        if (open != live_.end()) open->second.heard_snr = db;
        return {};
    }

    const std::vector<std::string> w = words_after_to(text);
    Qso &qso = touch(from, now);
    qso.heard_snr = db;
    if (const auto r = report_in(w)) qso.rcvd_snr = r;
    qso.grid = better_grid(qso.grid, find_grid(after_colon(text)));
    if (heartbeat_ack(w)) return {};
    qso.they_sent = true;
    return offer(qso, closes_qso(w));
}

std::optional<std::string> QsoTracker::sent(const std::string &text, const std::string &mine, std::int64_t now) {
    const MessageClass mc = classify(text, mine);
    if (!callsign_like(mc.to)) return {};
    if (base_callsign(mc.to) == base_callsign(mine)) return {};

    const std::vector<std::string> w = words_after_to(text);
    Qso &qso = touch(mc.to, now);
    if (const auto r = report_in(w)) qso.sent_snr = r;
    if (heartbeat_ack(w)) return {};
    qso.we_sent = true;
    return offer(qso, closes_qso(w));
}

std::optional<Qso> QsoTracker::get(const std::string &call, std::int64_t now) const {
    const auto found = live_.find(base_callsign(call));
    if (found == live_.end()) return {};
    if (now - found->second.last_ms > EXPIRE_MS) return {};
    return found->second;
}

void QsoTracker::logged(const std::string &who) {
    live_.erase(base_callsign(who));
}

/* ---- ADIF ---- */

std::string format_snr(int db) {
    char text[8]; /* "%+03d" of any int, cut to 7 characters */
    std::snprintf(text, sizeof text, "%+03d", db);
    return text;
}

std::string adif_band(std::uint64_t hz) {
    struct Band {
        std::uint64_t low, high; /* edges in Hz, both inside the band */
        const char *name;
    };
    static const Band table[] = {
        {1'800'000, 2'000'000, "160m"},
        {3'500'000, 4'000'000, "80m"},
        {5'060'000, 5'450'000, "60m"},
        {7'000'000, 7'300'000, "40m"},
        {10'100'000, 10'150'000, "30m"},
        {14'000'000, 14'350'000, "20m"},
        {18'068'000, 18'168'000, "17m"},
        {21'000'000, 21'450'000, "15m"},
        {24'890'000, 24'990'000, "12m"},
        {28'000'000, 29'700'000, "10m"},
        {50'000'000, 54'000'000, "6m"},
        {144'000'000, 148'000'000, "2m"},
    };
    for (const Band &b : table) {
        if (b.low <= hz && hz <= b.high) return b.name;
    }
    return std::string();
}

/* Appends " <name:len>value" (no leading space for the first field). An
 * empty value writes nothing. Line breaks would break the one-record-per-
 * line layout, so they become spaces. */
static void add_field(std::string &rec, const char *name, const std::string &text) {
    if (text.empty()) return;
    std::string v(text);
    std::replace_if(v.begin(), v.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
    if (!rec.empty()) rec.push_back(' ');
    rec.append("<").append(name).append(":").append(std::to_string(v.size())).append(">").append(v);
}

/* UTC date or time of `ms` (ms since the epoch) in strftime `format`. */
static std::string utc_text(std::int64_t ms, const char *format) {
    const std::time_t secs = (std::time_t)(ms / 1000);
    std::tm parts;
    gmtime_r(&secs, &parts);
    char text[16];
    std::strftime(text, sizeof text, format, &parts);
    return text;
}

std::string adif_record(const LogEntry &entry) {
    char mhz[24];
    std::snprintf(mhz, sizeof mhz, "%.6f", entry.freq_hz / 1e6);

    std::string rec;
    add_field(rec, "call", entry.call);
    add_field(rec, "gridsquare", entry.grid);
    add_field(rec, "mode", "MFSK");
    add_field(rec, "submode", "JS8");
    add_field(rec, "rst_sent", entry.rst_sent);
    add_field(rec, "rst_rcvd", entry.rst_rcvd);
    add_field(rec, "qso_date", utc_text(entry.on_ms, "%Y%m%d"));
    add_field(rec, "time_on", utc_text(entry.on_ms, "%H%M%S"));
    add_field(rec, "qso_date_off", utc_text(entry.off_ms, "%Y%m%d"));
    add_field(rec, "time_off", utc_text(entry.off_ms, "%H%M%S"));
    add_field(rec, "band", adif_band(entry.freq_hz));
    add_field(rec, "freq", mhz);
    add_field(rec, "station_callsign", entry.my_call);
    add_field(rec, "my_gridsquare", entry.my_grid);
    add_field(rec, "comment", entry.comment);
    add_field(rec, "name", entry.name);
    add_field(rec, "operator", entry.op_call);
    if (entry.tx_pwr_w > 0) {
        char watts[16];
        std::snprintf(watts, sizeof watts, "%g", entry.tx_pwr_w);
        add_field(rec, "tx_pwr", watts);
    }
    if (!entry.pota_ref.empty()) {
        add_field(rec, "my_sig", "POTA");
        add_field(rec, "my_sig_info", entry.pota_ref);
    }
    add_field(rec, "my_sota_ref", entry.sota_ref);
    rec += " <eor>\n";
    return rec;
}

static const char ADIF_HEADER[] = "X6100 JS8 log, desktop JS8Call format\n"
                                  "<adif_ver:5>3.1.4 <programid:9>X6100_JS8 <eoh>\n";

bool adif_append(const std::string &path, const LogEntry &entry, std::string &err) {
    FILE *fp = std::fopen(path.c_str(), "a");
    if (fp == nullptr) {
        err.assign("can't open ").append(path);
        return false;
    }
    /* In append mode the position is only known after seeking to the end. */
    bool good = std::fseek(fp, 0, SEEK_END) == 0;
    if (good && std::ftell(fp) == 0) good = std::fputs(ADIF_HEADER, fp) >= 0;
    if (good) good = std::fputs(adif_record(entry).c_str(), fp) >= 0;
    if (good) good = std::fflush(fp) == 0;
    /* Straight to the SD card: the radio may lose power at any moment. */
    fsync(fileno(fp));
    const bool closed = std::fclose(fp) == 0;
    good = closed && good;
    if (!good) err = "can't write " + path;
    return good;
}

} // namespace js8
} // namespace x6100
