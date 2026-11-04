# XARA (built from source, patched)

XARA is a VARA-compatible HF and FM modem for Linux, macOS and FreeBSD by x0rro, written in Rust, with VARA's TCP
host interface (commands on 8300, data on 8301). On the radio it is the third Winlink modem, experimental: Pat talks
to it through its VARA HF transport (varahf), as to Mercury, and through it to the VARA HF gateways of Pat's list.
It is an independent program that speaks VARA's protocol on the air, not VARA itself.

Source: https://github.com/x0rro/xara
Release: `v0.1.3` (2026-10-31), the signed tag `e32422a9` on commit `09a9201381b00499ada9d9c816bd05bb8f175705`; main
was still that commit on 4 November. The release's tarballs, their SHA-256 from its `SHA256SUMS`:

- `xara-0.1.3.tar.xz` (the source): `34f6fce5af2c32eca07359301ba2f58a49d1cc534b2a25b14c9606dcf84fc4cb`
- `xara-0.1.3-vendor.tar.xz` (the crates it depends on, with the `.cargo/config.toml` that points cargo at them):
  `dbac90dc109f2f6ab4d7840cf04e735f784fedd878d370f2d501b0fde8a8ae80`

License: MIT, Copyright (c) 2026 x0rro (`LICENSE` in the source).

Not vendored: `build.sh` downloads the two tarballs at build time and checks them. `build.sh`, `patches/` and this
file are ours.

## The build (`build.sh`)

- Rust: the official toolchain at 1.99.1 (2026-10-15), the version XARA's own releases are built with
  (`.rust-version`; the workspace needs 1.85 or newer, edition 2024). Buildroot's Rust (1.67.1 in both trees) is too
  old, so XARA is built outside buildroot, as Pat is. From static.rust-lang.org, each part's SHA-256 checked before
  it is unpacked: `rustc` `2ff05ff9c08a90861e7f6e4065b40a8b87463b4576186fd613871c3e37f2091f`, `cargo`
  `53f3521949404d595efb06a55c12c5658d4ed1c75566f33d3d514eaebe70bd8a`, `rust-std` for x86_64
  `109b074225bf5b9ea3820890876bfdf4ed161adbfaf94b2b4b5984c85ee5d958` (build scripts) and for
  armv7-unknown-linux-gnueabihf `53fdf6581a24f510def5173813a0d23518a8cde547b9fbf4876499f19feda18c`, unpacked into
  `$HOME/.cache/openx6x00/rust-1.99.1` (in the container `/work/.brhome/.cache`), about 720 MB, the downloads 150 MB.
- Offline from there: `cargo build --frozen --release -p xara --bin xara --target armv7-unknown-linux-gnueabihf
  --no-default-features --features registered,x6x00`, the vendored crates and `Cargo.lock` as the release has
  them, no variable from the environment that steers Rust or cargo. No PulseAudio or ALSA (the radio's audio is the
  GUI's), our socket backend (`x6x00`, below) and `registered` (below). Linked with the gcc the caller names in
  `CC`: `buildroot/local-build.sh` gives the buildroot tree's own (`arm-buildroot-linux-gnueabihf-gcc`, glibc 2.36,
  the GUI's toolchain, after `make toolchain`).
- Checked: a 32-bit ARM ELF that needs only `libc.so.6`, `libm.so.6`, `libgcc_s.so.1` and `ld-linux-armhf.so.3`, and
  glibc 2.36 at most (it needs 2.35). Stripped, the build's directories left out of it
  (`--remap-path-prefix`): 2513732 bytes, SHA-256 `182ce95e159d2d9e34bb37c6a08dc596f4f085d6925b5c0956a4ababe5306553`,
  the same with the X6100 tree's gcc and the X6200 tree's, and from two builds in different directories. Under qemu
  with either radio's libraries it starts, `--version` says `xara 0.1.3` and `--registered` is taken.
- Kept as `$HOME/.cache/openx6x00/xara/xara-0.1.3-rust1.99.1-<target>-<hash>/xara`, the hash being that of the
  patches, of `build.sh` and of the linker (its version, target and C library); a second run prints that path and
  downloads nothing. Beside it `doc/`: XARA's `LICENSE`, `THIRD-PARTY` (each crate compiled in, from `cargo tree`,
  with its MIT text) and `rust-std-COPYRIGHT.html.gz` (the Rust standard library's notices).
- `buildroot/local-build.sh` passes the path to the GUI's CMake (`-DXARA_BINARY`), which installs it as
  `/usr/bin/xara` and `doc/` as `/usr/share/x6100/licenses/xara`: in the card image and in the update bundle.
- `build.sh --host` builds the same for the PC (x86_64), for `tools/winlink`'s tests: XARA to XARA through the GUI's
  socket code, with Pat.

The upstream ARMv7 release binary is not used: it links PulseAudio and ALSA, has neither our backend nor the
`registered` feature.

## The local patch (`patches/`)

Made with `git format-patch` against the release's source; `git apply` takes it.

- `0001-x6x00-socket-backend.patch` (MIT, as XARA): `--audio x6x00` and `--ptt x6x00`, the radio's GUI as XARA's
  sound card and PTT through the GUI's Winlink modem socket (`src/winlink.c`, the one ardopcf and Mercury use;
  `crates/xara/src/audio/x6x00.rs`, behind the cargo feature `x6x00`). `--capture` and `--playback` are both the
  socket, `/run/ardop.sock` on the radio. A thread connects, and again every second while the GUI is not there, and
  says hello in protocol 2: 12000 Hz, keys of 30 s at most (XARA's own `--ptt-max-s`). The receiver comes as R
  messages of 20 ms, up from 12 kHz to XARA's 48 kHz with XARA's own resampler; `read()` is paced by them and gives
  zeros at the wall clock's pace while the radio transmits (the GUI sends no R then) or the GUI is away. XARA's
  audio goes down to 12 kHz and out as T messages only between a P 1 and its P 0, which the PTT sends under the same
  lock: the GUI keys exactly what it was asked to, through tx_player and its checks (the amateur bands, simplex,
  the power caps, the SWR hold, a digital app open). What XARA's audio loop writes in the moment before its PTT
  thread keys is held and sent right after the P 1. After P 0 the GUI plays out every T it has, then unkeys, so a
  short PTT tail is enough (`ptt-tail-ms = 20` from the supervisor: the resampler's last samples). XARA times its
  replies by its audio path: `path()` gives the capture (the GUI's 20 ms fragments and the resamplers), the key (the
  GUI's checks and the base keying) and the GUI's low-latency playback stream (prebuf 60 ms, tlength 100 ms) as 25,
  50 and 80 ms, guesses until they are measured on the radios; `--x6x00-path-ms CAPTURE,KEYING,OUT` sets them (the
  supervisor's `xara_path_ms=`). No rate matching (`queue: false`): during receive XARA's clock is the GUI's capture.
  `main.rs` checks the options together, as upstream does for its trusdx backend. With `--audio x6x00` the station
  starts with listening off (`listen_at_start`): it answers no call until its host sends `LISTEN ON`, which Pat sends
  only to listen, never on the radio. Upstream starts listening, until a host says `LISTEN OFF` or goes; on the
  radio XARA runs from the start with no host until Pat calls out, and an answer would key the radio with nobody
  there. With unit tests on a fake GUI (the hello, R paced and resampled, zeros while keyed and without the GUI, T
  only between P 1 and P 0, the held first block, a GUI that goes and comes back, one that closes at once tried once
  a second and not more, XARA's stop sending P 0) and `main.rs`'s own (a call to a station made for the radio is not
  answered; the same station with another backend answers it). One upstream test took the default backend (ALSA)
  for built; in a build with `x6x00` alone it is not, and the test now allows that. `cargo test` of the workspace
  with the patch: 465 passed (pulse, alsa, registered, x6x00), and 187 of the `xara` crate with `x6x00` alone,
  2026-11-04.

## Free speed, and the `registered` feature

VARA HF without a licence runs its free speed levels; with one, all of them. On the air the difference is one bit
in the caller's connect frame: the callee follows the caller's, and nothing else is checked (XARA's author,
x0rro/xara#9). XARA has the same: without its `registered` cargo feature, or with it but not started with
`--registered` (a `registered` line in its config), a link XARA starts runs VARA HF's levels 1 to 4
(`UNREGISTERED_MAX_LEVEL`) without compression; with it, up to the top level of the bandwidth.

XARA's author builds his releases without the feature "out of respect for the VARA author", so that nobody can
download a binary from him and use VARA's paid speed without a licence, and does not document how to build it
(x0rro/xara#9, #31). His MIT licence allows building it. The owner's decision for OpenX6X00 (4 November 2026): the
feature is built in, and off. The radio runs XARA at the free levels; only when the user ticks the Winlink page's
"VARA HF licence" box, their own statement that they hold a VARA HF licence, does the supervisor add `registered`
to XARA's settings. Nothing on the radio checks the statement, as nothing in VARA does; the page says so. DigiPi is
reported to do the same with a tick box (yo4cil on OpenX6X00#120; its public repository predates XARA). The radio
always calls (it answers no call: the patch starts XARA with listening off and Pat never turns it on there), so its
bit is the one that counts on its links.

XARA is an independently written program that speaks VARA's protocol. The owner's view (4 November 2026): reverse
engineering for compatibility with a protocol is lawful in Europe, so XARA is fine to ship.

## The CW ID

Not changed by the patch. XARA 0.1.3 answers Pat's `CWID ON` with OK in HF and does not act on it (`tnc.rs`:
`Command::Gateway | Command::Cwid(_) => M::CWID_GATEWAY`; the HF session's `cwid` is set only in its tests). It
sends a CW ID at the end of a session only when its callsign is of the US kind (`is_us_call` in
`crates/xara-phy/src/call.rs`: roughly, 4 to 6 letters and digits, one digit as the second or third character,
starting with K, N or W, or with AA to AL), so with a call from outside the US there is none. `make modems` shows
both: no CW ID at the free speed, with ON4AAA and ON4BBB; one at full speed, with N0AAA and N0BBB.

## Licences of what is compiled in

- XARA: MIT, Copyright (c) 2026 x0rro.
- The crates (`cargo tree -e normal` of the build): adler2 2.0.1 (0BSD OR MIT OR Apache-2.0), libc 0.2.191,
  miniz_oxide 0.8.9 (MIT OR Zlib OR Apache-2.0), num-complex 0.4.6, num-integer 0.1.47, num-traits 0.2.19,
  primal-check 0.3.4, rustfft 6.4.1, strength_reduce 0.2.4, transpose 0.2.3 (MIT OR Apache-2.0): each used under
  MIT. alsa-sys and pkg-config are in the vendor tarball but not compiled in.
- The Rust standard library: Apache-2.0 OR MIT, with the notices in its `COPYRIGHT-library.html`.

All three in `/usr/share/x6100/licenses/xara` on the radio.

## A newer version

XARA moves fast: 0.1.4 is announced as "a significant departure from the design of 0.1.3 (libxara is coming)", so
the patch will need a rebase, or a program of ours that links libxara as `x6x00-mercury` links Mercury. A new pin
is a new test: `cargo test`, `tools/winlink`'s `make modems`, the bench radios, then the air. Change the version,
the commit and the two hashes in `build.sh` (and the Rust version and its four hashes if `.rust-version` moved),
rebase `patches/`, and update this file.
