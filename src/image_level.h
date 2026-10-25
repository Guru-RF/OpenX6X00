/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The card image against what the running radio software wants: the in-radio update replaces the GUI, the
 *  web server and their files, not the kernel, its modules (the WiFi driver) or U-Boot. Each version lists the
 *  image level it wants per radio (/usr/share/x6100/image-required); the image build stamps its own level into
 *  /etc/openx6x00-image (none before v0.3.4: level 1). x6100-update does the same comparison for a release
 *  before it is installed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when a newer card image than the running one is wanted; *note then says what it brings */
bool image_newer_wanted(char *note, size_t size);

/* A word on the screen once per version when it is, 20 s after start */
void image_level_notice_init(void);

#ifdef __cplusplus
}
#endif
