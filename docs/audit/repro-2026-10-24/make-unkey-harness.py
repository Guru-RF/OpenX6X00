import os
from pathlib import Path
p = Path(os.environ['AUDIT_TMP'])
s = Path('src/tci_server.c').read_text()
def fn(name):
    start = s.index(name)
    start = s.rfind('\n', 0, start) + 1
    brace = s.index('{', start)
    level = 1
    end = brace + 1
    while level:
        level += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
(p/'radio.h').write_text('#pragma once\n#include <stdbool.h>\ntypedef enum { RADIO_RX, RADIO_TX } radio_state_t;\nradio_state_t radio_get_state(void);\n')
(p/'control_owner.h').write_text(Path('src/control_owner.h').read_text())
(p/'control_owner.c').write_text(Path('src/control_owner.c').read_text())
pre = r'''
#define _GNU_SOURCE
#include "control_owner.h"
#include "radio.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdarg.h>
#define LV_CLAMP(lo,x,hi) ((x)<(lo)?(lo):(x)>(hi)?(hi):(x))
#define AUDIO_RATE 24000
static bool keyed;
radio_state_t radio_get_state(void) {return keyed ? RADIO_TX : RADIO_RX;}
typedef struct {pthread_mutex_t lock; bool rx_sensors,tx_sensors,audio_float,audio_on; uint32_t audio_rate;} client_t;
static client_t clients[4];
typedef struct {int32_t freq,mode,flo,fhi,agc,rit; int drive; bool tx;} rstate_t;
static atomic_bool trust_operate = true;
static struct {int owner; uint64_t start_ms,last_audio_ms,chrono_ms; uint32_t rate;} txs;
static int ptt_owner;
static uint64_t ptt_ms;
static int ends, last_command = -1;
enum {C_FREQ,C_MODE,C_FILTER,C_AGC,C_RIT,C_DRIVE,C_KEY,C_UNKEY};
static uint64_t now_ms(void) {return 0;}
static void state_read(rstate_t *s) {*s=(rstate_t){.tx=keyed};}
static void client_text(client_t *c,const char *s) {(void)c;puts(s);}
static void client_textf(client_t *c,const char *fmt,...) {(void)c;va_list ap;va_start(ap,fmt);vprintf(fmt,ap);puts("");va_end(ap);}
static void filter_line(char*b,size_t n,const rstate_t*s) {(void)s;snprintf(b,n,"filter");}
static const char *mode_name(int n) {(void)n;return "usb";}
static const char *agc_name(int n) {(void)n;return "fast";}
static int mode_index(const char *s) {(void)s;return 0;}
static int agc_index(const char *s) {(void)s;return 0;}
static void put_cmd(int id,int a,int b,bool tci) {(void)a;(void)b;(void)tci;last_command=id;}
static void tx_session_end(int ci,bool drain) {(void)ci;(void)drain;ends++;keyed=false;}
static void cap_update(void) {}
'''
main = r'''
int main(void) {
  ctl_init(); txs.owner=-1; ptt_owner=0;
  ctl_claim(CTL_TCI,0,"TCI",true); keyed=true;
  ctl_local_input();
  char stop1[]="trx:0,false";
  handle_cmd(0,stop1);
  printf("after local input and explicit unkey: keyed=%d, tx_session_end_calls=%d, queued_command=%d\n",keyed,ends,last_command);
  ctl_init(); ctl_claim(CTL_TCI,0,"TCI",true); keyed=true;
  atomic_store(&trust_operate,false);
  char stop2[]="trx:0,false";
  handle_cmd(0,stop2);
  printf("after listen-only and explicit unkey: keyed=%d, tx_session_end_calls=%d, queued_command=%d\n",keyed,ends,last_command);
  return !(!keyed && ends==2 && last_command==-1);   /* fixed (audit B01): both unkeys end the session */
}
'''
(p/'unkey-ownership.c').write_text(pre+'\n'+fn('static bool parse_bool(')+'\n'+fn('static int split_args(')+'\n'+fn('static void handle_cmd(')+'\n'+main)
