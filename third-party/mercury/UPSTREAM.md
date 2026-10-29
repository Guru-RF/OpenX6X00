# Mercury (vendored)

Mercury, the HF modem (TNC) of Rhizomatica's HERMES project (Rafael Diniz and
others), as v1.9.21. Pat talks to it on its VARA-style host ports with its
`varahf` transport, for Winlink. Only Mercury 1.9.20 and later connect to it
on the air.

Source: https://github.com/Rhizomatica/mercury
Tag: `v1.9.21`, commit `edca701ba2e432aecd7d4e217ec8fa717ed23aa3` (2026-10-24)
Tarball: https://github.com/Rhizomatica/mercury/archive/refs/tags/v1.9.21.tar.gz,
SHA-256 `785a84463581ba89420ff1a18b3f4ea88f92f2176fc13d16b899c1dbfdb883cd`
License: GPL-3.0-or-later (see `LICENSE`), except the parts below

## What is kept

Mercury's daemon build only: the engine without Mercury's own audio, PTT and
websocket server, for a program that links it in and owns the radio
(`host/mercury_host.h`). That is what upstream's
`make libmercury_host.a MERCURY_HOST=daemon` archives, with the FreeDV
library and the iniparser it is linked with. Copied from that commit: every
source file this build compiles and every header they include (the
compiler's dependency lists of the build), unchanged and at the same paths,
and `LICENSE`, `LICENSE-freedv`, `README.md` and `common/iniparser/LICENSE`.

- `modem/`: the modem, `mfsk/`, and from `freedv/` the 57 sources of
  `libfreedvdata.a` with their headers (not its programs, `unittest/`,
  `codebook/` or `original_docs/`).
- `datalink_arq/`, `datalink_broadcast/` (with `raptorq/`, the RaptorQ
  coder of the broadcast carousel), `data_interfaces/`, `common/` (with
  `iniparser/`), `host/`: the sources the build compiles and their headers.
- `gui_interface/`: the UI state (`ui_*.c`), `websocket/ws_json.[ch]`, and
  from `fyne-ui/` only `engine/mercury_bridge.[ch]` (`mercury_init()` and
  `mercury_shutdown()`).
- `audioio/` and `radio_io/`: two headers each; their code is the host's.

Left out: `main.c` (the standalone program), Mercury's audio backends,
radio control, websocket server and GUIs, the vendored Hamlib and hidapi,
`docs/`, `tests/`, `utils/`, `specs/` and the packaging (`debian/`,
`android/`, `ios/`, `macos/`, `windows-installer/`).

`CMakeLists.txt` (the daemon build as a static library, part of the GUI's
build, upstream's compile flags, the vendored iniparser in place of the
system's, `-latomic` on 32-bit ARM) and this file are ours.

To take a newer version: unpack it, build the same source list (ours, in
`CMakeLists.txt`, or upstream's `Makefile` for `libmercury_host.a`) with
dependency files, and copy what the dependency lists name. On-air
compatibility breaks between Mercury versions (1.9.20 and later do not
connect to 1.9.19 or older), so a new pin is a new test on the air.

## Licences

- Mercury itself: GPL-3.0-or-later, Rhizomatica (`LICENSE`, the file
  headers). Anything linked with it, such as our bridge, is GPL-3.0-or-later
  as a whole.
- `modem/freedv/`: a subset of FreeDV / codec2, LGPL-2.1 (`LICENSE-freedv`).
- `common/iniparser/`: MIT, Nicolas Devillard (`common/iniparser/LICENSE`).
- `datalink_broadcast/raptorq/`: nanorq (`lib/`, `include/`) and oblas
  (`deps/obl/`), both MIT, Joseph Calderon. Mercury's tree carries no
  licence file for them: `raptorq/LICENSE` and `raptorq/deps/obl/LICENSE` are
  ours, copied from https://github.com/ursoid-tech/nanorq (formerly
  sleepybishop/nanorq, branch `stable`) and
  https://github.com/sleepybishop/oblas.
- `audioio/ffaudio` and `ffbase` (Unlicense, `audioio/ffaudio/UNLICENSE`
  upstream): Mercury's own sound card code, none of it in this subset.

## Local patches

Each one is its own commit on top of the pristine import.

- `data_interfaces/tcp_interfaces.c`: the TNC ports (TCP 8300 commands,
  8301 data) and the broadcast port (TCP 8100) listen on 127.0.0.1 only.
  Upstream binds them to every interface, with no option to change it and
  no authentication, so anyone on the radio's network could key it.
