# TCI server, host test

`make` first runs `tci_audio_test` (`make audio_test`): `src/tci_audio.c`'s 24 to 48 kHz
halfband swept from 0.1 to 11.9 kHz (the tone's level, its image at 24 kHz - f), the stream
cut in reads of any length, the four sample types written to two channels and read back, and
what the interpolation costs a second of audio on this PC.

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
left and right), and a transmission as it keys, every TX_CHRONO answered with its frame; and
WSJT-X on a link slower than its RX audio (read at 250 kB/s through a 4 kB receive buffer),
keyed with the server's queue full: the TX_CHRONOs come ahead of the queued audio, the over
stays keyed for 3 s, every frame arrives whole. The
stub keeps what each transmission played in `played.raw` (PLAYED), int16 at 44.1 kHz, for the
client test to read: how many samples, the tone, its level.
`make syntax` only compiles; `make PORT=50411 CONF=/tmp/other.conf` runs on another port and
conf file.
