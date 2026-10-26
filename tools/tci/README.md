# TCI server, host test

`make` builds `src/tci_server.c` and `src/tci_ws.c` (with `src/control_owner.c`) on the PC
against the stubs in `stubs.c` (the radio, the settings, the scheduler, the meters,
PulseAudio: a few variables and a tone), starts it on 127.0.0.1:50001 and runs
`tci_client_test.py`, a raw WebSocket client that does what the VFO-Knob does on a
connection: the greeting, tuning, mode, filter, AGC, RIT, drive, PTT, the sensor streams, RX
audio, a transmission with TCI audio, a TX frame whose payload is not what its header says
(the transmission ends, nothing of it played), ping. Then a second connection opens as
WSJT-X's TCI client ("TCI Client RX1") does on Test CAT, its answers read as its parser reads
them. The stub keeps what each transmission played in `played.raw` (PLAYED), int16 at
44.1 kHz, for the client test to read.
`make syntax` only compiles; `make PORT=50411 CONF=/tmp/other.conf` runs on another port and
conf file.
