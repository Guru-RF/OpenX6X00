# Mercury (vendored)

Mercury, the HF modem (TNC) of Rhizomatica's HERMES project (Rafael Diniz and
others), as v1.9.25. Pat talks to it on its VARA-style host ports with its
`varahf` transport, for Winlink. Only Mercury 1.9.25 connects to it on the
air (below).

Source: https://github.com/Rhizomatica/mercury
Tag: `v1.9.25`, commit `33efac4e3089f64f493fde1232b8e96305d15185` (2026-11-21)
Tarball: https://github.com/Rhizomatica/mercury/archive/refs/tags/v1.9.25.tar.gz,
SHA-256 `a21196c9bc2b0f5bed303bd4e09f530a2bf1b59337fed7b816634f54f9219b6e`
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

- `modem/`: the modem, `mfsk/`, and from `freedv/` the 66 sources of
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
system's, `-latomic` on 32-bit ARM), `x6x00/x6x00_mercury.c` (the program
that links it: Mercury with the GUI's Winlink socket as its sound card and
PTT, installed as `/usr/bin/x6x00-mercury`, GPL-3.0-or-later) and this file
are ours.

To take a newer version: unpack it, build the same source list (ours, in
`CMakeLists.txt`, or upstream's `Makefile` for `libmercury_host.a`) with
dependency files, and copy what the dependency lists name; compare
upstream's `Makefile`, `modem/freedv/Makefile` and `host/` with the pin's
for sources added (v1.9.25: nine LDPC tables in `modem/freedv/`, 299 files
in the dependency lists against v1.9.21's 281, the same set as `gcc -MM`
gave for v1.9.21's import). Then the two local patches again, each its own
commit, `GIT_HASH` in `CMakeLists.txt`, and `tools/winlink`'s `make modems`.

## On-air compatibility

It breaks between Mercury versions, so a new pin is a new test on the air.
Upstream's table, in the tag's `docs/RELEASE-1.9.25.md` and in
https://github.com/Rhizomatica/mercury/discussions/637 ("Each group
connects only within itself"):

| Group | Versions | Connects to |
|---|---|---|
| 1 | 1.9.25 | 1.9.25 |
| 2 | 1.9.20, 1.9.21, 1.9.22, 1.9.23, 1.9.24 | one another |
| 3 | 1.9.19 | 1.9.19 |
| 4 | 1.9.18 | 1.9.18 |
| 5 | 1.9.17 and older | one another |

1.9.25 does not connect to 1.9.24 or any older version, in either direction
(connect minor 2: each bandwidth has its own ladder, and the wire carries a
rung's level, not its mode). A 1.9.24 station still answers a 1.9.25 CALL;
the 1.9.25 caller ignores that ACCEPT, logs it ("ACCEPT for ... from
Mercury 1.9.24 or older (protocol minor 1): the versions do not connect,
ignored") and gives up as if unanswered; `x6100-winlink` reads that line.
Nothing is said there of versions after 1.9.25. Our v1.9.21 (firmware v0.3.7
to v0.3.13) is in group 2.

## What v1.9.25 changed for us (from v1.9.21)

- Louder: the modulator raises every mode 4 dB (`modem/modem.c`,
  `TX_BASE_GAIN`), to peaks at -2.0 dBFS (MFSK -2.5), in the engine, so in
  the samples `host/audio_host.c` hands our program too. `x6100-winlink`
  writes `[audio] tx_gain_db = -4`, upstream's setting for the 1.9.24
  level: the drive on the radio stays what it was tried with.
- Wider: DATAC17 and QAM16C2 have 36 carriers, 2250 Hz (375-2625 Hz; 2100 Hz
  before), and BW2750 two modes of 44 carriers, 2750 Hz (125-2875 Hz). The
  Winlink page's Filter row and the manual go by these.
- `host/` defines `radio_io_set_usb_fd` and `audioio_set_android_devices`
  itself (upstream #453, #549): our stand-ins for them are gone.
- `mercury_request_shutdown()` first ends a session on the air
  (`mercury_engine_say_goodbye`, up to 40 s). Our program stops the audio
  exchange before Mercury, as `host/mercury_host.h` asks, so that DISCONNECT
  could not go out from there. With a session up (`arq_goodbye_progress()`)
  it therefore calls `mercury_say_goodbye()` itself, on a thread, while the
  exchange still runs, for 42 s at most or until a second SIGTERM; then the
  exchange ends, `mercury_stop` is set and Mercury stops with no goodbye of
  its own. `x6100-winlink` waits 55 s for it. Both calls and `mercury_stop`
  are not in `host/mercury_host.h`: declared in `x6x00/x6x00_mercury.c`.
- The TNC: `CWID ON` is accepted (no CW ID is sent), one host at a time on
  the ports (a second connection is refused while Pat holds them; nothing
  of ours connects to them, `x6100-winlink` looks in `/proc/net/tcp`),
  `DISCONNECTED` only once the session has closed. Log lines begin with
  the date since 1.9.22, and the log names the build ("Mercury 1.9.25 (git
  33efac4e)").

## Licences

- Mercury itself: GPL-3.0-or-later, Rhizomatica (`LICENSE`, the file
  headers). Anything linked with it, such as our bridge, is GPL-3.0-or-later
  as a whole.
- `modem/freedv/`: a subset of FreeDV / codec2, LGPL-2.1 (`LICENSE-freedv`),
  except KISS FFT in it (`kiss_fft.[ch]`, `kiss_fftr.[ch]`,
  `_kiss_fft_guts.h`): BSD-3-Clause, Mark Borgerding (the notice in the
  headers of `kiss_fft.c`, `kiss_fftr.c` and `_kiss_fft_guts.h`).
- `common/iniparser/`: MIT, Nicolas Devillard (`common/iniparser/LICENSE`).
- `datalink_broadcast/raptorq/`: nanorq (`lib/`, `include/`) and oblas
  (`deps/obl/`), both MIT, Joseph Calderon. Mercury's tree carries no
  licence file for them: `raptorq/LICENSE` and `raptorq/deps/obl/LICENSE` are
  ours, copied from https://github.com/ursoid-tech/nanorq (formerly
  sleepybishop/nanorq, branch `stable`) and
  https://github.com/sleepybishop/oblas.
- `audioio/ffaudio` and `ffbase` (Unlicense, `audioio/ffaudio/UNLICENSE`
  upstream): Mercury's own sound card code, none of it in this subset.
- New in v1.9.25's subset: nine LDPC tables in `modem/freedv/`, numbers only,
  with no notice of their own. `H_IRA_*`: Mercury's, made by its
  `utils/ldpc_ira_peg.py`. `H_16200_6480`, `_7200`, `_10800`, `_11880`,
  `_12600`: the DVB-S2 short-frame codes of ETSI EN 302 307, "read off GNU
  Radio's DVB-S2 LDPC encoder by utils/ldpc_dvbs2_gen.py" (their headers;
  GNU Radio is GPL-3.0-or-later, as the whole is). No other third-party
  code came with the 81 files that changed or were added.

## Local patches

Each one is its own commit on top of the pristine import.

- `data_interfaces/tcp_interfaces.c`: the TNC ports (TCP 8300 commands,
  8301 data) and the broadcast port (TCP 8100) listen on 127.0.0.1 only.
  Upstream binds them to every interface, with no option to change it and
  no authentication, so anyone on the radio's network could key it.
- `data_interfaces/tcp_interfaces.c`: the `VERSION` line of the answer is
  `VERSION Mercury 1.9.25` (`MERCURY_VERSION`) instead of upstream's
  `VERSION VARA HF v4.9.0`, after its unchanged `VARA version 4.9.0
  registered`. Pat's VARA driver waits, with no timeout, for a line that
  starts `VERSION` or `WRONG`, and takes the rest of it as the modem's
  version: Pat's log says "VARA modem (Mercury 1.9.25) initialized". Up to
  1.9.24 upstream sent no `VERSION` line at all and Pat's start of the
  modem could hang (our patch then replaced the whole answer); 1.9.25 sends
  one (#638), so the patch now only names Mercury and its version.
