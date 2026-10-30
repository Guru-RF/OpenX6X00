# Winlink modem socket, host test

`src/winlink.c` built on the PC against the stubs in `stubs.c` and `stub_include/`
(the radio, the settings, the player, PulseAudio: a few variables) as `winlink_host`.
Its receiver is a 1 kHz tone, or the other station's air; its player keeps the wall
clock as the low-latency stream does, keys the radio for the whole transmission and
writes what it played to `WL_PLAYED` (the variables are at the top of `stubs.c`).
liquid-dsp comes from `build-host/deps` (`host/build.sh`), or `make DEPS=...`.

`make` (`make test`) runs `winlink_proto_test.py`: it speaks to the socket as a modem
does, in protocol 1 (ardopcf: 12000 Hz, keys of 30 s) and protocol 2 (the rate and the
longest key in the hello). For each: the R messages (their size, 50 a second, the tone
in them, so the GUI records at the session's rate), a transmission of a 1 kHz tone (as
long as what was sent and still 1 kHz once played at 44.1 kHz, so it was resampled
from the session's rate), the key limit of the hello (2 s: stopped after 2 s, the
message on the screen), a key longer than the GUI allows (40 s), the hellos refused (a
rate other than 8000 or 12000, no key, version 3: the connection is closed), messages
before the hello (ignored), a second hello (ignored), a new connection replacing the
old one, a digital app opened during a key (`WL_APP`: the key stopped there, the next
one refused until the app is closed), and the radio file the Winlink page reads
(`build/radio` here): Winlink's cap of 7 W, 5 W with the X6200 on its battery
(`WL_PWR_MAX=5`, said in `cap_battery`), and 0.5 W under a transverter's TX power limit
(`WL_PWR_XVTR=0.5`, said in `cap_transverter`, not `cap_battery`). It prints `PROTO OK`.

`make modems` runs `winlink_modems_test.py` with the real modems, in a network
namespace of its own with loopback only (`unshare -rn`), so Pat reaches nothing
outside: no Winlink web service, no CMS. It needs the GUI's PC build with the two
modems built (`cmake --build build-host --target ardopcf x6x00-mercury`; `make
BUILD=...` for another build directory) and a Pat for this PC (`make PAT=...`: Pat
v1.0.0 with `tools/pat/patches`, built for linux/amd64).

- ardopcf, as `x6100-winlink` starts it: it connects in protocol 1 (12000 Hz, keys of
  30 s), and the ID frame asked on its host port (`SENDID`) is played whole (1.75 s).
- Mercury: two stations, each a `winlink_host` and an `x6x00-mercury`, what one
  transmits the other hears with a little noise under it. Mercury answers Pat's
  `VERSION` with a `VERSION` line even when the `WRONG` for `CWID ON` came first (the
  order that hung Pat). Pat B listens on varahf; Pat A sends B a 1500-byte message
  peer to peer (`varahf:///N0BBB?p2p=true`): connected, sent, disconnected, the message
  in B's inbox, no key stopped by the GUI, both bridges stop on SIGTERM. About a minute
  (2026-10-29: 58 s, six keys a station, the longest 3.8 s).

It prints `MODEMS OK`; `--keep DIR` (run the script by hand) keeps the logs, `--only
ardop|mercury` runs one.

Also here, in Python alone: `winlink_test.py`, the tests of the supervisor
(`rootfs/usr/sbin/x6100-winlink`: the settings, Pat's config, Mercury's, the
outcome of each connection from the logs in `fixtures/`), and
`winlink_web_test.py`, the web configurator's Winlink page and its API, which
needs bottle (`PYTHONPATH=<a directory with bottle.py>`). Both run with
`python3 -B <file>`. The web test also runs `winlink_page_test.js` when Node is
there: the page's own script on the audit's small DOM
(`docs/audit/repro-2026-10-24/browser-dom.js`), its Power row from the radio file.
