#!/bin/sh
# Replaces AetherX6100Buildroot's bt_start.sh (run by udev when the Bluetooth
# adapter appears). At boot S94btcat runs btcat-setup; if the adapter is added
# again later, run btcat-setup again so CAT keeps working. One agent, one SDP
# record, and the radio is left hidden and not pairable.
[ -e /tmp/btcat-setup.done ] || exit 0
/usr/sbin/btcat-setup >/tmp/btcat-setup.log 2>&1 &
