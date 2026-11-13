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
which only CW and CW-R refuse (#17). One more is a browser keying its microphone through its TCI
link: the start takes control from the browser's session and its end gives it back, free again
when it was free or the browser left during the over; it stays where it is when a knob took it
during the over or the client had control before its start. One more has two clients over
Tailscale Funnel (#58), one whose session may not transmit: the TX state asked, or an unkey from a client that keyed nothing,
is answered with the state and leaves control with the browser at home; only the session that may
transmit keys, and its unkey ends it. One more has a browser over Funnel on the air, through
the real handshake with tailscaled's header and its key: while its page answers, another
connection is refused at the handshake; once it has been silent 6 s (a phone gone from the WiFi
to its mobile data, its connection left stale), another browser's key is refused and the same
browser's key from another address takes its place, its transmission ended, the newcomer keying;
of two waiting for their key the older goes; wrong keys pause Funnel. One more has a client on
Listen only: its tuning, mode, filter, AGC, RIT, drive and key answered with the state, nothing
done, control left with the browser. One more sends `dds` (Hamlib's TCI backend sends one before
each `vfo`): answered with the frequency, set or asked, nothing done; an unknown command left
unanswered; the `vfo` after a `dds` tunes. One more has a client asking while a browser has
control (Hamlib's TCI backend asks `VFO:0,0;` and `TRX:0;` at every poll): a `vfo` without its
frequency, a `trx` without true or false, the filter without its two edges, the mode, AGC, RIT
and drive without their value, each answered with the state, nothing done; control stays where
it is: with the browser, with another client that tuned, with the asking client during its own
transmission, with the browser again after it, which that transmission's end gave it back to.
Every set takes control as before, a `trx` true or false too, also a value the radio has
already. Six more ask the operator before a client on the network transmits (Ask before TX):
its start answered `trx:0,false;`, keying nothing and taking no control, and the question shown
(the stub's `tci_ask_show`) with its address and the name it gave; a second start asking
nothing more, another client's start refused without a question; then Allow once (its next start
keys, its reconnect asks again), Always (kept in TCI_ALLOWED with its name, not asked after a
reconnect nor after a restart that reads the file back, asked again once forgotten, also on the
connection that is open), No (refused without asking until it reconnects), no answer in time
(No, with the note; a late answer changes nothing) and the client leaving while asked (the
question closed, nothing kept). Not asked: a program on the radio (127.0.0.1, with a User-Agent
as Pat's), a browser page from 127.0.0.1 with its Origin, a client through the radio's own proxy
(X-Forwarded-For's last address is the one asked about), a LAN client's X-Forwarded-For not
believed, its User-Agent cleaned for the screen, a browser that gave a paired key (keys at once),
one whose session may only watch (refused without a question), a wrong key (asked), a start while
the radio is on the air and one under Listen only (refused, nothing asked); with `tx_confirm` off
every client keys as before, and the file says `tx_confirm=0`. Ten more are CW from a client
(#179), on the stub keyer in `stubs.c` (it keeps what it was given, sends until the case ends
the text, all of it or cut short, and refuses its first key with `stub_tx_refused`): a text
keyed in CW and CW-R as the keyer takes it, every client hearing the radio on the air, another
client's key and text meanwhile refused, `cw_macros_empty:0;` to its client alone at the end and
control back with the browser; refused outside CW and CW-R (`trx:0,false;`, the note on the
screen once in 5 s, nothing asked), also when the mode changed on its way to the keyer; Ask
before TX at its first text (Allow once, then its next text keys) and Listen only (the state and
KEY Speed answered, nothing done); refused at its first key where the radio may not transmit; the
keyer shared with the operator's texts (a client's refused while the operator's goes, also when
it got past the socket thread first; the operator's started during a client's cuts it short,
`trx:0,false;` and `cw_macros_empty:0;`, what waited dropped, the operator's not stopped);
stopped by `cw_macros_stop;` (another client's stop changes nothing, no `cw_macros_empty`), by its
`trx:0,false;`, by its client leaving, and a text on its way when it left keys nothing; a stop lost
to a full queue sent again, nobody starting meanwhile; more text waiting and following as one
text, `cw_macros_empty` once, and the text stopped when the mode is set to USB; `cw_macros_speed`
and `cw_keyer_speed` asked (with no argument, with 0, with a word) and set (either form, either
name, 60 to 50, 2 to 5), both names told to every client, the speed it has answered to the setter
alone, a change on the radio told too; and the text's rules (`cw_text`: `^ ~ *` back, `|SK|` one
sign or the letters of a run with none, TCI's own example of `<` and `>`, what has no Morse left
out, a text longer than the queue cut with the speed back) with the commands ignored unanswered
(another transceiver, no text, nothing to key, `cw_msg`, `cw_terminal`) and a comma left in a
text kept. One more is a knob over Bluetooth (`tci_server_bt_take`, as bt_dev.c hands BlueZ's
socket over, a copy kept as BlueZ keeps one): known by the device's address, asked as
"Bluetooth AA:BB:CC:DD:EE:01 (VFO-Knob BT/1.0)", Always kept by that address and its next
connection keying without a question; silent 4 s on the air it stays keyed, 5 s and its
transmission ends with the connection kept, silent with nothing on the air it is only pinged;
BlueZ asking to let go (`tci_server_bt_drop`) closes it and the other end sees the end while
BlueZ's copy is open (a shutdown, not a close alone); the link gone unkeys at once; with TCI off
a connection is refused the same way. Each runs in a child process, its clients on socket
pairs, without a listener, and three times: the clients' commands as written, in capitals (as
Hamlib's TCI backend sends them, `TRX:0,TRUE,TCI;`, `AUTH:` over Funnel) and in mixed case
(`TrX:0,TrUe,tCi;`), which TCI says are the same: the same answers, in lower case. It prints
`OWNER OK: n cases, each as written, in capitals and in mixed case`.

Then it builds `src/tci_server.c` and `src/tci_ws.c` (with `src/tci_audio.c` and
`src/control_owner.c`) on the PC against the stubs in `stubs.c` (the radio, the settings, the
scheduler, the meters, PulseAudio: a few variables and a 1 kHz tone), starts it on
127.0.0.1:50001 and runs `tci_client_test.py`, a raw WebSocket client that does what the
VFO-Knob does on a connection: the greeting, tuning, mode, filter, AGC, RIT, drive, PTT, the
sensor streams (tx_sensors before the radio's first reading: 0 W, SWR 1.0), RX audio as the
knob asks for it (24 kHz int16, one channel, 512 a frame), a
transmission with TCI audio, a TX frame whose payload is not what its header says (the
transmission ends, nothing of it played), ping. Then four more connections: the browser
pages' format asked for in full; formats out of range and int24 stereo at 48 kHz in frames of
100; WSJT-X's TCI client ("TCI Client RX1") on Test CAT, its answers read as its parser
reads them, its RX audio in TCI's defaults (48 kHz float32 stereo: the tone's level, the image,
left and right), and a transmission as it keys, in the digu of its Mode Data/Pkt, every
TX_CHRONO answered with its frame; and
WSJT-X on a link slower than its RX audio (read at 250 kB/s through a 4 kB receive buffer),
keyed with the server's queue full: the TX_CHRONOs come ahead of the queued audio, the over
stays keyed for 3 s, every frame arrives whole. Then a connection in capitals and mixed case, as
Hamlib's TCI backend and fldigi's TCI build send their commands (`DDS:` then `VFO:`,
`MODULATION:0,CW;`, `TRX:0,true,Vac;`, `TRX:0,true,tci;`, `AUDIO_STREAM_SAMPLE_TYPE:INT16;`): each
taken as in lower case, every answer's name in lower case. Last, Tailscale Funnel (#58), where only the
browser Panel's own sound and microphone may come, with tailscaled's
`Tailscale-Funnel-Request` header: nothing before a key, closed after 5 s; a command before
the key, closed unanswered; the key of a browser paired at home (the stub's sessions in
`stubs.c`), then the greeting; a second one over Funnel refused, also after 7 s in which the
first sent nothing but answered the radio's pings (the other tab of the same browser); tuning,
mode and drive left unanswered and undone, also in capitals; the browser's audio format and RX
audio; a key with the radio's own microphone answered `trx:0,false;` (also `TRX:0,TRUE;`), one
with the page's audio keying and
unkeying; the first one silent 6 s, another browser's key closed and the same browser's key
from another address greeted, the first one closed; the session revoked (the file `REVOKED`
names exists) closing it; a session that may not transmit refused; three wrong keys pausing
Funnel (the stub's rule); then a request through Tailscale's proxy from the tailnet (no such
header) and one from the LAN, taken as before. Then Ask before TX on the running server, the
operator played through the files ASK (the question on the screen: its id and who) and ANSWER
(`once`, `always`, `no`, `off`, `on`, `forget ADDR`, read by the stub every 50 ms), the clients
on the network coming from loopback through X-Forwarded-For as from the radio's proxies: a
program on the radio keys unasked; a client asked, its second start asking nothing more and
another client's start refused unasked; Allow once keying, its reconnect asked again; No
refusing until it reconnects; Always keeping it in ALLOWED (`192.0.2.12`, `JTDX`), not asked
after a reconnect, asked again once forgotten; the Panel's key from a paired browser keying
unasked, a session that may only watch refused unasked; off keying everyone, on asking again.
Last, CW from a client as QLog's TCI driver sends it ("Morse over CAT", its commands in
capitals): the greeting's `cw_macros_speed` and `cw_keyer_speed`; a text in USB refused;
`CW_MACROS_SPEED;` answered and `CW_KEYER_SPEED:28;CW_MACROS_SPEED:28;` setting both;
`CW_MACROS:0,CQ DE ON6URE^ 5NN~ TU* |SK|;` keyed as `CQ DE ON6URE: 5NN, TU; <SK>` (the stub's
keyer writes each text it is given to the file KEYED names, the speed steps as `+ - =`, and
ends it by itself at 20 ms a character), then `cw_macros_empty:0;`; two texts in one frame, the
second after the first, with speed steps; `CW_MACROS_STOP;` and a client gone mid-text, each
stopping it; a client on the network asked at its first text, its next one keyed after Allow
once. The
stub keeps what each transmission played in `played.raw` (PLAYED), int16 at 44.1 kHz, for the
client test to read: how many samples, the tone, its level.
`make syntax` only compiles; `make PORT=50411 CONF=/tmp/other.conf` runs on another port and
conf file.
