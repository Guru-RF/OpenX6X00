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
