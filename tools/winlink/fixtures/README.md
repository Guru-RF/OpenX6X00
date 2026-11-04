# Winlink log fixtures

For `tools/winlink/winlink_test.py`.

From the air, as the reporters posted them (Pat's log as the mailbox shows it):

- `km4gkd-w4nwg-80m.pat.log`: KM4GKD, X6100, 80 m to W4NWG, one message sent
  ([#18](https://github.com/Guru-RF/OpenX6X00/issues/18), 2026-10-28).
- `m7sva-dm6ts-40m.pat.log`: M7SVA, UK to DM6TS on 40 m, one message sent; the gateway list's 2000MAX
  had not connected, 200 did (#18, 2026-10-29).
- `ea5jtt-hb9mm-20m.pat.log`: EA5JTT, X6200, 20 m to HB9MM at 2000MAX, twice: the link came up and the
  gateway's password challenge (`;PQ:`) went unanswered, "Exchange failed: context deadline exceeded"
  ([#80](https://github.com/Guru-RF/OpenX6X00/issues/80), 2026-10-29).

From the PC, Mercury (`tools/winlink`, `make modems`, 2026-10-29):

- `pc-n0aaa-n0bbb-mercury.pat.log`, `.mercury.log`, `.events.log`: station A's Pat log, its
  x6x00-mercury's log (Mercury's own lines and the bridge's `x6x00-mercury: ...`) and Pat's event log:
  Pat A sends N0BBB a message peer to peer over Mercury, two stubbed radios.

From the PC, XARA (`make modems`, 2026-11-04):

- `pc-n0aaa-n0bbb-xara.pat.log`, `.xara.log`, `.events.log`: the same with XARA at the free speed, as
  `x6100-winlink` starts it (`tools/xara`'s patch on the GUI's socket): station A's Pat log, its XARA's
  log (our patch's `xara: connected to the GUI's socket ...`, XARA's echo of the VARA host protocol:
  `REGISTERED` without a call, `CONNECTED N0AAA N0BBB 2300`, levels 3 and 4) and Pat's event log. The
  test's temporary paths are written as the radio's (`/run/ardop.sock`, `/run/winlink/xara.conf`).

Nobody posted ardopcf's console or Pat's event log. The tests give those lines in the words of the code
that writes them: ardopcf's `[STATUS: ...]` lines (`third-party/ardopcf/src/common/ARQ.c`) and
`Audio socket ...` lines (`src/linux/SockSound.c`), Pat's events (`app/connect.go`, `app/exchange.go` of
Pat v1.0.0, the same in v1.0.1) and errors (wl2k-go v1.0.1: `Connect timeout`, `aborted while waiting for clear channel`,
the gateway's `Secure login failed` text from `fbb/handshake_test.go`). With Mercury: Pat-Vara v1.2.0's
`connect timeout` and `bandwidth ... not supported` (`vara/transport.go`), Pat's `vara initialization
failed` (`app/connect.go`), and Mercury's `decoder ring full` (`third-party/mercury/modem/modem.c`).
XARA's `xara is falling behind real time` and its echo of `REGISTERED <call>`, `LINK REGISTERED` and
`LINK UNREGISTERED` (`crates/xara/src/audio/mod.rs`, `crates/xara-host/src/response.rs` of v0.1.3; the
registered run of `make modems` printed them).
