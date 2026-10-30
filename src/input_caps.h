/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - What an input device has, from sysfs (input_caps.h)
 *
 *  The keys, axes and ids of a /dev/input/eventN as EVIOCGBIT and EVIOCGID give them, without
 *  opening it. Opening one starts it: a USB HID device then waits 50 ms in the kernel (usbhid_open),
 *  and the keyboard and paddle scans look at every device, on the GUI's thread.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <linux/input.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bits of one event type ("key", "rel"...) of the input device whose node is dev (its st_rdev),
 * into n longs as EVIOCGBIT lays them out; false, all clear, when it is not an input device */
bool input_caps_bits(dev_t dev, const char *what, unsigned long *bits, size_t n);

/* Its bus, vendor, product and version, as EVIOCGID gives them; false, all 0, when it is not one */
bool input_caps_id(dev_t dev, struct input_id *id);

#ifdef __cplusplus
}
#endif
