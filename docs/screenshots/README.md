Screenshots of this firmware, from the radio.

- `tools/screenshots/drive.py <radio IP> <script>` runs a script of key presses, knob turns and
  shots on the radio over SSH (`x6drive.py` is the half that runs there) and writes the shots
  here as landscape PNGs; `grab.py` takes one shot. The GUI dumps the panel on SIGUSR1.
- On the radio, a key with the Screenshot action (Settings > Keys) writes a PNG to the DATA
  partition; the web Gallery shows them and `pull.py <radio IP>` copies them here.
- Without a radio: `host_shots.py <host build> <script> <out> [--theme NAME] [--set name=value]`
  runs the PC build (host/README.md) headless in a sandbox (bwrap: no network, no D-Bus, a
  PipeWire of its own) with the same script commands; the PC build dumps its window on SIGUSR1 in
  the radio's format (`raw2png.py` converts either). `compare.py A1 A2 B` tells whether two builds
  draw the same (A1, A2: two runs of the old one, whose differences are what moves by itself), and
  `check_1bit.py` whether a shot of the 1bit theme has only its two colours.

These were taken on 2026-10-25 on an X6200 (base 1.0.8) on an antenna, during the CQ WW SSB weekend: the radio tuned first and left to settle, so the waterfall, the cluster spots and the decoders (FT8 on 20 m, JS8 on 40 m, RTTY on the DWD weather broadcast at 10.1008 MHz) show real signals. drive.py ran over one shared SSH connection (DRIVE_SSH_OPTS).
`web_remote.png` is the browser remote page in Chromium (1280 px wide), taken over the DevTools
protocol with the radio streaming.

Added on 2026-10-31 for v0.3.8, on the bench X6100 (base 1.1.8) into a dummy load, with drive.py over one
shared SSH connection, so their waterfalls are quiet: `field_llota.png` (Field Ops with the LLOTA tile),
`cw_qrss3.png` and `cw_beacon.png` (MSG > CW with QRSS3 on, and its page 3: Start, Until, the QRSS
frequency), `lock.png` (a tap of LOCK: "Dial locked" and the LOCK mark), `power_volts.png` (Power display
Volts: the battery's and the supply's voltage), `swr_scan_2500.png` (the 2500 kHz span) and `tune_ext.png`
(TUNE for an external tuner at 10 W); and from the web configurator in Firefox, `web_display_bootpic.png` (the
Boot picture card) and `web_logs_textlog.png` (the keyboard modes' text log). The other shots still show the button text of v0.3.7 (every theme has
one button text size since v0.3.8): they are taken again on the X6200 on an antenna.
