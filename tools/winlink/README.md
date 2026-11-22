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
(`WL_PWR_XVTR=0.5`, said in `cap_transverter`, not `cap_battery`). Last, APP > Mail's
Connect as the app presses it (#322; `WL_PRESET`, the steps in `stubs.c`): the first
press puts the radio on the gateway's frequency in USB-D and keys nothing, with no
connection under way that would hold TUNE; another gateway, the dial turned, an app
open, a frequency the radio cannot tune, the radio on the air; the way back to where
the radio was, also once the modem's last frame is out; and the second press, where
Pat's QSY finds the radio there and its tune back leaves it there; no first press
within 4 s of a tune of Pat's (a connection that begins, also from the web mailbox,
before Pat says it is dialing). It prints `PROTO OK`. What Pat itself says of a
connection under way is `x6100-winlink-mail busy`, asked by the app before that
press: in `winlink_test.py` (`MailBusy`, a stand-in for Pat's `/api/status`, and the
switch lock).

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
- Mercury (1.9.25): two stations, each a `winlink_host` and an `x6x00-mercury`, what
  one transmits the other hears with a little noise under it. Mercury takes Pat's
  start-up commands, `CWID ON` too (1.9.21 said `WRONG` to it), and answers `VERSION`
  with the line `VERSION Mercury 1.9.25` (our patch; Pat waits for a `VERSION` line
  with no timeout). Pat B listens on varahf; Pat A sends B a 1500-byte message peer to
  peer (`varahf:///N0BBB?p2p=true`): connected, sent, disconnected, the message in B's
  inbox, no key stopped by the GUI. Each station's last key peaks between -7.5 and
  -5.5 dBFS as the GUI's player gets it: Mercury 1.9.21's level, which the
  supervisor's `tx_gain_db = -4` keeps (2026-11-22: -6.0 dBFS a data mode, -6.5 the
  MFSK; with `tx_gain_db = 0`, 1.9.25's own level, -2.0 and -2.5, and the check
  fails). Then the stop. A session of the two Mercurys alone (`CONNECT` on A's host
  port) and SIGTERM to A's bridge: it has Mercury end the session on the air first,
  so B's Mercury says `DISCONNECTED` (22 s later here: the DISCONNECT waits for its
  turn), the goodbye is keyed through the GUI and played to its end, A leaves with
  code 0 within the bridge's 42 s and the stop, and keys no more; B, with no session
  left, stops at once (0.0 s) with no goodbye. Both bridges start again for a second
  session: a second SIGTERM during A's goodbye stops it at once (0.7 s after the
  first), and B's goodbye, with nobody there to answer, still ends (14.5 s). The
  bound itself (a goodbye that never gets out: given up at 42 s, SIGALRM behind it)
  is not reached here; the supervisor's side of it is in `winlink_test.py`
  (`ModemStop`, with a stand-in that never ends). About
  three minutes (2026-11-22, 1.9.25: Pat's session 68 s, eight keys a station,
  the longest 3.8 s; 2026-10-29, 1.9.21: 58 s, six keys). Both stations are the same
  build: that 1.9.25 refuses 1.9.24 and older is upstream's word and its code
  (`datalink_arq/arq.c`), not tested here.
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
  Pat asks for a CW ID (`CWID ON`) each time, and both XARAs identify in CW as the
  session closes, with the European calls too (our patch 0004; XARA alone drops `CWID
  ON` and identifies only with a US call): the key that ends with the ID is played
  whole. A's unanswered call, whose host sent no `CWID ON`, ends without one. Then,
  with `CWID ON`, A's next call is cancelled in its reply window (`ABORT`, as Pat's
  force disconnect) and A's radio tuned elsewhere half a second later (`F` on its
  rigctld, as Pat's tune back): the ID is the only key after the `ABORT`, played whole
  on the call's frequency (each key's is in `WL_PLAYED.log`), and the radio goes where
  it was tuned once that key is over (our patch 0005; XARA alone waits out the reply
  window, 1.3 s, and its ID went out on the frequency tuned to). Each time: connected,
  the message in B's inbox, no key stopped by the GUI, both XARAs stop on SIGTERM
  (their code 130). About two and a half minutes, and 12 s for the unanswered call
  (XARA 0.1.3 with our patches then, 2026-11-04, eight runs: 89-112 s at the free
  speed, levels 2 to 4; 60-62 s at full speed, the highest level 9 to 14; keys of 5.2
  s at the most). With the CW ID patch, 2026-11-04, two runs: 96 and 98 s free, 61 and
  62 s full, the highest level 9; the IDs' keys 3.9 to 4.1 s (A's, after its QRT) and
  5.6 to 5.9 s (B's, which waits out A's ID first), the longest. The whole `make
  modems`: 257, 266 and 266 s. With the cancelled-call patch and the cancelled call,
  2026-11-04: 258 s, its ID 2.8 s; without that patch it fails there, the ID on the
  frequency tuned to. Not run yet on NR5P's XARA, pinned since 2026-11-09
  (`tools/xara/UPSTREAM.md`).

It prints `MODEMS OK`; `--keep DIR` (run the script by hand) keeps the logs, `--only
ardop|mercury|xara` runs one.

`make mail` runs `winlink_mail_test.py`: APP > Mail's helper
(`rootfs/usr/sbin/x6100-winlink-mail`, #294) against a real Pat (`make PAT=...`, as
above) on loopback, its config and mailbox under a temporary root as the radio lays
them out. Sending (with the checks the helper makes before Pat's: no recipient, no
subject, no text, a subject over 128 characters, an address with a space or a
control character), the lists, reading (an unread message marked read), deleting,
a message ID that is a path refused; the gateways from Pat's cached list (no
internet), ARDOP only, the nearest first, by band, the last one connected to on
top; a session Pat cannot start (no modem): Pat's account question passed on and
answered, then a failure, not a hang; and a session over telnet with a CMS of the
test's own (the only transport the helper takes besides the radio's two modems,
and only on a PC): its secure login challenge, Pat's password question passed on
through the helper and answered, the reply the CMS expects, Pat's log lines of the
session and none from before. Also: a subject and a text in Latin-1 sent as
typed, what is beyond Latin-1 written out before Pat would make it "?", a broken
UTF-8 letter sent as "?" (not refused); Mercury
and XARA listing the VARA HF gateways; on a band chosen, a gateway called
lately on another band not put on top. It prints `MAIL OK` (2026-11-16: about
6 s).

Also here, in Python alone: `winlink_test.py`, the tests of the supervisor
(`rootfs/usr/sbin/x6100-winlink`: the settings, Pat's config, Mercury's and
XARA's, XARA's callsign, its speed and the VARA HF licence tick, the outcome of
each connection from the logs in `fixtures/`, Pat's own switch of the modem,
#296, Pat's service codes, all three always with the user's own kept, #324, and
`x6100-winlink modem`, APP > Mail's Modem, #322 and #323: chosen as the page does,
refused when the modem is not there, XARA without a callsign, during a switch of
Pat's), and `winlink_web_test.py`, the web
configurator's Winlink page and its API, which needs bottle 0.13
(`PYTHONPATH=<a directory with bottle.py>`; 0.12 does not import on Python 3.13
and later). Both run with `python3 -B <file>`, and `make` (`make test`) runs them
after the protocol test: the supervisor's in a network namespace of its own
(`unshare -rn`, `WINLINK_NETNS=1`), the page's with `BOTTLE=<a directory with
bottle.py>`. The switch (`x6100-winlink switch ardop|varahf`, which our Pat runs
before it dials, `tools/pat` 0008): with a stand-in for the supervisor, VARA HF
while ARDOP runs (XARA, at its bandwidth), ARDOP while XARA runs, Mercury when it
was the last of the two (also as the page leaves it), Mercury when XARA is not
there, no switch when the right modem runs (the settings file untouched, no
line), only the modem changed in the file, the modem failing to start and not
ready in time (a line in Pat's log, Pat does not call, the setting stays), its
TNC port not open yet, XARA without a callsign (no switch), a change of the
page's under way first (waited for, and Pat restarting with it: not for ever),
the page changing the modem meanwhile, no supervisor, other transports refused,
one switch at a time, the command line as Pat runs it, Pat's Abort (SIGTERM,
`tools/pat` 0010: before anything changed, and while the new modem starts,
which goes on; also to the command as Pat runs it, with a stand-in holding the
supervisor's lock), and the failure line as the last connection: a refusal
while another connection is under way shows until that one comes to something
(the bench's order of 19 Nov; also when it connects, with ardopcf's word for
it, and with Pat restarted). With the supervisor itself (`x6100-winlink run`,
in the namespace only): stand-ins for Pat (its mailbox's status on 8080) and the three
modems (their TNC ports, their lines on the GUI's socket); VARA HF from ARDOP,
then ARDOP again, Pat not restarted for either, then the page's own choice of
Mercury, for which Pat restarts as before (about 10 s). The web test also runs
`winlink_page_test.js` when Node is there: the page's own script on the audit's
small DOM (`docs/audit/repro-2026-10-24/browser-dom.js`), its Power row from the
radio file, the Filter row for XARA's bandwidths, XARA's speed in the Modem row
and the Station card's rows for the modem chosen; and the whole page, whose
Station card's Modem row follows the radio's choice (a switch of Pat's) unless
a choice made on the page is not saved yet.
