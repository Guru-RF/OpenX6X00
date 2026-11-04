# Winlink modem socket, host test

`src/winlink.c` built on the PC against the stubs in `stubs.c` and `stub_include/`
(the radio, the settings, the player, PulseAudio: a few variables) as `winlink_host`.
Its receiver is a 1 kHz tone, or the other station's air; its player keeps the wall
clock as the low-latency stream does, keys the radio for the whole transmission and
writes what it played to `WL_PLAYED` (the variables are at the top of `stubs.c`). The
band table the SWR hold goes by is the real one (`src/cluster/spots.c`, copied beside it).
liquid-dsp comes from `build-host/deps` (`host/build.sh`), or `make DEPS=...`.

`make` (`make test`) runs `winlink_proto_test.py`: it speaks to the socket as a modem
does, in protocol 1 (ardopcf: 12000 Hz, keys of 30 s) and protocol 2 (the rate and the
longest key in the hello). For each: the R messages (their size, 50 a second, the tone
in them, so the GUI records at the session's rate), a transmission of a 1 kHz tone (as
long as what was sent and still 1 kHz once played at 44.1 kHz, so it was resampled
from the session's rate), XARA's way of sending (protocol 2 at 12000 Hz with keys of
30 s, its audio in 10 ms T messages when they are due and none ahead, 20 ms of
silence after it, then P 0: played whole, never unkeyed for want of audio), the key
limit of the hello (2 s: stopped after 2 s, the message on the screen), a key longer
than the GUI allows (40 s), the hellos refused (a rate other than 8000 or 12000, no
key, version 3: the connection is closed), messages before the hello (ignored), a
second hello (ignored), a new connection replacing the old one, a digital app opened
during a key (`WL_APP`: the key stopped there, the next one refused until the app is
closed), an SWR trip during a key (`WL_SWR`, #147: on
10.145 MHz the keys on 30 m are held, also after Pat's QSY to 10.120, while 7.102 MHz
on 40 m keys at once, the radio file saying where the hold is and whether the radio is
there; on 7.250 MHz, outside the table's 40 m, the hold covers 7.300 MHz, within
100 kHz of it, and 7.400 keys; `WL_SWR_LATE`, a trip counted outside a key: just
after one it is that key's, 2.5 s later another transmission's and holds nothing),
and the radio file the Winlink page reads
(`build/radio` here): Winlink's cap of 7 W, 5 W with the X6200 on its battery
(`WL_PWR_MAX=5`, said in `cap_battery`), and 0.5 W under a transverter's TX power limit
(`WL_PWR_XVTR=0.5`, said in `cap_transverter`, not `cap_battery`). It prints `PROTO OK`.

`make modems` runs `winlink_modems_test.py` with the real modems, in a network
namespace of its own with loopback only (`unshare -rn`), so Pat reaches nothing
outside: no Winlink web service, no CMS. It needs the GUI's PC build with the two
modems built (`cmake --build build-host --target ardopcf x6x00-mercury`; `make
BUILD=...` for another build directory), a Pat for this PC (`make PAT=...`: Pat
v1.0.1 with `tools/pat/patches`, built for linux/amd64; 2026-11-01: MODEMS OK, 58 s)
and XARA for this PC (`make XARA=...`; by default `tools/xara/build.sh --host` builds
it, the first time with a download of about 150 MB, then it only gives the path;
`XARA=` leaves XARA out).

- ardopcf, as `x6100-winlink` starts it: it connects in protocol 1 (12000 Hz, keys of
  30 s), and the ID frame asked on its host port (`SENDID`) is played whole (1.75 s).
  A call nobody answers (`ARQCALL N0BBB 2`): two ConReq frames, then `NEWSTATE DISC`
  and `STATUS CONNECT TO N0BBB FAILED`, and on its console the
  `[STATUS: CONNECT TO N0BBB FAILED]` that `x6100-winlink` reads as no answer (8 s).
  Then its clock 10 minutes on (`clockskip.so`, built here, as its `LD_PRELOAD`: each
  SIGUSR1 moves `CLOCK_MONOTONIC` on 10 minutes): no 10 minute ID, the ConReq frames
  carried its call; the same after `ARQCALL N0BBB 1` (one ConReq, no repeat) and after
  `PING N0BBB 1`; after a `TXFRAME DataNAK` (no call in it), the ID frame. Its
  channel is noise only: a tone is a busy channel, which holds that ID back.
  `PROTOCOLMODE FEC`, then `BOGUS`: `FAULT Syntax Err`, the mode still FEC;
  `PROTOCOLMODE ARQ`, as Pat sets it: `PROTOCOLMODE now ARQ`.
- Mercury: two stations, each a `winlink_host` and an `x6x00-mercury`, what one
  transmits the other hears with a little noise under it. Mercury answers Pat's
  `VERSION` with a `VERSION` line even when the `WRONG` for `CWID ON` came first (the
  order that hung Pat). Pat B listens on varahf; Pat A sends B a 1500-byte message
  peer to peer (`varahf:///N0BBB?p2p=true`): connected, sent, disconnected, the message
  in B's inbox, no key stopped by the GUI, both bridges stop on SIGTERM. About a minute
  (2026-10-29: 58 s, six keys a station, the longest 3.8 s).
- XARA: two stations the same way (ON4AAA and ON4BBB at the free speed, N0AAA and
  N0BBB at full speed), each a `winlink_host` and our patched XARA (`tools/xara`),
  started as `x6100-winlink` starts it (`xara --config` with the settings it writes,
  no `XARA_*` or `STATE_DIRECTORY`): it connects in protocol 2 (12000 Hz, keys of
  30 s), says so in its log as the supervisor reads it, and answers `VERSION` with
  `VERSION VARA HF v4.9.0`. First, at the free speed and before any Pat, A's XARA
  calls B's (`CONNECT` on its host port): B neither answers nor keys, since on the
  radio XARA starts with listening off, and A gives the call up on `ABORT`. Pat B
  listens on varahf (its `LISTEN ON`); Pat A sends B a message peer to peer, twice. At
  the free speed (no `registered`): A's XARA says `REGISTERED` without a call, B's
  `LINK UNREGISTERED`, no level past VARA's free 4. With `registered` on A, as the
  Winlink page's VARA HF licence box asks: A's says `REGISTERED N0AAA`, B's `LINK
  REGISTERED` (the callee follows the caller's bit) and the link goes past level 4.
  Pat asks for a CW ID (`CWID ON`) each time; XARA 0.1.3 sends one only with a US
  call, so the first run sends none and the second one does. Each time: connected, the
  message in B's inbox, no key stopped by the GUI, both XARAs stop on SIGTERM (their
  code 130). About two and a half minutes, and 12 s for the unanswered call
  (2026-11-04, eight runs: 89-112 s at the free speed, levels 2 to 4; 60-62 s at full
  speed, the highest level 9 to 14; keys of 5.2 s at the most). The whole `make
  modems`: 257 and 266 s.

It prints `MODEMS OK`; `--keep DIR` (run the script by hand) keeps the logs, `--only
ardop|mercury|xara` runs one.

Also here, in Python alone: `winlink_test.py`, the tests of the supervisor
(`rootfs/usr/sbin/x6100-winlink`: the settings, Pat's config, Mercury's and
XARA's, XARA's callsign, its speed and the VARA HF licence tick, the outcome of
each connection from the logs in `fixtures/`), and `winlink_web_test.py`, the web
configurator's Winlink page and its API, which needs bottle 0.13
(`PYTHONPATH=<a directory with bottle.py>`; 0.12 does not import on Python 3.13
and later). Both run with `python3 -B <file>`. The web test also runs
`winlink_page_test.js` when Node is there: the page's own script on the audit's
small DOM (`docs/audit/repro-2026-10-24/browser-dom.js`), its Power row from the
radio file, the Filter row for XARA's bandwidths, XARA's speed in the Modem row
and the Station card's rows for the modem chosen.
