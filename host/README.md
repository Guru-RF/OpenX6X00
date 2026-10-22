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
libudev, PulseAudio and ALSA. gpsd's library is optional: without it the GUI builds without
GPS.

## Keys

The front keys and the knobs are FIFOs under `build-host/data/run`, named as the board's
devices, in the kernel's input event format: the radio's own drivers read them, the browser
panel (port 50002) writes into them, and so does `src/host/host_panel.c` for the PC's keys.
Everything else typed goes into the GUI as a USB keyboard would.

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

The mouse is a pointer: what is clickable on the screen can be clicked.

## What is not there

No audio in or out of a radio (PulseAudio's default devices are opened, so the apps that
play or record use the PC's sound card), no CAT port unless `X6100_CAT_DEV=<pty>` names one,
no GPS without gpsd, no Bluetooth, no RTC. The TCI server (50001) and the browser panel
(50002) run as on the radio.
