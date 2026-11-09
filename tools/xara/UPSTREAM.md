# XARA (built from source, patched)

XARA is a VARA-compatible HF and FM modem written in Rust by x0rro, with VARA's TCP host interface (commands on
8300, data on 8301). x0rro discontinued it on 8 November 2026 (x0rro/xara is now one README commit, its releases and
issues gone); NR5P (Nathan Rigg) carries it on in his fork, which OpenX6X00 follows from then on (the owner's
decision, 9 November 2026). On the radio it is the third Winlink modem, experimental: Pat talks to it through its
VARA HF transport (varahf), as to Mercury, and through it to the VARA HF gateways of Pat's list. It is an
independent program that speaks VARA's protocol on the air, not VARA itself.

Source: https://github.com/NR5P/xara (MIT; a fork of 9M2PJU/xara, which is x0rro's 0.1.0 and the root of the fork
network since x0rro's repository went; the network still has x0rro's commits, 0.1.3's included)
Pinned: NR5P's main of 9 November 2026, commit `bd8300a36e4f76fae7d561335bf08649474bdc6d` (the merge of NR5P/xara#1),
whose `Cargo.toml` says 0.1.2: NR5P's own numbering, not x0rro's 0.1.2 (below). The fork has no release tarballs
(its tags v0.1.0 to v0.1.2 and android-test-1 have no assets), so the source is GitHub's tarball of the commit,
`https://codeload.github.com/NR5P/xara/tar.gz/<commit>`:

- the tar in it (what `build.sh` pins): `3aa6bc2cf8e175b373ded6bde1554b0adf98677bc2cd395c5df803c16c67b520`. It is
  `git archive`'s, byte for byte the same from any clone: `git archive --format=tar --prefix=xara-<commit>/
  <commit>` (git 2.56.0, 2026-11-09).
- the `.tar.gz` as GitHub gave it on 9 November: `b845659fc78c046f3e98e0d1ab4a827d4b0835ed92ece143479305173fce52e6`
  (gzip's bytes are GitHub's own, so not pinned).

The crates come from crates.io, as its `Cargo.lock` names them: `cargo fetch --locked --target <target>` into cargo's
cache (`$HOME/.cache/openx6x00/xara/cargo`), and cargo takes a crate only with the SHA-256 the lock gives it. For
either target that is 13 crates (the lock's Windows crates, and the proc macros under them, are not fetched); the 10
compiled in are those of x0rro's 0.1.3 vendor tarball, the same versions with the same SHA-256.

License: MIT, Copyright (c) 2026 x0rro (`LICENSE` in the source, as NR5P's fork has it).

Not vendored: `build.sh` downloads the source tarball and the crates at build time and checks them. `build.sh`,
`patches/` and this file are ours.

## NR5P's fork and x0rro's 0.1.3

x0rro's releases were one commit each: 0.1.0 `549bf6a5` (21 October), 0.1.1 `2e96fcd1` (25 October), 0.1.2
`5ffffbb8` (29 October), 0.1.3 `09a92013` (31 October; its release tarball, which v0.3.9 was built from, is that
commit without `.gitignore`). NR5P's main is x0rro's 0.1.1 and NR5P's own work on it: the macOS port (x0rro took it
into 0.1.3 as x0rro/xara#19, in a version of his own), the iOS, Android, Windows and Linux apps and `xara-ffi` (the
modem as a library, for those and for NR5P's OpenSlot), the (tr)uSDX's audio over CAT, the BUSY detector that
learns the receiver's noise shape, the modulation names, `--monitor`, `--audio-latency-ms`, `--debug-log`,
`registered` among the default features (off until `--registered`), the program moved into `daemon.rs`; from x0rro's
0.1.2 the SSID encoding only; then a PTT by a tone on the right channel (`--ptt tone`, Core Audio and AAudio only),
the WASAPI and Core Audio capture no longer cut for a dry playback, CI and `docs/building-and-releasing.md`.

What x0rro's 0.1.2 and 0.1.3 have that it lacks, and what of it the radio needs:

- The modem core: FM's reply windows (x0rro/xara#20), sine tables, a shared FFT plan, FFTs in place, FM OFDM's
  catch-up search in parallel, `warm()` for the tables, the PTT tail in the reply window (x0rro/xara#28), the
  scheduler's capture lag. NR5P has ported them on its branch `upstream-0.1.3-fixes` (`0cb88b94`, `3838b398`, 6
  November), not merged into main: patch 0001. With it `xara-arq` and `xara-phy` are 0.1.3's but for NR5P's BUSY
  detector and modulation names.
- The resampler of 0.1.2: patch 0002. NR5P's is still 0.1.1's, far too slow for the radio (below).
- 0.1.2's and 0.1.3's audio path for PulseAudio and ALSA (rate matching, `--tx-buffer-ms`, the TX latency model of
  `txpath.rs`) and the "xara is falling behind real time" line: the radio's build has neither backend. Our backend
  (0003) gives its path in NR5P's terms and says that line itself, as the supervisor reads it.
- `--log` (NR5P has `--debug-log`): the supervisor takes XARA's console instead.
- The host interface: the same commands and replies (`xara-host`'s `command.rs` and `response.rs` are 0.1.3's). 0.1.3
  reports a host gone only once its port listens again, NR5P (as 0.1.1) just before; Pat keeps its connection, and
  `tools/winlink` tries again a client refused (`xara_client`). 0.1.3's bouncer is for macOS and FreeBSD.
- FreeBSD, the HID and macOS work: not for the radio.

What the radio relies on is otherwise as in 0.1.3: the host interface on 8300 and 8301 and the console's echo of it
(`   12.345 < REGISTERED`, which the supervisor reads), the `registered` feature and the levels and bandwidths
(`xara-phy`'s tables and `UNREGISTERED_MAX_LEVEL`, the same; NR5P adds the names), every setting the supervisor
writes. Neither acts on `CWID ON` nor gives a cancelled call up at once: 0004 and 0005 stay, and their tests fail on
NR5P's main without them. `--version` says `xara 0.1.2`.

## The build (`build.sh`)

- Rust: the official toolchain at 1.99.1 (2026-10-15), the version x0rro's 0.1.3 was built with (its
  `.rust-version`; NR5P's fork has none: it needs 1.85 or newer, edition 2024, and its CI runs stable and 1.85).
  Buildroot's Rust (1.67.1 in both trees) is too old, so XARA is built outside buildroot, as Pat is. From
  static.rust-lang.org, each part's SHA-256 checked before it is unpacked: `rustc`
  `2ff05ff9c08a90861e7f6e4065b40a8b87463b4576186fd613871c3e37f2091f`, `cargo`
  `53f3521949404d595efb06a55c12c5658d4ed1c75566f33d3d514eaebe70bd8a`, `rust-std` for x86_64
  `109b074225bf5b9ea3820890876bfdf4ed161adbfaf94b2b4b5984c85ee5d958` (build scripts) and for
  armv7-unknown-linux-gnueabihf `53fdf6581a24f510def5173813a0d23518a8cde547b9fbf4876499f19feda18c`, unpacked into
  `$HOME/.cache/openx6x00/rust-1.99.1` (in the container `/work/.brhome/.cache`), about 720 MB, the downloads 150 MB.
- The crates: `cargo fetch --locked --target <target>` (above), once. Offline from there: `cargo build --frozen
  --release -p xara --bin xara --target armv7-unknown-linux-gnueabihf --no-default-features --features
  registered,x6x00`, `Cargo.lock` as the source has it, no variable from the environment that steers Rust or cargo.
  No PulseAudio or ALSA (the radio's audio is the GUI's), our socket backend (`x6x00`, below) and `registered`
  (below). Linked with the gcc the caller names in `CC`: `buildroot/local-build.sh` gives the buildroot tree's own
  (`arm-buildroot-linux-gnueabihf-gcc`, glibc 2.36, the GUI's toolchain, after `make toolchain`).
- Checked: a 32-bit ARM ELF that needs only `libc.so.6`, `libm.so.6`, `libgcc_s.so.1` and `ld-linux-armhf.so.3`, and
  glibc 2.36 at most (it needs 2.35). Stripped, the build's directories left out of it
  (`--remap-path-prefix`): 2726788 bytes, SHA-256 `2713555fa2d96c847620edaa82df0a22db25083b21e36ee49c83c7d3e8e553c7`
  with the five patches, the same with the X6100 tree's gcc and the X6200 tree's, from builds in different
  directories (2026-11-09). Under qemu with either radio's libraries it starts, `--version` says `xara 0.1.2`, and
  it takes the settings the supervisor writes.
- Kept as `$HOME/.cache/openx6x00/xara/xara-0.1.2-bd8300a36e4f-rust1.99.1-<target>-<hash>/xara`, the hash being that
  of the patches, of `build.sh` and of the linker (its version, target and C library); a second run prints that path
  and downloads nothing. Beside it `doc/`: XARA's `LICENSE`, `THIRD-PARTY` (each crate compiled in, from `cargo
  tree`, with its MIT text) and `rust-std-COPYRIGHT.html.gz` (the Rust standard library's notices).
- `buildroot/local-build.sh` passes the path to the GUI's CMake (`-DXARA_BINARY`), which installs it as
  `/usr/bin/xara` and `doc/` as `/usr/share/x6100/licenses/xara`: in the card image and in the update bundle.
- `build.sh --host` builds the same for the PC (x86_64), for `tools/winlink`'s tests: XARA to XARA through the GUI's
  socket code, with Pat.

No upstream binary is used: NR5P's fork has none for the radios, and the ARMv7 one x0rro released linked PulseAudio
and ALSA and had neither our backend nor the `registered` feature.

## The local patches (`patches/`)

Made with `git format-patch`; `build.sh` applies them in order with `git apply`.

- `0001-modem-core-from-xara-0.1.3.patch` (MIT, as XARA): NR5P's branch `upstream-0.1.3-fixes` as it is, its two
  commits by Nathan: x0rro's 0.1.2 and 0.1.3 changes to `xara-arq` and `xara-phy` ("Modem core from upstream xara
  0.1.2 and 0.1.3: FM timing, speedups", `0cb88b94`) and the PTT tail allowed for in the reply window, with the
  scheduler's capture lag (`rx_lag`) and `set_tx_latency` ("Allow for the PTT tail in the reply window (from upstream
  xara 0.1.3)", `3838b398`). It applies to main as it is. So the modem on the air is 0.1.3's, as in v0.3.9, but for
  NR5P's BUSY detector: the speedups count on the radio's processor, and `warm()` takes the tables' cost off the
  first frame.
- `0002-resampler-from-xara-0.1.3.patch` (MIT, as XARA): 0.1.3's `resample.rs` (made in 0.1.2), without the
  `silent()` its rate matcher used: a table of the windowed sinc at 512 phases, the taps between two interpolated.
  NR5P's (0.1.1's) works out each tap's sinc and Kaiser window, a 30-term Bessel series, for each sample. Our backend
  resamples the receiver from 12 to 48 kHz all the time and the transmitter from 48 to 12 kHz while keyed: on this PC
  (x86_64), 10 s of audio in 523 and 497 ms with 0.1.1's, 12 and 11 ms with 0.1.3's; the radio's Cortex-A7 is many
  times slower.
- `0003-x6x00-socket-backend.patch` (MIT, as XARA): `--audio x6x00` and `--ptt x6x00`, the radio's GUI as XARA's
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
  replies by its audio path: the capture (the GUI's 20 ms fragments and the resamplers), the key (the GUI's checks
  and the base keying) and the GUI's low-latency playback stream (prebuf 60 ms, tlength 100 ms), 25, 50 and 80 ms,
  guesses until they are measured on the radios; `--x6x00-path-ms CAPTURE,KEYING,OUT` sets them (the supervisor's
  `xara_path_ms=`). In NR5P's terms the backend's `latency()` is the round trip, 155 ms, and a new
  `capture_latency()` its capture's 25 ms, which goes to 0001's `rx_lag`: the PTT is held after a transmission for the
  keying and the playback only, 130 ms, as 0.1.3 held it (0.1.3 also added the mean capture backlog it measured,
  about 5 ms with the GUI's 20 ms blocks; NR5P's daemon takes the latency once, at the start). The capture waiting to
  be read is said in 0.1.3's words past 100 ms and at each doubling ("xara is falling behind real time"), which
  NR5P's pump does not say; it has "audio loop slow" lines of its own, which come at the start of many keys too,
  where a read still waits for an R the GUI no longer sends (0.1.3 waited the same, and said nothing). The PTT comes
  from the backend through a new `Audio::ptt()`. `daemon.rs` checks the options together, as for the (tr)uSDX. With
  `--audio x6x00` the station starts with listening off (`listen_at_start`): it answers no call until its host sends
  `LISTEN ON`, which Pat sends only to listen, never on the radio. Upstream starts listening, until a host says
  `LISTEN OFF` or goes; on the radio XARA runs from the start with no host until Pat calls out, and an answer would
  key the radio with nobody there. With unit tests on a fake GUI (the hello, R paced and resampled, zeros while keyed
  and without the GUI, T only between P 1 and P 0, the held first block, a GUI that goes and comes back, one that
  closes at once tried once a second and not more, XARA's stop sending P 0, the latency, the backlog said) and
  `daemon.rs`'s own (a call to a station made for the radio is not answered; the same station with another backend
  answers it). Rewritten on NR5P's audio interface from the patch v0.3.9 had on 0.1.3 (its `path()`, `backlog()` and
  `queued()` are not in NR5P's), its socket code the same.
- `0004-cwid-from-the-host.patch` (MIT, as XARA): XARA acts on its host's `CWID ON` and `CWID OFF`, which it answers
  OK and drops (below). `CWID ON` sets the HF session's `cwid`, so XARA identifies in CW as each session closes,
  whatever the callsign; `CWID OFF` clears it. Without either nothing changes, and a US call identifies after `CWID
  OFF` too, as before; VARA FM still answers WRONG. Pat sends `CWID ON` each time it starts its varahf modem. The
  owner's decision (4 November 2026): the radio identifies with any callsign. Made on x0rro's 0.1.3 (4 November) and
  prepared for him, it applies to NR5P's main as it is, without 0001-0003, and its test in upstream's own harness
  (`cwid_follows_the_host` in `tests/host_protocol.rs`) fails on NR5P's main without it.
- `0005-cancelled-call-ends-at-once.patch` (MIT, as XARA): a call its host cancels (`DISCONNECT` or `ABORT` while
  calling) gives up at once, as when its last reply window ends: `UnableToConnect`, the CW ID if there is one, then
  `DISCONNECTED`; when its request is still on the air, as the request ends, the ID in the same key. XARA only used
  up the call's retries, so the call still waited out its reply window, then identified (below). On top of 0004 (its
  test sits beside 0004's); with 0004 it too applies to NR5P's main alone, and its test
  (`a_cancelled_call_ends_at_once`) fails there without it: the ID keyed 1.28 s after `DISCONNECT`, in a key of its
  own.

`cargo test` with the five patches (Rust 1.99.1, 2026-11-09): the workspace with pulse, alsa, registered and x6x00,
444 passed (7 ignored); the `xara` crate with `registered` and `x6x00` alone, 169 (5 ignored).

`tools/winlink`'s `make modems` has not run on this pin yet: it needs a Pat for the PC. Its parts without Pat, and a
session driven on XARA's host ports as Pat drives it, did on 9 November: two stations through `winlink_host`, started
as the supervisor starts them; B answered nothing and keyed nothing before its `LISTEN ON`; the cancelled call's ID at
once, on the call's frequency; 2368 bytes A to B at the free speed (level 4, `LINK UNREGISTERED`, 143 s) and with
`registered` on A (levels up to 11, `LINK REGISTERED`, 28 s); both stations' CW IDs played whole; no key stopped by
the GUI, the longest 8.7 s.

## Free speed, and the `registered` feature

VARA HF without a licence runs its free speed levels; with one, all of them. On the air the difference is one bit
in the caller's connect frame: the callee follows the caller's, and nothing else is checked (XARA's author,
x0rro/xara#9). XARA has the same: without its `registered` cargo feature, or with it but not started with
`--registered` (a `registered` line in its config), a link XARA starts runs VARA HF's levels 1 to 4
(`UNREGISTERED_MAX_LEVEL`) without compression; with it, up to the top level of the bandwidth.

x0rro built his releases without the feature "out of respect for the VARA author", so that nobody could download a
binary from him and use VARA's paid speed without a licence, and did not document how to build it (x0rro/xara#9,
#31, gone with his repository). His MIT licence allows building it. NR5P's fork builds it in by default, off until
`--registered`, with a "Registered (VARA licence)" switch in its apps. The owner's decision for OpenX6X00 (4
November 2026): the feature is built in, and off. The radio runs XARA at the free levels; only when the user ticks
the Winlink page's "VARA HF licence" box, their own statement that they hold a VARA HF licence, does the supervisor
add `registered` to XARA's settings. Nothing on the radio checks the statement, as nothing in VARA does; the page
says so. Another setup is reported to do the same with a tick box (yo4cil on OpenX6X00#120). The radio always calls
(it answers no call: patch 0003 starts XARA with listening off and Pat never turns it on there), so its bit is the
one that counts on its links.

XARA is an independently written program that speaks VARA's protocol. The owner's view (4 November 2026): reverse
engineering for compatibility with a protocol is lawful in Europe, so XARA is fine to ship.

## The CW ID

XARA (x0rro's 0.1.3 and NR5P's main alike) answers Pat's `CWID ON` with OK in HF and does not act on it (`tnc.rs`:
`Command::Gateway | Command::Cwid(_) => M::CWID_GATEWAY`; the HF session's `cwid` is set only in its tests). It sends
a CW ID at the end of a session, and after a call that got no answer or was cancelled (`UnableToConnect`, then
`close(0)`), only when `cwid` is set or its callsign is of the US kind (`crates/xara-arq/src/hf/session.rs`,
`cwid()`; `is_us_call` in `crates/xara-phy/src/call.rs`: roughly, 4 to 6 letters and digits, one digit as the second
or third character, starting with K, N or W, or with AA to AL), so with a call from outside the US there was none.
Patch 0004 makes `CWID ON` set `cwid`, so with Pat XARA identifies with any callsign; the US rule stays as it is.

A call the host cancelled (`DISCONNECT` or `ABORT` while calling) only had its retries used up: it waited out its
reply window (about 1.3 s after the request at 2300 Hz), then gave up and identified. Pat's force disconnect (the
second click on a call it is cancelling: Pat-VARA's `Abort` says `DISCONNECTED` itself) ends Pat's dial at once, and a
second later Pat tunes the radio back to where it was before the call. The GUI keys the modem where Pat last tuned
(`tx_off_freq`), so the ID went out there, on a frequency nobody chose for the call. Patch 0005 gives the call up at
once: the ID keys straight away, or in the same key as a request still on the air, and the GUI holds a tune asked
during a key until the key is over (`rig_tune`), so the ID goes out on the call's frequency.

The ID (`crates/xara-phy/src/cwid.rs`, `station/hf.rs`): the callsign without its SSID, at about 28 words a minute
(units of 2048 samples at 48 kHz), 9.6 dB under the drive. The QRT that ends a link says whether its sender will
identify (code 3, or 46 without); it sends its ID right after it (in the same key on the PC). After a 3 the other
station waits out that ID, keyed and silent, for 9 to 12 blank words (`QRT_CWID_BLANKS`, about 3 s), then sends its
own; after a 46 it sends its own at once. On the radio the ID is XARA's audio like any other: T messages between a
P 1 and its P 0 on the GUI's socket, keyed by the GUI only after its checks (`src/winlink.c`, `tx_refused`,
`tx_off_freq`, `swr_held`), played through `tx_player` at the radio's power and digital level, and stopped by
`tx_abort` like any key. The silent wait does not move that level: the GUI's level loop (`tx_level.c`) takes no
readings in whole blocks of zeros nor in the 0.75 s of audio after them. Before it did, and each such key took the
level all the digital apps share up by 1-2 dB (`tools/txlevel`: on a model X6100 towards where the ALC reads 3, on a
model X6200 to the codec's top).

`make modems` shows it (0.1.3 with the patches, 4 November): with ON4AAA and ON4BBB at the free speed both stations
identify (the last keys 4.1 and 5.9 s, played whole), as with N0AAA and N0BBB at full speed; A's call before any
Pat, whose host sent no `CWID ON`, ends without one with ON4AAA. With `CWID ON`, A's next call is cancelled in its
reply window (`ABORT`) and the radio tuned elsewhere half a second later: its ID is the only key after the `ABORT`,
played whole on the call's frequency, and the radio goes where it was tuned after it.

## Licences of what is compiled in

- XARA: MIT, Copyright (c) 2026 x0rro.
- The crates (`cargo tree -e normal` of the build): adler2 2.0.1 (0BSD OR MIT OR Apache-2.0), libc 0.2.191,
  miniz_oxide 0.8.9 (MIT OR Zlib OR Apache-2.0), num-complex 0.4.6, num-integer 0.1.47, num-traits 0.2.19,
  primal-check 0.3.4, rustfft 6.4.1, strength_reduce 0.2.4, transpose 0.2.3 (MIT OR Apache-2.0): each used under
  MIT. alsa-sys, autocfg and pkg-config are fetched (the lock's set for Linux) but not compiled in.
- The Rust standard library: Apache-2.0 OR MIT, with the notices in its `COPYRIGHT-library.html`.

All three in `/usr/share/x6100/licenses/xara` on the radio.

## A newer version

x0rro's 0.1.4 ("libxara") will not come. A new pin is NR5P's main at a newer commit, and a new test: `cargo test`,
`tools/winlink`'s `make modems`, the bench radios, then the air. Drop 0001 once NR5P merges `upstream-0.1.3-fixes`
into main (0003 to 0005 are made on top of it), 0002 once its resampler is 0.1.2's, 0004 once it acts on `CWID ON`
itself (with that patch or a change of its own), 0005 once a cancelled call gives up at once there (it waits the same
way with a US call); 0004 and 0005 are not offered to NR5P yet. Keep `make modems`' CW ID checks. Change the commit,
the tar's SHA-256 and the version its `Cargo.toml` says in `build.sh` (and the Rust version and its four hashes if
the fork's needs move past 1.99.1), rebase `patches/`, and update this file.
