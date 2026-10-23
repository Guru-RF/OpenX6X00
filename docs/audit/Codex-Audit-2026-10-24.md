# OpenX6X00 code audit 24 October 2026

Reviewed revision: `00f91e1d446aaca7cf6bcaeee85ec83dbd800420` (`v0.3.1`). Baseline: the [22 October audit](Codex-Audit.md) of `d6bf443ac07cb7978a4c7499decea25d95a25c90`, with its [usability proposals](Codex-Possible-Improvements.md) and [browser proposals](Codex-Remote-Improvements.md).

The original fixes address many of their specific failure paths. The current implementation still has defects in transmission release, browser pairing and logging, ADIF memory handling, and coordination between workers and the GUI. The new browser session model and X6200 integration introduce additional inconsistencies. This audit records **17 findings: 9 High and 8 Medium**. Twelve have isolated reproductions; five are established by source paths.

Only audit documentation and reproduction harnesses were added. Application source, configuration and dependencies were not changed; no radio services, live update, restore or RF transmission were exercised.

## Scope and evidence

The review compared the baseline with the current implementation across browser Panel/Screen control, TCI, control ownership, CAT, scheduler and observers, recording/audio, settings persistence, ADIF and Field Ops logs, web configuration, backup/restore, updater recovery, and the new hardware abstraction. Build and packaging changes were inspected, and the existing host GUI was rebuilt. The modem mathematics, vendored libraries, kernel and baseband firmware were outside an exhaustive review. X6100 and X6200 hardware behavior requires separate validation.

**Reproduced** means production functions or JavaScript ran in an isolated host harness. Some harnesses extract functions and replace requests, services, audio or hardware with stand-ins; their limits are stated with the finding. **Code-established** means the implementation contains the described failure path, without a runtime reproduction. **Follow-up** items are kept separate.

**High** denotes credible unintended continued TX, loss of authorization boundaries, memory corruption/crash, wrong installed software or lost QSO data. **Medium** denotes narrower operating, persistence or recovery defects. These are engineering priorities, not CVSS scores. IDs **B01–B17** identify this review; **A01–A22** retain their original meanings.

## Current findings

| ID | Priority | Finding | Evidence |
|---|---|---|---|
| B01 | High | TCI refuses its own unkey after control or permission changes | Reproduced |
| B02 | High | Unpaired viewers can read the pairing code in the screen mirror | Code-established |
| B03 | High | Logging a VHF frequency overflows the ADIF formatting buffer | Reproduced with AddressSanitizer |
| B04 | High | A truncated ADIF length makes the parser read beyond the file buffer | Reproduced with a guard page |
| B05 | High | FT8 still enters LVGL and notifies observers from its worker | Code-established |
| B06 | High | The separate Field Ops log parser reads beyond a truncated line | Reproduced with AddressSanitizer |
| B07 | High | Browser log receipts can confirm another QSO or cause duplicate retries | Reproduced |
| B08 | High | A rejected upload can replace the bundle of an accepted update | Reproduced |
| B09 | High | A stale log row number can delete or edit the wrong QSO | Reproduced |
| B10 | Medium | Confirming browser TX cancels the PTT renewal timer | Reproduced |
| B11 | Medium | Tuning the inactive VFO can change the active mode and lose the requested frequency | Reproduced |
| B12 | Medium | A shared SQLite rollback loses the new settings queue's retry state | Reproduced |
| B13 | Medium | Cluster configuration still loses successful concurrent edits | Reproduced |
| B14 | Medium | The browser mirror injects the wrong keys on X6200 | Code-established |
| B15 | Medium | The browser Panel rejects X6200 SAM and WFM selections | Code-established |
| B16 | Medium | A dropped playback completion permanently blocks later playback | Code-established |
| B17 | Medium | Log mode edits leave contradictory MODE and SUBMODE fields | Reproduced |

### B01 TCI refuses its own unkey after ownership changes

**High · Reproduced.** Sources: [TCI authorization](../../src/tci_server.c#L597), [unkey handling](../../src/tci_server.c#L687), [local takeover](../../src/control_owner.c#L90), [physical PTT timeout](../../src/tci_server.c#L819).

The general permission and control-owner checks run before the handler distinguishes `trx:0,false;` from a new key request. During a TCI transmission, a physical key or knob makes LOCAL the control owner. The original TCI client then cannot reclaim control while the radio reports TX, so its explicit unkey returns before the TX-session cleanup. Changing TCI clients to Listen only during a transmission rejects that release too.

The harness ran the production command handler with unchanged `control_owner.c` and a stub reporting active TX. In both cases, `keyed=1`, `tx_session_end_calls=0` and no unkey command was queued. A physical-microphone TCI transmission can therefore continue until disconnect or the 180-second timeout. The original non-voice audio-key bug A01 has a targeted fix; this is a different release failure introduced by authorization ordering.

**Remedy and verification:** authorize release against the TX session that actually keyed, independently of permission to start another transmission. Test explicit unkey, local takeover, Listen only, revoke, disconnect and timeout for each keying mechanism.

### B02 The screen mirror exposes the pairing secret

**High · Code-established.** Sources: [pairing request](../../src/remote_server.c#L1437), [code display](../../src/remote_server.c#L435), [ordinary label rendering](../../src/msg_box.c#L62), [display forwarding](../../src/screen_rec.c#L343), [unpaired viewer defaults](../../src/remote_server.c#L1660), [frame capture](../../src/remote_server.c#L1844), [JPEG delivery](../../src/remote_server.c#L1943).

An unpaired client can send `pair?`. The radio displays the code in the ordinary LVGL message label. Every display region enters the remote shadow buffer, and the resulting JPEG goes to all connected screen viewers without a role check or code redaction. New connections are view-only but have screen delivery enabled.

A network viewer can request the code, read it through its own screen mirror, and submit it to pair. The first paired session can become admin. This defeats the browser's intended requirement to obtain a code from the local display. The finding concerns that pairing boundary; the separate documented trusted-network TCI interface already has its own access policy. The complete rendering path was traced in source, without a full GUI pairing experiment.

**Remedy and verification:** keep pairing secrets out of all streams available to unpaired clients, or require a separate local approval action that a viewer cannot reproduce. Test both the Screen page and the Panel's screen tab while pairing and creating the first admin.

### B03 VHF log formatting writes beyond the stack buffer

**High · Reproduced with AddressSanitizer.** Sources: [frequency formatting](../../src/adif.c#L354), [FT8 log call](../../src/ft8_window.c#L232), [default 2 m band](../../sql/params.sql#L45), [transverter ranges](../../src/cfg/settings_manager.h#L455).

`write_freq()` formats MHz with four fractional digits using `sprintf()` into `char str_freq[8]`. `145.5000` requires nine bytes including the terminator. The shipped configuration includes a 145.5 MHz transverter frequency; this is within supported application settings.

Calling the production formatter with `145.5f` produced an AddressSanitizer **stack-buffer-overflow, WRITE of size 9**, at `adif.c:356`. Logging a VHF FT8 contact reaches this formatter and can corrupt memory or crash the GUI. The formatter predates this review; it was not covered by A09's import finding.

**Remedy and verification:** use a sufficiently sized buffer and bounded formatting. Exercise complete ADIF logging at HF, 145.5 MHz, 433.5 MHz and the supported frequency limits under a sanitizer.

### B04 Truncated ADIF lengths still escape the input bounds

**High · Reproduced with a guard-page allocation.** Sources: [length conversion](../../src/adif.c#L138), [exact-sized input allocation](../../src/adif.c#L187), [startup import](../../src/gui_main.c#L250).

The A09 fix checks that each field value fits inside the file. The replacement parser nevertheless calls `strtoul()` on the original input, which is allocated with exactly `malloc(size)` and has no terminating NUL. A file ending with `<CALL:5` lets the numeric conversion read beyond that allocation before the later `q < end` check runs.

The production reader was called with that seven-byte file. A malloc wrapper placed its seven-byte input allocation immediately before an unreadable page; AddressSanitizer recorded a read SEGV inside libc, called from `tag_parse()` at line 138. This placement makes the boundary deterministic. A normal heap allocation may mask the defect, so the reproduction does not establish that every such file crashes on a radio.

**Remedy and verification:** parse digits within the explicit end pointer, checking conversion and overflow. A sentinel allocation can prevent this particular libc overread but does not replace length validation. Test EOF immediately after the colon, within the digits, before `>`, and after extreme/negative length strings.

### B05 FT8 worker callbacks still manipulate the GUI

**High · Code-established.** Sources: [worker callback contract](../../src/ft8/audio_worker.h#L24), [decode callback](../../src/ft8_window.c#L1068), [repeat completion](../../src/ft8_window.c#L1210), [delayed observer](../../src/cfg/subject.cpp#L40), [observer delivery](../../src/cfg/subject.cpp#L168), [button observer teardown](../../src/button_bar.cpp#L479).

FT8's `on_message` and `on_tick` callbacks run on the audio/decode worker. When an automatic reply is generated, `add_rx_text()` directly calls `lv_finder_set_cursor()` and may change the TX frequency and CQ subject. When CQ repeats expire, `transmit()` also sets that subject from the worker. Its button observer calls `lv_async_call()`, which itself enters LVGL immediately.

The current Subject implementation logs off-GUI notification but continues dispatching. Its new membership check handles deletion by an earlier callback on the same thread; it releases the mutex before dereferencing the observer, so concurrent GUI deletion still has an unsafe window. Changing button pages during a worker-originated notification can therefore retain the A04 lifetime problem. The direct finder update also races GUI rendering independently of observer lifetime.

The CAT producer from A05 now uses `scheduler_call()`. That closes the original CAT route but does not establish the all-producers GUI ownership rule on which the A04 remedy depends.

**Remedy and verification:** deliver FT8 UI changes through GUI-owned jobs and define ownership of shared QSO/TX state. Do not put a blocking GUI call into a worker that the GUI joins during teardown without resolving that shutdown dependency. Stress automatic reply and CQ completion while changing pages, closing the app and rebuilding themes, with thread assertions and sanitizers.

### B06 Field Ops has a separate unbounded ADIF reader

**High · Reproduced with AddressSanitizer.** Sources: [field extraction](../../src/field/flog.c#L183), [line reader](../../src/field/flog.c#L211), [activation reload](../../src/dialog_field.c#L140).

Field Ops does not use `adif.c` for its activation history. Its `get()` limits the declared length against the destination buffer but does not check how much input remains after `>`. `field_log_read()` supplies a 1,024-byte stack line.

A fixture containing `<eor>`, 1,009 spaces and `<CALL:15>` is 1,023 bytes long. The reader accepts the line as a record, then copies 15 bytes from its final NUL through the end of the stack buffer. AddressSanitizer reported **stack-buffer-overflow, READ of size 15**, at `flog.c:197`. A malformed or truncated activation log can crash history reload. Fixing the main importer alone does not protect this reader.

**Remedy and verification:** share a bounded field parser or pass the record length through every extraction. Reject incomplete records, and test truncated fields near both the logical line end and the physical buffer boundary.

### B07 Browser log receipts do not identify an operation uniquely

**High · Reproduced.** Sources: [browser ID allocation](../../webserver/src/x6100_webserver/views/panel.html#L791), [answer cache](../../src/remote_server.c#L1033), [broadcast receipts](../../src/remote_server.c#L1087), [log submission](../../src/remote_server.c#L1160), [browser reconciliation](../../webserver/src/x6100_webserver/views/panel.html#L761).

Each browser starts its log counter at 1. The radio caches results by that number alone and broadcasts completion to every panel. After one browser logs ID 1, another browser with its own pending ID 1 can receive or query that success, clear its draft and show “logged” even if its request never reached the radio. The single control holder does not prevent this: pending requests survive disconnect and are reconciled after another browser has operated the radio.

The answer ring has only 16 entries. An evicted successful operation becomes `unknown`; the browser treats `unknown` in the same radio boot as permission to resend once. The server queues an existing ID as a fresh log write, without deduplication. A successful contact can therefore be duplicated after delayed reconciliation.

The harness ran production cache, query and submission functions with a counting scheduler. It reproduced another browser receiving `logged 1 ok`, a successful ID becoming `unknown` after 16 newer results, and two dispatches for two identical submissions. The downstream disk writer was stubbed; the test establishes incorrect receipts and duplicate dispatch, not two actual disk writes.

**Remedy and verification:** give each operation a stable identity across browsers and reconnects, bind receipts to that identity, and make submission idempotent. Cache eviction must mean uncertain delivery. Test two browsers starting at ID 1, lost acknowledgements, radio restart, eviction and retries.

### B08 A rejected upload can change an accepted update

**High · Reproduced.** Sources: [job reservation](../../webserver/src/x6100_webserver/apps.py#L391), [shared upload path](../../webserver/src/x6100_webserver/apps.py#L436).

`upload_update()` writes `/mnt/update/upload.tar.gz.part` and replaces the shared `upload.tar.gz` before calling `update_start()`, where the busy check occurs. Bundle A can be accepted while its worker has not yet opened the file. Upload B replaces that same path, then receives “An update is already running.” A's worker subsequently installs B.

The reproduction delayed the accepted worker's open, submitted B, and then ran the production updater's install/stage/apply functions against temporary paths. A was accepted; B received the busy error; the installed application and version were B's. Development unsigned fixtures and stubbed service restarts isolated the test. On a production radio B still has to pass signature and model validation; the defect substitutes one otherwise acceptable bundle for another. The helper's new process lock cannot protect HTTP writes outside the helper.

**Remedy and verification:** reserve a job before mutating upload state, give every upload an immutable path, and bind the job to that exact file/digest. Interleave two uploads and an existing upgrade, including rejected and interrupted requests.

### B09 Stale log row numbers can target a different contact

**High · Reproduced.** Sources: [ordinal record selection](../../webserver/src/x6100_webserver/apps.py#L742), [delete endpoint](../../webserver/src/x6100_webserver/apps.py#L783), [edit endpoint](../../webserver/src/x6100_webserver/apps.py#L878).

The web log API identifies a QSO by its current ordinal within a file, without a record identity or revision precondition. The directory lock added for A11 serializes writes, but it cannot establish that the current row is the one the user reviewed.

In a simulated two-tab sequence, both tabs had loaded contacts A/B/C. The first deleted A at index 0. The second deleted its stale index 1, intending B. Both production calls succeeded; the remaining contact was B because C had been deleted instead. The same indexing affects edits and selected exports.

**Remedy and verification:** submit a stable record identity or a digest/revision of the original record and validate it while holding the lock. A stale selection should fail with a conflict. Test delete/delete, delete/edit and delete/export from two tabs.

### B10 TX confirmation stops browser PTT renewal

**Medium · Reproduced.** Sources: [renewal interval](../../webserver/src/x6100_webserver/static/js/radio_session.js#L194), [timer clearing](../../webserver/src/x6100_webserver/static/js/radio_session.js#L180), [TX confirmation](../../webserver/src/x6100_webserver/static/js/radio_session.js#L214), [server lease](../../src/remote_server.c#L1774).

`RadioPtt.press()` starts a one-second renewal interval. When actual TX is confirmed, `radioTx(true)` calls `clear()`, which cancels both the confirmation timeout and the renewal interval. Holding PTT with the radio microphone then stops renewing the server's three-second lease, so transmission ends while the button remains held. Both Panel and Screen use this component.

The complete production JavaScript ran with a fake clock: press at 0 ms, TX confirmation at 100 ms, hold until 4,000 ms. Only the initial `key PTT 1` was sent; the renewal timer was null. This checks timer behavior, not microphone or network latency.

**Remedy and verification:** cancel the confirmation timeout independently, keeping renewal active until release, refusal or disconnect. Test a confirmed hold longer than the lease, delayed confirmation and cancellation during startup.

### B11 Inactive VFO tuning switches the active context

**Medium · Reproduced.** Sources: [background frequency setter](../../src/cfg/settings_manager.cpp#L416), [frequency observer](../../src/cfg/settings_manager.cpp#L616), [implicit band loading](../../src/cfg/settings_manager.cpp#L775), [deferred write after notification](../../src/cfg/parameter.h#L143).

Changes to either VFO can trigger an implicit global band switch. That loader preserves the active VFO's frequency, although the inactive VFO may have caused the switch. Loading the new band overwrites the requested inactive frequency and changes the active VFO's mode. The original parameter setter then enqueues its requested value using the context that changed during notification, leaving memory and disk inconsistent.

The production SettingsManager and SQLite defaults began with A/B at 14.1 MHz, USB, band 7. Setting `cp_bg_freq` to 7.1 MHz produced band 4, A/B still at 14.1 MHz, and active mode LSB. A flush persisted 7.1 MHz for band 4's B frequency despite the in-memory B remaining 14.1 MHz.

**Remedy and verification:** define how inactive-VFO tuning affects band context, preserve the VFO that actually changed, and capture the intended persistence context before callbacks can change it. Test both active VFO choices with cross-band background tuning, split use and reload.

### B12 Settings retry state can be lost inside another writer's transaction

**Medium · Reproduced with rollback fault injection.** Sources: [shared database initialization](../../src/params/params.c#L479), [legacy transaction](../../src/params/db.c#L146), [failed commit rollback](../../src/params/db.c#L187), [pending entry removal](../../src/cfg/pending_writes.cpp#L91).

The legacy settings worker and new SettingsManager flush worker share one SQLite connection. SQLite serializes individual calls, but the new worker's writes can execute inside the legacy worker's open transaction. `PendingWrites` removes an entry as soon as its SQL step returns success. A later failed legacy commit rolls back that row after its retry entry has been discarded.

The harness used production SettingsManager/storage/pending-write code with the default database. It opened a transaction, set and flushed volume 42, then injected rollback and flushed again. Persisted volume returned to 30 while memory stayed 42; the second flush had nothing to retry. This deliberately reproduces the rollback condition, not a physical SD failure. The original A14 legacy dirty flags now wait for commit; the new queue does not share that protection.

**Remedy and verification:** give transactions ownership across their entire lifetime, use separate connections with defined transaction boundaries, or retain pending state until the owning transaction commits. Inject failure after new settings have been stepped inside a legacy transaction and verify both writers recover.

### B13 Cluster edits still overwrite a stale snapshot

**Medium · Reproduced; A12 partially unresolved.** Sources: [write-only lock](../../webserver/src/x6100_webserver/apps.py#L207), [alert read and mutation](../../webserver/src/x6100_webserver/apps.py#L1575).

Cluster handlers read current options and construct the new state before `write_cluster_conf()` takes `config_lock`. Two requests can therefore read the same starting list and then serialize two incompatible replacements. Unique temporary files and a lock around writing prevent rename collisions but do not preserve both changes.

Two production `add_cluster_alert()` calls were synchronized after reading. Both returned success; only one of the two alerts remained. Channel and macro mutations have a wider lock and no longer share that original A12 path.

**Remedy and verification:** lock the full read/change/write operation for cluster resources, and use revision checks for whole-list editing. Test concurrent alert additions and overlapping server/filter edits.

### B14 X6200 mirror buttons use X6100 input codes

**Medium · Code-established.** Sources: [browser injection map](../../src/remote_server.c#L237), [X6200 input decoder](../../src/front_keys.c#L33).

The remote server's fixed key table retains the X6100 mapping. The new X6200 decoder interprets several of those codes differently: browser APP invokes MSG, KEY invokes APP, MSG invokes DFN, DFN invokes KEY, PRE invokes ATU, and ATU invokes FST. Other displayed controls have no corresponding X6200 physical key. In particular, browser PRE toggles ATU on a tap and can start a tuning cycle on a hold; see [ATU handling](../../src/screen_main.c#L755).

**Remedy and verification:** map semantic browser keys through the board/backend and advertise supported controls. Check every press, release, long press and knob push against both models, including the screenshot-driving tool's separate mapping.

### B15 The Panel silently rejects SAM and WFM

**Medium · Code-established.** Sources: [Panel mode list](../../webserver/src/x6100_webserver/views/panel.html#L287), [mode application guard](../../src/remote_server.c#L635), [TCI capability-aware parser](../../src/tci_server.c#L529), [TCI greeting](../../src/tci_server.c#L460).

The browser lists SAM and WFM as modes 8 and 9, but `panel_apply(P_MODE)` only applies values below 8. Selecting either on an X6200 leaves the previous mode in use. TCI's parser accepts the extra modes when the capability exists, while its greeting still advertises only the original eight.

**Remedy and verification:** derive the advertised and accepted mode set from the same capability data. Test local, Panel, CAT and TCI mode changes on both models, and return an explicit rejection for unsupported modes.

### B16 Playback cleanup can be dropped when the queue is full

**Medium · Code-established; A08 partially unresolved.** Sources: [worker completion](../../src/recorder_window.c#L146), [thread join and flag reset](../../src/recorder_window.c#L152), [new-play guard](../../src/recorder_window.c#L291), [bounded queue](../../src/scheduler.cpp#L53), [recording failure notification](../../src/recorder.c#L103).

The playback worker's only cleanup notification is `scheduler_put_noargs(play_done)`, whose rejection is ignored. If the queue is full at completion, `play_done()` never joins the worker or clears `play_thr_live`. Every subsequent Play returns immediately, including after closing and reopening the recorder. The ordinary queue must be full at that specific notification; this was established from the code rather than reproduced with audio hardware.

The recorder's storage-error notification has the same one-shot loss: it sets `failed=true`, attempts an unchecked enqueue, and then stops making writes or notifications. A full queue can leave the UI reporting recording after storage has failed.

**Remedy and verification:** keep completion/error state independently of a best-effort queued notification and reconcile it on the GUI thread. Fill the queue exactly when playback ends and when a write fails; cleanup and visible error reporting must eventually occur without restarting the GUI.

### B17 Editing a log mode leaves the old SUBMODE active

**Medium · Reproduced.** Sources: [display mode selection](../../webserver/src/x6100_webserver/apps.py#L864), [field-by-field editing](../../webserver/src/x6100_webserver/apps.py#L895).

The editor displays SUBMODE in preference to MODE, but editing MODE does not remove or reconcile SUBMODE. A production edit of an existing `MODE=MFSK, SUBMODE=JS8` QSO to FT8 returned OK and left `MODE=FT8, SUBMODE=JS8`. The log view still displayed JS8. Saving a displayed submode name directly as MODE can also produce an inconsistent ADIF representation.

**Remedy and verification:** translate the chosen display mode into a canonical MODE/SUBMODE pair and update both atomically. Round-trip JS8, FT4 and FT2 through unrelated field edits and transitions to FT8, SSB and other ordinary modes; verify both raw ADIF and rendered/exported results.

## Disposition of the original findings

“Addressed” below refers to the original demonstrated path. It does not certify the entire subsystem. Source-reviewed fixes are distinguished from cases rerun in this audit.

| Old ID | Current disposition | Current evidence |
|---|---|---|
| A01 | Addressed in source | Incompatible TCI audio modes are explicitly refused at `tci_server.c:389–393`; B01 is a different unkey defect. |
| A02 | Original windows addressed and regression-checked | Replacement intent is persisted first; PENDING precedes applied. Injected interruptions restored the old application/version. |
| A03 | Original retry deletion addressed and regression-checked | Two consecutive production undo calls retained the restored file. |
| A04 | Partially addressed | Same-thread peer deletion is checked; FT8 still violates the assumed GUI-only producer rule. See B05. |
| A05 | Original CAT path addressed in source | CAT request processing uses `scheduler_call()` at `cat.cpp:1176`. Other worker paths remain in B05. |
| A06 | Addressed in source | Screenshot work uses an owned job and serialized capture state in `screen_shot.c`. |
| A07 | Addressed in source | Recorder close/write use a shared mutex; on-state is atomic and the closed handle is cleared. |
| A08 | Main lifetime paths addressed; completion remains fallible | Playback captures its file before thread creation and no longer reasserts Play after Stop; B16 can prevent joining and later playback. |
| A09 | Original value-copy overread addressed; parser still unsafe | Value bounds are checked; B04 reproduces an unbounded numeric conversion in the replacement parser. |
| A10 | Addressed and regression-checked | Restore binds to a staged token; stale preview returned HTTP 409. |
| A11 | Original append/rewrite race addressed in source | Web rewrite and native field append take the same directory flock. Stale record identity is a separate defect in B09. |
| A12 | Partially addressed | Channels/macros protect full mutation; cluster still loses edits, reproduced in B13. |
| A13 | Addressed in source | Memory/channel editing retains exact original Hz for an unchanged displayed frequency. |
| A14 | Original legacy dirty-state loss addressed in source | Dirty flags clear after commit, with failed saves retried. Shared-connection interaction with new settings remains in B12. |
| A15 | Original layout/retirement paths addressed in source | Parser follows field lengths and EOR, accepts common layouts, reports broken records and preserves database failures for retry. B04 limits parser safety. |
| A16 | Original paths addressed in source | Capture demand is derived by the socket owner; capture uses one locked subscription/format snapshot. |
| A17 | Addressed in source | Atomic drive cache is valid only while actual configured power matches the cached setting. |
| A18 | Addressed in source | Both browser pages stop streams returned to an obsolete microphone request generation. |
| A19 | Addressed in source | Capture handles failed peek, empty data and null-buffer holes before forwarding samples. |
| A20 | Helper serialization addressed in source | Mutating helper commands take an OS flock; B08 occurs in HTTP upload before that helper owns the file. |
| A21 | Original false-OK configuration path addressed in source | Configuration checks enqueue success; urgent unkeys have reserved capacity. B16 shows an unchecked notification elsewhere. |
| A22 | Original stop-failure path addressed and regression-checked | Persistent-writer/nonzero-stop fixture returned HTTP 500 before any replacement. |

## Validation and reproduction

The [saved harnesses and commands](repro-2026-10-24/README.md) use temporary files, in-memory databases, fake clocks and stubs. They deliberately demonstrate the reviewed failures; a successful demonstration does not mean the corresponding defect is fixed.

| Check | Result |
|---|---|
| Saved `repro-2026-10-24/run.py` runner | All 12 defect reproductions and selected A02/A03/A10/A22 regression expectations matched |
| Existing host build, `cmake --build build-host --target x6100_gui params_sqlite -j 4` | Passed; optional libgps absent, existing CMake/LVGL enum warnings |
| Host test inventory, `ctest --test-dir build-host -N` | No registered tests; private `tests/` is absent |
| Current production TCI handler with control-owner implementation | Reproduced B01 with both local takeover and Listen only |
| Current JavaScript PTT component with fake clock | Reproduced B10 |
| Production browser log cache/query/dispatch functions | Reproduced B07 identity collision, eviction and repeated dispatch |
| Production ADIF formatter with AddressSanitizer | Reproduced B03 |
| Production ADIF reader with exact-size guarded input allocation | Reproduced B04 at EOF conversion |
| Production Field Ops reader with AddressSanitizer | Reproduced B06 |
| Production SettingsManager and storage code with SQLite defaults | Reproduced B11 and B12 |
| Production Python functions extracted through AST with request stand-ins | Reproduced B08, B09, B13 and B17 |
| Production updater functions with temporary install/recovery paths | A02/A03 interruption and repeated-undo checks passed |
| Production restore checks with request/process stand-ins | A10 stale-token and A22 surviving-writer rejection checks passed |
| Python AST checks | Six webserver modules, updater and bundler parsed successfully |
| Shell syntax | `buildroot/local-build.sh` and `host/build.sh` passed |
| Existing TCI syntax target in an isolated copy | Failed: `tci_server.h` now needs `board.h`, absent from the supplied stub tree |

The TCI host test also retains old enum names and the previous scheduler signature. Its failure is a test-maintenance gap, not evidence that firmware compilation fails. The host GUI build exercises the stub backend; it does not compile or validate the X6200 hardware library. No full Buildroot images or RF/hardware stress tests were run.

## Follow-up work

- **Update power-loss durability:** the process-interruption journal windows now have targeted fixes, but copying previous files without fsync and persisting rollback state before syncing affected rename directories still require power-loss tests. Successful process-crash recovery does not prove persistence after sudden power loss.
- **Settings durability:** the legacy database still selects `synchronous=OFF` with WAL. Establish the intended portable-radio durability guarantee and measure a safer policy.
- **Shared worked-before statement:** `qso_log_search_worked()` still shares a statement across reset/bind/step calls. This review did not establish an overlapping pair of current callers, so the previous concern remains follow-up rather than a new race finding.
- **Hardware integration:** validate X6200 key mappings, extra modes, spectrum scaling and TX level behavior on its backend. The host stub cannot establish actual control-library semantics or RF output.
- **Browser contract coverage:** U04, U10 and R3/R4/R6 now have implementations, but B01/B02/B07/B10 show that visible ownership, delivery and TX states still need end-to-end failure tests. The old usability documents remain proposals and do not establish release acceptance.

## Suggested repair order

1. Restore reliable session-owned unkey and keep pairing secrets out of unpaired viewing paths: B01, B02.
2. Remove the ADIF memory errors and remaining FT8 GUI-thread violations: B03–B06.
3. Bind logging and updates to the exact reviewed operation and preserve settings across rollback: B07–B09, B12, B17.
4. Correct PTT renewal, inactive-VFO behavior, concurrent cluster edits and X6200 browser controls: B10, B11, B13–B15.
5. Make recorder completion reliable under load and restore the public host regression harness: B16 and the TCI test-maintenance gap.
