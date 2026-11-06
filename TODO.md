# To do

## Waterfall: the 3D band scope on the radios (PR #161, merged)

Merged on main 5 November (Calin Rigo, timerealm, PR #161, squashed): Settings > Display > WATERFALL > 3D, all
drawing in src/waterfall_view.c (its "3D" section), for the DRM plane and the LVGL image. On by default since 5
November (the owner's call): a radio that never switched it starts in 3D, one switched off keeps its stored 0; under
the Amber Trey display it stays off. Run on the PC build only: d3_render() 0.18-0.65 ms a picture there (our timing, his
~175 us), the GUI thread from 46 % to 84-88 % of a PC core with 3D on, most of it LVGL drawing the picture, as the PC
has no DRM planes. The radios take the DRM path, which nobody has run yet; drm_wf_present_ring() then runs up to 30
times a second instead of 15. 1.5 MB for the background, allocated the first time 3D is on.

- **Before the next release, on an X6100 and an X6200, 3D on and off**: the DRM path (the picture the right way round,
  the labels at the front edge, the 2D waterfall back whole after 3D, a theme change and a new spectrum height with 3D
  on); CPU with `top -d 1 -p $(pidof x6100_gui)` in 2D and 3D receiving on a busy band, the dial turning fast, depth
  64, FT8 open, 3D glide on and off; FT8 decodes and CW keying with 3D on; the 1bit theme and the MFK 4:4 page; Hide
  buttons on (the front edge down to the bottom while hidden); walkie mode and the screen dark draw nothing (read from
  the code only), a remote viewer still gets the picture. If 30 pictures a second are heavy on the X6100, 3D glide off
  is the step at each row. He has no X6100; his `top` numbers from an X6200 may come first. As it is on by default: an
  update from v0.3.8 starts in 3D, and a radio with 3D switched off stays flat after the next update. So the release
  waits for this check on both radios. If the DRM path draws wrongly, takes the CPU FT8 needs, or the GUI falls over
  in 3D, the default goes back to 0 for that release: a GUI that does not stay up cannot switch it off (the web
  Display page goes through the GUI's socket), only wf_3d = 0 in params.db on the card would.
- **After that**: 3D shots for the README gallery from the radio, on an antenna. The front labels sit under the Knob
  info lines at the bottom left (seen on the PC build, zoomed in).

## Screensaver: Matrix rain on the radios

Settings > Display > BACKLIGHT, Screensaver and Screensaver (min) (src/screen_saver.c, the times src/screen_idle.c):
the rain of the VFO-Knob's screensaver after 5 minutes without use, never dark until Screen off is set; a radio whose
Screen off was changed keeps its moment (migrations.c _16). An LVGL object over the screen, the GUI under it not
drawn, the waterfall, 3D and the spectrum paused. Run on the PC build only: 14 pictures a second, ~90 000 pixels and
~40 areas each, 0.18 ms a picture of LVGL's own work there (lv_timer_handler with the screensaver up, less the SDL
flush and the dark screen's), so some 4 to 8 % of an A7 core by the usual factor of 15 to 25, plus the DRM copies
(MAX_RECTS raised to 128 for its two areas a column).

- **On an X6100 and an X6200**: `top -d 1 -p $(pidof x6100_gui)` with the screensaver up (Screensaver (min) 1), lit
  in 3D and dark, receiving with FT8 open; the rain on the panel (the overlay plane the right way round, no GUI
  showing through, the top layer's messages hidden, every glyph whole: tools/screen_saver draws it the radios' way
  off the radio), RF.Guru, 1bit and Amber Trey; the first key, a knob turn (all of it) and a hand mic key only bring
  the screen back; FT8 auto's TX and an SWR scan end it at once and none comes while on the air; saver then dark,
  then a key: the GUI at once, not the rain, the waterfall and the spectrum current; POWER on it, a theme change
  from the web page and the walkie mode under it; a radio updated with Screen off 3 goes dark at 3 with the rain
  from 2, on FT8 auto too. If the X6100 finds it heavy: a slower rain or fewer columns (the work follows the cells
  the columns move over a second, hardly the pictures a second).

## X6200: the short pulse at the start of each transmission, register 32 (#53)

Seen 5 November on an external transient analyser: the X6200 sends a narrow RF pulse at the start of each
transmission, well above the steady level at low power; another radio is clean. Why is not known. On main
(unreleased): at each start the X6200 sets its base's register 32 to 0x0a, as Xiegu's firmware does (the X6200
control library's copy of the stock app's writes, commented out in its low/control.c: 0, then 0x0a, between the CW
keyer's register and the QSK time's); before, it stayed at the library's 0. What the register does is not known, a
TX timing value maybe. Host test tools/hw_x6200: the real hw_x6200.c and control library on a base of the test's.
- **Next**: the same captures with this build (the GUI log says "base: register 32 = 0x0a, as Xiegu's firmware" at
  the start), at low power and at 8 W, SSB and CW: the pulse gone, smaller, or as it was. As it was: register 32 is
  not the cause, and it stays at Xiegu's value.

## FT8/FT4/FT2: answer other stations' CQs by themselves (when not worked yet)

Asked 2026-10-24. Today Auto answers the stations that call *you*, also while you call CQ (what
other programs call "Res"), and sends the next message of a QSO by itself; it never answers
somebody else's CQ (docs/MANUAL.md, FT8 section, "Auto"). The wish: an automatic hunt mode that
answers a CQ it decodes when that station is not in the log yet for this band and mode.

- **Worked before, with a time window**: "not in the log" counts QSOs within a window the
  operator chooses: this band and mode ever, this year, the last month, the last week, today.
  The log already answers worked / worked in this mode for a call and band
  (`qso_log_search_worked()` in src/qso_log.c, used for the list's colours); it needs a date
  limit (`QSO_DATE`/`TIME_ON` in the qso_log table) and the window as a setting.
- **Which CQs**: honour the CQ's modifier (`CQ DX` from the own continent, `CQ NA`, `CQ POTA`...
  only when it applies), skip stations already being worked, prefer the strongest or a POTA/
  SOTA/TOTA... activator (the cluster's activity and the FT8 POTA tag), never answer while a QSO
  of our own is going on, keep Hold Freq and TX Call as they are.
- **Safety**: an explicit switch, off at every start (the radio transmits by itself), visible on
  the screen and in the browser panel; a stop after N answers without a reply or after a time;
  the same retries as a normal QSO; logged as usual.
- **Other digital modes**: FT4 and FT2 share the FT8 app and come with it. JS8 deliberately
  never transmits to the group by itself (MANUAL, JS8 section): an auto answer there would be a
  separate decision. RTTY/PSK are keyboard modes and WSPR a beacon: not candidates.
- **Gatherer first** (#156, asked 2026-11-06 as the priority): CQ again by itself once a QSO is logged and the RR73
  or 73 has gone out, while nobody else calls; the same switch, time limit and stop on any key or knob as above.
  The smaller half, so it can come before the hunt.

## X6100: the patched base (gdyuldin/x6100_base_patch r13) on radios that came with base V1.1.8

Tested 2026-10-24 on the bench X6100 (shipped with stock base V1.1.8), then put back on stock V1.1.8,
where it stays for now. Backups, test tools and full measurements: ../x6100-baseband-backup/ on the
build PC (tx-test-2026-10-24.txt, i2c-baseline-2026-10-24.txt) and /mnt/baseband/ on the radio.

- **Flashing from OpenX6X00 works**, no SD card swap: fventuri/x6100-base-experiments
  `base-flasher/x6100_base_flash.py --gpio-backend sysfs --no-verify-addr` (our libgpiod CLI is v1, the
  flasher's cli backend expects v2), GUI stopped first, external power. r13 and back to stock each
  about a minute. The base cannot be read back: Xiegu's official X6100_BBFW_V1.1.8_240915003.xgf
  (same build string) is the way back.
- **What r13 changes here** (the radio's own meter, FM carrier into a dummy load):
  - TX level about 10 % high at 5 W and 12 W or more at the 10 W setting before calibration (stock: 4.9 /
    10.9 W, ALC holding it); "TX codec gain" alone does not fix it, the bands drift apart, per-band
    "Band output gain" does (160, 40, 20, 15, 10 m calibrated within -1.3..+1.2 dB at codec -3 dB;
    partial values in /mnt/baseband/r13-partial-calibration.txt).
  - **The first carrier after a band change sometimes has next to no power** (6 m nearly always; 17, 80,
    30, 12 m now and then); the second is fine. Never seen on stock in 40 tries. This blocks a full
    calibration and would send a first over on a new band with no power.
  - External supply voltage reported as 0 V (the GUI then shows a battery runtime instead of external
    power); whether charging still works on r13 is untested.
  - The calibration window over I2C (0xF000) reads zeros on r13; the stock calibration came back intact.
  - Linux-side I2C to the base: no errors (1000 reads), RTC fine; RX not compared (no antenna on the
    bench).
- **To look at later**: whether the band-change dropout is the patch's band/PA switching on this board
  revision (issue #3 reports I2C trouble on V1.1.8 radios, base-side), a TX hold-off after a band change
  in the GUI as a workaround, the external-voltage report, and Xiegu's stock V1.1.9 base (found unflashed
  on the radio's eMMC: OpenX6X00 already enables zoom and IF shift for it) as the low-risk alternative.

## CESSB in the base firmware, X6100 and X6200 (later: once there is a stable build)

Asked 2026-10-25 (CESSB first, the other TX options when they come for free); the owner, 5 November: CESSB is only
possible in the base's firmware, so this waits until OpenX6X00 has a stable build. CESSB (controlled-envelope SSB,
W9GR) gives SSB more average power without splatter: the SSB envelope is clipped and the overshoot the filtering
brings back is controlled. Today only gdyuldin's patched X6100 base has it (x6100_base_patch rev 12 and up, built on
Xiegu's base V1.1.6; `HW_CAP_CESSB`, Settings > TX CAL), and r13 is not usable on V1.1.8 radios (the section above).
Xiegu's newest bases, X6100 V1.1.9 and X6200 1.0.8, have no CESSB, and the X6200's no TX filter either
(src/hw/x6200/hw_x6200.c, `hw_caps()`).

- **Not on the Linux side.** The SSB signal is made in the base: its SSB filter, ALC and PA come after anything
  the GUI could do to the audio, so an envelope controlled on the audio would not stay controlled (the filtering
  after the clipper brings the overshoot back), and the GUI cannot see or steer those stages: the base doesn't
  expose them. The idea of a CESSB chain on the voice path (the mic recorded by Linux and played into the
  transmitter as voice messages are) is dropped for that reason; the same goes for a wider SSB bandwidth (#69).
- **The way**: see whether the bases' firmware can be reverse-engineered, as gdyuldin did for the X6100 from V1.1.6,
  and CESSB added for both radios: X6100 V1.1.8/V1.1.9 and the X6200's 1.0.8. Reverse engineering for
  interoperability is allowed in the EU. Start from gdyuldin's patch (what it hooks, how the GUI controls it),
  then the X6200 base, which is another firmware.
- **With it, maybe**: r13's other TX options on the stock bases: a TX filter (the X6200 has none), the compressor's
  shape, FM pre-emphasis, a TX equaliser.
- **Before any image carries a patched base**: how it is flashed and rolled back safely (flashing a base needs the
  owner's go), and that a radio without it keeps working as today.

## FreeDV: RADE and the Codec 2 modes (#37, #52)

Asked 2026-10-27 (#37; #52 asked the same and was closed for it). FreeDV's RADE (https://github.com/freedv/rade_c,
with the FARGAN vocoder model from the Opus project) as a digital voice mode in APP, and FreeDV's older Codec 2
modes (1600, 700D) with it. No date.

- **Open work: draft PR #130** (jochenhammes, 2026-10-31, in place of #129): RADE V1 as APP > Modes > RADE, receive
  (the speech to Bluetooth headphones only) and send as record-then-send, like a voice message, through
  `tx_player_stream()`; RADE V2 and the Codec 2 modes are not in it. Run on a PC only so far. Before a merge: a run
  on an X6100 and an X6200 (its decode, synthesis and encode times, the GUI's CPU), its build (tools/rade/build.sh,
  run by hand for now) from local-build.sh with every download pinned and checked, the licence notices on the
  radio, V1 only for the size, and a TX power of its own instead of the level the digital apps share.
- **The audio path is there**: RADE is a modem in an SSB passband, and the digital apps (FT8, Winlink) already take
  the receiver's audio into Linux and transmit audio from Linux (`radio_set_modem()`). What is new: the decoded
  voice on the speaker while receiving, and the mic recorded in Linux while transmitting (`AUDIO_PLAY_VOICE_REC`),
  the same path the CESSB entry above needs.
- **CPU and memory are the question**: fventuri's multithreaded radae-nopy decodes in real time on an X6100, using
  both cores (https://github.com/fventuri/x6100-buildroot, board/X6100/patches/radae-nopy). The encoder is the
  heavy part (strijar, on a dual-core ARM), and transmitting needs it next to the GUI and the spectrum. RADE's
  developers have efficiency work coming (peterbmarks, 2026-10-30): measure again when it is out. The FARGAN
  model's memory on 1 GB.
- **Not counted on**: fventuri's base firmware that streams audio and IQ over USART6 to Linux and back is a base
  of its own, so a card image cannot rely on it.
- **Codec 2 1600 and 700D** need far less CPU (they run on a microcontroller): a first step that
  needs only the audio path.
- **Reporting**: FreeDV Reporter (https://qso.freedv.org), later.

## Winlink over HF: Pat with ARDOP on the radio, Mercury and XARA (#18, #19, #43)

Asked 2026-10-26 (issues #18 and #19). Winlink email from the radio alone, with a phone on its WiFi to read and
write it: Pat (getpat.io, Go, MIT) as the client, ARDOP as the HF modem.

- **In v0.3.6**: Pat v1.0.0 built with a pinned Go (tools/pat, a few patches: its page answers
  only to the radio's own names and refuses other sites' requests, no release or password-recovery check) and
  ardopcf (github.com/pflarue/ardop, MIT, C) in third-party/ardopcf, both in the image and the bundle. ardopcf's
  audio and PTT go through the GUI (src/winlink.c: a socket, 12 kHz, TX through tx_player), which also answers
  Pat as a rigctld on 127.0.0.1:4532: it tunes and sets USB-D. A supervisor (x6100-winlink, S98winlink) runs both
  while Winlink is on; the web configurator's Operating > Winlink sets the callsign and locator (the radio's by
  default), the Winlink password (root filesystem, 0600, never shown, not in the Backup) and the ARDOP bandwidth,
  and links Pat's mailbox on port 8080 ([manual](docs/MANUAL.md#winlink)).
- **On the air, reported** (#18, #80): sessions with RMS gateways work (KM4GKD X6100 80 m, M7SVA 40 m UK to
  DM6TS, a message each; EA5JTT's X6200 reached HB9MM and stopped at the unset password). M7SVA: the gateway
  list's 2000MAX did not connect, 200 did: the page's bandwidth now caps the list's (Pat patch 0003; 200, 500,
  1000, 2000 Hz, 500 the default).
- **In v0.3.7** (#43): the radio's state before a connection (/run/winlink/radio from the GUI, the
  page's Before you connect card), the outcomes kept apart (service, modem, session, last connection with what to
  do next, also in the mailbox's log through Pat patch 0004), no key while a digital app is open, a refused QSY
  said, the SWR hold's minutes, where the password is set, the logs as a download, Funnel refused by Pat.
- **Still to do on the bench / air**: the acceptance write-up for both radios (ARQ at 200, 500 and 2000;
  turnaround; per-frame power and ALC with the shared level steady; split, a dial turned, the 30 s key limit and
  an SWR trip at low power into a mismatch; CPU). A linearity check of ARDOP's multi-carrier frames
  (TWOTONETEST, TXFRAME 4PSK.1000.100 and 16QAM.2000.100 into the dummy load) decides whether ARDOP needs a drive
  below the FT8-learned level and whether to start ardopcf with `-H "FASTSTART FALSE"`. P2P listen stays off
  (Pat's own setting is undone at each start) until it has its own refusal rules and an on-air P2P test.
  ardopcf's maintainer has stepped back (its README asks for forks), so fixes may have to be ours.
- **Mercury** (github.com/Rhizomatica/mercury, GPL-3.0-or-later): faster OFDM with a VARA-style TCP interface,
  which Pat's varahf transport talks to. Some Winlink RMS gateways do run it, behind RMS Trimode's or BPQ32's
  VARA driver: KO0OOO (its banner gives 7.101.5) and K7EK-10, seen in Mercury's issues (#162, #384, #442); only
  Mercury 1.9.20 and later talk to each other. VARA itself is closed and Windows-only: out.
  - **Built, experimental, chosen on the Winlink page** (v0.3.7): v1.9.21 vendored as its daemon build
    (third-party/mercury, two local patches: the TNC and broadcast ports on 127.0.0.1, VERSION answered with a
    VERSION line so Pat cannot hang), linked into x6x00-mercury, which takes the audio and the PTT through the
    GUI's modem socket in its protocol 2 (8000 Hz, keys of up to 40 s; ardopcf stays on protocol 1). In the image
    and the bundle. Operating > Winlink's Modem (ARDOP or Mercury, one at a time) and Mercury's bandwidth (500,
    2300, 2750); x6100-winlink runs x6x00-mercury in place of ardopcf with /run/winlink/mercury.ini, Pat's varahf
    on 127.0.0.1:8300 (P2P listening off), Mercury's readiness and outcomes, its decoders falling behind ("decoder
    ring full", "RX backlog exceeded") said on the page and in Pat's log ([manual](docs/MANUAL.md#mercury-experimental)). tools/winlink
    tests it on the PC: Pat to Pat peer to peer over two stubbed radios (make modems), the supervisor's and the
    page's side (winlink_test.py, winlink_web_test.py).
  - **Go/no-go on the bench X6100, 2026-10-29** (dummy load): go, as experimental. With `top -H` the radio at
    38.6 % of its four cores with Mercury listening (17.6 % with ARDOP), 51 % at most in calls, no "decoder ring
    full" or "RX backlog exceeded" in mercury.log; keys of up to 13.5 s at 5.7-6.3 W on average, 7.0 W at most,
    the shared digital level from -23.58 to -22.08 dB. The X6100 has no temperature sensor.
  - **Next**: the same on the bench X6200; a 30-40 s key at up to 7 W, PTT to RF and unkey to RX audio, the
    drive level loop on OFDM (QAM16C2 needs a linear chain). Then on the air: P2P with a Mercury station, then a
    gateway (KO0OOO, K7EK-10) once the Winlink account works. Upstream reports to post: the VERSION reply (Pat's
    start race), the daemon build's two missing Android hooks, an option for the TNC ports' address, nanorq's and
    oblas's licence files, PUBLIC in TNC.md.
- **XARA** (github.com/x0rro/xara, MIT, a VARA-compatible modem in Rust; #120): built, experimental, the third
  Winlink modem (branch gh44-xara, 2026-11-04; the owner's decision of 4 November). v0.1.3 from its release
  tarballs with a pinned Rust 1.99.1 (tools/xara, its UPSTREAM.md), our patch 0001 for the GUI's modem socket
  (--audio x6x00, --ptt x6x00), 0002 for the CW ID with any callsign when Pat asks (CWID ON; the owner's decision
  of 4 November: 0.1.3 identifies only with a US call) and 0003, a call Pat cancels given up at once, its ID on the
  call's frequency before Pat tunes back. VARA's free speed by default; its `registered` feature is
  compiled in and used only with the Winlink page's VARA HF licence tick box, the user's own statement, which nothing
  checks. Tested between two stations on a PC only (make modems, eight runs: free, levels 2 to 4; ticked, the
  highest level 9 to 14).
  - **Bench** (the owner; no air yet): `top -H` on the X6100 and the X6200 with XARA listening and in calls (48 kHz
    f64 DSP next to the GUI; "xara is falling behind real time" in xara.log). The audio path XARA times its
    replies by: path()'s 25/50/80 ms (capture, keying, out) are guesses; measure with XARA's `--record` (a FLAC
    with PTT) against the GUI's log and the dummy load, per radio, then set them (xara_path_ms= for the test, the
    patch's PATH_MS after). VARA calling XARA leaves about 150 ms of margin for XARA's answer. A GUI playback
    underrun (XARA's audio comes just in time, the stream's 60 ms prebuf is the only cushion) would cut a frame:
    listen for gaps in the dummy load's audio. P2P between the X6100 and the X6200 into dummy loads at 2300 Hz,
    free and ticked; a 30 s key; the PA's linearity on the full-speed OFDM levels (PAPR 8-9 dB) at up to 7 W and
    the shared digital level on XARA's frames (drive-db 0). The CW ID with an ON call at each session's end and after
    a call with no answer, heard whole on a receiver; the station that did not end the link keys about 3 s silent
    first, waiting out the other's ID: tx_level takes no readings in silence nor in the 0.75 s after it
    (tools/txlevel). Check that key on its own, in the GUI's log (/tmp/x6100_log.txt, then app_logs/): each key that
    moved the shared level has a "tx_player: level A -> B dB, N windows decided in M blocks, S of them silent" line.
    The ID-wait key (S about 58 to 77, 2.7 to 3.6 s) should have none, or N at most (M - S - 16) / 12 (about 3) and
    no more of a rise than the radio's own ID key at the end of a session it ends (S 0); before the fix about 7
    windows, 3 or 4 of them stepping up on the silence. The level before and after a whole session says nothing: on
    the X6100 every key's level is kept, and the session's data keys and the ID's audio move it too. A call
    cancelled in Pat (Dialing..., then the second click that forces it): the ID at once on the call's frequency,
    Pat's tune back after it (0003; make modems tunes half a second after ABORT).
  - **Then on the air**: yo4cil against his own gateway (YO4CIL, VARA and ARDOP on 4 bands), free speed first; a
    test build announced on #120 first.
  - **Upstream**: 0002 is prepared as a patch for x0rro/xara's main with a PR text (not sent yet); drop 0002 once
    upstream acts on CWID ON. 0003 is not offered yet (0.1.3 waits the same way with a US call).
  - **Watch**: 0.1.4, "a significant departure from 0.1.3 (libxara is coming)": 0001 then needs a rebase, or a
    program of ours that links libxara as x6x00-mercury links Mercury. Its open PRs for slow audio paths (#39 the
    extra answer lead, #40 the detector before a reply deadline, #38 the ACK margin), #37; any statement by EA5HVK
    (VARA's author).

## X6200: the H-22 hand mic's keys (#56)

Reported 2026-10-27 (X6200, base 1.0.7, the H-22 that comes with the radio). The GUI knows the X6100 mic's codes
only (the X6200 control library copied them from the X6100's; R1CBU's X6200 GUI notes list the hand mic as not
working). On the H-22 only F1, the step keys, 1, 2 and F-INP work; F2, F3, HOME, the volume keys, 3-9, 0 and the
dot do nothing. Every code the base reports is in the log now: `hand mic: key 0x.. (..)` (none while a text
window is open, which can hold a WiFi password).
- What the report shows: 3-9, 0 and the dot do nothing on the main screen and in the F-INP box alike, where 1 and 2
  work, and mic_keys.c takes every digit the same way. F2 tells nothing yet: it has no action until one is set in
  Settings > Keys (None by default). F3, HOME and the volume keys are not on the X6100's mic.
- The table, 2026-10-30 (ea4iln-debug, X6200 with v0.3.7 and base 1.0.8, next to what Xiegu's app 1.0.8 does with
  each code): 1 and 2 0xE1/0xE2, 3-9 0x18-0x1E, 0 0x17, * (the dot) 0x1F, # 0x28 (F-INP), F1/F2/F3 0x48/0x44/0x13,
  the arrows 0x84/0x04, HOME 0x24 (Xiegu's app: backspace, not a home frequency), VOL+/VOL- 0x14/0x15 (the RX
  volume +1/-1), A/B 0x16 (Xiegu's app: as XFC), MODE 0xE8, V/M 0x81. The side UP/DOWN keys send nothing (no line,
  short or held); Xiegu's app knows 0x12/0x11 as band up/down, never seen.
- Done in v0.3.8: the X6200 knows the H-22's own codes besides the X6100 mic's (no code in common), F3
  with its own actions (Settings > Keys, X6200 only), VOL+/VOL- the volume a step a press and repeating while held,
  A/B as the front A/B key, 0x12/0x11 as the BAND keys; the X6100 keeps its mic's table. Harness `mic-keys.c` walks
  both mics on both boards; manual: Getting started > Hand mic.
- After v0.3.8 (#145): PR #136 gave every digit on the X6200 the H-22's band, the X6100 mic's too (its 3 at 60 m).
  A key sent with one of the H-22's own codes is marked now and takes the H-22's print, the rest the X6100 mic's.
  This rests on the X6100 mic sending its own codes on the X6200, as the X6200 library's table has them: no log of
  it from a radio yet (on v0.3.8, which #145 reports right, 0xE4 and 0x18 both give 40 m).
- **Next**: on the bench X6200 with its H-22, on the main screen, each key pressed once (and VOL+, A/B, a digit
  held): the band on 3-9 and 0, the dot and HOME in the F-INP box, F3 with an action set, the volume stepping on
  while VOL+ is held and stopping when let go, A/B swapping and its hold copying. Then the X6100's mic in the same
  radio (#145): 3, 9 and the dot on the main screen log `hand mic: key 0xE4 (3)`, `0xA4 (9)` and `0x21 (.)` and
  give 40 m, 10 m and no band, and the H-22's 3 again `0x18 (3)` and 60 m. If the X6100 mic sends 0x18 there, the
  radio cannot tell the mics apart by their codes: the manual's line that it does goes, and #145 is open again.
- Open: the H-22's side UP/DOWN. If they are to work, they are outside the packet's key word (Xiegu's manual names
  two data lines on the mic's plug, MDATA and MSVSW): the packet's other words (`reserved_2`, `reserved_3`, the flag
  bits the library leaves unnamed) logged when they change, in hw_x6200.c.

## Keyboard: CapsLock's chords, the quick QSO log and PTT, on a radio (#175)

Asked 2026-11-04 (F4ESO, X6200, random SSB QSOs logged from a Bluetooth keyboard). Done after v0.3.8: CapsLock is the
keyboard's hyper key (src/kbd_hyper.c), the one way to shortcuts, so they work on any keyboard: L logs a QSO (the CW
line's in CW, else the main screen's Log QSO box; the owner: "reuse L"), K the dial lock, P a screenshot, S Settings,
F the frequency box, 1-5 the CW messages (1-8 JTTY's macros), Up/Down the band, Space a PTT toggle (src/kbd_ptt.c).
The keyboard's F keys and PgUp/PgDn do nothing any more. Field Ops' Log QSO takes the report sent first, as the box
does. Checked with tools/keyboard (kbd_hyper, kbd_keymap, kbd_ptt) and the two PC builds only: not on a radio, nor in
the PC build's window.
- **Next**: on the bench X6200 with a USB keyboard and a Bluetooth one, on the main screen in USB: CapsLock+L opens
  `Call [sent [rcvd]]:`, `F4ABC 59 57` and Enter log `QSO logged: F4ABC 59 57 SSB` (59 sent on the Logs page), no L
  typed and Caps Lock still off after it (a letter typed comes out small), the Caps Lock light off again once L
  went down. CapsLock tapped alone: the light and capitals on, tapped again both off. CapsLock+L in Settings, FT8
  and the box itself: nothing. In CW with `F4ABC 599` typed in the CW line: CapsLock+L logs it, the line empties,
  nothing keyed; with the line empty (sending) the box opens and the line stays. Field Ops, a test activation:
  `F4ABC 59 57` logs 59 sent, 57 received. Then the X6100 (the light is the console's: KDSKBLED on /dev/tty0).
- **Next, the same bench**: CapsLock+K and Scroll Lock say "Dial locked" / "Dial unlocked"; CapsLock+P and Print
  Screen take a screenshot (P also in Settings, an app and the quick log's box); CapsLock+S opens Settings; CapsLock+F
  the frequency box, in CW too (f goes into the CW line there); CapsLock+Up/Down one band a press, held too; in CW
  CapsLock+1-5 key the CW messages; in JTTY CapsLock+1-8 its macros. F1-F10, PgUp and PgDn: nothing anywhere. In
  the WiFi password box and the quick log's box none of them types. The Scroll Lock light is the console's own,
  which turns at each press: it can show the lock the wrong way round (left as it is). CapsLock tapped and A typed
  fast over it: a capital A, Caps Lock on. In walkie mode CapsLock+S, L and Space: nothing. The screen dark (Screen
  off 1 min): the first CapsLock+L only lights it. In USB after CW with a call decoded: CapsLock+L's box empty.
- **Next, PTT, into a dummy load**: CapsLock+Space keys (`PTT on: CapsLock+Space for off`, the TX panel or the S-meter
  box's power and SWR), again lets go; in CW the carrier. Then, each while keyed: the side PTT pressed and let go
  (unkeyed, `PTT off: CapsLock+Space for on`; the next chord keys), Settings opened from the front key then
  CapsLock+Space (lets go), the USB keyboard pulled and the Bluetooth one switched off (`PTT off: keyboard gone`), CW
  keyed then the mode to USB and CapsLock+Space (no carrier left down), CAT PTT off in CW (no carrier left down),
  three minutes keyed (`PTT off: three minutes on the air`). Out of band: `No TX on ...`, the next chord tries again.
  A browser paired meanwhile: its PTT refused ("controlled by the radio's operator").
- Open: more chords, a line each in kbd_hyper.c's table.
- **Asked next (F4ESO, 2026-11-06)**: name, QTH/locator and a comment in the same Log QSO box, after the reports, each
  after a comma, all optional (`F4ABC 59 57, Jean, Paris JN18du, nice signal`): a locator into GRIDSQUARE, the rest
  of the QTH into QTH, the comment into COMMENT. Settle a comment without a name or a QTH, and a comma inside one.
  A QRZ.com lookup: not now (the PC logger fills these in at import).

## WSPR: band hopping (#190)

Asked 2026-11-06 (rwensley). Today the WSPR app stays on the band chosen. A first version: each WSPR band Off, RX or
RX+TX, and a Hop switch on WSPR 2:2 F5; the coordinated 2-minute slots first (the minute of the hour picks the band,
160 to 10 m), the band changed between two cycles, never during a beacon; TX % as it is. The ATU's stored match per
frequency covers the hops once each WSPR frequency is tuned; an external tuner would need Ext tune at each hop, later.
The parts of the day (grayline, day, night) later: the radio doesn't work out sunrise and sunset yet. ALE: out of
scope.

## WiFi: a static address, as Ethernet has (#17)

Asked 2026-10-26. Ethernet (Settings > ETHERNET, web System > Network) has DHCP or Static; a WiFi network has
DHCP only, and a reporter's router never got on with the X6100's DHCP. A Static choice per saved network
(NetworkManager `ipv4.method manual`, address, gateway, DNS), with Ethernet's fields and checks.

## PSK, Olivia and JTTY: a Log QSO button in the apps (#55)

Asked 2026-10-27 (#55). FT8, FT4 and FT2 log their QSOs by themselves, and JS8 has Log QSO (page 5, and its popup
at 73 or SK); the keyboard apps log nothing themselves (docs/MANUAL.md, PSK31, PSK63 and Olivia: "The apps don't log
QSOs themselves yet"). Today a QSO from them goes into the log only from the browser panel's Log tab, in the app's
mode (#92), and since v0.3.8 the text log keeps what they receive and send (/mnt/digi_text), which is not a log of
QSOs.

- **The button**: on a page of PSK31, PSK63, Olivia (Contestia too) and JTTY, and of RTTY once it sends. It takes the
  DX call and the two RSTs the apps already keep (src/digi_qso.c, shared by them), the time, the frequency and the
  mode as the Log tab gives it, into the radio's log as JS8 does (`qso_log_record_create()` and
  `qso_log_record_save()`, `dialog_field_day_log()`; src/js8_window.c): so it shows on the Logs page and goes to
  Wavelog and Field Ops.
- **Only on a press** at first: the end of a QSO is hard to tell in a keyboard mode, so no offer at 73 or SK as JS8
  makes (the #55 reply of 29 October). The reporter cared most about the text and the reports, which the text log
  now keeps.

## CW Trainer: the receiver turned down while it is open (#110)

Asked 2026-10-31 (joanpao, #110). The trainer leaves the receiver as it is in Learn, Practice and Calls, so the
band's noise plays under the side tone, and the trainer's decoder, which listens to the side tone to check the
keying, hears it too: while it waits for an answer, noise can come out as extra letters. The idea: the trainer turns
the RF gain down by itself while it is open and gives it back when it closes, as it already does with key train and
the mode (src/dialog_cw_trainer.c, `construct_cb()` and `destruct_cb()`). By hand today: RF gain to 0 before opening
it (the #110 reply of 31 October).

- **To check first, on both radios**: that the side tone stays as loud with the RF gain at 0 (the X6200's is made in
  the base and follows the volume) and that the decoder still hears it, in all three modes.
- **How**: given back on every exit, as key train is. The RF gain is a saved setting (`rfgain`, sent to the base by
  radio.c as `HW_RFG`): better to set the base's gain alone and leave the setting as it is, so a power-off with the
  trainer open doesn't leave the radio deaf at the next start. Meanwhile the S-meter's correction for the RF gain
  (src/smeter.c) and the browser panel's RF gain (`P_RFG`) should see the gain the user set.

## Winlink: VOACAP predictions in Pat's gateway list (#153)

Asked 2026-11-02 (km4gkd, #153). Pat's gateway list (the mailbox's Connect dialog) has a VOACAP column with each
gateway's predicted path quality; on the radio it stays hidden, because x6100-winlink sets Pat's `prediction` engine
to `disabled` (the image has no `voacapl`). The wish: that column on the radio, to choose a gateway and a frequency in
the field. For the reporter a minute or two for the list to fill in is fine, and so is a VOACAP server on the device
Winlink is used from (the #153 replies of 3 and 4 November).

- **How Pat predicts**: it writes a deck (method 30, the CCIR maps, 50 W: app/rmslist.go and
  internal/propagation/voacap/encode.go in Pat v1.0.1), runs `voacapl` with an itshfbc data tree once per gateway
  frequency, as many at a time as GOMAXPROCS (all four cores), and reads its printed listing. Its `voacap-api` engine
  POSTs the same deck to `<address>/predict` and reads the same listing back.
- **The engine**: `voacapl` is Fortran, which the radios' build does not compile. hfcast-engine
  (github.com/jonathanmcsweet/hfcast-engine, linked on #153 by doodlecomms, 5 November) is VOACAP translated into
  Rust (Apache-2.0, no dependencies, rust-version 1.87), checked cell by cell against the Fortran, with the
  coefficients compiled in through its `embedded-coefficients` feature (a build from the repository only) so no
  itshfbc tree; it covers method 30 with the CCIR maps. It would build with tools/xara's pinned Rust (1.99.1, armv7).
  It takes JSON (its `predict`) or a library call, not Pat's deck: a small program of ours on the library, Pat's deck
  in and the method-30 listing out (its `api::listing`), either as a `voacapl` for Pat's `voacap` engine or as a
  server on 127.0.0.1 for Pat's `voacap-api` engine, which needs no Pat patch.
- **Speed**: fewer workers than Pat's default, next to the GUI; a full gateway list measured on an X6100. A prediction
  must never disturb a Winlink session or the radio.
- **Power**: our Pat build predicts for the radio's 7 W at most instead of Pat's fixed 50 W, or the list looks better
  than the radio can do.
- **The data's status**: hfcast's own notes (NOTICE, docs/licence.md, embedded/README.md) leave the redistribution of
  the coefficient files partly open (CCIR Reports 322 and 340, which ITU-R Study Group 3 publishes "free from any
  copyright assertions", plus NTIA/ITS work). The same data comes in `voacapl`'s itshfbc, so either route ships it:
  settle it before an image carries it.
- **Then** x6100-winlink sets the engine when the program is in the image, instead of `disabled`.
