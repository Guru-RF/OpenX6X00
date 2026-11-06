# Themes to copy to the card

None of these is installed with the image. Copy a theme file to `/mnt/themes/<name>.json`, or a folder to
`/mnt/themes/<name>/`, and choose it in Settings > Display > INTERFACE > Theme. The format, the images and the list of
colour roles: [docs/THEMES.md](../docs/THEMES.md).

| Theme | What | By | Made by |
|---|---|---|---|
| `Holo.json` | black with dark grey keys, cyan for what is on, Roboto | Joeri Van Dooren (ON6URE) | by hand |
| `Material 3.json` | dark blue-grey surfaces, light blue for what is on, Roboto | Joeri Van Dooren (ON6URE) | by hand |
| `Codec/` | green phosphor boxes with corner brackets on a grid with scanlines, its own font (Share Tech Mono, OFL-1.1), the S-meter as a SIGNAL bar | Trey (rewtraw), #186 | `python3 make_codec.py` |
| `Ops Room/` | amber operations-room frames on a hexagon grid, the clock in 7-segment digits with the power and the battery's time left, the S-meter as a row of hexagons, a red TRANSMIT band while the radio transmits, the screensaver's rain in amber | Trey (rewtraw), #187 | `python3 make_ops_room.py` |
| `RS/` | personal use only: cut from a screenshot of another maker's remote-control program (`rsba1.png`); up to v0.3.5 it was installed with the image | Joeri Van Dooren (ON6URE) | `python3 make_rs.py` |

The `make_*.py` scripts need Pillow only; they draw every image and write `theme.json` into the folder beside them.
`zoom.py` and `lum.py` help read coordinates and brightness off the RS screenshot.
