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
#define LOG_ANSWERS 16
static struct {uint32_t id; uint8_t result; bool told;} log_ans[LOG_ANSWERS];
static int log_ans_next;
static atomic_bool log_pending;
static pthread_mutex_t app_mux = PTHREAD_MUTEX_INITIALIZER;
typedef struct {int browser;} client_t;
typedef struct {int id; uint32_t log_id; char text[200];} acmd_t;
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
  log_queued(1); log_answer(1,true); /* browser 0's first successful log */
  log_query(&second,1); /* browser 1 asks whether its first log ever arrived */
  for (uint32_t id=2;id<=17;id++) log_answer(id,true);
  log_query(&first,1); /* same GUI run, successfully logged but result evicted */
  app_log_cmd(&first,2,"ON6URE|59|59|First");
  app_log_cmd(&first,2,"ON6URE|59|59|First");
  printf("same logq id submitted twice: writes=%d\n",writes);
  return writes != 2;
}
'''
(p/'log-identities.c').write_text(pre+'\n'+'\n'.join(fn(x) for x in ['static int log_slot(', 'static void log_queued(', 'static void log_answer(', 'static void log_query(', 'static void app_log_cmd('])+'\n'+main)
