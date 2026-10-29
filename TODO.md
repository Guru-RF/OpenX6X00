# To do

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

## CESSB on Xiegu's newest bases, X6100 and X6200 (and the other TX options with it)

Asked 2026-10-25; CESSB first, the rest when it comes for free. CESSB (controlled-envelope SSB, W9GR) gives
SSB more average power without splatter: the SSB envelope is clipped and the overshoot the filtering brings
back is controlled. Today only gdyuldin's patched X6100 base has it (x6100_base_patch rev 12 and up,
`HW_CAP_CESSB`, Settings > TX CAL), and r13 is not usable on V1.1.8 radios (the section above). Xiegu's newest
bases, X6100 V1.1.9 and X6200 1.0.8, have no CESSB, and the X6200's no TX filter either
(src/hw/x6200/hw_x6200.c, `hw_caps()`).

- **The idea**: do it in the GUI, on the voice path, so it works on both radios and any base. The mic is
  recorded by Linux, processed and played into the transmitter the way voice messages, FT8, TCI and the
  browser panel already transmit (`radio_set_modem()`; `AUDIO_PLAY_ON` mutes the base's mic inputs,
  src/radio.c). Chain: TX filter (band-pass), compressor, CESSB (Hilbert to the complex envelope, clip,
  filter, overshoot clip), then a fixed level into the base so its ALC stays out of it.
- **To find out first**: whether both radios' mics (hand and built-in) can be recorded by Linux while the base
  transmits from the modem input (voice messages record the mic: `AUDIO_PLAY_VOICE_REC`); the delay from mic
  to RF (aim under 40 ms: PulseAudio's buffers) and a monitor that hears the processed audio; who keys (the
  hand mic's PTT keys the base, the GUI must key the modem path instead), VOX; the X6200 base's compressor
  off meanwhile; power and ALC (the X6200 holds power without ALC, `HW_CAP_TX_NO_ALC`); a measurement
  (average power at the same peak, splatter seen on a second receiver).
- **Maybe also, from the same chain**: r13's other TX options for the stock bases: TX filter (the X6200 has
  none), compressor shape, FM pre-emphasis, a TX equaliser. The band output gain stays base-side.

## Winlink over HF: Pat with ARDOP on the radio, Mercury and XARA (#18, #19, #43)

Asked 2026-10-26 (issues #18 and #19). Winlink email from the radio alone, with a phone on its WiFi to read and
write it: Pat (getpat.io, Go, MIT) as the client, ARDOP as the HF modem.

- **Built, for the next release**: Pat v1.0.0 built with a pinned Go (tools/pat, a few patches: its page answers
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
- **For the next release** (#43): the radio's state before a connection (/run/winlink/radio from the GUI, the
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
  - **Built, experimental, chosen on the Winlink page** (next release): v1.9.21 vendored as its daemon build
    (third-party/mercury, two local patches: the TNC and broadcast ports on 127.0.0.1, VERSION answered with a
    VERSION line so Pat cannot hang), linked into x6x00-mercury, which takes the audio and the PTT through the
    GUI's modem socket in its protocol 2 (8000 Hz, keys of up to 40 s; ardopcf stays on protocol 1). In the image
    and the bundle. Operating > Winlink's Modem (ARDOP or Mercury, one at a time) and Mercury's bandwidth (500,
    2300, 2750); x6100-winlink runs x6x00-mercury in place of ardopcf with /run/winlink/mercury.ini, Pat's varahf
    on 127.0.0.1:8300 (P2P listening off), Mercury's readiness and outcomes, its decoders falling behind ("decoder
    ring full", "RX backlog exceeded") said on the page and in Pat's log ([manual](docs/MANUAL.md#mercury-experimental)). tools/winlink
    tests it on the PC: Pat to Pat peer to peer over two stubbed radios (make modems), the supervisor's and the
    page's side (winlink_test.py, winlink_web_test.py).
  - **Next**: the go/no-go on the bench X6100, before a release offers it: Mercury's CPU next to the GUI with
    `top -H` (idle, and in a session; no "decoder ring full" or "RX backlog exceeded" in
    mercury.log), the temperature, a 30-40 s key at up
    to 7 W, PTT to RF and unkey to RX audio, the drive level loop on OFDM (QAM16C2 needs a linear chain). Then on
    the air: P2P with a Mercury station, then a gateway (KO0OOO, K7EK-10) once the Winlink account works. Upstream
    reports to post: the VERSION reply (Pat's start race), the daemon build's two missing Android hooks, an option
    for the TNC ports' address, nanorq's and oblas's licence files, PUBLIC in TNC.md.
- **XARA** (github.com/x0rro/xara, a VARA-compatible modem in Rust, first release 2026-10-21; #18): a watch
  item, not built. Watch it regularly: its fix for HF calls with an SSID (XARA issue #22, PR #23), the sound-card
  clock drift that makes a VARA peer time out (XARA #20), its releases, and any takedown or statement by EA5HVK
  (VARA's author). Implement it when it stabilises; earlier has no added value. Never its "registered" feature
  (VARA's paid speed without a licence).

## X6200: the H-22 hand mic's keys (#56)

Reported 2026-10-27 (X6200, base 1.0.7, the H-22 that comes with the radio). The GUI knows the X6100 mic's codes
only (the X6200 control library copied them from the X6100's; R1CBU's X6200 GUI notes list the hand mic as not
working). On the H-22 only F1, the step keys, 1, 2 and F-INP work; F2, F3, HOME, the volume keys, 3-9, 0 and the
dot do nothing. Every code the base reports is in the log now: `hand mic: key 0x.. (..)` (none while a text
window is open, which can hold a WiFi password).
- **Next**: on the bench X6200 with its H-22, each key pressed once in order (F1 F2 F3, the step keys, HOME, the
  volume keys, A/B, MODE, V/M, 1-9, the dot, 0, F-INP, DWN and UP on the back), and once held; the codes read off
  the log.
- Then: an H-22 table for the X6200 (the X6100 keeps its mic's), the H-22's own keys (F3 next to F1 and F2 in
  Settings > Keys, HOME to a frequency set in Settings as in Xiegu's app, the volume keys, A/B), the manual's hand
  mic section per radio, and the H-22's codes in the harness (`mic-keys.c`).

## WiFi: a static address, as Ethernet has (#17)

Asked 2026-10-26. Ethernet (Settings > ETHERNET, web System > Network) has DHCP or Static; a WiFi network has
DHCP only, and a reporter's router never got on with the X6100's DHCP. A Static choice per saved network
(NetworkManager `ipv4.method manual`, address, gateway, DNS), with Ethernet's fields and checks.
