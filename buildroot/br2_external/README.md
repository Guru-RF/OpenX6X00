# Additions to the buildroot tree

Copied over AetherX6100Buildroot's `br2_external` by `local-build.sh` and the workflow, which
also switch them on in the defconfig and the kernel configuration:

- `package/rtl8723bu-wifi`: lwfinger's out-of-tree Realtek driver instead of the kernel's
  rtl8xxxu, with five patches (station only without power saving, the antenna kept on WiFi
  during the WPA handshake, firmware from a file, 802.11w protected management frames, the HT
  Capabilities element at its 26 octets with the received elements' copies bounded) and
  `etc/modprobe.d/x6100-wifi.conf` (20 MHz, channel plan with 12 and 13). The first four come
  from the v0.129 sources, the fifth from #29 (MediaTek access points never answered the
  25-octet element).
- `etc/init.d/S00adata` and `etc/fstab`: the DATA partition (FAT, settings and logs) is
  checked and repaired at every start before anything mounts it; it replaces S01create_data.
  On a card without one it makes the partition in the free space, and only formats the
  partition it just made; a third partition of another kind is left alone.
- `etc/init.d/S04restore_backup_config`: the WiFi profiles and SSH keys kept on the DATA
  partition come back at boot as before, but only those files, regular files under their own
  folders; the upstream script unpacked the whole archive over `/`. The legacy updater
  `S02update` (any `x6100_r1cbu_*.tgz` on the DATA partition unpacked over `/` at boot,
  unsigned) is removed by the build: OpenX6X00 is installed as a card image.
- `etc/ssh/sshd_config` is no longer here: it is in `rootfs/etc/ssh` (keys only), so the
  update bundle carries it, and `local-build.sh` removes the upstream overlay's copy (root
  logged in with the image's password).
- `etc/NetworkManager/system-connections/usb-tether.nmconnection` and `device_table.txt`:
  Android USB tethering, never the default route; the kernel gets RNDIS and policy routing.
- `etc/bluetooth/main.conf`: no pairing timeouts of its own, the GUI's Bluetooth window sets
  the pairing window. `etc/ntp.conf`: a late GPS may step the clock by any amount.
  `etc/pulse/daemon.conf`: mono, so Bluetooth headphones get SBC mono and skip less.
  `S50pulseaudio` waits for the old daemon on restart; `S50gpsd` keeps running with no GPS
  attached and takes one plugged in later.

These come from the published GPL sources of the X6100 firmware this project was forked from
(v0.129, buildroot tree and patches, GPL-2.0), taken as they are.
