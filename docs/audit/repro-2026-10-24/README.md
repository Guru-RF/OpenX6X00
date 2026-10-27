# Audit reproduction harnesses 24 October 2026

These host-only harnesses accompany the [audit of revision 00f91e1](../Codex-Audit-2026-10-24.md). They exercise production functions with temporary files, in-memory databases and service/hardware stand-ins. No radio connection, RF, real update, restore or application configuration is involved.

Run from the repository root:

```sh
python3 docs/audit/repro-2026-10-24/run.py
```

Requirements are Python 3, Node.js, a C/C++ compiler, SQLite development files, the checked-out LVGL submodule and the existing host dependency headers under `build-host/deps/include` (particularly liquid-dsp). The runner does not download dependencies. `CC` and `CXX` can select compilers. Generated C, binaries and fixtures live in an automatically removed temporary directory. AddressSanitizer leak detection is disabled because it is unavailable under the environment's process tracing; memory-access checks remain enabled.

**Expected results now demonstrate the fixes.** As delivered (commit `4cad3f5`) the runner expected the known erroneous results, including three sanitizer/guard-page failures. Each fix flipped the matching expectation, so the runner is now a regression suite for the harnessed findings: the twelve of this audit, and those of later audits whose fixes added a check (in the table below). A run ends with "All reproduced findings and the selected original-fix checks matched the audits" when every fix still holds. The original expectations can be read in the runner's history.

| Harness | Evidence |
|---|---|
| `make-unkey-harness.py` | B01: extracts current TCI parsing/handler code and links current control ownership; stubs radio/session operations |
| `adif-frequency.c` | B03: calls the production static frequency formatter at 145.5 MHz |
| `adif-input.c`, `guard-malloc.c` | B04: runs the production reader with `<CALL:5`; places its seven-byte allocation before an unreadable page |
| `field-input.c` | B06: runs the production Field Ops reader with a truncated 1,023-byte record |
| `make-log-harness.py` | B07: extracts current receipt/cache/submission functions (with their payload digest since C03); counts scheduled dispatches |
| `web_checks.py` | B08/B13: extracts current upload/cluster functions, forces interleavings and runs the production updater against temporary installation paths |
| `log_checks.py` | B09/B17: extracts current log functions and simulates stale ordinal requests and a MODE edit. C10 of the [26 October review](../Codex-Audit-2026-10-26.md): names, QTH and comments with ü, Ł and ✓ next to other tags and a Latin-1 record, edited and deleted, read back by the web and by x6100-wavelog |
| `ptt-renewal.js` | B10: evaluates the complete production PTT component with a fake clock |
| `vfo.cpp` | B11: links production SettingsManager/storage code and loads `sql/params.sql` into SQLite |
| `settings-rollback.cpp` | B12: uses the same production code and injects rollback after a settings flush |
| `updater_checks.py` | A02/A03: runs production apply/undo/boot-check with interruption injection and temporary recovery state |
| `backup_checks.py` | A10/A22: checks stale-token and persistent-writer rejection using production restore logic and request/process stand-ins |
| `keyboard-scope.js` | C09 of the [26 October audit](../Codex-Audit-2026-10-26.md): the Panel's and the Screen page's production keyboard code with `radio_session.js` and its dialogs, on the pages' own markup; Space and Enter on focused buttons, a select, contenteditable, open dialogs, the PTT button, nothing focused; a click on a button giving the keyboard back to the page; the picture list chosen with the mouse or the keyboard |
| `edit-flow.js` | C12 of the same audit: `edit_flow.js`, `list_editor.js` and the CW messages, Memories, Macros and Bands pages' own scripts, the web server a stand-in answering late, out of order or with a failure; an edit typed during a save, a rename, a new entry, a timeout, a retry without another edit, Save all and a delete going out one at a time, Close on Bands during a save and after a failure |
| `browser-dom.js` | Not a check: a small DOM and jQuery stand-in under which the pages' own scripts run in Node, without a browser or an npm package |

Checks for the [26 October audit](../Codex-Audit-2026-10-26.md) on the browser remote, added with their fixes:

| Harness | Evidence |
|---|---|
| `ptt-hold.js` | C02, the page: production RadioLink and RadioPtt with a fake socket and clock; a link that stops delivering ends the hold (release sent, no renewal), refuses presses until a ping sent after the silence is answered, and numbers every press; pings are numbered too, so a pong the radio dropped (or the sync ping's own) stalls the link only until a later ping is answered, and a radio answering a bare pong still works; an err that is not the PTT's leaves a press waiting for the radio's "tx 1", the PTT's own refusal ends it with its release |
| `make-remote-harness.py`, `remote-scenarios.c` | C02, the radio: extracts the remote server's `handle_text` with the PTT lease, key, log and set functions it calls, links the real control owner, stubs the clock, input devices, GUI queue and sockets; `ptt` replays late renewals after the lease ran out, a refused press's renewal and a page without numbers; `ping` checks that a pong repeats its ping's number (a bare ping, from a page before, gets a bare pong); `log` (C03) has two tabs of one session send different QSOs under one number (a conflict, one write) and under the fixed page's 53-bit numbers (two writes, a resend answered not written); `set` (C13) refuses a set for view only, control elsewhere, a locked or out-of-range frequency, a missing mode, a full GUI queue and an unknown name, each with an err and the state due again |

The Panel's own side of C03 and C13 (the tabs' numbers and storage, the tune shown as asked until confirmed) lives in the page's inline script and was checked with the whole page in jsdom; those page harnesses are not part of this runner, which needs no npm packages.

The AST-based Python harnesses omit HTTP decorators and supply only the required globals. The generated C harnesses retain the selected production function bodies while replacing surrounding services. They establish the reported local failure paths and regression results; they do not establish full HTTP, audio, rendering or hardware behavior. The C++ harnesses replace GUI logging/async entry points and use the real storage implementation.

The five code-established findings B02/B05/B14/B15/B16 require the source traces and follow-up integration checks described in the audit. The runner does not reproduce those five; their fixes are verified by build and by hand on the radio.
