# Additions to the buildroot tree

Copied over AetherX6100Buildroot's `br2_external` by `local-build.sh` and the workflow, which
also switch them on in the defconfig and the kernel configuration:

- `package/rtl8723bu-wifi`: lwfinger's out-of-tree Realtek driver instead of the kernel's
  rtl8xxxu, with four patches (station only without power saving, the antenna kept on WiFi
  during the WPA handshake, firmware from a file, 802.11w protected management frames) and
  `etc/modprobe.d/x6100-wifi.conf` (20 MHz, channel plan with 12 and 13).
- `etc/init.d/S00adata` and `etc/fstab`: the DATA partition (FAT, settings and logs) is
  checked and repaired at every start before anything mounts it; it replaces S01create_data.
- `etc/NetworkManager/system-connections/usb-tether.nmconnection` and `device_table.txt`:
  Android USB tethering, never the default route; the kernel gets RNDIS and policy routing.
- `etc/bluetooth/main.conf`: no pairing timeouts of its own, the GUI's Bluetooth window sets
  the pairing window. `etc/ntp.conf`: a late GPS may step the clock by any amount.
  `etc/pulse/daemon.conf`: mono, so Bluetooth headphones get SBC mono and skip less.
  `S50pulseaudio` waits for the old daemon on restart; `S50gpsd` keeps running with no GPS
  attached and takes one plugged in later.

These come from the published GPL sources of the X6100 firmware this project was forked from
(v0.129, buildroot tree and patches, GPL-2.0), taken as they are.
