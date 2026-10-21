Screenshots of this firmware, from the radio.

- `tools/screenshots/drive.py <radio IP> <script>` runs a script of key presses, knob turns and
  shots on the radio over SSH (`x6drive.py` is the half that runs there) and writes the shots
  here as landscape PNGs; `grab.py` takes one shot. The GUI dumps the panel on SIGUSR1.
- On the radio, a key with the Screenshot action (Settings > Keys) writes a PNG to the DATA
  partition; the web Gallery shows them and `pull.py <radio IP>` copies them here.

The ones in the README were taken on 2026-10-21 on the bench.
