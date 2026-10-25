Screenshots of this firmware, from the radio.

- `tools/screenshots/drive.py <radio IP> <script>` runs a script of key presses, knob turns and
  shots on the radio over SSH (`x6drive.py` is the half that runs there) and writes the shots
  here as landscape PNGs; `grab.py` takes one shot. The GUI dumps the panel on SIGUSR1.
- On the radio, a key with the Screenshot action (Settings > Keys) writes a PNG to the DATA
  partition; the web Gallery shows them and `pull.py <radio IP>` copies them here.

These were taken on 2026-10-25 on an X6200 (base 1.0.8) on an antenna, during the CQ WW SSB weekend: the radio tuned first and left to settle, so the waterfall, the cluster spots and the decoders (FT8 on 20 m, JS8 on 40 m, RTTY on the DWD weather broadcast at 10.1008 MHz) show real signals. drive.py ran over one shared SSH connection (DRIVE_SSH_OPTS).
`web_remote.png` is the browser remote page in Chromium (1280 px wide), taken over the DevTools
protocol with the radio streaming.
