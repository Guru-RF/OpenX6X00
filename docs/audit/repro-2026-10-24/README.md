# Audit reproduction harnesses 24 October 2026

These host-only harnesses accompany the [audit of revision 00f91e1](../Codex-Audit-2026-10-24.md). They exercise production functions with temporary files, in-memory databases and service/hardware stand-ins. No radio connection, RF, real update, restore or application configuration is involved.

Run from the repository root:

```sh
python3 docs/audit/repro-2026-10-24/run.py
```

Requirements are Python 3, Node.js, a C/C++ compiler, SQLite development files, the checked-out LVGL submodule and the existing host dependency headers under `build-host/deps/include` (particularly liquid-dsp). The runner does not download dependencies. `CC` and `CXX` can select compilers. Generated C, binaries and fixtures live in an automatically removed temporary directory. AddressSanitizer leak detection is disabled because it is unavailable under the environment's process tracing; memory-access checks remain enabled.

**Expected results now demonstrate the fixes.** As delivered (commit `4cad3f5`) the runner expected the known erroneous results, including three sanitizer/guard-page failures. Each fix flipped the matching expectation, so the runner is now a regression suite for the twelve harnessed findings: a run ends with "All 12 reproduced findings and the selected original-fix checks matched the audit" when every fix still holds. The original expectations can be read in the runner's history.

| Harness | Evidence |
|---|---|
| `make-unkey-harness.py` | B01: extracts current TCI parsing/handler code and links current control ownership; stubs radio/session operations |
| `adif-frequency.c` | B03: calls the production static frequency formatter at 145.5 MHz |
| `adif-input.c`, `guard-malloc.c` | B04: runs the production reader with `<CALL:5`; places its seven-byte allocation before an unreadable page |
| `field-input.c` | B06: runs the production Field Ops reader with a truncated 1,023-byte record |
| `make-log-harness.py` | B07: extracts current receipt/cache/submission functions; counts scheduled dispatches |
| `web_checks.py` | B08/B13: extracts current upload/cluster functions, forces interleavings and runs the production updater against temporary installation paths |
| `log_checks.py` | B09/B17: extracts current log functions and simulates stale ordinal requests and a MODE edit |
| `ptt-renewal.js` | B10: evaluates the complete production PTT component with a fake clock |
| `vfo.cpp` | B11: links production SettingsManager/storage code and loads `sql/params.sql` into SQLite |
| `settings-rollback.cpp` | B12: uses the same production code and injects rollback after a settings flush |
| `updater_checks.py` | A02/A03: runs production apply/undo/boot-check with interruption injection and temporary recovery state |
| `backup_checks.py` | A10/A22: checks stale-token and persistent-writer rejection using production restore logic and request/process stand-ins |

Checks for the [26 October audit](../Codex-Audit-2026-10-26.md) on the browser remote, added with their fixes:

| Harness | Evidence |
|---|---|
| `ptt-hold.js` | C02, the page: production RadioLink and RadioPtt with a fake socket and clock; a link that stops delivering ends the hold (release sent, no renewal), refuses presses until a ping sent after the silence is answered, and numbers every press; pings are numbered too, so a pong the radio dropped (or the sync ping's own) stalls the link only until a later ping is answered, and a radio answering a bare pong still works |
| `make-remote-harness.py`, `remote-scenarios.c` | C02, the radio: extracts the remote server's `handle_text` with the PTT lease, key, log and set functions it calls, links the real control owner, stubs the clock, input devices, GUI queue and sockets; `ptt` replays late renewals after the lease ran out, a refused press's renewal and a page without numbers; `ping` checks that a pong repeats its ping's number (a bare ping, from a page before, gets a bare pong) |

The AST-based Python harnesses omit HTTP decorators and supply only the required globals. The generated C harnesses retain the selected production function bodies while replacing surrounding services. They establish the reported local failure paths and regression results; they do not establish full HTTP, audio, rendering or hardware behavior. The C++ harnesses replace GUI logging/async entry points and use the real storage implementation.

The five code-established findings B02/B05/B14/B15/B16 require the source traces and follow-up integration checks described in the audit. The runner does not reproduce those five; their fixes are verified by build and by hand on the radio.
