# SVXConnect (built from source, patched)

SVXConnect-CLI is a client for SvxLink reflectors (protocol v3: a TLS login with a client
certificate the reflector's sysop signs, voice as Opus over AES-128-GCM UDP, talkgroups), by
Joeri Van Dooren (ON6URE) / Guru-RF, in plain C. On the radio it is the SVXConnect app's
helper: the GUI starts `svxconnect --stdio`, sends it the app's commands and takes its events
and its audio over one socket (src/svx.c).

Source: https://github.com/Guru-RF/SVXConnect-CLI
Tag: `v0.1.4` (annotated), commit `578e5de47c228576a86d00b487cb03478ab5c6e4`. `build.sh`
checks the commit and stops when it differs. `git clone --branch` of an annotated tag says
"is not a commit!": it clones the commit the tag names all the same.
License: MIT, Copyright (c) 2026 Joeri Van Dooren (`LICENSE` in the source). It links OpenSSL
and Opus (both in the image); `THIRD-PARTY-NOTICES` comes with it. Built with `TUI=0`, neither
ncurses nor miniaudio is in the binary.

Not vendored: `build.sh` clones the tag at build time. `build.sh`, `patches/` and this file
are ours.

## The build (`build.sh`)

- The patches below applied in order with `git apply`, then
  `make AUDIO=pipe TUI=0` with the radios' gcc and Cortex-A7 flags; OpenSSL (1.1.1 in both
  trees) and Opus found with pkg-config in the compiler's sysroot.
- A native build (no `CC`, or this PC's) also runs SVXConnect's own tests (`make test`) and
  keeps `fakerefl` beside the binary, for `tools/svx`'s tests and the PC build of the GUI.
- The binary (stripped) is kept as
  `$HOME/.cache/openx6x00/svxconnect/svxconnect-<tag>-<target>-<hash>/svxconnect`, with `doc/`
  (LICENSE, THIRD-PARTY-NOTICES) beside it; the hash is that of the patches, of `build.sh` and
  of the compiler's version. A second run prints that path and clones nothing.
- `buildroot/local-build.sh` passes the path to the GUI's CMake (`-DSVXCONNECT_BINARY`), which
  installs it as `/usr/bin/svxconnect` and `doc/` in `/usr/share/x6100/licenses/svxconnect`:
  it is in the card image and in the update bundle.

## Our patches (`patches/`, to take upstream)

All four are new code paths beside the existing ones: a build without the new switches is the
same program, and the project's own tests pass with and without them (also built for ARMv7
and run under qemu with OpenSSL 1.1.1).

1. `0001` Audio: `src/audio/dev_pipe.c`, a backend for a host program that carries the audio
   (`make AUDIO=pipe`), and `svx_audio_host_attach()` / `svx_audio_host_feed()` in `dev.h`
   (no-ops in the miniaudio backend and the tests' fake).
2. `0002` `--stdio` (`src/hostio.c`, `src/hostio.h` with the whole exchange): the session of
   `--headless`, its commands from stdin and its state to stdout as lines, sent when they
   change; over a `SOCK_SEQPACKET` socket the audio in records of its own. `app_control()`
   and `ctl_dispatch_line()` let it use the control FIFO's commands without a FIFO.
   `--cert-status`: the certificate on one line of key=value.
3. `0003` Tests: the fake reflector's `FR_SCRIPT` (talkers, Opus audio, errors, a report of
   what the client did) and `tests/fakerefl_main.c`, the fake as a program (`make fakerefl`).
4. `0004` Build: `make TUI=0`, no terminal interface and no ncurses (`src/ui/ui_none.c`).

## Seen while integrating (not patched, to raise upstream)

- `example.conf` says `default_tg = 0` starts monitoring only; `tgm_init()` selects the first
  switchable talkgroup instead (`tg 0` from the control commands does give monitoring only).
  The radio's web page says what the program does.
- The capture watchdog (`CAP_GRACE_MS` 500 and one reopen) ends an over after about a second
  without frames: "the microphone delivers no audio". The app sends the hand mic from the
  over's start and starts a Bluetooth over with the headset mic's first frame (src/svx.c); a
  host backend could take a longer grace.
- `THIRD-PARTY-NOTICES` names OpenSSL 3, ncurses and miniaudio. The radio's build links
  OpenSSL 1.1.1 (its own licence, not Apache-2.0) and has neither ncurses nor miniaudio
  (`TUI=0`, `AUDIO=pipe`): the notices could say which parts go with which build.
