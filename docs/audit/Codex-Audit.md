# Code audit — OpenX6X00

Date: 2026-10-22. Reviewed revision: `d6bf443ac07cb7978a4c7499decea25d95a25c90`.

This is a findings report, not an implementation plan approved for execution. No application code, configuration, dependencies or hardware state was changed. The companion documents contain [general usability ideas](Codex-Possible-Improvements.md) and [browser remote ideas](Codex-Remote-Improvements.md).

## Scope and evidence

The review concentrated on C/C++ thread and object ownership, CAT/TCI and browser control, audio and recording, settings persistence, ADIF/log handling, Python HTTP handlers, backup/restore and application updates. Vendored LVGL was inspected where application behavior depended on it; the vendored modem implementations, kernel, baseband firmware and every digital-mode algorithm were not exhaustively audited.

Evidence labels used below:

- **Reproduced:** exercised production functions in an isolated host harness, with external services, paths or radio operations replaced by temporary files or stubs. This establishes the local defect, not its frequency on hardware.
- **Code-established:** the implementation contains a concrete failure path or unsafe interleaving; it was not reproduced on a radio.
- **Follow-up:** a plausible concern requiring more investigation, listed separately from established findings.

**High** means a credible stuck transmission, crash, lost data or failed recovery. **Medium** means incorrect state, interference between operations or a narrower reliability defect. These are engineering priorities, not CVSS ratings.

## Findings at a glance

| ID | Priority | Finding | Evidence |
|---|---|---|---|
| A01 | High | TCI audio PTT in a non-voice mode can remain asserted | Reproduced |
| A02 | High | Update journal can miss a file already replaced | Reproduced |
| A03 | High | Repeating an interrupted rollback can delete restored files | Reproduced |
| A04 | High | Observer notification can use a deleted observer | Reproduced with AddressSanitizer |
| A05 | High | CAT updates can enter LVGL from the CAT worker thread | Code-established |
| A06 | High | Overlapping screenshots share and free the same buffers | Code-established |
| A07 | High | RX recorder can close a file while the audio callback writes it | Code-established |
| A08 | High | Recorder playback outlives its UI and lacks thread cleanup | Code-established |
| A09 | High | Malformed ADIF lengths cause an out-of-bounds read | Reproduced with AddressSanitizer |
| A10 | High | Restore confirmation is not bound to the uploaded backup | Code-established |
| A11 | High | Web log editing can discard a QSO appended by the radio | Reproduced |
| A12 | Medium | Concurrent web configuration saves collide and lose edits | Reproduced |
| A13 | Medium | Memory/channel editing silently rounds frequencies to kHz | Reproduced |
| A14 | Medium | Legacy settings discard retry state after failed writes | Reproduced |
| A15 | Medium | ADIF import skips supported-looking input and retires failed imports | Reproduced parsing; code-established archival behavior |
| A16 | Medium | TCI RX audio has shared-state races and a lost-start window | Code-established |
| A17 | Medium | TCI drive reporting becomes stale after a local power change | Reproduced |
| A18 | Medium | A late microphone permission result can leave capture active after Off | Code-established |
| A19 | Medium | Audio capture forwards holes/errors as sample buffers | Code-established |
| A20 | High | Independent updater processes can mutate the same recovery state | Code-established |
| A21 | Medium | Queued configuration commands can be dropped after an OK response | Code-established |
| A22 | High | Restore proceeds after a failed GUI stop command | Code-established; conditional on service failure |

## Detailed findings

### A01 — TCI audio PTT can escape its release path

**High · Reproduced.** Evidence: [TCI command application](../../src/tci_server.c#L270), particularly lines 275–287 and 326–343; [session ownership assignment](../../src/tci_server.c#L581), lines 589–599.

A client sending `trx:0,true,tci;` gets a TCI audio-session owner. In CW, CW-R, DIGU or DIGL, `voice_mode()` is false, so `C_KEY` falls back to `radio_set_ptt(true)` without setting `txs.keyed` or the physical-PTT owner. The corresponding TCI unkey returns immediately when `txs.keyed` is false. The physical PTT remains asserted after session ownership is cleared; disconnect and the physical-PTT timeout no longer identify its owner.

The isolated harness selected CW and called the production handlers: after key, `ptt=1`, `modem=0`, `keyed=0`; after explicit unkey, `ptt=1`, both owners cleared. Advancing the watchdog past its timeout still left PTT asserted. The browser microphone uses this same TCI key command.

This establishes a failed session-release path; independent local controls or hardware protections may still stop RF.

**Future direction:** reject incompatible audio-TX modes before acquiring ownership, or record the actual keying mechanism and release exactly that mechanism. Ownership and hardware keying need one consistent lifecycle.

**Verification:** cover every mode with key/unkey, rejected key, disconnect, server disable, missing audio and timeout. Perform hardware verification into a dummy load after host checks.

### A02 — Update recovery overlooks a replacement made before its journal entry

**High · Reproduced.** Evidence: [update apply and undo](../../rootfs/usr/sbin/x6100-update#L239), especially lines 261–263 and 274–275.

The updater replaces the installed file, then persists `done=true`. A process failure or power loss between those operations leaves the new file installed while the durable journal still says it was untouched. `undo()` skips that entry. Recovery can therefore leave a mixture of old and new application files.

Fault injection at the journal save after replacement left the destination containing `new`, with persisted `done=false`; invoking the production undo function left `new` in place.

There is a second marker gap at lines 264–266: the journal becomes `applied` before `PENDING` is written. An interruption there leaves neither an `applying` state nor a pending marker for boot-check's automatic crash-count rollback.

**Future direction:** use a recoverable per-file state machine that records intent before replacement and reconciles both sides after interruption. Include backup and parent-directory durability in the recovery design.

**Verification:** inject interruption before and after every copy, replace and journal persistence step; recovery must consistently produce the old installation or the complete new installation.

### A03 — Rollback is destructive when retried

**High · Reproduced.** Evidence: [undo](../../rootfs/usr/sbin/x6100-update#L271), lines 278–282.

Undo moves the saved previous file back into place, consuming the backup. If rollback is interrupted before completion and run again against the same journal, the backup is missing. The `else` branch then removes the destination even when the journal says that destination existed before the update. A successful restoration becomes a deletion.

Calling the production undo function twice on the same temporary journal restored the old file on the first pass and deleted it on the second.

**Future direction:** make rollback idempotent; distinguish “original file absent” from “backup already consumed or unexpectedly missing,” and journal restoration progress durably.

**Verification:** repeat rollback, including interruption between individual restorations. A previously restored file must remain restored.

### A04 — Observer snapshots do not preserve observer lifetime

**High · Reproduced with AddressSanitizer.** Evidence: [notification snapshot](../../src/cfg/subject.cpp#L150), [unsubscribe](../../src/cfg/subject.cpp#L99), [observer deletion on button unload](../../src/button_bar.cpp#L488), [SWR-window subscriptions and teardown](../../src/swrscan_window.c#L235).

`notify_impl()` copies raw observer pointers under the subscription mutex, releases that mutex, then dereferences the copy. Another thread can unsubscribe and delete an observer after the copy. Protecting the vector does not protect the copied objects. Main-thread-only deletion is insufficient while a background thread can still be notifying.

An isolated harness paused the first callback after the snapshot, unsubscribed/deleted the second observer on the other thread, then resumed notification. AddressSanitizer reported heap-use-after-free at `subject.cpp:157`. CAT provides a real background producer; GUI teardown provides a deletion path.

**Future direction:** establish one notification/destruction owner, or give notification snapshots lifetime protection with cancellation semantics. The invariant documented in `subject.h:68–83` needs to cover concurrent producers as well as deletion inside callbacks.

**Verification:** repeatedly modify parameters while switching apps, unloading button pages and rebuilding themes; use deterministic lifetime tests plus sanitizers.

### A05 — CAT parameter changes can manipulate LVGL off the GUI thread

**High · Code-established.** Evidence: [CAT worker](../../src/cat.cpp#L1147), [frequency/mode mutations](../../src/cat.cpp#L583), [delayed notification](../../src/cfg/subject.cpp#L19), [LVGL async implementation](../../lvgl/src/misc/lv_async.c#L45), [GUI loop](../../src/gui_main.c#L250).

The CAT thread directly processes requests and mutates parameters. Their observers run on the producer thread. `ObserverDelayed::notify()` calls `lv_async_call()`, which allocates LVGL memory and creates an LVGL timer immediately; it does not merely push into the application's protected scheduler. These operations can overlap the GUI's `lv_timer_handler()`. The CAT request mutex serializes CAT requests, not CAT against GUI work.

**Future direction:** route CAT mutations and GUI-facing observer delivery through an explicitly owned GUI command queue. Audit other worker-originated parameter changes for the same path.

**Verification:** add thread-ownership assertions around UI entry points and stress CAT tuning/mode changes during screen transitions. This is related to A04 but remains a separate problem even if observer objects stay alive.

### A06 — Two screenshots can corrupt each other's memory

**High · Code-established.** Evidence: [screenshot globals and worker](../../src/screen_shot.c#L24), especially lines 54–65, 81–86 and 90–103.

Every screenshot overwrites global `snap` and `argb` and starts a detached writer. Workers also share the `rows` array. Trigger another screenshot before the first writer finishes: the first worker can read the second image, free its buffer, or free row pointers another worker still uses. Both workers can ultimately free the same current global pointer. The first allocation can also become unreachable. Slow SD writes make overlap more likely.

**Future direction:** pass an independently owned snapshot job to each worker, or serialize captures with a visible busy state. Check allocation, snapshot and thread-creation results too.

**Verification:** trigger bursts of screenshots with delayed writes and both landscape/portrait transitions under AddressSanitizer.

### A07 — RX recording has a close-versus-write race

**High · Code-established.** Evidence: [recorder state and file operations](../../src/recorder.c#L56), [audio callback use](../../src/dsp.cpp#L512).

The audio thread checks `recorder_is_on()` and subsequently writes through the global `SNDFILE *`. The UI can set `on=false` and close that file between those operations. Neither the Boolean nor the file handle is protected. A callback that already passed the check can use a closed handle. Repeated start/stop calls also have no idempotence guard; stopping does not clear `file`.

**Future direction:** give recording one owner and synchronize stop with in-flight writes. A bounded sample queue would also move SD/MP3 work out of the PulseAudio callback. Surface short writes and full-card failures.

**Verification:** repeatedly start/stop recording while capture callbacks run, introduce slow/full storage, and verify clean file closure and continued audio processing.

### A08 — Playback can access a destroyed recording list or restart after Stop

**High · Code-established.** Evidence: [playback worker](../../src/recorder_window.c#L114), [close callback](../../src/recorder_window.c#L264), [thread creation](../../src/recorder_window.c#L280).

The worker reads the selected LVGL table row after starting audio setup. Closing the screen can destroy that table before `selected()` executes. Close/Stop merely stores `playing=false`, while the worker later unconditionally sets it true at line 135: a delayed worker can start playback after the user has stopped or left the app. Normal worker completion is neither joined nor detached, leaking joinable-thread resources over repeated playback.

The worker can additionally call `recorder_set_on(false)`, which updates LVGL buttons/list directly, and uses a shared playback buffer. Rapid close/reopen/play sequences can overlap old and new workers.

**Future direction:** capture the filename on the UI thread, give each playback a session identity, and cancel/join it before UI teardown or a new playback. Deliver UI updates through the GUI queue.

**Verification:** delay Bluetooth/file opening, then immediately Stop, leave, reopen and Play again. Confirm no stale UI access, late playback or accumulating threads.

### A09 — ADIF field lengths are trusted beyond the input buffer

**High · Reproduced with AddressSanitizer.** Evidence: [field-length parsing](../../src/adif.c#L147), [string extraction](../../src/adif.c#L292).

The parser takes a length from the tag and copies that many bytes without checking how many remain in the line. A truncated `<MODE:99999>X<EOR>` record with CRLF reaches `memcpy()` with a 99,999-byte read from a much smaller allocation. Startup imports `/mnt/incoming_log.adi`, so a malformed imported file can crash the GUI during import.

The harness compiled copied production `adif.c` with a minimal utility-header shim and radio-independent band helper. AddressSanitizer reported heap-buffer-overflow at `adif.c:295`, called from line 161.

**Future direction:** parse bounded fields, reject invalid/overflowing lengths, advance past each value, and validate allocations and record limits before use.

**Verification:** malformed/truncated tags, extreme lengths, embedded tag-like text, missing fields and large files must report import errors without a crash or partial silent success.

### A10 — Restore can apply a different backup from the one previewed

**High · Code-established.** Evidence: [global staging directory](../../webserver/src/x6100_webserver/apps.py#L2128), [upload replacement](../../webserver/src/x6100_webserver/apps.py#L2132), [restore selection](../../webserver/src/x6100_webserver/apps.py#L2225).

All users/tabs share one staged upload. The lock protects an individual request, but no upload identifier or manifest digest binds the preview to a later restore. Tab A can preview backup A; tab B uploads B; tab A then confirms and restores B using only its selected component names.

**Future direction:** bind restore to an immutable staged backup identity. Treat restore as a transaction with explicit partial-failure reporting; see A22 for its service-stop check.

**Verification:** interleave two tabs' uploads/previews/restores; simulate a service returning failure from Stop. Restore must refuse an identity mismatch or a still-running writer.

### A11 — Editing a log can delete a newly appended QSO

**High · Reproduced.** Evidence: [web log read/modify/replace](../../webserver/src/x6100_webserver/apps.py#L706), especially the size check and replacement at lines 736–740; [native Field Ops append](../../src/field/flog.c#L110).

The web editor checks that the original log size has not changed, then replaces it with its edited snapshot. The radio can append a QSO after that check and before replacement. That successful append is discarded with the old file. The Python lock does not coordinate with the native append writer; a size check alone is not mutual exclusion.

An isolated run of the production edit helper injected a native-style append between the size check and replacement. Editing reported success, but the appended QSO disappeared.

**Future direction:** coordinate all log writers through one owner or a shared locking protocol, or edit records in a transactional store and regenerate exports. An optimistic version check must be atomic with the update to protect against concurrent writers.

**Verification:** append during every phase of edit/delete and verify that the new QSO survives or the edit is rejected without replacing the log.

### A12 — Threaded HTTP saves share temporary filenames and stale snapshots

**Medium · Reproduced.** Evidence: [thread-per-connection server](../../webserver/src/x6100_webserver/__main__.py#L22), [channel writer](../../webserver/src/x6100_webserver/apps.py#L1376), [macro writer](../../webserver/src/x6100_webserver/apps.py#L1231), [cluster writer](../../webserver/src/x6100_webserver/apps.py#L185).

Several writers use a fixed `path + ".tmp"` without serializing the complete read/modify/write operation. Two requests can overwrite the same temporary file, race its rename, or replace newer state with a stale snapshot. Unique temporary files alone would fix the filename collision but still permit lost updates.

Forcing two production channel writes to reach replacement together produced one successful request, one HTTP 500/`ENOENT`, and only one channel addition surviving.

**Future direction:** serialize each resource's complete update and use unique temporary files; add revision checking where clients edit a whole list. Account for native writers of shared configuration too.

**Verification:** simultaneous add/edit/delete from two tabs and overlapping native configuration changes should merge safely or report a conflict.

### A13 — Saving a memory can change its frequency without a frequency edit

**Medium · Reproduced.** Evidence: [memory/channel row rendering and serialization](../../webserver/src/x6100_webserver/views/memories.html#L57), lines 63 and 74.

The editor formats integer Hz as MHz with three decimals, then saves that rounded displayed value back as Hz. Changing only mode, AGC or a channel name can therefore change the frequency. The shared row builder affects both memories and channels.

The actual JavaScript conversion gives `14,074,500 Hz → "14.075" MHz → 14,075,000 Hz`: a silent 500 Hz shift. The input also advertises only a 0.001 MHz step.

**Future direction:** preserve the original integer Hz unless the user edits frequency; display enough precision and use the same frequency conventions throughout the UI.

**Verification:** round-trip memories at 1 Hz, 10 Hz and 100 Hz offsets while changing unrelated fields; the frequency must remain exact.

### A14 — Failed legacy settings writes are marked saved

**Medium · Reproduced.** Evidence: [legacy database writers](../../src/params/db.c#L137), [transaction](../../src/params/params.c#L367), [unchecked commit](../../src/params/params.c#L443).

Each legacy `params_write_*()` ignores bind/step results and clears `dirty` unconditionally. An I/O error, full card or other failed insert removes the pending retry. Dirty bits are also cleared before the surrounding transaction has committed successfully. The newer [pending-write queue](../../src/cfg/pending_writes.cpp#L67) retains failed entries, so the two settings systems have inconsistent guarantees.

A harness used the production writer against an in-memory SQLite table with an aborting insert trigger. Result: `dirty=0 persisted_rows=0`.

**Future direction:** retain dirty state until successful transaction commit, report persistent failures and coordinate transaction ownership across legacy/new settings paths.

**Verification:** inject statement and commit failures, restore storage availability, then flush. The latest values must eventually persist without another user edit.

### A15 — ADIF import silently skips formats and archives incomplete work

**Medium · Reproduced parsing behavior; code-established import handling.** Evidence: [ADIF reader](../../src/adif.c#L119), particularly lines 139, 152–179 and 183; [import completion](../../src/qso_log.c#L190).

Records are accepted only if a single line ends exactly with uppercase `<EOR>\r\n`. LF endings, multiline records and other layouts are skipped. The tag regex ignores typed fields such as `<CALL:5:S>`, and the local call is taken from `OPERATOR` without a `STATION_CALLSIGN` fallback. The importer then renames the source to `.bak` even when zero records were recognized or inserts failed, making the next startup treat the import as completed.

For the same small record, the harness read one CRLF record, zero LF records, and one record with an empty remote callsign after using a typed CALL tag. That empty callsign is refused by the database save function.

**Future direction:** use a field-length-aware record parser independent of line endings, report accepted/rejected/failed counts, and retire the source only after confirmed success. Preserve a resumable failure state.

**Verification:** fixtures for LF/CRLF, multiline records, typed/lowercase tags, station/operator differences and failed inserts; no unreported record loss.

### A16 — TCI RX subscription and format changes race with capture

**Medium · Code-established.** Evidence: [subscription updates](../../src/tci_server.c#L620), [capture loop](../../src/tci_server.c#L796), [client closure](../../src/tci_server.c#L255).

The capture thread reads `fd`, `audio_on` and `audio_float` while the socket thread changes them without a common synchronization discipline. `audio_float` is read separately for the frame header and payload; a concurrent change can produce a header for one representation and data in the other. Locking the output buffer later does not make those earlier reads safe.

There is also a lost-start window: capture finds no listeners, a socket request enables one and stores `cap_wanted=true`, then capture overwrites it with false. The active subscriber can be left waiting until another start request changes the flag.

**Future direction:** snapshot per-client capture state under a shared lock with a client generation, and derive capture demand under that same ownership model rather than racing unconditional Boolean stores.

**Verification:** rapidly start/stop audio, switch int16/float32 and disconnect/reuse slots. Validate every frame length against its header and ensure every active subscription receives audio.

### A17 — TCI power reporting stops following actual settings

**Medium · Reproduced.** Evidence: [drive command](../../src/tci_server.c#L322), [state reporting](../../src/tci_server.c#L363).

After the first remote drive change, `drive_set` permanently overrides the conversion from actual `cfg_pwr`. A later front-panel or other-client power change is not reflected in TCI reports. The plain integer is also written by the GUI thread and read by the socket thread without synchronization.

The host harness set remote drive to 50 and then actual power to 1.0 W. Reported drive remained 50; the current conversion from actual power gives approximately 9.

**Future direction:** derive reports from authoritative effective power, or explicitly invalidate/synchronize any requested-value cache on every source of power changes.

**Verification:** alternate remote, local and protection-induced power changes and verify all clients converge on actual state.

### A18 — Microphone Off does not cancel an outstanding permission request

**Medium · Code-established.** Evidence: [microphone toggle](../../webserver/src/x6100_webserver/views/remote.html#L266), lines 268–274.

Mic is enabled before the asynchronous permission result. If the user turns it off before accepting the prompt, there is no stream to stop yet. The late success handler unconditionally stores the live stream and says “Microphone ready,” even though the toggle is now Off. On/Off/On sequences can also leave an older granted stream untracked.

**Future direction:** associate each permission request with the current capture session; stop every stream returned to a cancelled request. Model requesting, ready, active, denied and off explicitly.

**Verification:** delayed grant/deny with On→Off and On→Off→On, navigation, reconnect and device removal; Off must leave no live capture tracks.

### A19 — Main audio capture does not handle a missing sample buffer

**Medium · Code-established.** Evidence: [capture callback](../../src/audio.c#L103), [decoder dereference](../../src/dsp.cpp#L529), [screen-recording dereference](../../src/screen_rec.c#L332), and the already guarded [monitor callback](../../src/audio.c#L514).

The capture callback ignores the result of `pa_stream_peek()` and forwards its pointer/count directly to DSP. Unlike the monitor callback in the same file, it does not handle a hole represented by a null buffer with a nonzero byte count. Active DSP/recording paths dereference that buffer. A peek error can similarly leave an invalid pointer/count pair.

**Future direction:** distinguish data, silence/hole and error before forwarding; define whether missing samples become silence or a discontinuity. Keep drop/error handling consistent with the stream state.

**Verification:** feed null/nonzero, empty and failed peek results through an audio shim while decoding/recording; processing should recover without dereferencing null.

### A20 — Updater locks do not span independent processes

**High · Code-established.** Evidence: [web update lock](../../webserver/src/x6100_webserver/apps.py#L333), [native update invocation](../../src/settings_window.cpp#L4137), [helper dispatch](../../rootfs/usr/sbin/x6100-update#L371), [shared stage and previous-version files](../../rootfs/usr/sbin/x6100-update#L200).

The browser's `threading.Lock` and GUI's `upd_busy` protect only their own callers. The updater helper has no process-wide lock. Two independently launched updates can both pass the journal check before either begins applying. They then share staging, previous files, fixed temporary paths and the same journal. One can remove or overwrite another's recovery material. Rollback, confirmation and boot-check also mutate that state.

**Future direction:** make serialization a responsibility of the helper, with an OS-level lock and operation identity covering the relevant state transitions. Return one authoritative busy/status result to all callers.

**Verification:** use temporary installations to interleave browser-style, GUI-style and boot-check invocations; only one mutation may own the recovery state. This scenario was inspected, not run against system services.

### A21 — Successful configuration acknowledgements can describe dropped work

**Medium · Code-established.** Evidence: [bounded scheduler](../../src/scheduler.cpp#L30), [configuration acknowledgements](../../src/cfg_socket.cpp#L284), [TCI enqueue](../../src/tci_server.c#L353).

The scheduler returns no acceptance result and drops work when its queue is full. The configuration socket nevertheless replies `ok` immediately after calling it. Under load a slider, display change or Ethernet update can appear successful without being applied. The same queue carries TCI control commands. Its `>` bound also admits 65 items for a declared limit of 64; allocation is unchecked before copying the argument.

**Future direction:** distinguish rejected, accepted and applied commands; coalesce replaceable updates and ensure stop/release commands have a reliable path. Give the GUI a bounded processing budget so a producer cannot monopolize it.

**Verification:** deliberately fill the queue while the GUI is busy. Every acknowledgement must correspond to an accepted/applied command, and overload must leave stop/release operational.

### A22 — Restore does not establish that database writers stopped

**High · Code-established, conditional on service failure.** Evidence: [unchecked service result](../../webserver/src/x6100_webserver/apps.py#L2221), [restore stop and replacement](../../webserver/src/x6100_webserver/apps.py#L2241), [GUI init stop behavior](../../rootfs/etc/init.d/S95gui#L31).

`gui_service()` ignores the exit status of `subprocess.run()`. Restore catches launch/timeout exceptions but accepts a normal nonzero Stop result, waits one second, then replaces databases and removes WAL/SHM files. A failed stop or surviving supervisor can leave a live writer using the old database. The restart exit code is ignored too, while the response can report `gui_restarted=true`.

**Future direction:** check the command result and verify the GUI and its supervisor have relinquished the files before replacement. Report restart failure truthfully and retain an actionable recovery state.

**Verification:** simulate a Stop command returning nonzero, a delayed exit and a failed restart. No database replacement should occur while a writer survives, and success must not claim a restart that failed.

## Additional inconsistencies and follow-up risks

These are deliberately separate from the established findings above.

- **Power-loss durability tradeoff:** `params/db.c:109` explicitly selects `synchronous=OFF` with WAL. This favors performance over durability on a portable radio whose power may disappear. Measure a safer policy and document the intended guarantee; this setting alone is not evidence of a particular corrupted database.
- **GPS reconnect:** `gps.c:86–100` waits without a predicate or timeout after connection failure and is woken only by USB-add events. A late-starting/restarting gpsd or a notification arriving just before the wait can leave it asleep. Verify daemon-start/reconnect behavior and prefer a bounded retry with an explicit predicate.
- **Worked-before statement sharing:** `qso_log.c:160–187` reuses one statement across reset/bind/step/column operations. SQLite serialization of individual calls does not serialize that complete sequence. FT8 decoder and JS8 paths both call it; establish whether their lifetimes can overlap before assigning a runtime race severity.
- **Multiple remote controllers:** `remote_server.c:308–328` tracks held keys per client but emits releases onto one shared physical input device. One client's release/disconnect can interfere with another client's held key. Verify two-controller and local/remote interaction; a control lease is also a usability improvement.
- **Documentation provenance:** README's removal list says the web Remote page was removed, while later sections describe the new browser remote. Clarify that the proprietary Link service was removed and a local replacement added. Likewise distinguish the removed update service from the current GitHub bundle updater. `webserver/README.md` also needs its remote, microphone and HTTPS description brought alongside the manual.

## Validation performed and limits

| Check | Result |
|---|---|
| Production TCI handlers with radio/audio stubs | Reproduced A01 and A17 without transmitting |
| Production observer implementation with controlled thread interleaving | AddressSanitizer reproduced A04 |
| Production updater functions with temporary paths and fault injection | Reproduced A02 and A03 |
| Production web helpers with temporary files and controlled interleaving | Reproduced A11 and A12 |
| JavaScript frequency round-trip | Reproduced A13 |
| Production legacy settings writer with an aborting SQLite trigger | Reproduced A14 |
| Production ADIF reader with small input fixtures | Reproduced A09 and A15 |
| Python AST parsing | All six webserver Python modules parsed successfully |
| Existing TCI host syntax target, copied into temporary workspace | Failed: `screen_light.h` missing from the supplied stub include tree |

The TCI host harness also needs current shims for `backlight_tick`, `cfg_pwr_set` and `radio_set_tci_mute` before its full build can reflect current production dependencies. Temporary shims were used only for isolated audit checks. This is a test-maintenance gap, not evidence that the target firmware build fails.

Web-helper checks executed the production functions extracted through Python's AST with minimal request/response stand-ins, because Bottle was unavailable in the host environment. They establish the file-operation interleavings, not a full HTTP integration result.

The top-level CMake file refers to private `tests/` that are absent from this checkout. No complete firmware build, RF transmission, on-device stress test, live update, restore or power-interruption test was performed. Temporary harnesses lived outside the repository. Passing isolated checks should not be read as a clean bill of health for unreviewed paths.

## Suggested future order

1. Close TX ownership/release holes and make stop/disconnect behavior verifiable (A01).
2. Make update rollback repeatable and interruption-safe; bind restore to the reviewed upload and require stopped writers (A02, A03, A10, A20, A22).
3. Establish GUI/observer ownership and recording job lifetimes (A04–A08).
4. Protect imported/logged data and concurrent configuration updates (A09, A11–A15).
5. Correct audio/state convergence and command acknowledgements, then expand the remote interface (A16–A19, A21).

All remedies and verification steps above are suggestions for later work; none was implemented as part of this audit.
