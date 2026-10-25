/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Bluetooth headphones: btaudio-ctl runner and "Mute speaker".
 */

#include "btaudio.h"
#include "board.h"

#include "cfg/settings_manager.h"
#include "cfg/subject_api.h"

#include <string>
#include <thread>

extern "C" {
#include "scheduler.h"
#include "radio.h"
#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
}

#define BTAUDIO_CTL   "/usr/sbin/btaudio-ctl"
#define BTAUDIO_CONF  BOARD_DATA "/btaudio.conf"
#define HP_STATE      "/tmp/btaudio-hp"     /* written by btaudio-ctl: headphones connected */
#define HP_VOL_FILE   BOARD_DATA "/btaudio.hpvol"  /* last headphone volume set with the VOL control */
#define SAVED_AF_VOL  BOARD_DATA "/btaudio.afvol"  /* AF volume before muting, survives a power off */

struct result_t {
    std::string       out;
    int               status;
    btaudio_done_cb_t cb;
    void             *user;
};

static bool mute_on   = true;   /* speaker silent while headphones are connected (default) */
static int  saved_vol = -1;
static bool hp_on     = false;
static int  hp_vol    = 30;     /* headphone volume on the VOL scale, 0..BOARD_VOL_MAX */
static bool vol_busy  = false;
static int  vol_pending = -1;

static void done(void *arg) {
    result_t *r = *(result_t **)arg;
    if (r->cb) r->cb(r->out.c_str(), r->status, r->user);
    delete r;
}

void btaudio_run(const char *args, btaudio_done_cb_t cb, void *user) {
    sh_run((std::string(BTAUDIO_CTL " ") + args).c_str(), cb, user);
}

/* Any command in a thread, its output and status to cb on the GUI thread */
void sh_run(const char *command, btaudio_done_cb_t cb, void *user) {
    std::string cmd = std::string(command) + " 2>&1";
    std::thread([cmd, cb, user]() {
        result_t *r = new result_t{"", -1, cb, user};
        FILE *fp = popen(cmd.c_str(), "r");
        if (fp) {
            char buf[256];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) r->out.append(buf, n);
            r->status = pclose(fp);
        }
        scheduler_put(done, &r, sizeof(r));
    }).detach();
}

static void save_vol(int vol) {
    saved_vol = vol;
    if (vol < 0) {
        unlink(SAVED_AF_VOL);
        return;
    }
    FILE *fp = fopen(SAVED_AF_VOL, "w");
    if (fp) {
        fprintf(fp, "%d\n", vol);
        fclose(fp);
    }
}

static void hp_vol_send();

static void hp_vol_done(const char *out, int status, void *user) {
    vol_busy = false;
    hp_vol_send();
}

/* One volume command at a time, the last value wins */
static void hp_vol_send() {
    if (vol_busy || vol_pending < 0) return;
    std::string args = "volume " + std::to_string(vol_pending * 100 / BOARD_VOL_MAX);

    vol_pending = -1;
    vol_busy = true;
    btaudio_run(args.c_str(), hp_vol_done, NULL);
}

/* While headphones are connected the VOL control is the headphone volume */
static void vol_changed_cb(Subject *subj, void *user) {
    if (!hp_on) return;
    hp_vol = cfg_sm.p_volume.get();
    vol_pending = hp_vol;
    hp_vol_send();
    FILE *fp = fopen(HP_VOL_FILE, "w");
    if (fp) {
        fprintf(fp, "%d\n", hp_vol);
        fclose(fp);
    }
}

static void mute_timer_cb(lv_timer_t *t) {
    bool hp = access(HP_STATE, F_OK) == 0;

    if (hp && saved_vol < 0) {
        /* connected: VOL becomes the headphone volume, speaker AF is forced to 0 by radio.c */
        hp_on = true;
        save_vol(cfg_sm.p_volume.get());
        cfg_sm.p_volume.set(hp_vol);
        vol_changed_cb(NULL, NULL);
        radio_vol_refresh();
    } else if (!hp && saved_vol >= 0) {
        hp_on = false;
        cfg_sm.p_volume.set(saved_vol);
        save_vol(-1);
        radio_vol_refresh();
    }
}

void btaudio_init(void) {
    FILE *fp = fopen(BTAUDIO_CONF, "r");
    if (fp) {
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "BTAUDIO_MUTE=", 13) == 0) mute_on = strstr(line, "yes") != NULL;
        }
        fclose(fp);
    }
    fp = fopen(SAVED_AF_VOL, "r");
    if (fp) {
        if (fscanf(fp, "%d", &saved_vol) != 1) saved_vol = -1;
        fclose(fp);
    }
    fp = fopen(HP_VOL_FILE, "r");
    if (fp) {
        if (fscanf(fp, "%d", &hp_vol) != 1) hp_vol = 30;
        fclose(fp);
    }
    hp_on = saved_vol >= 0;
    if (hp_on) radio_vol_refresh();    /* the speaker muted again: the radio set its volume before this */
    subject_subscribe((Subject *)&cfg_sm.p_volume, vol_changed_cb, NULL);
    lv_timer_create(mute_timer_cb, 1000, NULL);
}

bool btaudio_mute_get(void) {
    return mute_on;
}

void btaudio_mute_set(bool on) {
    mute_on = on;
    btaudio_run(on ? "mute on" : "mute off", NULL, NULL);
    radio_vol_refresh();
}

bool btaudio_speaker_muted(void) {
    return hp_on && mute_on;
}

/* The headphone sink (first line btaudio-ctl writes to HP_STATE) while
 * the speaker is silenced for them; false otherwise. Any thread. */
bool btaudio_hp_sink(char *buf, size_t size) {
    if (!btaudio_speaker_muted()) return false;
    FILE *fp = fopen(HP_STATE, "r");
    if (!fp) return false;
    bool ok = fgets(buf, size, fp) != NULL;
    fclose(fp);
    if (!ok) return false;
    buf[strcspn(buf, "\n")] = '\0';
    return buf[0] != '\0';
}
