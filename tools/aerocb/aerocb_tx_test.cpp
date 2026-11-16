/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test: PTT on the CB and HF Air Band apps' frequencies follows the radio's own TX rules, through the real
 *  cfg_tx_refusal on the real settings (a database made from sql/params.sql, as cfg_api_init loads it on the radio).
 *  The apps add no rule of their own. Every CB channel of every plan and every HF aero frequency:
 *  - as the radio comes (TX ranges Ham bands, TX outside ham bands off): refused, "outside the amateur bands";
 *  - TX outside ham bands on: allowed, as anywhere the hardware transmits;
 *  - an own TX list covering them: still refused with the switch off (the list stays inside the amateur bands), allowed
 *    with it on; an own list not covering them, or South Korea's ranges: refused with the switch on too.
 *    aerocb_tx_x6100 / aerocb_tx_x6200 <params.sql>
 */

#include "aero.h"
#include "cb.h"
#include "cfg/cfg_api.h"
#include "cfg/settings_manager.h"
#include "cfg/settings_manager_api.h"
#include "cfg/tx_ranges.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* Every frequency of both apps, Hz */
static std::vector<int32_t> app_freqs() {
    std::vector<int32_t> v;

    for (int p = 0; p < CB_PLANS; p++)
        for (int ch = 1; ch <= cb_channels(p); ch++) v.push_back(cb_freq(p, ch));
    for (int i = 0; i < aero_families(); i++)
        for (const uint16_t *k = aero_family(i)->khz; *k; k++) v.push_back(*k * 1000);
    return v;
}

/* All of them: refused for why (HAM_TX_OK: none refused) */
static void all(const std::vector<int32_t> &freqs, ham_tx_t why, const char *what) {
    for (int32_t f : freqs) {
        const char *no = cfg_tx_refusal(f);

        if (why == HAM_TX_OK) {
            check(no == nullptr, what, f);
        } else {
            check(no != nullptr && cfg_tx_check(f) == why, what, f);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: aerocb_tx_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;
    sqlite3          *db = nullptr;
    char             *err = nullptr;

    sql << f.rdbuf();
    if (!f.good() || sqlite3_open(":memory:", &db) != SQLITE_OK ||
        sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &err) != SQLITE_OK || !cfg_db_init(db)) {
        printf("params.sql into a database failed: %s\n", err ? err : "?");
        return 2;
    }
    cfg_api_init(nullptr);

    std::vector<int32_t> freqs = app_freqs();

    check(freqs.size() > 500, "frequencies of both apps", (long)freqs.size());
    check(cfg_sm.p_tx_outside_bands.get() == 0 && cfg_sm.p_tx_ranges.get() == TX_RANGES_BANDS, "as the radio comes",
          0);

    /* The radio as it comes: nowhere, and with main's words */
    all(freqs, HAM_TX_OFF_BAND, "refused with TX outside ham bands off");
    check(!strcmp(cfg_tx_refusal(27185000), tx_ranges_why(HAM_TX_OFF_BAND)), "the refusal's words", 0);
    check(cfg_tx_refusal(14200000) == nullptr, "20 m transmits", 0);

    /* TX outside ham bands on (the MARS/CAP setting): wherever the hardware transmits */
    cfg_sm.p_tx_outside_bands.set(1);
    all(freqs, HAM_TX_OK, "allowed with TX outside ham bands on");

    /* An own list covering CB and the HF aero bands: the switch still decides */
    tx_ranges_own_t st = tx_ranges_own_set("2.850-22.000\n26.000-28.000\n");

    check(st.count == 2 && st.bad == 0, "own list read", st.count);
    cfg_sm.p_tx_ranges.set(TX_RANGES_OWN);
    all(freqs, HAM_TX_OK, "own list covering them, switch on: allowed");
    cfg_sm.p_tx_outside_bands.set(0);
    all(freqs, HAM_TX_OFF_BAND, "own list covering them, switch off: refused");

    /* An own list elsewhere, or South Korea's: outside their ranges whatever the switch */
    cfg_sm.p_tx_outside_bands.set(1);
    tx_ranges_own_set("7.000-7.200\n");
    all(freqs, HAM_TX_OFF_RANGE, "own list elsewhere, switch on: refused");
    cfg_sm.p_tx_ranges.set(TX_RANGES_KR);
    all(freqs, HAM_TX_OFF_RANGE, "South Korea, switch on: refused");

    printf("aerocb_tx (%s): %d checks, %d failed\n",
#ifdef BOARD_X6200
           "X6200",
#else
           "X6100",
#endif
           checks, fails);
    return fails != 0;
}
