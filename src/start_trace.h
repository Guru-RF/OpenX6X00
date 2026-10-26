/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the GUI's start, stage by stage, on the card: app_logs/start_trace.txt, a line
 *  for each stage, written through at once. The run's log is in /tmp until the GUI exits or the
 *  POWER key switches off (x6100_daemon, radio.c save_log): a start that stops before the screen
 *  comes up (#17: at the splash) left nothing on the card to say where it stopped. With DATA not
 *  mounted, or read-only, the lines go to the system partition's /var/lib/x6100/start_trace.txt.
 *
 *  x6100_daemon writes its own line into the same file before each start of the GUI and when the
 *  GUI ends (rootfs/usr/sbin/x6100-starttrace, same format): a start that never got to the GUI, and
 *  a GUI that died before its first line, are told apart.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* One line, printf style, after the UTC time and the time since the kernel started; any thread.
 * Nothing with X6100_START_TRACE=off in the environment (x6100_daemon, after 10 starts in a row) */
void start_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
