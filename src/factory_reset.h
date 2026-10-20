/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - factory reset (Settings > Info)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include <stdio.h>
#include <unistd.h>

/*
 *  The files on /mnt that a factory reset deletes: plain configuration, rebuilt with the defaults
 *  when the GUI starts again. Everything else on /mnt stays, the user's data among it:
 *  logs (ft_log.adi, js8call_log.adi, incoming_log.adi, qso_log.db, field/, app_logs/, tx_logs/),
 *  memories (channels.json, walkie_channels.json), macros and texts (psk_macros.txt,
 *  olivia_macros.txt, js8_*.txt, msg/), pictures and recordings (sstv/, rec/, screenrec/),
 *  themes/, cw_trainer.json and snake_best (progress), the Bluetooth autoconnect list
 *  (btaudio.conf; the pairings live in /var/lib/bluetooth), https/ and device_id.
 */
static const char *const factory_reset_files[] = {
    "params.db", "params.db-wal", "params.db-shm", "params.db-journal",  /* all GUI settings */
    "app_freqs",            /* last frequency of each app */
    "cluster.conf",         /* DX cluster server and filters */
    "btcat.conf",           /* Bluetooth CAT on/off */
    "btaudio.hpvol",        /* headphone volume */
    "btaudio.afvol",        /* AF volume saved while headphones mute the speaker */
    "ft8_udp.conf",         /* FT8 UDP to a logger */
    "psk.conf",             /* PSK squelch */
    "tci.conf",             /* TCI server */
    "tz.conf",              /* time zone */
    "ft8lib_min_score.txt", /* FT8 decoder threshold */
};

/* Deletes them from dir, returns how many were there */
static inline int factory_reset_dir(const char *dir) {
    char path[256];
    int  n = 0;

    for (size_t i = 0; i < sizeof(factory_reset_files) / sizeof(factory_reset_files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, factory_reset_files[i]);
        if (unlink(path) == 0) n++;
    }
    sync();
    return n;
}
