/* Issue #91, the radio's side: the filter edges the radio sets for the browser Panel's "set filter LO HI", by its
 * own rules. The production SettingsManager (filter_mode(), filter_high_max(), the edges' checks, the CW width
 * around the key tone) with sql/params.sql in memory, and the remote server's own P_FILTER case, extracted from
 * src/remote_server.c by run.py into panel_filter.inc. Lines on stdin, each answered with the radio's state after
 * it, "MODE KT FLO FHI":
 *   mode M          the mode (the X6200's SAM and WFM too: the rules are the settings', not the base's)
 *   kt K            the CW key tone
 *   filter LO HI    as the Panel sends it: panel_set's order of the two values, then panel_apply's P_FILTER
 *   low V, high V   one edge alone, as the radio's own Filter low and Filter high set it (radio_controls.cpp)
 *   state           nothing changed */
#include "src/cfg/settings_manager.h"
#include "src/cfg/computed_api.h"
#include "src/scheduler.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
extern "C" void _lv_log_add(lv_log_level_t, const char *, int, const char *, const char *, ...) {}
extern "C" lv_res_t lv_async_call(lv_async_cb_t, void *) { return LV_RES_OK; }
extern "C" lv_res_t lv_async_call_cancel(lv_async_cb_t, void *) { return LV_RES_OK; }
extern "C" bool scheduler_on_gui_thread() { return true; }

/* What the extracted case reads and sets, as cfg_api.cpp points them */
static ComputedParamInt *cfg_cur_mode, *cfg_cur_filter_low, *cfg_cur_filter_high, *cfg_cur_filter_bw;
enum { P_FILTER, P_ATU };
typedef struct { int id; int32_t a, b; } pcmd_t;
static void panel_filter(const pcmd_t *p) {
    switch (p->id) {
#include "panel_filter.inc"
    }
}

int main() {
    sqlite3 *db;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    std::ifstream f("sql/params.sql");
    std::string sql((std::istreambuf_iterator<char>(f)), {});
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    cfg_db_init(db);
    auto *s = new SettingsManager;
    s->init_load(nullptr);
    cfg_cur_mode        = &s->cp_cur_mode;
    cfg_cur_filter_low  = &s->cp_cur_filter_low;
    cfg_cur_filter_high = &s->cp_cur_filter_high;
    cfg_cur_filter_bw   = &s->cp_cur_filter_bw;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string what;
        in >> what;
        if (what == "mode") {
            int32_t m;
            in >> m;
            s->cp_cur_mode.set(m);
        } else if (what == "kt") {
            int32_t k;
            in >> k;
            s->p_key_tone.set(k);
        } else if (what == "filter") {
            int32_t a, b;
            in >> a >> b;
            a = std::abs(a), b = std::abs(b);        /* panel_set: the two values in order */
            if (a > b) std::swap(a, b);
            pcmd_t p = {P_FILTER, a, b};
            panel_filter(&p);
        } else if (what == "low" || what == "high") {
            int32_t v;
            in >> v;
            (what == "low" ? s->cp_cur_filter_low : s->cp_cur_filter_high).set(v);
        } else if (what != "state") {
            std::cerr << "filter-rules: unknown line: " << line << "\n";
            return 2;
        }
        std::cout << s->cp_cur_mode.get() << " " << s->p_key_tone.get() << " " << s->cp_cur_filter_low.get() << " "
                  << s->cp_cur_filter_high.get() << std::endl;
    }
}
