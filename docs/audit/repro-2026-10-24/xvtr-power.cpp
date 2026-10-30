/* Issue #104: a transverter's TX power limit, on the production SettingsManager. Settings from before it (no row: no
 * limit), where the limit counts (the dial, the TX frequency, the lower of two), PWR held to it on every set and
 * following a new one, the PWR from before it back off the transverter (after a restart on it too), the X6200's
 * battery top over that */
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
using X = SettingsManager::XvtrPwr;
static const char *what(X r) { return r == X::HELD ? "held" : r == X::RESTORED ? "restored" : "same"; }
static SettingsManager *s;
static void pwr(const char *at, X r) {
 std::cout<<at<<": "<<what(r)<<" pwr="<<s->p_pwr.get()<<" held="<<s->p_pwr_xvtr_held.get()<<" top="<<s->pwr_top()<<"\n";
}
int main() {
 sqlite3 *db; assert(sqlite3_open(":memory:",&db)==SQLITE_OK);
 std::ifstream f("sql/params.sql"); std::string sql((std::istreambuf_iterator<char>(f)),{});
 assert(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
 /* settings from before #104: transverter 1 set up for 2 m through a 28 MHz IF, transverter 2 for 70 cm, no limits */
 assert(sqlite3_exec(db,"INSERT INTO transverter VALUES (0,'from',144000000),(0,'to',146000000),(0,'shift',116000000),"
                        "(1,'from',432000000),(1,'to',438000000),(1,'shift',404000000)",nullptr,nullptr,nullptr)==SQLITE_OK);
 cfg_db_init(db);
 s=new SettingsManager; s->init_load(nullptr);
 std::cout<<"old settings: limit 1="<<s->p_transverter_0_pwr.get()<<" 2="<<s->p_transverter_1_pwr.get()
          <<", at 145.5 MHz "<<s->transverter_pwr_for(145500000)<<" W\n";
 /* Settings: 0.5 W on 2 m; 0.05 W asked on 70 cm, 12 W asked of 2 m first */
 s->p_transverter_0_pwr.set(12.0f);
 std::cout<<"12 W asked: "<<s->p_transverter_0_pwr.get()<<" W\n";
 s->p_transverter_0_pwr.set(0.5f); s->p_transverter_1_pwr.set(0.05f);
 std::cout<<"limits: 1="<<s->p_transverter_0_pwr.get()<<" 2="<<s->p_transverter_1_pwr.get()<<"\n";
 std::cout<<"cap: 2 m="<<s->transverter_pwr_cap(145500000,145500000)<<" 20 m="<<s->transverter_pwr_cap(14100000,14100000)
          <<" 20 m split to 2 m="<<s->transverter_pwr_cap(14100000,145500000)
          <<" 2 m split to 20 m="<<s->transverter_pwr_cap(145500000,14100000)
          <<" 2 m split to 70 cm="<<s->transverter_pwr_cap(145500000,433500000)
          <<" above the range="<<s->transverter_pwr_cap(147000000,147000000)<<"\n";
 s->p_transverter_0_shift.set(0);
 std::cout<<"transverter 1 off: 2 m="<<s->transverter_pwr_cap(145500000,145500000)<<"\n";
 s->p_transverter_0_shift.set(116000000);
 /* PWR 10 W on HF, onto 2 m */
 s->p_pwr.set(10.0f);
 pwr("onto 2 m", s->pwr_xvtr_set(0.5f));
 s->p_pwr.set(10.0f);
 std::cout<<"10 W set on 2 m (the knob, the Panel, TCI, CAT): pwr="<<s->p_pwr.get()<<"\n";
 s->p_pwr.set(0.2f);
 pwr("turned down to 0.2 W, off 2 m", s->pwr_xvtr_set(0.0f));
 /* under the limit already: left as it is, there and after */
 s->p_pwr.set(0.3f);
 pwr("0.3 W onto 2 m", s->pwr_xvtr_set(0.5f));
 pwr("0.3 W off 2 m", s->pwr_xvtr_set(0.0f));
 /* two limits one after the other: PWR at each, the PWR from before the first comes back; one turned down stays */
 s->p_transverter_1_pwr.set(1.0f);
 s->p_pwr.set(10.0f);
 pwr("onto 70 cm at 1 W", s->pwr_xvtr_set(1.0f));
 pwr("onto 2 m at 0.5 W", s->pwr_xvtr_set(0.5f));
 pwr("back on 70 cm", s->pwr_xvtr_set(1.0f));
 s->p_pwr.set(0.3f);
 pwr("turned down to 0.3 W, onto 2 m", s->pwr_xvtr_set(0.5f));
 pwr("off both", s->pwr_xvtr_set(0.0f));
 /* the limit stepped up in Settings on its band, from Off: PWR follows it up to the PWR from before, 0.8 W */
 s->p_pwr.set(0.8f);
 pwr("Settings, Off to 0.1 W", s->pwr_xvtr_set(0.1f));
 pwr("0.2 W", s->pwr_xvtr_set(0.2f));
 pwr("0.5 W", s->pwr_xvtr_set(0.5f));
 pwr("1.0 W", s->pwr_xvtr_set(1.0f));
 pwr("down to 0.4 W", s->pwr_xvtr_set(0.4f));
 pwr("Off", s->pwr_xvtr_set(0.0f));
 /* a restart on 2 m: the PWR from before it is in the settings */
 s->p_pwr.set(10.0f);
 pwr("onto 2 m before a restart", s->pwr_xvtr_set(0.5f));
 s->flush_all(); delete s; s=new SettingsManager; s->init_load(nullptr);
 std::cout<<"restart: limit 1="<<s->p_transverter_0_pwr.get()<<" 2="<<s->p_transverter_1_pwr.get()
          <<" pwr="<<s->p_pwr.get()<<" held="<<s->p_pwr_xvtr_held.get()<<" row pwr_max="
          <<TransverterTable::Load<int32_t>(0,"pwr_max").value<<"\n";
 pwr("restart, on 2 m", s->pwr_xvtr_set(0.5f));
 pwr("restart, off 2 m", s->pwr_xvtr_set(0.0f));
 /* the X6200 on its battery meanwhile: back at its top, not above */
 s->p_pwr.set(8.0f);
 pwr("8 W onto 2 m", s->pwr_xvtr_set(0.5f));
 s->pwr_max_.store(5.0f);
 pwr("on battery, off 2 m", s->pwr_xvtr_set(0.0f));
}
