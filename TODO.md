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

## Winlink over HF: Pat with ARDOP on the radio, Mercury after (#18, #19)

Asked 2026-10-26 (issues #18 and #19). Winlink email from the radio alone, with a phone on its WiFi to read and
write it: Pat (getpat.io, Go, MIT) as the client, ARDOP as the HF modem. Both run on ARM Linux today, so it should
be possible on both radios.

- **Pat**: linux armhf builds (v1.0.0 has a .deb), or built in Buildroot. Its web UI (port 8080) is the mailbox
  on a phone or laptop, linked from the configurator (Operating > Winlink). Downloaded once to DATA with a pinned
  checksum, as Tailscale is, rather than 20+ MB in every image. The Winlink password kept as the QRZ key is
  (0600, not in the Backup). Telnet to the CMS over WiFi works the day Pat runs: a first step without HF.
- **ARDOP**: ardopcf (github.com/pflarue/ardop, MIT, C, ARM builds), the open mode the Winlink RMS gateways run
  next to VARA and Pactor. Its host interface (TCP 8515/8516) is what Pat speaks. Audio at 12 kHz through ALSA
  or `pulse`; the radio's audio belongs to the GUI, so the GUI gives ardopcf a PulseAudio sink and source for RX
  and TX (or ardopcf's modem goes into the GUI, which MIT allows), and TX goes through tx_player
  (`radio_set_modem()`, the learned drive level).
- **PTT and frequency**: Pat keys and tunes through hamlib's rigctld protocol. A small rigctld-compatible server
  in the GUI on localhost:4532 (`F`, `f`, `M`, `m`, `T`, `t`) does both, and serves other programs too.
  ardopcf's own CAT keystring PTT is the other road.
- **To find out first**: both radios' TX/RX turnaround (ARQ wants it short: PTT to RF, RF off to RX audio);
  ardopcf's CPU on the X6100's A7 next to the GUI; levels and the 12 kHz resampling; the RX filter for ARDOP's
  200-2000 Hz; the TX watchdog and the SWR guard while ARQ keys every few seconds. ardopcf's maintainer has
  stepped back (its README asks for forks), so fixes may have to be ours.
- **Mercury** (github.com/Rhizomatica/mercury, GPL-3.0, a separate process): faster OFDM with a VARA-style TCP
  interface (Pat's VARA transport), a `pulse` backend, PTT left to the client. No Winlink RMS gateway runs it
  (its README has no Winlink), so it is for station to station (Pat P2P) and HERMES networks: after ARDOP.
  VARA itself is closed and Windows-only: out.

## WiFi: a static address, as Ethernet has (#17)

Asked 2026-10-26. Ethernet (Settings > ETHERNET, web System > Network) has DHCP or Static; a WiFi network has
DHCP only, and a reporter's router never got on with the X6100's DHCP. A Static choice per saved network
(NetworkManager `ipv4.method manual`, address, gateway, DNS), with Ethernet's fields and checks.
