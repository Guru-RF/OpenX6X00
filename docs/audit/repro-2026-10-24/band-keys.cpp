/* Issue #168: the band keys from the browser, the Panel's BAND buttons and the Screen page's, as the radio's own. The
 * production SettingsManager and cfg_band_load_next with sql/params.sql in memory; the remote server's own P_BAND
 * case (the Panel's "set band 1") extracted from src/remote_server.c and the radio's band_step (its BAND keys, which
 * the Screen page's BAND keys press) from src/screen_main.c, by run.py into band_keys.inc. Each line names the band
 * the dial is on after each press, as the Panel's band chip and the radio's screen show it */
#include "src/cfg/settings_manager.h"
#include "src/cfg/settings_manager_api.h"
#include "src/scheduler.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
extern "C" void _lv_log_add(lv_log_level_t, const char *, int, const char *, const char *, ...) {}
extern "C" lv_res_t lv_async_call(lv_async_cb_t, void *) { return LV_RES_OK; }
extern "C" lv_res_t lv_async_call_cancel(lv_async_cb_t, void *) { return LV_RES_OK; }
extern "C" bool scheduler_on_gui_thread() { return true; }

SettingsManager cfg_sm;     /* cfg_api.cpp's, which cfg_band_load_next works on */

/* What the extracted code reads and calls: no LOCK, no app holding the band */
static std::atomic<bool> state_resend;
static bool              band_lock;
static bool              main_screen_band_locked(void) { return band_lock; }
static uint32_t          EVENT_BAND_UP = 1, EVENT_BAND_DOWN = 2;
static void              dialog_send(uint32_t, void *) {}
enum { P_BAND, P_VFO };
typedef struct { int id; int32_t a, b; } pcmd_t;
#include "band_keys.inc"    /* static void panel_band(const pcmd_t *p), static void band_step(bool up) */

static std::string name_of(int32_t id) {
    BandInfoLoadResult b = BandsTable::get_by_id(id);
    return b.rc == SUCCESS ? b.value.name : "-";
}

/* The band of the dial, and whether it is the band the key chose */
static int outside;
static std::string where() {
    BandInfoLoadResult b = BandsTable::get_by_freq(static_cast<uint32_t>(cfg_sm.cp_fg_freq.get()));
    if (b.rc != SUCCESS || b.value.id != cfg_sm.current_band_id()) outside++;
    return b.rc == SUCCESS && b.value.id != BAND_UNDEFINED ? b.value.name : "gap";
}

/* n presses, by the Panel ("set band 1" / "set band -1") or by the radio's BAND key */
static std::string presses(bool panel, bool up, int n) {
    std::string out;
    outside = 0;
    for (int i = 0; i < n; i++) {
        if (panel) {
            pcmd_t p = {P_BAND, up ? 1 : -1, 0};
            panel_band(&p);
        } else {
            band_step(up);
        }
        out += (i ? ", " : "") + where();
    }
    return out;
}

static void on(int32_t band_id) {
    cfg_sm.p_band_id.set(band_id);
}

int main() {
    sqlite3 *db;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    std::ifstream f("sql/params.sql");
    std::string   sql((std::istreambuf_iterator<char>(f)), {});
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    cfg_db_init(db);
    cfg_sm.init_load(nullptr);

    /* A new radio: every band of the list, both ways */
    on(1);
    std::string up = presses(true, true, 16);
    std::cout << "new radio, the Panel's BAND up from 160m: " << up << "; the dial outside the band chosen: " << outside
              << "\n";
    on(12);
    std::cout << "new radio, the Panel's BAND down from 6m: " << presses(true, false, 16)
              << "; the dial outside the band chosen: " << outside << "\n";

    /* Independent VFOs on (Settings > General > STATION), the VFO key on 30m: VFO B comes up on its own band, 160m
     * here, and on a band it has no frequency of its own on yet starts where A has that band (#137). Up to v0.3.8 B
     * took A's frequency there, the 10.100 of 30m where A stayed: up went 80m, 30m, 20m CW, 30m, 20m CW..., down 40m
     * SSB, 40m CW, 40m SSB..., the dial on 10.100 all along (outside the band chosen 24 times of 32), until each band
     * had been tuned with B. The likeliest way to #168; the VFO key back to A, or Independent VFOs off, got all back */
    cfg_sm.p_indep_vfo.set(1);
    on(5);
    cfg_sm.vfo_toggle();
    std::string indep_on   = where();
    std::string indep_up   = presses(true, true, 16);
    int         indep_out  = outside;
    std::string indep_down = presses(true, false, 16);
    indep_out += outside;
    std::cout << "#168 Independent VFOs, the VFO key to B on 30m: B on " << indep_on
              << "; the Panel's BAND up: " << indep_up << "; down: " << indep_down
              << "; the dial outside the band chosen: " << indep_out << "\n";

    /* Back on A with Independent VFOs off, and B's rows gone again: the cases below are on a radio that never used B */
    cfg_sm.vfo_toggle();
    cfg_sm.p_indep_vfo.set(0);
    on(1);
    cfg_sm.flush_all();
    assert(sqlite3_exec(db, "DELETE FROM band_params WHERE name LIKE 'vfob_%'", nullptr, nullptr, nullptr) ==
           SQLITE_OK);

    /* #168: each band but 20 m and 30 m has a frequency saved on 20 m or 30 m (CAT tuning VFO A while B was in use,
     * edges changed under it, an older version). Up to v0.3.8 a key went to that frequency and the next one went on
     * from there: 20 m and 30 m only, until the others were tuned by hand */
    assert(sqlite3_exec(db,
                        "UPDATE band_params SET val = CASE WHEN bands_id % 2 THEN 14200000 ELSE 10120000 END "
                        "WHERE name = 'vfoa_freq' AND bands_id NOT IN (5, 6, 7)",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    on(5);
    std::string panel_up = presses(true, true, 15);
    int         out_up   = outside;
    std::cout << "#168 the Panel's BAND up from 30m: " << panel_up << "; the dial outside the band chosen: " << out_up
              << "\n";
    on(12);
    std::string panel_down = presses(true, false, 15);
    int         out_down   = outside;
    std::cout << "#168 the Panel's BAND down from 6m: " << panel_down << "; the dial outside the band chosen: "
              << out_down << "\n";

    /* The radio's own BAND keys (and the Screen page's, which press them) do the same */
    on(5);
    bool same = presses(false, true, 15) == panel_up && outside == out_up;
    on(12);
    same = presses(false, false, 15) == panel_down && outside == out_down && same;
    std::cout << "#168 the radio's BAND keys as the Panel's: same=" << same << "\n";

    /* A band tuned by hand opens where it was left, and one never left elsewhere where it was saved */
    on(4);
    cfg_sm.cp_fg_freq.set(7100000);
    on(5);
    on(4);
    std::cout << "#168 40m SSB tuned to 7.100, away and back: " << cfg_sm.cp_fg_freq.get() << " on "
              << name_of(cfg_sm.current_band_id()) << "; 20m SSB: ";
    on(7);
    std::cout << cfg_sm.cp_fg_freq.get() << "\n";

    /* One way there: CAT tunes VFO A (the unselected VFO, cat.cpp's 0x25 01) to 14.200 while B is in use on 40m SSB;
     * A is only stored there, and the VFO key back to A follows it to 20m SSB. 40m SSB keeps 14.200 for A */
    on(4);
    cfg_sm.vfo_toggle();
    cfg_sm.cp_bg_freq.set(14200000);
    cfg_sm.vfo_toggle();
    std::cout << "#168 CAT's VFO A on 14.200 while B was in use on 40m SSB, then A: " << where()
              << "; the Panel's BAND down: " << presses(true, false, 3) << " on " << cfg_sm.cp_fg_freq.get()
              << "; the dial outside the band chosen: " << outside << "\n";

    /* VFO B in use: on a band with no B of its own B starts as A, and A's 14.200 on 40m put B on 20m the same way */
    on(5);
    cfg_sm.vfo_toggle();
    std::string b_up = presses(true, true, 15);
    std::cout << "#168 VFO B in use, the Panel's BAND up from 30m: same as on A=" << (b_up == panel_up)
              << "; the dial outside the band chosen: " << outside << "\n";
    cfg_sm.vfo_toggle();

    /* LOCK (or an app holding the band): the Panel's key changes nothing and the pages get the state again */
    on(7);
    band_lock = true;
    pcmd_t p  = {P_BAND, 1, 0};
    panel_band(&p);
    band_lock = false;
    std::cout << "locked: " << name_of(cfg_sm.current_band_id()) << " state again=" << state_resend.load() << "\n";

    /* REV with a TX shift on 10m: VFO B in use transmits 500 kHz up and lies beyond the band, whose dial is A's;
     * away by a band key and back, B is kept, not put on the band's start */
    on(11);
    cfg_sm.cp_fg_freq.set(29500000);
    cfg_sm.tx_shift_set(500000);
    cfg_sm.vfo_toggle();
    on(12);
    on(11);
    std::cout << "REV with a TX shift on 10m, away and back: vfo=" << cfg_sm.p_band_current_vfo.get()
              << " B=" << cfg_sm.p_band_vfob_freq.get() << " A=" << cfg_sm.p_band_vfoa_freq.get()
              << " band=" << name_of(cfg_sm.current_band_id()) << "\n";
}
