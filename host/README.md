# The GUI on a PC

The radio's GUI builds and runs on a Linux desktop: the panel is an SDL window, the radio
behind `src/hw/hw.h` is the stub (`src/hw/stub/hw_stub.c`, noise with one carrier), the data
lives in a directory. It is for working on the screens and the apps without a radio, and for
bringing up a new radio's backend: the GUI is the same code, only the board (`src/board.h`)
and the backend differ.

    host/build.sh           # build and run
    host/build.sh build     # build only
    host/build.sh run
    host/build.sh clean     # removes build-host/, the dependencies and the data with it

Everything lands in `build-host/`: the dependencies the distribution does not have
(liquid-dsp 1.4.0 and ft8_lib at the revision the radio uses, built once into
`build-host/deps`), the staged read-only files (`build-host/share`: icons, themes, the
country list, the default settings) and the data directory (`build-host/data`: the radio's
`/mnt`, with `params.db` copied from the defaults on the first run).

What the PC needs: a C/C++ toolchain, cmake and ninja, sqlite3, and the development packages
for SDL2, libdrm, xkbcommon, NetworkManager (libnm), dbus-glib, libpng, libjpeg, libsndfile,
libudev, PulseAudio, ALSA and FFTW 3 (WSPR and JS8). gpsd's library is optional: without it
the GUI builds without GPS.

## In Docker (macOS, Windows)

Where those packages are not at hand (macOS, Windows, a Linux without them), `host/docker/run.sh` builds a
Debian image with them (`ox-host`, the first time) and runs a command in it with this repository at `/src`; the
dependencies built once stay in a Docker volume (`ox-deps`). There is no window: `host/docker/shot.sh` runs the
GUI on a virtual display, presses keys, touches the screen and saves a screenshot.

    host/docker/run.sh host/build.sh build
    host/docker/run.sh host/docker/shot.sh /src/build-docker/shot.png 8 F9 sleep:1 click:400,240

The container builds in `build-docker/`, beside a `build-host/` of the PC's own: on a Linux host its files are
root's, and its CMake cache names `/src`. `host/docker/run.sh host/build.sh clean` removes it.

The arguments after the file are the seconds to wait for the GUI and the steps: a key as the PC's keys below
(xdotool's names), `sleep:<s>`, or `click:<x>,<y>` on the 800x480 screen. The `HOST_STUB_*` switches set where
run.sh is started go into the container; a file they name has to be inside the repository.

## Real signals

The stub radio's stream is noise with one carrier. `HOST_STUB_IQ=<file>` plays a recording instead, round and
round: 100 kS/s complex float32 (I and Q interleaved), centred on the dial and the other way up as the radio's
stream is (a signal f above the dial at -f). Tune the GUI to the frequency it was recorded around, so the scale
reads true.

For the Scanner and the Propagation app's beacons the carrier can stand on chosen frequencies only, nothing
anywhere else: `HOST_STUB_SCAN=118300000,27185000` (Hz, on 15 s and off 15 s, so the scanner stops and moves on)
and `HOST_STUB_BEACONS=13,0` (the NCDXF beacons by their place in the round, here OH2B and 4U1UN, each keyed in
its own slots on 14.100 to 28.200 MHz, at the CW pitch).

## Keys

The front keys and the knobs are FIFOs under `build-host/data/run`, named as the board's
devices, in the kernel's input event format: the radio's own drivers read them, the browser
panel (port 50002) writes into them, and so does `src/host/host_panel.c` for the PC's keys.
Everything else typed goes into the GUI as a USB keyboard would. The F keys and PageUp/PageDown
here are the radio's front keys; a USB keyboard's do nothing on the radio.

| PC                          | Radio                         |
|-----------------------------|-------------------------------|
| F1 .. F5                    | F1 .. F5                      |
| F6, F7, F8, F9              | GEN, APP, KEY, MSG            |
| F10, F11, F12               | DFN, DFL, LOCK                |
| Left / Right, mouse wheel   | main knob                     |
| Up / Down, Ctrl+wheel       | MFK                           |
| Shift+Up / Down, Shift+wheel| VOL                           |
| Enter, Shift+Enter          | MFK press, VOL press          |
| PageUp / PageDown           | band up / down                |
| Ctrl+1, Ctrl+2, Ctrl+3      | AM, CW, SSB                   |
| Ctrl+B, Ctrl+R, Ctrl+T      | A/B, PRE, ATU                 |
| Ctrl+V, Ctrl+G, Ctrl+F      | V/M, AGC, FST                 |
| Ctrl+P                      | PTT (the stub only pretends)  |
| CapsLock+L, K, P, S, F      | a USB keyboard's chords (docs/MANUAL.md, Keyboard): log a QSO, lock, screenshot, Settings, frequency box |
| CapsLock+1 .. 8, Up / Down  | CW messages 1-5 (JTTY's macros 1-8), band up / down |
| CapsLock+Space              | PTT on, again off, as a USB keyboard's (this PC's Caps Lock still turns with each chord) |

The mouse is a pointer: what is clickable on the screen can be clicked. The F1–F5
slots on the bottom bar take a click and a hold the same way the front keys do.

## What is not there

No audio in or out of a radio (PulseAudio's default devices are opened, so the apps that
play or record use the PC's sound card), no CAT port unless `X6100_CAT_DEV=<pty>` names one,
no GPS without gpsd, no Bluetooth, no RTC. The TCI server (50001) and the browser panel
(50002) run as on the radio.
