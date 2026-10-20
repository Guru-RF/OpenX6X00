# Menus, and where things are

OpenX6X00 opens its apps and settings from grids of icons: the **APP** key shows the apps
grid, **Modes** in it the digital modes grid, and Settings has an icon rail. The MFK moves
along the tiles, a press opens, ESC or the key again closes. The text button pages under
the grid still work as before, and Settings > Display > INTERFACE > **Icon menus** brings
the old text menus back.

The icons are ours (`tools/icons/mkicons.py` draws them into
`rootfs/usr/share/x6100/icons`, white, coloured by the theme). Anyone coming from the
X6100 firmware this project was forked from, which grew icon menus of its own in its
closed v0.129, finds the same things under these names:

## APP grid

| Here | There (v0.129) | What it is |
|---|---|---|
| Modes, "FT8, JS8, PSK, SSTV..." | Modes, "FT8, JS8, PSK, RTTY, SSTV..." | the digital modes grid |
| Cluster, "DX spots" | Cluster, "DX spots" | the DX cluster app |
| Field Ops, "SOTA, POTA, WWFF" | Field Ops, "POTA, SOTA activations" | activations |
| Bands, "Band activity" | Bands, "Band activity" | cluster and RBN activity per band |
| Channels, "Memory channels" | Channels, "Memory channels" | named channels |
| SWR Scan, "Sweep, transmits" | SWR scan, "SWR sweep, transmits" | |
| Recorder, "RX audio" | Recorder, "Record and play RX" | the RX recorder |
| CW Trainer, "Learn CW" | CW Trainer, "Learn CW" | |
| GPS, "Position, time" | GPS, "Position and time" | |
| Screen Rec, "Screen and audio" | Screen Rec, "Record screen and audio" | starts or stops a screen recording |
| Walkie, "Portrait screen" | Settings > Walkie | the Walkie Talkie screen |
| Settings, "All settings" | Settings, "All settings" | |
| Birdie Cal, "Find birdies, antenna off" | Birdie Cal, "Find birdies, antenna off" | the radio's own carriers: found, marked, notched |

## Modes grid

The same thirteen modes as the text menu, five to a row, with an icon by family: a clock
for the timed modes (FT8, FT4, FT2), a keyboard for the typed ones (RTTY, JTTY, PSK31,
PSK63, Olivia), a bubble for JS8, a beacon for WSPR, a picture for SSTV and WeFax, lines
of text for NavTex.

## Settings rail

| Here | There (v0.129) |
|---|---|
| General (station, date and time, controls, transverters, TCI server) | Station, "Call, clock" |
| Display (backlight, spectrum, waterfall, main screen, interface) | Display, "Screen, theme" |
| Audio (RX and TX audio, microphone, VOX, AGC) | Audio, "Mic, VOX, AGC" |
| Wireless (WiFi, Ethernet) | Wireless |
| Bluetooth | Bluetooth |
| Keys (long presses, hand mic keys, paddle) | Keys, "Keys, paddle" |
| Cluster (filters, view, PSK Reporter) | Cluster, "Spots" |
| TX Cal (calibration, CESSB, protection, S-meter front end) | TX Cal, "TX audio, SWR" |
| Info (version, baseband, broadcast list, factory reset, power) | System, "Update, power" |
| Walkie | (in the APP grid here as well) |

The web configurator has the same **Icon menus** switch on its Display page.
