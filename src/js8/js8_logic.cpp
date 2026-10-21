/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  Xiegu X6100 LVGL GUI - JS8 operating helpers: the C side of the C++
 *  modules underneath (commands, stations, auto-reply, QSO log, inbox,
 *  held messages, alerts, macros)
 */

#include "js8_logic.h"

#include "alert_rules.hpp"
#include "auto_answer.hpp"
#include "msg_kind.hpp"
#include "command_words.hpp"
#include "directed_msg.hpp"
#include "msg_store.hpp"
#include "macros.hpp"
#include "qso_book.hpp"
#include "heard_list.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace js8 = x6100::js8;

/* The opaque handles of the C interface. A store remembers its file so that
 * every change can be written straight back. */
struct js8_stations { // heard stations
    js8::StationList stations;
};

struct js8_inbox { // messages for us
    js8::Inbox  store;
    std::string file;
};

struct js8_held { // messages kept for other stations
    js8::HeldMessages store;
    std::string       file;
};

struct js8_auto { // reply rate limits
    js8::AutoPolicy limits;
};

struct js8_qsos { // QSOs in progress
    js8::QsoTracker qsos;
};

/* The C enums list their values in the same order as the C++ ones. */
static_assert((int)js8::ReplyKind::Query == JS8_REPLY_QUERY && (int)js8::ReplyKind::HeartbeatAck == JS8_REPLY_HB_ACK &&
              (int)js8::ReplyKind::MsgAck == JS8_REPLY_ACK && (int)js8::ReplyKind::Suggest == JS8_REPLY_SUGGEST &&
              (int)js8::ReplyKind::Relay == JS8_REPLY_RELAY && (int)js8::ReplyKind::Stored == JS8_REPLY_STORED);
static_assert((int)js8::AutoPolicy::Action::Ignore == JS8_AUTO_IGNORE &&
              (int)js8::AutoPolicy::Action::Send == JS8_AUTO_SEND &&
              (int)js8::AutoPolicy::Action::Offer == JS8_AUTO_OFFER);
static_assert((int)js8::Query::SeventyThree + 1 == JS8_Q_COUNT);

namespace { // helpers for this file only

/* Strings from C may be NULL; the C++ side wants "" then. */
const char *or_empty(const char *s) {
    return s ? s : "";
}

/* Copies src into a C buffer of `size` bytes, cutting it short if needed;
 * a NULL or zero-sized buffer is left alone. */
void put(const std::string &src, char *dst, std::size_t size) {
    if (dst == nullptr || size == 0) return;
    std::size_t len = src.size() < size ? src.size() : size - 1;
    src.copy(dst, len);
    dst[len] = '\0';
}

template <std::size_t N> void put(const std::string &src, char (&dst)[N]) {
    put(src, dst, N);
}

/* An optional result goes out as a flag and a value (0 when absent). */
template <class T> void put_opt(const std::optional<int> &v, bool &has, T &value) {
    has   = v.has_value();
    value = (T)(has ? *v : 0);
}

js8::Incoming incoming(const js8_rx_msg_t &m) {
    js8::Incoming dec;
    dec.from           = m.from;
    dec.to             = m.to;
    dec.text           = m.text;
    dec.to_me          = m.to_me;
    dec.to_group       = m.to_group;
    dec.heartbeat      = m.heartbeat;
    dec.low_confidence = m.low_confidence;
    dec.checksum_ok    = m.checksum == 1;
    dec.snr            = m.snr;
    return dec;
}

std::vector<js8::Heard> heard_vector(const js8_heard_t *heard, unsigned n) {
    std::vector<js8::Heard> v;
    if (heard == nullptr) return v;
    for (const js8_heard_t *h = heard; h != heard + n; ++h)
        if (h->call) v.emplace_back(h->call, h->snr, h->heard_ms);
    return v;
}

/* Splits a groups field at spaces and commas, in capitals. */
std::vector<std::string> split_groups(const char *typed) {
    std::vector<std::string> words;
    std::string              word;
    for (const char *p = or_empty(typed);; ++p) {
        if (*p == '\0' || *p == ' ' || *p == ',') {
            if (!word.empty()) words.push_back(word);
            word.clear();
            if (*p == '\0') break;
        } else {
            word += (char)std::toupper((unsigned char)*p);
        }
    }
    return words;
}

/* Copies up to max entries of a list into C structs; the count copied. */
template <class Src, class Dst, class Fill> int copy_list(const std::vector<Src> &all, Dst *out, int max, Fill fill) {
    int count = max > 0 ? std::min(max, (int)all.size()) : 0;
    for (int i = 0; i < count; i++) fill(all[i], &out[i]);
    return count;
}

void to_c(const js8::InboxMessage &m, js8_inbox_msg_t *c) {
    std::memset(c, 0, sizeof *c);
    c->id     = m.id;
    c->utc_ms = m.utc_ms;
    put(m.from, c->from);
    put(m.to, c->to);
    put(m.path, c->path);
    put(m.text, c->text);
    c->read = m.read;
}

void to_c(const js8::HeldMessage &m, js8_held_msg_t *c) {
    std::memset(c, 0, sizeof *c);
    c->id     = m.id;
    c->utc_ms = m.utc_ms;
    put(m.from, c->from);
    put(m.to, c->to);
    put(m.path, c->path);
    put(m.text, c->text);
    c->delivered = m.delivered;
    c->group     = m.is_group();
    c->got       = (int)m.got.size();
}

/* Each caller keeps its own generator, seeded once from the system. */
std::mt19937 seeded() {
    std::random_device dev;
    return std::mt19937(dev());
}

} // unnamed namespace

/* ---- One-press messages and heartbeats ---- */

const char *js8_query_label(js8_query_t which) {
    static const char *const names[] = {"SNR?",  "Send SNR", "GRID?", "My grid", "INFO?",
                                        "STATUS?", "HEARING?", "AGN?", "RR",      "73"};
    static_assert(sizeof names / sizeof *names == JS8_Q_COUNT);
    return (unsigned)which < JS8_Q_COUNT ? names[which] : "";
}

bool js8_query_text(js8_query_t which, const char *to_call, int their_snr, const char *my_grid, char *out,
                    unsigned out_len) {
    if ((unsigned)which >= (unsigned)JS8_Q_COUNT) return false;
    std::string msg = js8::query_text((js8::Query)which, or_empty(to_call), their_snr, or_empty(my_grid));
    put(msg, out, out_len);
    return !msg.empty();
}

void js8_heartbeat_text(const char *call, const char *grid, char *out, unsigned out_len) {
    put(js8::heartbeat_text(or_empty(call), or_empty(grid)), out, out_len);
}

int js8_heartbeat_offset(const float *offsets_hz, const int64_t *heard_ms, unsigned count, int64_t now_ms) {
    static std::mt19937 gen = seeded();
    std::vector<js8::OffsetActivity> busy(count);
    for (unsigned i = 0; i < count; i++) busy[i] = {offsets_hz[i], heard_ms[i]};
    return js8::find_free_offset(busy, now_ms, gen);
}

int64_t js8_next_heartbeat_ms(int64_t now_ms, int minutes) {
    static std::mt19937 gen = seeded();
    return js8::next_heartbeat_ms(now_ms, minutes, gen);
}

/* ---- Heard stations ---- */

js8_stations_t *js8_stations_create(void) { return new (std::nothrow) js8_stations_t; }

void js8_stations_add(js8_stations_t *list, const js8_rx_msg_t *msg, const char *my_call, int64_t now_ms) {
    if (list == nullptr || msg == nullptr || msg->tx) return; // our own transmissions are not "heard"
    js8::StationEvent e;
    e.from    = msg->from;
    e.to      = msg->to;
    e.text    = msg->text;
    e.to_me   = msg->to_me;
    e.snr     = msg->snr;
    e.freq_hz = msg->freq_hz;
    e.mode    = msg->submode;
    e.when_ms = now_ms;
    list->stations.add(e, or_empty(my_call));
}

int js8_stations_list(js8_stations_t *list, int64_t now_ms, js8_station_t *rows, int max) {
    if (list == nullptr || rows == nullptr) return 0;
    return copy_list(list->stations.sorted(now_ms), rows, max, [](const js8::Station &st, js8_station_t *c) {
        *c = js8_station_t{};
        put(st.call, c->call);
        put(st.grid, c->grid);
        c->heard_ms    = st.heard_ms;
        c->snr         = (int16_t)st.snr;
        c->freq_hz     = st.freq_hz;
        c->submode     = (uint8_t)st.mode;
        c->heard_me    = st.heard_me;
        c->heard_me_ms = st.heard_me_ms;
        put_opt(st.reported_snr, c->has_reported_snr, c->reported_snr);
        c->x6100       = st.x6100;
    });
}

void js8_stations_clear(js8_stations_t *list) {
    if (list != nullptr) list->stations.clear();
}

/* ---- Automatic replies ---- */

js8_auto_t *js8_auto_create(void) { return new (std::nothrow) js8_auto_t; }

void js8_auto_destroy(js8_auto_t *a) { delete a; }

/* Puts a message where process() said it belongs (the inbox when it is for
 * us, the held store when it is for someone else) and describes what was
 * done. A repeat of a stored message is not written to the file again. */
static js8_stored_t store_message(const js8::StoreAction &act, int64_t now_ms, js8_inbox_t *inbox,
                                  js8_held_t *held) {
    using Kind = js8::StoreAction::Kind;
    js8_stored_t res{};
    bool         inserted = false;
    bool         written  = true;
    /* The store says whether it kept the message: a full store drops its oldest for a new one,
     * so its size does not tell, and the new message was taken for a resend and never saved */
    if (act.kind == Kind::Inbox && inbox != nullptr) {
        res.kind   = JS8_STORED_INBOX;
        res.id     = inbox->store.add(act.from, act.text, now_ms, act.to, act.path, &inserted);
        res.resend = !inserted;
        if (inserted) written = inbox->store.save(inbox->file);
    } else if (act.kind == Kind::Held && held != nullptr) {
        res.kind   = JS8_STORED_HELD;
        res.id     = held->store.add(act.from, act.to, act.text, now_ms, act.path, &inserted);
        res.resend = !inserted;
        if (inserted) written = held->store.save(held->file);
    } else {
        return res; // nothing stored: all zero
    }
    if (!written) res.id = -1;
    put(act.from, res.from);
    put(act.to, res.to);
    put(act.path, res.path);
    put(act.text, res.text);
    return res;
}

void js8_process(js8_auto_t *a, const js8_rx_msg_t *msg, const js8_auto_settings_t *s, const js8_heard_t *heard,
                 unsigned n_heard, const char *last_tx, int64_t now_ms, js8_inbox_t *inbox, js8_stored_t *stored,
                 js8_auto_result_t *out) {
    if (out != nullptr) *out = js8_auto_result_t{};
    if (stored != nullptr) *stored = js8_stored_t{};
    // Our own transmissions and messages still arriving are not answered.
    if (a == nullptr || msg == nullptr || s == nullptr || msg->tx || msg->partial) return;

    js8::AutoSettings cfg;
    cfg.autoreply = s->autoreply;
    cfg.heartbeat = s->heartbeat;
    cfg.hb_ack    = s->hb_ack;
    cfg.relay     = s->relay;
    cfg.my_call   = or_empty(s->my_call);
    cfg.my_grid   = or_empty(s->my_grid);
    cfg.info      = or_empty(s->info);
    cfg.status    = or_empty(s->status);
    cfg.groups    = split_groups(s->groups);
    if (s->held != nullptr) cfg.held = &s->held->store;

    js8::Incoming in = incoming(*msg);
    in.when_ms       = now_ms;
    js8::Processed result = js8::process(in, cfg, heard_vector(heard, n_heard), or_empty(last_tx));

    // Store first: the reply decision may depend on what is now held.
    js8_stored_t kept = store_message(result.store, now_ms, inbox, s->held);
    if (stored != nullptr) *stored = kept;

    if (out == nullptr || !result.reply) return;
    const js8::AutoReply &reply = *result.reply;
    out->action  = (js8_auto_action_t)a->limits.decide(reply, cfg, now_ms);
    out->kind    = (js8_reply_kind_t)reply.kind;
    out->hb_ack  = reply.kind == js8::ReplyKind::HeartbeatAck;
    out->allcall = reply.allcall;
    put(reply.text, out->text);
    put(reply.to, out->to);
    put(reply.command, out->command);
    out->deliver_id = reply.deliver_id;
    put(reply.deliver_group_call, out->deliver_group_call);
}

void js8_auto_sent(js8_auto_t *a, const js8_auto_result_t *res, int64_t now_ms) {
    if (a == nullptr || res == nullptr) return;
    // A kind the C side made up counts as a plain query.
    bool known = (unsigned)res->kind <= (unsigned)JS8_REPLY_STORED;
    a->limits.sent(js8::AutoReply{.text    = res->text,
                                  .to      = res->to,
                                  .command = res->command,
                                  .kind    = known ? (js8::ReplyKind)res->kind : js8::ReplyKind::Query,
                                  .allcall = res->allcall},
                   now_ms);
}

void js8_auto_user_activity(js8_auto_t *a, int64_t when_ms) {
    if (a != nullptr) a->limits.user_activity(when_ms);
}

bool js8_auto_idle(js8_auto_t *a, int64_t when_ms) {
    return a != nullptr && a->limits.idle(when_ms);
}

bool js8_starts_qso(const js8_rx_msg_t *rx) {
    if (rx == nullptr || rx->tx) return false;
    return js8::starts_qso(incoming(*rx));
}

/* ---- Time Sync ---- */

bool js8_sync_drift(const js8_sync_sample_t *s, unsigned n, int64_t now_ms, int64_t window_ms, int64_t current_ms,
                    int64_t *drift_ms, unsigned *decodes, unsigned *stations) {
    struct Vote {
        int64_t when_ms, drift_ms;
    };
    std::vector<int64_t>              every;  // one suggestion per usable decode
    std::map<std::string, Vote>       newest; // per station, its latest decode
    for (unsigned i = 0; s != nullptr && i < n; i++) {
        const js8_sync_sample_t &d = s[i];
        bool usable = d.when_ms != 0 && d.when_ms <= now_ms && !(now_ms - d.when_ms > window_ms);
        if (!usable) continue;
        // A drift is only known modulo the slot length, so bring it to the
        // copy nearest the drift in use (-14 s and +1 s in a 15 s slot are
        // the same thing).
        int64_t slot = d.period_ms > 0 ? d.period_ms : 15000;
        int64_t off  = ((int64_t)d.drift_ms - current_ms) % slot;
        if (off > slot / 2) off -= slot;
        if (off < -(slot / 2)) off += slot;
        int64_t suggested = current_ms + off;
        every.push_back(suggested);
        if (d.call[0] == '\0') continue;
        auto [it, fresh] = newest.try_emplace(d.call, Vote{d.when_ms, suggested});
        if (!fresh && it->second.when_ms < d.when_ms) it->second = Vote{d.when_ms, suggested};
    }
    if (decodes != nullptr) *decodes = (unsigned)every.size();
    if (stations != nullptr) *stations = (unsigned)newest.size();
    if (every.size() < JS8_SYNC_MIN_DECODES || drift_ms == nullptr) return false;

    if (newest.size() >= JS8_SYNC_MIN_STATIONS) {
        every.clear();
        for (const auto &kv : newest) every.push_back(kv.second.drift_ms);
    }
    std::sort(every.begin(), every.end());
    std::size_t half = every.size() / 2;
    if (every.size() & 1)
        *drift_ms = every[half];
    else
        *drift_ms = (every[half - 1] + every[half]) / 2;
    return true;
}

/* ---- Locator ---- */

/* One axis of a locator: `frac` is the position across the whole range
 * (0..1); each step splits the current box into `div[i]` parts and writes
 * one character every second byte. */
static void locator_axis(double frac, int steps, char *dst) {
    static const int div[5] = {18, 10, 24, 10, 24}; // field, square, subsquare, then again
    for (int i = 0; i < steps; i++) {
        frac *= div[i];
        int k = (int)frac;
        if (k > div[i] - 1) k = div[i] - 1; // exactly on the top edge (lat 90, lon 180)
        dst[2 * i] = (char)((i & 1 ? '0' : 'A') + k);
        frac -= k;
    }
}

bool js8_latlon_to_grid(double lat_deg, double lon_deg, int chars, char *out, unsigned out_size) {
    if (out == nullptr || chars < 2 || chars > 10 || (chars & 1) || out_size <= (unsigned)chars) return false;
    bool on_earth = lat_deg >= -90 && lat_deg <= 90 && lon_deg >= -180 && lon_deg <= 180; // false for NaN too
    if (!on_earth) return false;
    locator_axis((lon_deg + 180.0) / 360.0, chars / 2, out);
    locator_axis((lat_deg + 90.0) / 180.0, chars / 2, out + 1);
    out[chars] = 0;
    return true;
}

/* ---- QSO log ---- */

js8_qsos_t *js8_qsos_create(void) { return new (std::nothrow) js8_qsos_t; }

void js8_qsos_destroy(js8_qsos_t *q) { delete q; }

/* The tracker names the station when a QSO has just ended. */
static bool report_end(const std::optional<std::string> &call, char *ended, unsigned ended_len) {
    if (call) put(*call, ended, ended_len);
    return call.has_value();
}

bool js8_qsos_received(js8_qsos_t *q, const js8_rx_msg_t *msg, const char *my_call, int64_t now_ms, char *ended,
                       unsigned ended_len) {
    if (q == nullptr || msg == nullptr || my_call == nullptr) return false;
    if (msg->tx || msg->low_confidence) return false;
    auto call = q->qsos.received(msg->from, msg->text, msg->to_me, msg->snr, my_call, now_ms);
    return report_end(call, ended, ended_len);
}

bool js8_qsos_sent(js8_qsos_t *q, const char *text, const char *my_call, int64_t now_ms, char *ended,
                   unsigned ended_len) {
    if (q == nullptr || text == nullptr || my_call == nullptr) return false;
    return report_end(q->qsos.sent(text, my_call, now_ms), ended, ended_len);
}

bool js8_qsos_get(js8_qsos_t *q, const char *station, int64_t now_ms, js8_qso_t *out) {
    if (q == nullptr || station == nullptr || out == nullptr) return false;
    std::optional<js8::Qso> found = q->qsos.get(station, now_ms);
    if (!found) return false;
    const js8::Qso &qso = *found;
    std::memset(out, 0, sizeof *out);
    put(qso.call, out->call);
    put(qso.grid, out->grid);
    out->start_ms = qso.start_ms;
    put_opt(qso.sent_snr, out->has_sent_snr, out->sent_snr);
    put_opt(qso.rcvd_snr, out->has_rcvd_snr, out->rcvd_snr);
    put_opt(qso.heard_snr, out->has_heard_snr, out->heard_snr);
    out->two_way = qso.we_sent && qso.they_sent;
    return true;
}

void js8_qsos_logged(js8_qsos_t *q, const char *station) {
    if (q != nullptr && station != nullptr) q->qsos.logged(station);
}

bool js8_log_append(const char *path, const js8_log_entry_t *e, char *err, unsigned err_size) {
    if (path == nullptr || e == nullptr) return false;
    js8::LogEntry rec;
    rec.call     = e->call;
    rec.grid     = e->grid;
    rec.name     = e->name;
    rec.comment  = e->comment;
    rec.rst_sent = e->rst_sent;
    rec.rst_rcvd = e->rst_rcvd;
    rec.on_ms    = e->on_ms;
    rec.off_ms   = e->off_ms;
    rec.freq_hz  = e->freq_hz;
    rec.my_call  = e->my_call;
    rec.op_call  = e->my_call; // the operator is the station owner
    rec.my_grid  = e->my_grid;
    rec.tx_pwr_w = e->tx_pwr_w;
    rec.pota_ref = e->pota_ref;
    rec.sota_ref = e->sota_ref;
    std::string why;
    bool        ok = js8::adif_append(path, rec, why);
    if (!ok) put(why, err, err_size);
    return ok;
}

const char *js8_log_band(uint64_t hz) {
    // The pointer handed back stays valid until this thread calls again.
    static thread_local std::string name;
    name = js8::adif_band(hz);
    return name.c_str();
}

/* ---- Inbox ---- */

js8_inbox_t *js8_inbox_open(const char *file) {
    auto *box = new (std::nothrow) js8_inbox_t;
    if (box == nullptr) return nullptr;
    box->file = or_empty(file);
    box->store.load(box->file); // a missing file is an empty inbox
    return box;
}

void js8_inbox_close(js8_inbox_t *b) { delete b; }

int js8_inbox_add(js8_inbox_t *b, const char *from, const char *text, int64_t utc_ms) {
    if (b == nullptr || from == nullptr || text == nullptr) return -1;
    int id = b->store.add(from, text, utc_ms);
    if (!b->store.save(b->file)) return -1;
    return id;
}

int js8_inbox_list(js8_inbox_t *b, js8_inbox_msg_t *msgs, int max) {
    if (b == nullptr || msgs == nullptr) return 0;
    return copy_list(b->store.list(), msgs, max, [](const js8::InboxMessage &m, js8_inbox_msg_t *c) { to_c(m, c); });
}

bool js8_inbox_get(js8_inbox_t *b, int msg_id, js8_inbox_msg_t *out) {
    if (b == nullptr || out == nullptr) return false;
    std::optional<js8::InboxMessage> m = b->store.get(msg_id);
    if (m) to_c(*m, out);
    return m.has_value();
}

void js8_inbox_mark_read(js8_inbox_t *b, int msg_id) {
    if (b != nullptr && b->store.mark_read(msg_id)) b->store.save(b->file);
}

void js8_inbox_delete(js8_inbox_t *b, int msg_id) {
    if (b != nullptr && b->store.remove(msg_id)) b->store.save(b->file);
}

int js8_inbox_unread(js8_inbox_t *b) {
    return b == nullptr ? 0 : b->store.unread();
}

int js8_inbox_count(js8_inbox_t *b) {
    return b == nullptr ? 0 : (int)b->store.size();
}

bool js8_msg_for_me(const js8_rx_msg_t *msg, const char *my_call, char *out, unsigned out_size) {
    // Only a complete received message with a good checksum counts.
    if (msg == nullptr || my_call == nullptr || msg->tx || msg->checksum != 1) return false;
    std::optional<std::string> body = js8::msg_body(msg->text, my_call);
    if (body) put(*body, out, out_size);
    return body.has_value();
}

/* ---- Held messages ---- */

js8_held_t *js8_held_open(const char *file) {
    auto *held = new (std::nothrow) js8_held_t;
    if (held == nullptr) return nullptr;
    held->file = or_empty(file);
    held->store.load(held->file); // a missing file is an empty store
    return held;
}

void js8_held_close(js8_held_t *h) { delete h; }

int js8_held_add(js8_held_t *h, const char *from, const char *to, const char *text, int64_t utc_ms) {
    if (h == nullptr || from == nullptr || to == nullptr || text == nullptr) return -1;
    int id = h->store.add(from, to, text, utc_ms);
    if (!h->store.save(h->file)) return -1;
    return id;
}

int js8_held_list(js8_held_t *h, js8_held_msg_t *msgs, int max) {
    if (h == nullptr || msgs == nullptr) return 0;
    return copy_list(h->store.list(), msgs, max, [](const js8::HeldMessage &m, js8_held_msg_t *c) { to_c(m, c); });
}

bool js8_held_get(js8_held_t *h, int msg_id, js8_held_msg_t *out) {
    if (h == nullptr || out == nullptr) return false;
    std::optional<js8::HeldMessage> m = h->store.get(msg_id);
    if (m) to_c(*m, out);
    return m.has_value();
}

void js8_held_delivered(js8_held_t *h, int msg_id) {
    if (h != nullptr && h->store.mark_delivered(msg_id)) h->store.save(h->file);
}

void js8_held_group_delivered(js8_held_t *h, int msg_id, const char *station) {
    if (h == nullptr || station == nullptr) return;
    if (h->store.mark_group_delivered(msg_id, station)) h->store.save(h->file);
}

bool js8_held_push_due(js8_held_t *h, const js8_heard_t *heard, unsigned n_heard, int64_t now_ms, char *text,
                       unsigned text_len) {
    if (h == nullptr) return false;
    auto due = h->store.push_due(heard_vector(heard, n_heard), now_ms);
    if (!due.has_value()) return false;
    h->store.save(h->file); // the reminder time is part of the file
    put(due->second, text, text_len);
    return true;
}

void js8_held_delete(js8_held_t *h, int msg_id) {
    if (h != nullptr && h->store.remove(msg_id)) h->store.save(h->file);
}

int js8_held_waiting(js8_held_t *h) {
    return h == nullptr ? 0 : h->store.waiting();
}

int js8_held_count(js8_held_t *h) {
    return h == nullptr ? 0 : (int)h->store.size();
}

bool js8_msg_to_for_me(const js8_rx_msg_t *msg, const char *my_call, char *to, unsigned to_len, char *text,
                       unsigned text_len) {
    if (msg == nullptr || my_call == nullptr || msg->tx || msg->checksum != 1) return false;
    auto parts = js8::msg_to_body(msg->text, my_call);
    if (!parts) return false;
    put(parts->first, to, to_len);
    put(parts->second, text, text_len);
    return true;
}

/* ---- Paths, signatures, groups, alerts ---- */

void js8_path_display(const char *path, char *out, unsigned out_size) {
    put(js8::path_display(or_empty(path)), out, out_size);
}

bool js8_delivered_signature(const char *text, char *from, unsigned from_size, int *next_id) {
    std::optional<js8::Signature> sig = js8::delivered_signature(or_empty(text));
    if (!sig.has_value()) return false;
    put(sig->from, from, from_size);
    if (next_id != nullptr) *next_id = sig->next_id;
    return true;
}

void js8_groups_normalise(const char *typed, char *out, unsigned out_size) {
    std::vector<std::string> kept;
    std::string              line;
    for (std::string g : split_groups(typed)) {
        if (g.front() != '@') g.insert(0, 1, '@');
        bool seen = std::find(kept.begin(), kept.end(), g) != kept.end();
        if (g.size() == 1 || seen) continue; // a lone '@', or a repeat
        if (kept.size() == 10) break;
        kept.push_back(g);
        line += line.empty() ? g : " " + g;
    }
    put(line, out, out_size);
}

bool js8_x6100_joined(const char *groups) {
    const std::vector<std::string> in = split_groups(groups);
    return std::find(in.begin(), in.end(), js8::X6100_GROUP) != in.end();
}

bool js8_to_x6100(const js8_rx_msg_t *msg, const char *groups) {
    return msg != nullptr && !msg->tx && js8::is_x6100_group(msg->to) && js8_x6100_joined(groups);
}

void js8_alert_words_normalise(const char *typed, char *out, unsigned out_size) {
    put(js8::format_alert_words(js8::parse_alert_words(or_empty(typed))), out, out_size);
}

bool js8_alert_hit(const char *text, const char *from, const char *words, char *hit, unsigned hit_size) {
    if (text == nullptr || words == nullptr || *words == '\0') return false;
    std::string first = js8::alert_word_hit(text, or_empty(from), js8::parse_alert_words(words));
    if (first.empty()) return false;
    put(first, hit, hit_size);
    return true;
}

/* ---- Macros ---- */

void js8_macros_expand(const char *text, const js8_macro_values_t *v, bool prune, char *out, unsigned out_len) {
    if (out == nullptr || out_len == 0) return;
    *out = '\0';
    if (text == nullptr) return;
    js8::MacroInput mi;
    if (v != nullptr) {
        mi.my_call   = or_empty(v->my_call);
        mi.my_grid   = or_empty(v->my_grid);
        mi.my_info   = or_empty(v->my_info);
        mi.my_status = or_empty(v->my_status);
        mi.version   = or_empty(v->version);
        mi.idle_ms   = v->idle_ms;
        mi.call      = or_empty(v->call);
        if (v->has_snr) mi.snr = v->snr;
        if (v->has_tdelta) mi.tdelta_ms = v->tdelta_ms;
    }
    put(js8::replace_macros(text, js8::macro_values(mi), prune), out, out_len);
}

bool js8_macros_need_station(const char *text) {
    return text != nullptr && js8::macros_need_station(text);
}
