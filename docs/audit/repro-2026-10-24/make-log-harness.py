import os
from pathlib import Path
p = Path(os.environ['AUDIT_TMP'])
s = Path('src/remote_server.c').read_text()
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
pre = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#define LOG_ANSWERS 64
static struct {uint64_t who; uint64_t id; uint64_t sum; uint8_t result; bool told;} log_ans[LOG_ANSWERS];
static int log_ans_next;
static atomic_bool log_pending;
static pthread_mutex_t app_mux = PTHREAD_MUTEX_INITIALIZER;
typedef struct {int browser;} client_t;
typedef struct {int id; uint64_t log_id; char text[200]; uint64_t who; uint64_t sum;} acmd_t;
static uint64_t sess_key(const client_t *c) {return 1000 + c->browser;}   /* each browser its own session */
enum {A_LOG};
static int writes;
static void client_text(client_t*c,const char*s) {printf("browser %d: %s\n",c->browser,s);}
static void client_textf(client_t*c,const char*f,...) {va_list ap;va_start(ap,f);printf("browser %d: ",c->browser);vprintf(f,ap);puts("");va_end(ap);}
static void app_apply(void *arg) {(void)arg;writes++;}
static bool scheduler_put(void (*fn)(void*),void*arg,size_t n) {(void)n;fn(arg);return true;}
'''
main=r'''
int main(void) {
  client_t first={.browser=0}, second={.browser=1};
  app_log_cmd(&first,1,"ON6URE|59|59|First"); log_answer(sess_key(&first),1,log_sum("ON6URE|59|59|First"),true); /* browser 0's first log, written */
  log_query(&first,1,NULL);  /* its own receipt: ok */
  log_query(&second,1,NULL); /* browser 1 asks about ITS first log, never sent: unknown, not browser 0's ok (fixed, audit B07) */
  for (uint32_t id=2;id<=70;id++) log_answer(sess_key(&first),id,0,true);
  log_query(&first,1,NULL);  /* too old to be kept: unknown, the operator decides */
  app_log_cmd(&first,2,"ON6URE|59|59|Second");
  app_log_cmd(&first,2,"ON6URE|59|59|Second");   /* the same number again: answered again, not written again */
  printf("same logq id submitted twice: writes=%d\n",writes);
  return writes != 2;   /* the first command and the second QSO: one write each */
}
'''
(p/'log-identities.c').write_text(pre+'\n'+'\n'.join(fn(x) for x in ['static uint64_t log_sum(', 'static int log_slot(', 'static void log_queued(', 'static void log_answer(', 'static void log_query(', 'static void app_log_cmd('])+'\n'+main)
