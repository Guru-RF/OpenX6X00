/* Included first in src/winlink.c's host build (-include): its socket path and rigctld port from the environment
 * (WL_SOCK, WL_RIG_PORT), so two of it run side by side. The radio file beside the copied sources, a literal
 * (winlink.c takes its size): two stations write the same one, nobody reads it here */
#pragma once
const char *stub_sock(void);
int         stub_rig_port(void);
#define WINLINK_ARDOP_SOCK stub_sock()
#define WINLINK_RIG_PORT   stub_rig_port()
#define WINLINK_RADIO_FILE "build/radio"
