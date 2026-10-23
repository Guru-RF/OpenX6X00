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
 auto v=BandParamsTable::Load<int32_t>(4,"vfob_freq");
 std::cout<<"persisted B="<<v.value<<" rc="<<v.rc<<"\n";
 s->p_volume.set(42);
 assert(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK);
 s->flush_all();
 std::cout<<"before rollback volume="<<ParamsTable::Load<int32_t>("vol").value<<"\n";
 assert(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK);
 s->flush_all();
 std::cout<<"after rollback and retry volume="<<ParamsTable::Load<int32_t>("vol").value<<" memory="<<s->p_volume.get()<<"\n";
}
