# TCI server, host test

`make` first runs `tci_audio_test` (`make audio_test`): `src/tci_audio.c`'s 24 to 48 kHz
halfband swept from 0.1 to 11.9 kHz (the tone's level, its image at 24 kHz - f), the stream
cut in reads of any length, the four sample types written to two channels and read back, and
what the interpolation costs a second of audio on this PC.

Then `tci_own_test` (`make own_test`): who owns a transmission (audit C01). It includes
`src/tci_server.c` whole, calls its socket thread's steps one at a time and holds the LVGL
thread's work back in the stub scheduler (`stub_defer`, `stub_run`), so a second start, a
cancel, a refusal, control taken at the radio, the operator keying at the radio, a client
leaving, a key lost to a full queue, a key that runs late or one that never puts the radio on
the air comes between a key and the LVGL thread applying it. Each case checks that one start
keys the radio and that its client's unkey or leaving ends it, that a cancelled start keys
nothing, that an unkey never drops the operator's PTT, and that a refused start is answered
`trx:0,false;` and leaves control where it is. One more keys TCI audio in DIGU and DIGL,
which only CW and CW-R refuse (#17). One more has two clients over Tailscale Funnel (#58), one
whose session may not transmit: the TX state asked, or an unkey from a client that keyed nothing,
is answered with the state and leaves control with the browser at home; only the session that may
transmit keys, and its unkey ends it. One more has a browser over Funnel on the air, through
the real handshake with tailscaled's header and its key: while its page answers, another
connection is refused at the handshake; once it has been silent 6 s (a phone gone from the WiFi
to its mobile data, its connection left stale), another browser's key is refused and the same
browser's key from another address takes its place, its transmission ended, the newcomer keying;
of two waiting for their key the older goes; wrong keys pause Funnel. Each runs in a child
process, its clients on socket pairs, without a listener. It prints `OWNER OK: n cases`.

Then it builds `src/tci_server.c` and `src/tci_ws.c` (with `src/tci_audio.c` and
`src/control_owner.c`) on the PC against the stubs in `stubs.c` (the radio, the settings, the
scheduler, the meters, PulseAudio: a few variables and a 1 kHz tone), starts it on
127.0.0.1:50001 and runs `tci_client_test.py`, a raw WebSocket client that does what the
VFO-Knob does on a connection: the greeting, tuning, mode, filter, AGC, RIT, drive, PTT, the
sensor streams, RX audio as the knob asks for it (24 kHz int16, one channel, 512 a frame), a
transmission with TCI audio, a TX frame whose payload is not what its header says (the
transmission ends, nothing of it played), ping. Then four more connections: the browser
pages' format asked for in full; formats out of range and int24 stereo at 48 kHz in frames of
100; WSJT-X's TCI client ("TCI Client RX1") on Test CAT, its answers read as its parser
reads them, its RX audio in TCI's defaults (48 kHz float32 stereo: the tone's level, the image,
left and right), and a transmission as it keys, in the digu of its Mode Data/Pkt, every
TX_CHRONO answered with its frame; and
WSJT-X on a link slower than its RX audio (read at 250 kB/s through a 4 kB receive buffer),
keyed with the server's queue full: the TX_CHRONOs come ahead of the queued audio, the over
stays keyed for 3 s, every frame arrives whole. Last, Tailscale Funnel (#58), where only the
browser Panel's own sound and microphone may come, with tailscaled's
`Tailscale-Funnel-Request` header: nothing before a key, closed after 5 s; a command before
the key, closed unanswered; the key of a browser paired at home (the stub's sessions in
`stubs.c`), then the greeting; a second one over Funnel refused, also after 7 s in which the
first sent nothing but answered the radio's pings (the other tab of the same browser); tuning,
mode and drive left unanswered and undone; the browser's audio format and RX audio; a key with
the radio's own microphone answered `trx:0,false;`, one with the page's audio keying and
unkeying; the first one silent 6 s, another browser's key closed and the same browser's key
from another address greeted, the first one closed; the session revoked (the file `REVOKED`
names exists) closing it; a session that may not transmit refused; three wrong keys pausing
Funnel (the stub's rule); then a request through Tailscale's proxy from the tailnet (no such
header) and one from the LAN, taken as before. The
stub keeps what each transmission played in `played.raw` (PLAYED), int16 at 44.1 kHz, for the
client test to read: how many samples, the tone, its level.
`make syntax` only compiles; `make PORT=50411 CONF=/tmp/other.conf` runs on another port and
conf file.
