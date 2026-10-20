# OpenX6X00 by RF.Guru

Open firmware for the **Xiegu X6100**: a complete microSD image with every
digital mode decoded and sent on the radio itself, the DX cluster on the
waterfall, SOTA and POTA activations with big outdoor tiles, themes, Bluetooth
headphones and CAT, a web configurator, and a **TCI server** so the
[VFO-Knob](https://github.com/Guru-RF/VFO-Knob) and other TCI clients tune,
hear and key the radio over the network. In the RF.Guru identity: near-black
surfaces, gold for what is on, electric blue as the second accent.

## Where it comes from

OpenX6X00 is a fork of the last fully open-source release of the X6100
firmware by Giovanni Ricciardi IU1WXY, v0.127 of 17 October 2026, which was
published under the LGPL-2.1-or-later. That firmware stands on the R1CBU GUI by
Oleg Belousov R1CBU and Georgy Dyuldin R2RFE
([gdyuldin/x6100_gui](https://github.com/gdyuldin/x6100_gui)) and on the
decoders of 1KO125 by Hany El Imam. From v0.128 the IU1WXY firmware went closed;
this project continues the open line. The GUI stays LGPL-2.1-or-later, the JS8,
WSPR and JTTY parts are GPL-3, so the image as a whole is GPLv3, exactly as
before. The copyright notices of every author stay in the files.

## What changed from the fork

Removed, so that nothing on the radio talks to a server of its own:

- the X6100 Link phone app and its remote API: the `/link/v2` routes, the
  web Remote page, the GUI's remote socket with its spectrum, audio and event
  streams, and the phone voice PTT on UDP 50010;
- the wfview (Icom LAN) server;
- the x6100-link.it update channel: the update without the microSD, the
  signed packages, the update alert, the remote updates and the Settings
  Update tab;
- the diagnostics upload, the site ping and every other telemetry;
- the private tests and tools.

Added:

- a **TCI v2.0 server** (Expert Electronics' protocol, as AetherSDR serves
  it) over a WebSocket on port 50001, on by default on whatever network the
  radio is on, WiFi or Ethernet: Settings > General > TCI SERVER on the radio,
  System > TCI on the web page. It carries `vfo`,
  `modulation`, `rx_filter_band`, `agc_mode`, RIT, `drive`, `trx` (PTT, with
  `,tci` for the client's audio as the microphone), the S-meter and the TX
  meters, the RX audio stream (int16 mono, 24 kHz) and the TX audio frames.
  No login: turn it off on a network you don't trust. Config in `/mnt/tci.conf`
  (`enabled=0|1`, `port=50001`);
- the RF.Guru default theme and the OpenX6X00 boot logo;
- a local configurator socket (`/tmp/x6100_cfg.sock`) for the web pages that
  set the display and the Ethernet port, in place of the remote socket.

## What it does

| | |
|---|---|
| **Digital modes** | FT8, FT4 and FT2 (CQ, reply, auto sequence, Hound, POTA tag, ADIF log), JS8 (JS8Call compatible, all four speeds at once, inbox, relays, heartbeats, APRS), WSPR (decode and beacon, WSPRnet upload), PSK31, PSK63, Olivia, JTTY, RTTY, SSTV (receive with VIS, transmit with templates, denoise), WeFax and NavTex. No PC, no sound card cable: APP > Modes. |
| **CW** | A decoder that copies down to about 0 dB SNR with Parse splitting run-together words, a CW keyboard with a send queue and Live mode, messages with `{CALL}` and `{HIS}`, USB and Bluetooth paddles, a CW trainer. |
| **DX Cluster** | Spots on the waterfall, in a column beside it and in a full app that follows the VFO; filters by mode, band and activity (POTA, SOTA, WWBOTA, HOTA), alerts on calls and prefixes, spot snap when turning slowly, ten public servers built in. |
| **Field Ops** | A second interface for SOTA and POTA activations with big tiles for gloves: the summit or park found from the GPS or the locator, the references of the whole world offline, CQ, a log straight to ADIF with the references, self-spots to pota.app, wwbota.org and cqhota.app, QRZ Logbook upload. |
| **Bands** | A live picture of 160 to 6 m from the cluster and the Reverse Beacon Network, and a test beacon that shows who hears you from your own antenna. |
| **Radio** | IARU band plan strip, Birdie Cal (the radio's own carriers found, marked, notched), S-meter in bars or a needle, long-press actions on every key, editable bands, memories and channels, transverters, RX recorder, screen recording and screenshots, voice messages, SWR scan, Walkie Talkie portrait mode, local time on screen with UTC in the logs. |
| **Screen** | DRM/KMS output with the GUI on a transparent layer over a waterfall drawn one line at a time, about half the CPU of the old path; a main knob with every detent timed in its own thread and acceleration that grows with the speed. |
| **Themes** | Every colour, corner, font and waterfall palette comes from the theme; RF.Guru, Classic, Outdoor, Outdoor-Negative, VISION, Black Metal, Total Black, RS, Holo and Material 3 come with the image, your own go on the SD card ([docs/THEMES.md](docs/THEMES.md)). |
| **Wireless** | WiFi, a USB Ethernet adapter with DHCP or a fixed address, USB tethering; Bluetooth CAT for three clients, headphones and keyboards with pairings that survive a new image. |
| **Web configurator** | `http://<radio IP>/`: bands, digital frequencies, memories, cluster, Field Ops, CW messages, macros, logs, files, a gallery, backup and restore, display, time, Ethernet, TCI. |
| **TCI server** | The VFO-Knob and any TCI client on the network: tune, mode, filter, AGC, PTT, the meters and the audio both ways. |

Screenshots of this firmware are taken on the radio (a key with the Screenshot action,
Settings > Keys) and land on the DATA partition and in the web Gallery;
`python3 tools/screenshots/pull.py <radio IP>` copies them into `docs/screenshots/`.

## Install

1. Download `OpenX6X00-v<version>-sdcard.img.zip` from the
   [latest release](https://github.com/Guru-RF/OpenX6X00/releases/latest).
2. Write the image to a microSD card with [balenaEtcher](https://etcher.balena.io/)
   or Rufus.
3. Put the card in the radio and switch on. Settings and logs live on the
   card's DATA partition (`/mnt`).

Writing a new image erases DATA too: download the Backup from the web
configurator first and restore it after the first start (the
[manual](docs/MANUAL.md) lists the files). A patched baseband
([x6100_base_patch](https://github.com/gdyuldin/x6100_base_patch)) r13 or
newer is recommended: the TX audio and calibration options (CESSB, band
output gain) and a battery percentage from a measured discharge curve.

## Build

The image is built by
[AetherX6100Buildroot](https://github.com/gdyuldin/AetherX6100Buildroot), with
this repository as its GUI package. The GitHub Actions workflow
(`.github/workflows/main.yml`, *Build image*, run by hand, about an hour)
checks out both, patches the buildroot tree (local GUI and web server,
`boost` and `fftw-single` for JS8, no RHVoice, the boot logo) and uploads
`OpenX6X00-<ref>-sdcard.img.zip`; on a tag it attaches the image to the
release. Locally, the same build runs in a container: clone
AetherX6100Buildroot next to this repository, then

```
docker build -t openx6x00-build buildroot
docker run --rm -u $(id -u):$(id -g) -e HOME=/work/.brhome -v $(dirname $PWD):/work \
    openx6x00-build bash /work/OpenX6X00/buildroot/local-build.sh
```

leaves `../AetherX6100Buildroot/build/images/OpenX6X00-local-sdcard.img` (about
an hour the first time, 20 cores; `buildroot/local-build.sh` has the patch
steps, `buildroot/Dockerfile` the Ubuntu 22.04 host with CMake 3.28 and
sqlite3). With a buildroot tree already built, `buildroot/build.sh` rebuilds
the GUI alone with its toolchain
([docs/UPSTREAM_README.md](docs/UPSTREAM_README.md), [manual, Build](docs/MANUAL.md#build)).

## Layout

```
src/                     the GUI (x6100_gui): main screen, apps, settings, decoders, TCI server
  cfg/ params/           the settings (SQLite params.db) and their C/C++ API
  ft8/ js8/ wspr/ sstv/ wefax/ navtex/ cw_decoder/ cluster/ field/ qth/
  fonts/ widgets/        LVGL fonts and custom widgets
third-party/             js8core (JS8Call's modem, GPL-3), wsprd (WSJT-X, GPL-3), cty.dat
lvgl/ lv_drivers/        LVGL 8.3 (gdyuldin's NEON fork, a submodule) and its drivers
webserver/               the web configurator (Python, bottle)
rootfs/                  init scripts, helper scripts (x6100-field, x6100-pota-spots,
                         x6100-broadcast-update, btcat-ctl, btaudio-ctl), data files
btcat/ bootanim/         Bluetooth CAT bridge, boot animation
boot/ u-boot/            boot logo, the U-Boot binary written over buildroot's
sql/                     the default params.db
themes/                  the RS theme (images) and the JSON themes copied to the SD card
buildroot/               build.sh: the GUI alone with the buildroot toolchain
docs/                    MANUAL.md, THEMES.md, MENUS.md, screenshots pulled from a radio
```

## Documentation

- [Manual](docs/MANUAL.md): every app and setting, the web pages, the TCI
  server, CAT commands, files, building.
- [Themes](docs/THEMES.md): make your own.

## Licences

The GUI is LGPL-2.1-or-later ([LICENSE](LICENSE)); the vendored js8core and
wsprd, and JTTY (ported from WSJT-X by K1JT and the WSJT-X team, `src/jtty*`),
are GPLv3, so the firmware image is GPLv3. The web server is LGPL-2.1
([webserver/LICENSE](webserver/LICENSE)). Every file keeps the copyright
notice of its authors.

## Credits

Oleg Belousov R1CBU and Georgy Dyuldin R2RFE wrote the R1CBU GUI this firmware
stands on (CW receiver, CW tune indicator, DRM/KMS output, direct spectrum
rendering, the main knob handling); Giovanni Ricciardi IU1WXY the rewrite of
the GUI, the theme system, Field Ops, Bands, the DX cluster, WSPR, SSTV TX,
RTTY, WeFax TX, the CW keyboard, the Bluetooth work and the layer system;
Hany El Imam (1KO125) the WeFax, NavTex and SSTV decoders; Jordan Sherer
KN4CRD and the JS8Call contributors the JS8 engine; K1JT and K9AN wsprd; K1JT
and the WSJT-X team JTTY; SP2KMO the Bluetooth tab; 2E0JFS the Black Metal and
Total Black themes. Libraries: LVGL, cJSON, FFTW, liquid-dsp, ft8_lib, libpng,
bottle, PulseAudio, BlueZ. Fonts: Roboto, DejaVu, Arial, Sony Sketch. The
skimmer spots come from the Reverse Beacon Network and the broadcast schedule
from the EiBi list by Eike Bierwirth. OpenX6X00 is maintained by RF.Guru
(Joeri Van Dooren ON6URE).
