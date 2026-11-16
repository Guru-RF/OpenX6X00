/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GhostNet: the translation's worker (see ghostnet.h). One thread, started with the first request, takes
 *  the requests in turn and runs rootfs/usr/sbin/x6100-translate for each: its argv, never a shell, so a station's
 *  text can't be read as a command. The helper's one line comes back through the scheduler to the LVGL thread.
 *
 *  After the translation of the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ghostnet.h"

#include "i18n.h"
#include "scheduler.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

#define QUEUE 16                 /* requests waiting; an arriving message past them is not translated */

extern char **environ;

typedef struct {
    ghostnet_tr_kind_t kind;
    char               lang[16];
    char               text[GHOSTNET_TR_TEXT + 1];
    uint32_t           id;
    int32_t            utc;
    ghostnet_tr_cb     done;
} job_t;

typedef struct {
    ghostnet_tr_cb       done;
    uint32_t             id;
    int32_t              utc;
    ghostnet_tr_result_t result;
    char                 text[GHOSTNET_TR_TEXT + 1];
} answer_t;

static const char     *helper = X6100_SBIN "/x6100-translate";
static pthread_mutex_t lock   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wake   = PTHREAD_COND_INITIALIZER;
static job_t           queue[QUEUE];
static int             q_head, q_count;
static bool            started;

static void deliver(void *arg) {
    answer_t *a = arg;
    a->done(a->id, a->utc, a->result, a->text);
}

/* The helper for one job: its line of output, or GHOSTNET_TR_BAD when it could not run */
static void run(const job_t *j, answer_t *a) {
    int                        out[2];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    sigset_t                   none, dfl;
    char                       line[GHOSTNET_TR_TEXT * 2 + 32];
    size_t                     got = 0;
    pid_t                      pid;
    char *argv[] = {(char *)helper, (char *)ghostnet_tr_verb(j->kind), (char *)j->lang, (char *)j->text, NULL};

    a->result  = GHOSTNET_TR_BAD;
    a->text[0] = '\0';
    if (pipe2(out, O_CLOEXEC) != 0) return;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGINT);
    sigaddset(&dfl, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dfl);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    int r = posix_spawn(&pid, helper, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(out[1]);
    if (r != 0) {
        close(out[0]);
        return;
    }
    /* The helper gives up on MyMemory after 15 s itself: read to its end */
    for (;;) {
        ssize_t n = read(out[0], line + got, sizeof(line) - 1 - got);
        if (n > 0) {
            got += (size_t)n;
            if (got == sizeof(line) - 1) break;
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
    line[got] = '\0';
    close(out[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    a->result = ghostnet_tr_parse(line, a->text, sizeof(a->text));
}

static void *worker(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "translate");
    for (;;) {
        job_t    j;
        answer_t a;

        pthread_mutex_lock(&lock);
        while (!q_count) pthread_cond_wait(&wake, &lock);
        j      = queue[q_head];
        q_head = (q_head + 1) % QUEUE;
        q_count--;
        pthread_mutex_unlock(&lock);

        run(&j, &a);
        a.done = j.done;
        a.id   = j.id;
        a.utc  = j.utc;
        scheduler_put(deliver, &a, sizeof(a));
    }
    return NULL;
}

bool ghostnet_translate(ghostnet_tr_kind_t kind, const char *text, uint32_t id, int32_t utc, ghostnet_tr_cb done) {
    bool ok = false;

    pthread_mutex_lock(&lock);
    if (!started) {
        pthread_t      t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&t, &a, worker, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    if (started && q_count < QUEUE) {
        job_t *j = &queue[(q_head + q_count) % QUEUE];
        j->kind = kind;
        ghostnet_tr_lang(i18n_code(), j->lang, sizeof(j->lang));
        snprintf(j->text, sizeof(j->text), "%s", text);
        j->id   = id;
        j->utc  = utc;
        j->done = done;
        q_count++;
        pthread_cond_signal(&wake);
        ok = true;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
