/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - factory reset (Settings > Info)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 *  The files on /mnt that a factory reset deletes: plain configuration, rebuilt with the defaults
 *  when the GUI starts again. Everything else on /mnt stays, the user's data among it:
 *  logs (ft_log.adi, js8call_log.adi, incoming_log.adi, qso_log.db, field/, app_logs/, tx_logs/),
 *  memories (channels.json, walkie_channels.json), macros and texts (psk_macros.txt,
 *  olivia_macros.txt, jtty_macros.txt, js8_*.txt, digi_text/, msg/), pictures and recordings (sstv/,
 *  rec/, screenrec/), themes/, cw_trainer.json and snake_best (progress), the Bluetooth autoconnect
 *  list (btaudio.conf; the pairings live in /var/lib/bluetooth), the paired browsers
 *  (remote_sessions.conf), what was measured or downloaded (birdies.txt, battery_model.json,
 *  broadcast.csv, conditions.txt), the own TX list (tx_ranges.conf: the choice of list is a setting,
 *  reset), and the network and the accounts: the radio's name (hostname.conf), the Network page's
 *  own names (own_names.conf), Tailscale (tailscale.conf, tailscale/), Winlink (winlink.conf,
 *  winlink/), the upload accounts (field/), https/ and device_id. The WiFi networks and the SSH keys
 *  are not on /mnt but on the system partition (S04restore_backup_config keeps their copy in
 *  backups/, for a rewritten card): a reset leaves them, unless its WiFi choice deletes the WiFi
 *  networks too (factory_reset_wifi below).
 *  A card image written over the card leaves all of /mnt as it was (S00adata keeps DATA).
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
    "tci_allowed.conf",     /* the TCI clients allowed to transmit: each asks again (#257) */
    "remote.conf",          /* the browser remote: on, at its port 50002 (#257) */
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

/* The saved WiFi networks deleted, with their copy on DATA (the panel's WiFi choice, #257): x6100-wifi-forget, waited
 * for. Never on the PC build, whose helpers come from the tree: this PC's WiFi networks are not the radio's. False:
 * not run, or it failed (the reset goes on all the same) */
static inline bool factory_reset_wifi(void) {
#ifdef HOST_BUILD
    return false;
#else
    int r = system("/usr/sbin/x6100-wifi-forget > /dev/null 2>&1");

    return r != -1 && WIFEXITED(r) && WEXITSTATUS(r) == 0;
#endif
}
