
#include "qso.h"

#define DEFAULT_SNR 100
#define ASIDE_SLOTS 4       /* a QSO put aside waits this many slots for its station's late message */
#define ASIDE_MAX   4       /* QSOs put aside at most, the oldest goes */

extern "C" {
    #include "../qth/qth.h"
    #include "string.h"
    #include "utils.h"
}

static std::string make_answer_text(ftx_msg_type_t last_rx_type, std::string remote_callsign,
                                    std::string local_callsign, const int local_snr, std::string grid);

std::vector<std::string> split_text(std::string text) {
    std::vector<std::string> tokens;

    const char  delim = ' ';
    size_t      initialPos = 0;
    size_t      pos;
    std::string token;
    do {
        pos = text.find(delim, initialPos);
        token = text.substr(initialPos, pos - initialPos);
        if (!token.empty())
            tokens.push_back(token);
        initialPos = pos + 1;
    } while (pos != std::string::npos);
    return tokens;
}

Candidate::Candidate(std::string remote_callsign) {
    _remote_callsign = remote_callsign;
    _sent_snr = DEFAULT_SNR;
    _rcvd_snr = DEFAULT_SNR;
}

void Candidate::set_grid(std::string grid) {
    _grid = grid;
}

void Candidate::set_report(int report) {
    _rcvd_snr = report;
}

void Candidate::set_msg_type(ftx_msg_type_t msg_type) {
    _last_rx_type = msg_type;
    _quiet_slots = 0;
    if ((msg_type == FTX_MSG_TYPE_CQ) || (msg_type == FTX_MSG_TYPE_GRID) || (msg_type == FTX_MSG_TYPE_REPORT))
        _ended = false;     // begun again: a call, a grid or a report goes on until its own end
}

void Candidate::set_local_snr(int snr) {
    _local_snr = snr;
}

void Candidate::set_rcvd_snr(int snr) {
    _rcvd_snr = snr;
}

bool Candidate::match_callsign(std::string callsign) {
    return callsign == _remote_callsign;
}

std::string Candidate::callsign() {
    return _remote_callsign;
}

bool Candidate::is_finished() {
    return (_last_rx_type == FTX_MSG_TYPE_73) || (_last_rx_type == FTX_MSG_TYPE_RR73);
}

/* Ended (logged, nothing begun since), and a slot of ours since its station's last message: our
 * RR73 or 73 went (or the message after Force QSO save), a caller may take over (its station
 * repeating would have sent again). A logged QSO taken up again goes on as any other */
bool Candidate::is_over() {
    return _ended && _quiet_slots >= 2;
}

/* One more slot without its message: true once our last try (RR73 and 73 go once) had its answer slot, and a slot spare */
bool Candidate::quiet_slot(int max_repeats) {
    int tries = (_last_rx_type == FTX_MSG_TYPE_R_REPORT || is_finished()) ? 1 : max_repeats;
    return tries > 0 && ++_quiet_slots > 2 * tries + 1;
}

bool Candidate::can_be_saved() {
    return (!_remote_callsign.empty()) && (_rcvd_snr != DEFAULT_SNR) && (_sent_snr != DEFAULT_SNR) && !_saved;
}

bool Candidate::is_saved() {
    return _saved;
}

/* Our report went out (transmit() said so), not only made: its station can still end the QSO */
bool Candidate::sent_report() {
    return _report_out;
}

/* A message of ours went out: our report, when it is the one made last for this QSO */
void Candidate::sent(std::string text) {
    if (!_report_text.empty() && (text == _report_text)) _report_out = true;
}

void Candidate::set_aside() {
    _quiet_slots = 0;
}

/* One more slot aside without its message: true once it has waited long enough */
bool Candidate::aside_slot() {
    return ++_quiet_slots > ASIDE_SLOTS;
}

std::string Candidate::get_tx_text(std::string local_callsign, std::string local_qth) {
    std::string answer = make_answer_text(_last_rx_type, _remote_callsign, local_callsign, _local_snr, local_qth);
    if ((_last_rx_type == FTX_MSG_TYPE_GRID) || (_last_rx_type == FTX_MSG_TYPE_REPORT)) {
        _sent_snr = _local_snr;
        _report_text = answer;
    }
    return answer;
}

/* Logged once; its end again (an R+report, RR73, RRR or 73 repeated) ends it again */
void Candidate::save_qso(save_qso_cb_t save_qso_cb, bool aside) {
    if (save_qso_cb && can_be_saved()) {
        save_qso_cb(_remote_callsign.c_str(), _grid.c_str(), _rcvd_snr, _sent_snr, aside);
        _saved = true;
    }
    if (_saved) _ended = true;
}

FTxQsoProcessor::FTxQsoProcessor(std::string local_callsign, std::string local_qth, save_qso_cb_t save_qso_cb, int max_repeats) {
    _local_callsign = local_callsign;
    if (local_qth.size() > 4) {
        _local_qth = local_qth.substr(0, 4);
    } else {
        _local_qth = local_qth;
    }
    _save_qso_cb = save_qso_cb;
    _max_repeats = max_repeats;
}

FTxQsoProcessor::~FTxQsoProcessor() {
    clear();
}

void FTxQsoProcessor::add_rx_text(std::string text, const int snr, ftx_msg_meta_t *meta, ftx_tx_msg_t *tx_msg) {
    meta->type = FXT_MSG_TYPE_OTHER;
    meta->local_snr = snr;
    meta->to_me = false;
    meta->grid[0] = '\0';

    std::vector<std::string> tokens = split_text(text);

    if (tokens.empty()) {
        return;
    }

    if ((tokens.size() >= 5) && (text.find(';') != text.npos)) {
        // "A2AA RR73; R2RFE <RP79AA> +05"
        std::vector<std::string> new_tokens;
        if (tokens[0] == _local_callsign) {
            new_tokens.push_back(tokens[0]);
            new_tokens.push_back(tokens[3]);
            new_tokens.push_back(tokens[1].substr(0, tokens[1].size() - 1));
        } else {
            new_tokens.push_back(tokens[2]);
            new_tokens.push_back(tokens[3]);
            new_tokens.push_back(tokens[4]);
        }
        tokens = new_tokens;
    }

    for (auto &token : tokens) {
        if (!token.empty() && token[0] == '<') {
            token = token.substr(1, token.length() - 2);
        }
    }

    if (tokens[0] == "CQ") {
        process_cq(meta, tokens, snr);
    } else if (tokens.size() >= 3) {
        if (tokens[2] == "73") {
            process_73(meta, tokens, snr, *tx_msg);
        } else if ((tokens[2] == "RRR") || (tokens[2] == "RR73")) {
            process_rr73(meta, tokens, snr, *tx_msg);
        } else if ((tokens[2][0] == 'R') && ((tokens[2][1] == '+') || (tokens[2][1] == '-'))) {
            process_r_report(meta, tokens, snr, *tx_msg);
        } else if ((tokens[2][0] == '+') || (tokens[2][0] == '-')) {
            process_report(meta, tokens, snr, *tx_msg);
        } else if (qth_grid_check(tokens[2].c_str())) {
            process_grid(meta, tokens, snr, *tx_msg);
        }
    }
}

void FTxQsoProcessor::process_grid(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                   ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    auto grid = tokens[2];
    meta->type = FTX_MSG_TYPE_GRID;
    strcpy(meta->grid, grid.c_str());
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        auto candidate_to_update = get_candidate_to_update(call_de);
        if (candidate_to_update != NULL) {
            (*candidate_to_update)->set_msg_type(meta->type);
            (*candidate_to_update)->set_local_snr(snr);
            (*candidate_to_update)->set_grid(grid);
            if ((*candidate_to_update == _cur_candidate) && _auto) {
                tx_msg.repeats = _max_repeats;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth).c_str());
            }
        }
    }
}

void FTxQsoProcessor::process_report(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                     ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    auto rcvd_snr = std::stoi(tokens[2]);
    meta->type = FTX_MSG_TYPE_REPORT;
    meta->remote_snr = rcvd_snr;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        auto candidate_to_update = get_candidate_to_update(call_de);
        if (candidate_to_update != NULL) {
            (*candidate_to_update)->set_msg_type(meta->type);
            (*candidate_to_update)->set_local_snr(snr);
            (*candidate_to_update)->set_rcvd_snr(rcvd_snr);
            if ((*candidate_to_update == _cur_candidate) && _auto) {
                tx_msg.repeats = _max_repeats;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth).c_str());
            }
        }
    }
}

void FTxQsoProcessor::process_r_report(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                       ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    auto rcvd_snr = std::stoi(tokens[2].substr(1, tokens[2].npos));
    meta->type = FTX_MSG_TYPE_R_REPORT;
    meta->remote_snr = rcvd_snr;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        if (((_cur_candidate != NULL) && (_cur_candidate->match_callsign(call_de))) || take_back(call_de) ||
            take_up(call_de)) {
            _cur_candidate->set_msg_type(meta->type);
            _cur_candidate->set_local_snr(snr);
            _cur_candidate->set_rcvd_snr(rcvd_snr);
            _cur_candidate->save_qso(_save_qso_cb);
            if (_auto) {
                tx_msg.repeats = 1;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth).c_str());
            }
        } else if (Candidate *aside = find_aside(call_de)) {
            /* A QSO we moved on from (we called CQ), another in progress: logged now, not answered */
            aside->set_msg_type(meta->type);
            aside->set_local_snr(snr);
            aside->set_rcvd_snr(rcvd_snr);
            aside->save_qso(_save_qso_cb, true);
        }
    }
}

void FTxQsoProcessor::process_rr73(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                   ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    meta->type = FTX_MSG_TYPE_RR73;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        if (((_cur_candidate != NULL) && (_cur_candidate->match_callsign(call_de))) || take_back(call_de)) {
            _cur_candidate->set_msg_type(meta->type);
            _cur_candidate->set_local_snr(snr);
            _cur_candidate->save_qso(_save_qso_cb);
            if (_auto) {
                tx_msg.repeats = 1;
                strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth).c_str());
            }
        } else if (Candidate *aside = find_aside(call_de)) {
            /* A QSO we moved on from: logged now, no 73 for it */
            aside->set_msg_type(meta->type);
            aside->set_local_snr(snr);
            aside->save_qso(_save_qso_cb, true);
        }
    }
}

void FTxQsoProcessor::process_73(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr,
                                 ftx_tx_msg_t &tx_msg) {
    auto call_to = tokens[0];
    auto call_de = tokens[1];
    meta->type = FTX_MSG_TYPE_73;
    strcpy(meta->call_de, call_de.c_str());
    if (call_to == _local_callsign) {
        meta->to_me = true;
        if ((_cur_candidate != NULL) && (_cur_candidate->match_callsign(call_de))) {
            /* Their 73 ends the QSO as well as an RR73 we missed: logged if both reports went */
            _cur_candidate->save_qso(_save_qso_cb);
            put_aside(_cur_candidate);
            if (_next_candidate != NULL) {
                _cur_candidate = _next_candidate;
                _next_candidate = NULL;
                if (_auto) {
                    tx_msg.repeats = 1;
                    strcpy(tx_msg.msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth).c_str());
                }
            } else {
                _cur_candidate = NULL;
            }
        } else if (Candidate *aside = find_aside(call_de)) {
            aside->save_qso(_save_qso_cb, true);
        }
    }
}

void FTxQsoProcessor::process_cq(ftx_msg_meta_t *meta, std::vector<std::string> &tokens, const int snr) {
    meta->type = FTX_MSG_TYPE_CQ;
    size_t call_de_pos;
    if (is_cq_modifier(tokens[1].c_str()) && tokens.size() > 2) {
        call_de_pos = 2;
    } else {
        call_de_pos = 1;
    }
    strcpy(meta->call_de, tokens[call_de_pos].c_str());
    if (tokens.size() > call_de_pos + 1) {
        strcpy(meta->grid, tokens[call_de_pos + 1].c_str());
    }
}

void FTxQsoProcessor::set_auto(bool val) {
    _auto = val;
}

void FTxQsoProcessor::set_max_repeats(int val) {
    _max_repeats = val;
}

void FTxQsoProcessor::set_is_pota(is_pota_cb_t cb) {
    _is_pota = cb;
}

/* Hound on or off: no QSO in progress, the ones with our report out kept aside for their late
 * message. In Hound mode our messages are Hound's: none of the sequence's goes out, so no QSO it
 * makes there is kept aside, and a late R+report is not taken up for an RR73 that can't go */
void FTxQsoProcessor::set_hound(bool on) {
    reset();
    _hound = on;
}

/* A message of ours went out: the QSO whose report it is has it out */
void FTxQsoProcessor::sent(std::string text) {
    if (_cur_candidate) _cur_candidate->sent(text);
    if (_next_candidate) _next_candidate->sent(text);
    for (auto candidate : _aside) candidate->sent(text);
}

void FTxQsoProcessor::start_new_slot() {
    _cur_fresh = false;
    _gave_way.clear();
    for (auto it = _aside.begin(); it != _aside.end();) {
        if ((*it)->aside_slot()) {
            delete *it;
            it = _aside.erase(it);
        } else {
            ++it;
        }
    }
    if (_cur_candidate != NULL && _cur_candidate->quiet_slot(_max_repeats)) {
        // Given up, or over without a 73: the next caller gets the answer, a late message still logs it
        put_aside(_cur_candidate);
        _cur_candidate = NULL;
    }
    if (_next_candidate != NULL) {
        put_aside(_next_candidate);
        _next_candidate = NULL;
    }
}

void FTxQsoProcessor::reset() {
    _gave_way.clear();
    put_aside(_cur_candidate);
    _cur_candidate = NULL;
    put_aside(_next_candidate);
    _next_candidate = NULL;
}

void FTxQsoProcessor::clear() {
    reset();
    for (auto candidate : _aside) delete candidate;
    _aside.clear();
}

/* No longer the current QSO (or the next): kept aside once our report went out, so that its
 * station's late R+report, RR73, RRR or 73 still logs it, and logs it once. One logged is kept
 * too, sent or not (Force QSO save before our slot): its station calling again goes on with it,
 * never a second entry. One whose report was only made (the operator moved on before our slot)
 * goes: its station never had it */
void FTxQsoProcessor::put_aside(Candidate *candidate) {
    if (candidate == NULL) return;
    if (!candidate->sent_report() && !candidate->is_saved()) {
        delete candidate;
        return;
    }
    if (_aside.size() >= ASIDE_MAX) {
        delete _aside.front();
        _aside.erase(_aside.begin());
    }
    candidate->set_aside();
    _aside.push_back(candidate);
}

Candidate *FTxQsoProcessor::find_aside(std::string call) {
    for (auto candidate : _aside)
        if (candidate->match_callsign(call)) return candidate;
    return NULL;
}

Candidate *FTxQsoProcessor::take_aside(std::string call) {
    for (auto it = _aside.begin(); it != _aside.end(); ++it) {
        if ((*it)->match_callsign(call)) {
            Candidate *candidate = *it;
            _aside.erase(it);
            return candidate;
        }
    }
    return NULL;
}

/* The station's QSO, taken from where it is: the next one, the one put aside (so a logged one is not
 * logged again), or a new one */
Candidate *FTxQsoProcessor::candidate_for(std::string call) {
    if ((_next_candidate != NULL) && _next_candidate->match_callsign(call)) {
        Candidate *candidate = _next_candidate;
        _next_candidate = NULL;
        return candidate;
    }
    Candidate *candidate = take_aside(call);
    return candidate ? candidate : new Candidate(call);
}

/* The station's QSO becomes the current one, the current goes aside: taken out first, as putting
 * one aside may drop the oldest there */
void FTxQsoProcessor::take_over(std::string call) {
    Candidate *candidate = candidate_for(call);
    put_aside(_cur_candidate);
    _cur_candidate = candidate;
}

/* The station whose logged QSO a caller took over in this slot sends again (our RR73 or 73 was
 * missed): its QSO is the current one again, the caller the next, as it would have been */
bool FTxQsoProcessor::take_back(std::string call) {
    if (_gave_way.empty() || _gave_way != call) return false;
    _gave_way.clear();
    return take_ahead(call);
}

/* No QSO in progress (none, or one over), or only a caller of this slot: the station whose QSO we
 * moved on from sends its R+report late (we called CQ). Its QSO is the current one again, for our
 * RR73, without which that station can't end it; the caller is the next. One over gives way as to
 * a caller, and is back if its station sends again in this slot */
bool FTxQsoProcessor::take_up(std::string call) {
    if (_hound || (find_aside(call) == NULL)) return false;
    if ((_cur_candidate != NULL) && _cur_candidate->is_over()) {
        _gave_way = _cur_candidate->callsign();
        take_over(call);
        _cur_fresh = false;
        return true;
    }
    if ((_cur_candidate != NULL) && !_cur_fresh) return false;
    return take_ahead(call);
}

/* The station's QSO put aside is the current one again, the current one (if any) the next */
bool FTxQsoProcessor::take_ahead(std::string call) {
    Candidate *candidate = take_aside(call);
    if (candidate == NULL) return false;
    if (_next_candidate == NULL) {
        _next_candidate = _cur_candidate;
    } else {
        put_aside(_cur_candidate);
    }
    _cur_candidate = candidate;
    _cur_fresh = false;
    return true;
}

void FTxQsoProcessor::start_qso(ftx_msg_meta_t *meta, ftx_tx_msg_t *tx_msg) {
    auto type = meta->type;
    if (!meta->to_me) {
        type = FTX_MSG_TYPE_CQ;
    } else if (meta->type == FTX_MSG_TYPE_73) {
        return;
    }
    get_or_create_cur_candidate(meta->call_de);
    _cur_candidate->set_local_snr(meta->local_snr);
    _cur_candidate->set_msg_type(type);

    tx_msg->repeats = _max_repeats;
    switch (type) {
    case FTX_MSG_TYPE_CQ:
    case FTX_MSG_TYPE_GRID:
        _cur_candidate->set_grid(meta->grid);
        break;
    case FTX_MSG_TYPE_R_REPORT:
        tx_msg->repeats = 1;
    case FTX_MSG_TYPE_REPORT:
        _cur_candidate->set_rcvd_snr(meta->remote_snr);
        break;
    case FTX_MSG_TYPE_RR73:
        tx_msg->repeats = 1;
        break;

    default:
        break;
    }
    strcpy(tx_msg->msg, _cur_candidate->get_tx_text(_local_callsign, _local_qth).c_str());
}

bool FTxQsoProcessor::can_save_qso() {
    return _cur_candidate && _cur_candidate->can_be_saved();
}

/* Logged by hand, as the desktop FT8 program logs: the TX message and the sequence go on, its
 * RR73 gets our 73, a report again our R-report, never a second entry. A caller takes over once
 * it is over (is_over) */
bool FTxQsoProcessor::force_save_qso() {
    _gave_way.clear();
    if (_cur_candidate) {
        _cur_candidate->save_qso(_save_qso_cb);
        return true;
    } else {
        return false;
    }
}

Candidate **FTxQsoProcessor::get_candidate_to_update(std::string call_de) {
    bool pota = _is_pota && _is_pota(call_de.c_str());
    if (_cur_candidate == NULL) {
        // Start new QSO
        _cur_candidate = candidate_for(call_de);
        _cur_fresh = !pota;
    } else if (_cur_candidate->is_over() && !_cur_candidate->match_callsign(call_de)) {
        // Logged, our last message out: the caller gets the answer, not after the slots it waits out
        _gave_way = _cur_candidate->callsign();
        take_over(call_de);
        _cur_fresh = !pota;
    } else if (_cur_fresh && pota) {
        // Callers in the same slot: a POTA activator goes first
        take_over(call_de);
        _cur_fresh = false;
    }
    Candidate **candidate_to_update = NULL;
    if (_cur_candidate->match_callsign(call_de)) {
        candidate_to_update = &_cur_candidate;
    } else if (_next_candidate == NULL) {
        _next_candidate = candidate_for(call_de);
        candidate_to_update = &_next_candidate;
    }
    return candidate_to_update;
}

Candidate *FTxQsoProcessor::get_or_create_cur_candidate(std::string remote_callsign) {
    _cur_fresh = false;     // the operator chose
    _gave_way.clear();
    // Try to continue current QSO
    if ((_cur_candidate == NULL) || !_cur_candidate->match_callsign(remote_callsign)) {
        take_over(remote_callsign);
    }
    return _cur_candidate;
}

bool FTxQsoProcessor::has_current() {
    return _cur_candidate != NULL;
}

static std::string make_answer_text(ftx_msg_type_t last_rx_type, std::string remote_callsign,
                                    std::string local_callsign, const int local_snr, std::string grid) {
    char answer[35];
    int  len = 0;
    switch (last_rx_type) {
    case FTX_MSG_TYPE_CQ:
        len = snprintf(answer, 35, "%s %s %s", remote_callsign.c_str(), local_callsign.c_str(), grid.c_str());
        break;
    case FTX_MSG_TYPE_GRID:
        len = snprintf(answer, 35, "%s %s %+03d", remote_callsign.c_str(), local_callsign.c_str(), local_snr);
        break;
    case FTX_MSG_TYPE_REPORT:
        len = snprintf(answer, 35, "%s %s R%+03d", remote_callsign.c_str(), local_callsign.c_str(), local_snr);
        break;
    case FTX_MSG_TYPE_R_REPORT:
        len = snprintf(answer, 35, "%s %s RR73", remote_callsign.c_str(), local_callsign.c_str());
        break;
    case FTX_MSG_TYPE_RR73:
        len = snprintf(answer, 35, "%s %s 73", remote_callsign.c_str(), local_callsign.c_str());
        break;
    }
    return std::string(answer, len);
}

FTxQsoProcessor *ftx_qso_processor_init(const char *local_callsign, const char *qth, save_qso_cb_t save_qso_cb,
                                        int max_repeats) {
    return new FTxQsoProcessor(local_callsign, qth, save_qso_cb, max_repeats);
}

void ftx_qso_processor_delete(FTxQsoProcessor *p) {
    delete p;
}

void ftx_qso_processor_set_auto(FTxQsoProcessor *p, bool val) {
    p->set_auto(val);
}

void ftx_qso_processor_set_max_repeats(FTxQsoProcessor *p, int max_repeats) {
    p->set_max_repeats(max_repeats);
}

void ftx_qso_processor_set_is_pota(FTxQsoProcessor *p, is_pota_cb_t is_pota) {
    p->set_is_pota(is_pota);
}

void ftx_qso_processor_sent(FTxQsoProcessor *p, const char *text) {
    if (p) p->sent(text);
}

void ftx_qso_processor_start_new_slot(FTxQsoProcessor *p) {
    p->start_new_slot();
}

void ftx_qso_processor_set_hound(FTxQsoProcessor *p, bool on) {
    p->set_hound(on);
}

void ftx_qso_processor_reset(FTxQsoProcessor *p) {
    p->reset();
}

void ftx_qso_processor_add_rx_text(FTxQsoProcessor *p, const char *text, const int snr, ftx_msg_meta_t *meta,
                                   ftx_tx_msg_t *tx_msg) {
    p->add_rx_text(text, snr, meta, tx_msg);
}

void ftx_qso_processor_start_qso(FTxQsoProcessor *p, ftx_msg_meta_t *meta, ftx_tx_msg_t *tx_msg) {
    p->start_qso(meta, tx_msg);
}

bool ftx_qso_processor_can_save_qso(FTxQsoProcessor *p) {
    return p->can_save_qso();
}

bool ftx_qso_processor_force_save_qso(FTxQsoProcessor *p) {
    return p->force_save_qso();
}

bool ftx_qso_processor_has_current(FTxQsoProcessor *p) {
    if (!p) return false;
    return p->has_current();
}
