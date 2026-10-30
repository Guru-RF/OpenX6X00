#include "src/cfg/settings_manager.h"
#include "src/scheduler.h"
#include <cassert>
#include <cstdarg>
#include <fstream>
#include <iterator>
#include <iostream>
extern "C" void _lv_log_add(lv_log_level_t, const char *, int, const char *, const char *, ...) {}
extern "C" lv_res_t lv_async_call(lv_async_cb_t,void *) { return LV_RES_OK; }
extern "C" lv_res_t lv_async_call_cancel(lv_async_cb_t,void *) { return LV_RES_OK; }
extern "C" bool scheduler_on_gui_thread() { return true; }
int main() {
 sqlite3 *db; assert(sqlite3_open(":memory:",&db)==SQLITE_OK);
 std::ifstream f("sql/params.sql"); std::string sql((std::istreambuf_iterator<char>(f)),{});
 assert(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
 cfg_db_init(db);
 auto *s=new SettingsManager; s->init_load(nullptr);
 std::cout<<"before: band="<<s->p_band_id.get()<<" A="<<s->p_band_vfoa_freq.get()<<" B="<<s->p_band_vfob_freq.get()<<" mode="<<s->cp_cur_mode.get()<<"\n";
 s->cp_bg_freq.set(7100000);
 std::cout<<"after B=7100000: band="<<s->p_band_id.get()<<" A="<<s->p_band_vfoa_freq.get()<<" B="<<s->p_band_vfob_freq.get()<<" mode="<<s->cp_cur_mode.get()<<"\n";
 s->flush_all();
 auto v=BandParamsTable::Load<int32_t>(7,"vfob_freq");   /* fixed (audit B11): stored with the band that stays */
 std::cout<<"persisted B="<<v.value<<" rc="<<v.rc<<"\n";
 s->vfo_toggle();
 std::cout<<"after toggle: band="<<s->p_band_id.get()<<" A="<<s->p_band_vfoa_freq.get()<<" B="<<s->p_band_vfob_freq.get()<<" vfo="<<s->p_band_current_vfo.get()<<"\n";
 /* independent VFOs: B, active on 40m, tuned there; a band key on A moves A only; the VFO key brings B back as it was */
 s->p_indep_vfo.set(1);
 s->cp_fg_freq.set(7030000);
 s->vfo_toggle();
 std::cout<<"indep, on A: band="<<s->p_band_id.get()<<" A="<<s->p_band_vfoa_freq.get()<<" B="<<s->p_band_vfob_freq.get()<<"\n";
 s->p_band_id.set(9);
 std::cout<<"indep, band key: band="<<s->p_band_id.get()<<" A="<<s->p_band_vfoa_freq.get()<<" B="<<s->p_band_vfob_freq.get()<<"\n";
 s->vfo_toggle();
 std::cout<<"indep, VFO key: band="<<s->p_band_id.get()<<" A="<<s->p_band_vfoa_freq.get()<<" B="<<s->p_band_vfob_freq.get()<<" vfo="<<s->p_band_current_vfo.get()<<"\n";
 /* a restart: back on B, the VFO in use (40m had A saved, by the VFO key that left it) */
 s->flush_all(); delete s; s=new SettingsManager; s->init_load(nullptr);
 std::cout<<"indep, restart: band="<<s->p_band_id.get()<<" vfo="<<s->p_band_current_vfo.get()<<" B="<<s->p_band_vfob_freq.get()<<"\n";
 /* independent VFOs, a band key into split: B from 15m (left there in split, B on 21.010), the VFO it transmits on, not B's 7.030 kept */
 s->vfo_toggle();
 s->p_band_id.set(3);
 assert(sqlite3_exec(db,"INSERT INTO band_params VALUES (9,'split',1),(9,'vfob_freq',21010000)",nullptr,nullptr,nullptr)==SQLITE_OK);
 s->p_band_id.set(9);
 std::cout<<"indep, band key into split: band="<<s->p_band_id.get()<<" A="<<s->p_band_vfoa_freq.get()<<" B="<<s->p_band_vfob_freq.get()<<" split="<<s->p_band_split.get()<<"\n";
}
