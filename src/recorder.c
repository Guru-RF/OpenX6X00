/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/time.h>
#include <sndfile.h>

#include "audio.h"
#include "dialog_recorder.h"
#include "recorder.h"
#include "msg.h"
#include "params/params.h"
#include "scheduler.h"

char            *recorder_path = "/mnt/rec";

/* The file is written from the audio thread and opened and closed from the GUI (or the player's)
 * thread: the lock keeps a close out of a write in progress (audit A07) */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool     on   = false;
static SNDFILE        *file = NULL;      /* under lock */
static bool            failed;          /* under lock: the card refused a write, said once */

static bool create_file() {
    SF_INFO sfinfo;

    memset(&sfinfo, 0, sizeof(sfinfo));

    sfinfo.samplerate = AUDIO_CAPTURE_RATE;
    sfinfo.channels = 1;
    sfinfo.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;

    char        filename[64];
    time_t      now = time(NULL);
    struct tm   *t = localtime(&now);

    snprintf(filename, sizeof(filename),
        "%s/REC_%04i%02i%02i_%02i%02i%02i.mp3",
        recorder_path, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec
    );

    SNDFILE *f = sf_open(filename, SFM_WRITE, &sfinfo);

    if (f == NULL) {
        const char* err = sf_strerror(NULL);
        LV_LOG_ERROR("Problem with create file: %s", err);
        return false;
    }

    double q = 0.25;
    sf_command(f, SFC_SET_VBR_ENCODING_QUALITY, &q, sizeof(q));

    pthread_mutex_lock(&lock);
    file   = f;
    failed = false;
    pthread_mutex_unlock(&lock);
    return true;
}

/* GUI thread, from the audio thread's failure */
static void write_failed(void *arg) {
    msg_update_text_fmt("RX recorder: the microSD could not be written");
    recorder_set_on(false);
}

void recorder_set_on(bool x) {
    if (x == atomic_load(&on))
        return;                         /* on already, or off already: nothing to do twice */
    if (x) {
        if (!create_file()) {
            msg_update_text_fmt("Problem with create file");
            return;
        } else {
            msg_update_text_fmt("RX recorder is on");
        }
        atomic_store(&on, true);
    } else {
        msg_update_text_fmt("RX recorder is off");
        atomic_store(&on, false);
        pthread_mutex_lock(&lock);
        if (file)
            sf_close(file);             /* no write is in progress: the audio thread holds the lock for its */
        file = NULL;
        pthread_mutex_unlock(&lock);
    }

    dialog_recorder_set_on(x);
}

bool recorder_is_on() {
    return atomic_load(&on);
}

/* Audio thread */
void recorder_put_audio_samples(size_t nsamples, int16_t *samples) {
    pthread_mutex_lock(&lock);
    if (file && !failed) {
        if (sf_write_short(file, samples, nsamples) != (sf_count_t)nsamples) {
            /* a full or failing card: said once, and the recording stopped on the GUI thread */
            failed = true;
            LV_LOG_ERROR("Recorder: write failed: %s", sf_strerror(file));
            scheduler_put_noargs(write_failed);
        }
    }
    pthread_mutex_unlock(&lock);
}
