# Audit reproduction harnesses 24 October 2026

These host-only harnesses accompany the [audit of revision 00f91e1](../Codex-Audit-2026-10-24.md). They exercise production functions with temporary files, in-memory databases and service/hardware stand-ins. No radio connection, RF, real update, restore or application configuration is involved.

Run from the repository root:

```sh
python3 docs/audit/repro-2026-10-24/run.py
```

Requirements are Python 3, Node.js, a C/C++ compiler, SQLite development files, the checked-out LVGL submodule and the existing host dependency headers under `build-host/deps/include` (particularly liquid-dsp). The runner does not download dependencies. `CC` and `CXX` can select compilers. Generated C, binaries and fixtures live in an automatically removed temporary directory. AddressSanitizer leak detection is disabled because it is unavailable under the environment's process tracing; memory-access checks remain enabled.

**Expected results demonstrate the audited defects.** The runner expects the known erroneous results, including three sanitizer/guard-page failures. After a fix, the corresponding expectation must change. These are audit evidence harnesses, not a passing release test suite.

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

The AST-based Python harnesses omit HTTP decorators and supply only the required globals. The generated C harnesses retain the selected production function bodies while replacing surrounding services. They establish the reported local failure paths and regression results; they do not establish full HTTP, audio, rendering or hardware behavior. The C++ harnesses replace GUI logging/async entry points and use the real storage implementation.

The five code-established findings B02/B05/B14/B15/B16 require the source traces and follow-up integration checks described in the audit. The runner does not reproduce those five.
